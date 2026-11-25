// The FPGA XM (FastTracker II) player: the first version. It parses the whole file in the SRAM,
// decodes the delta samples in place and mixes 32 channels at 48 kHz into the audio FIFO.
// Now: linear frequency, volume/pan, volume envelope/fadeout, the common effects.
#include <string.h>
#include "fpga.h"
#include "audio.h"
#include "mixer.h"
#include "mmio.h"
#include "analysis.h"
#include "fastmix.h"
#include "resample.h"

#define XM_CH 32
#define XM_PAT 256
#define XM_INS 128
#define XM_SMP 256
#define XM_CHUNK 256
#define XM_MIXBUF 128

typedef struct {
  const uint8_t *data;
  uint32_t len, loop, llen;   // in samples, 8-bit data
  uint8_t vol, pan;
  int8_t ft, rel;
} xsmp_t;

typedef struct {
  int nsmp, first;
  const uint8_t *keymap;
  const uint8_t *venv;
  int nvol, vtype, vsus, vls, vle;
  uint16_t fade;
} xins_t;

typedef struct {
  const uint8_t *data;
  uint32_t len, loop, llen, end;
  uint32_t pos, step, base_step, target_step;
  int note, vol, pan, active, keyoff, smp, inst;
  uint8_t eff, par, volcol;
  int env, fade;
  int vib_s, vib_d, vib_p, porta, tone, arp_x, arp_y;
  int vol_slide, pan_slide;
  int porta_speed;
  int mix_vol, lgain, rgain;
} xchan_t;

typedef struct {
  const uint8_t *mod;
  uint32_t size;
  char title[21];
  int chn, song, restart, npat, nins, linear, interp;
  uint8_t orders[256];
  struct { const uint8_t *d, *end; int rows; } pat[XM_PAT];
  xins_t ins[XM_INS];
  xsmp_t smp[XM_SMP];
  int nsmp;
  xchan_t ch[XM_CH];
  int order, row, tick, speed, bpm, tleft, spt;
  int jump, brk, brow;
  int gvol;
  int active, ended;
  char err[48];
  int vol, paused;
  int32_t last_l, last_r, peak_l, peak_r;
  uint8_t vu[FMT_CHANNELS];
  uint32_t rate;
  rs_t rs;
} xm_t;

static xm_t X;
static int32_t mixbuf[XM_MIXBUF * 2];
static uint8_t mix_skip[XM_CH];
#ifdef XM_PROFILE
// host-only diagnostic: how many channels are actually mixed, not how many the module has
uint64_t xm_prof_chan_samples;
uint32_t xm_prof_peak_chan, xm_prof_blocks;
uint32_t xm_prof_out_samples;
#define XM_PROF_SEC 128
uint64_t xm_prof_sec_chan_samples[XM_PROF_SEC];
uint32_t xm_prof_sec_samples[XM_PROF_SEC], xm_prof_sec_peak[XM_PROF_SEC];
#endif

static const uint32_t semi12[12] = {
  65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715
};
// the per-tick portamento factors (16.16): 2^(+/-param/192)
static const uint32_t porta_up[16] = {
  65536, 65773, 66011, 66250, 66489, 66730, 66971, 67213,
  67456, 67700, 67945, 68191, 68438, 68685, 68933, 69183
};
static const uint32_t porta_down[16] = {
  65536, 65300, 65065, 64830, 64596, 64364, 64132, 63901,
  63670, 63441, 63212, 62984, 62757, 62531, 62306, 62081
};
// 2^(+/-16*q/192) = 2^(+/-q/12), for the high nibble of a porta parameter
static const uint32_t porta_big_up[16] = {
  65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193,
  104032, 110218, 116772, 123715, 131072, 138866, 147124, 155872
};
static const uint32_t porta_big_down[16] = {
  65536, 61858, 58386, 55094, 51978, 49045, 46293, 43687,
  41226, 38907, 36719, 34653, 32704, 30868, 29136, 27497
};
// sin(2*pi*i/64) * 32767
static const int16_t vib_tab[64] = {
  0, 3212, 6393, 9512, 12539, 15446, 18204, 20787,
  23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609,
  32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329,
  23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
  0, -3212, -6393, -9512, -12539, -15446, -18204, -20787,
  -23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
  -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
  -23170, -20787, -18204, -15446, -12539, -9512, -6393, -3212
};

static uint16_t le16(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint32_t get_base_step(uint32_t rate) {
  return (uint32_t) (((uint64_t) 11418 * 48000) / rate);
}

static void set_error(const char *s) {
  int i = 0;
  for (; s[i] && i < (int) sizeof(X.err) - 1; i++) X.err[i] = s[i];
  X.err[i] = 0;
}

// the XM linear frequency: 11418 at C-4 (note 48), 12 semitones per octave
static uint32_t note_step(int note0, int ft) {
  if (note0 < 0) note0 = 0;
  if (note0 > 119) note0 = 119;
  int oct = note0 / 12;
  int rem = note0 % 12;
  uint32_t base = get_base_step(X.rate);
  uint32_t s = (uint32_t) (((uint64_t) base * semi12[rem]) >> 16);
  if (oct >= 4) s <<= (oct - 4);
  else s >>= (4 - oct);
  // 2^(ft/1536), ft = -128..127, linearly between the 1/128 semitone points
  if (ft) {
    uint32_t d = semi12[1] - 65536;
    if (ft > 0) s = (uint32_t) (((uint64_t) s * (65536 + (uint64_t) d * ft / 128)) >> 16);
    else {
      uint32_t f = 65536 - (uint32_t) ((uint64_t) d * (-ft) / 128);
      s = (uint32_t) (((uint64_t) s * f) >> 16);
    }
  }
  return s;
}

// the full 8-bit porta parameter: p = 16*q + r, 2^(p/192) = 2^(q/12) * 2^(r/192)
static uint32_t porta_factor(int p, int down) {
  if (p <= 0) return 65536;
  if (p > 255) p = 255;
  const uint32_t *big = down ? porta_big_down : porta_big_up;
  const uint32_t *sm = down ? porta_down : porta_up;
  return (uint32_t) (((uint64_t) big[p >> 4] * sm[p & 15]) >> 16);
}

static uint32_t semi_factor(int n) {
  if (n < 0) n = 0;
  int oct = n / 12, rem = n % 12;
  uint32_t f = semi12[rem];
  return oct ? (f << oct) : f;
}

// Decode the delta samples in place. 16-bit samples are also compacted to 8-bit: the mixer reads
// them as one byte anyway (the old mixer did >>8 per sample), and sequential 8-bit data is twice
// as cache friendly. Writing d[i] while reading d[2i], d[2i+1] is safe (i <= 2i).
static void sample_decode(xsmp_t *s, uint8_t *d, uint32_t bytes, int bits) {
  if (bits == 16) {
    int16_t old = 0;
    uint32_t n = bytes / 2;
    for (uint32_t i = 0; i < n; i++) {
      old = (int16_t) (old + (int16_t) (d[i * 2] | (d[i * 2 + 1] << 8)));
      d[i] = (uint8_t) (int8_t) (old >> 8);
    }
  } else {
    int8_t old = 0;
    for (uint32_t i = 0; i < bytes; i++) {
      old = (int8_t) (old + (int8_t) d[i]);
      d[i] = (uint8_t) old;
    }
  }
}

static void set_sample(xchan_t *c, int smp, int note) {
  if (smp < 0 || smp >= X.nsmp) return;
  xsmp_t *s = &X.smp[smp];
  c->data = s->data;
  c->len = s->len;
  c->loop = s->loop;
  c->llen = s->llen;
  c->end = s->llen ? s->loop + s->llen : s->len;
  c->smp = smp;
  c->vol = s->vol;
  c->pan = s->pan;
  c->pos = 0;
  c->env = 0;
  c->fade = 65536;
  c->keyoff = 0;
  c->active = s->len ? 1 : 0;
  c->step = note_step(note - 1 + s->rel, s->ft);
  c->base_step = c->step;
  c->target_step = c->step;
}

static void reset_env(xchan_t *c) { c->env = 0; c->fade = 65536; c->keyoff = 0; }

// the volume envelope of the instrument; return 0..64. The sustain point holds the envelope
// until keyoff; the loop point wraps while the note is held.
static int env_point_x(const xins_t *ins, int i) { return le16(ins->venv + i * 4); }

static int env_value(const xins_t *ins, int pos, int keyoff) {
  if (!ins->nvol || !(ins->vtype & 1)) return 64;
  const uint8_t *p = ins->venv;
  int n = ins->nvol;
  if (n > 12) n = 12;
  if (n > 0 && (ins->vtype & 2) && !keyoff && ins->vsus < n) {
    int sx = env_point_x(ins, ins->vsus);
    if (pos > sx) pos = sx;
  }
  if (n > 0 && (ins->vtype & 4) && !keyoff && ins->vls < n && ins->vle < n) {
    int lx = env_point_x(ins, ins->vls), ex = env_point_x(ins, ins->vle);
    if (ex > lx && pos >= ex) pos = lx + (pos - lx) % (ex - lx);
  }
  int x0 = le16(p), y0 = le16(p + 2);
  if (pos <= x0) return y0 > 64 ? 64 : y0;
  for (int i = 1; i < n; i++) {
    int x1 = le16(p + i * 4), y1 = le16(p + i * 4 + 2);
    if (pos <= x1) {
      int dx = x1 - x0;
      int y = dx ? y0 + (y1 - y0) * (pos - x0) / dx : y1;
      return y < 0 ? 0 : (y > 64 ? 64 : y);
    }
    x0 = x1; y0 = y1;
  }
  return y0 > 64 ? 64 : y0;
}

static void vol_slide(xchan_t *ch, int par) {
  int up = par >> 4, dn = par & 15;
  int v = ch->vol + (up ? up : -dn);
  ch->vol = v < 0 ? 0 : (v > 64 ? 64 : (uint8_t) v);
}

static void parse_row(void) {
  int pat = X.orders[X.order];
  if (pat >= X.npat) pat = 0;
  const uint8_t *p = X.pat[pat].d;
  const uint8_t *end = X.pat[pat].end;
  if (!p) return;
  int rows = X.pat[pat].rows;
  if (X.row >= rows) return;
  for (int r = 0; r < X.row; r++) {
    for (int c = 0; c < X.chn; c++) {
      if (p >= end) return;
      uint8_t b = *p++;
      if (b & 0x80) {
        int n = 0;
        if (b & 1) n++;
        if (b & 2) n++;
        if (b & 4) n++;
        if (b & 8) n++;
        if (b & 16) n++;
        if (p + n > end) return;
        p += n;
      } else {
        if (p + 4 > end) return;
        p += 4;
      }
    }
  }
  for (int c = 0; c < X.chn; c++) {
    xchan_t *ch = &X.ch[c];
    if (p >= end) return;
    uint8_t b = *p++;
    int note = 0, inst = 0, vol = 0, eff = 0, par = 0;
    if (b & 0x80) {
      if (b & 1) { if (p >= end) return; note = *p++; }
      if (b & 2) { if (p >= end) return; inst = *p++; }
      if (b & 4) { if (p >= end) return; vol = *p++; }
      if (b & 8) { if (p >= end) return; eff = *p++; }
      if (b & 16) { if (p >= end) return; par = *p++; }
    } else {
      if (p + 4 > end) return;
      note = b; inst = p[0]; vol = p[1]; eff = p[2]; par = p[3];
      p += 4;
    }
    ch->eff = (uint8_t) eff;
    ch->par = (uint8_t) par;
    ch->volcol = (uint8_t) vol;

    uint32_t old_step = ch->step, old_base = ch->base_step, old_pos = ch->pos;
    int is_tone = (eff == 0x03 || eff == 0x05) && note >= 1 && note <= 96;

    if (inst > 0 && inst <= X.nins) {
      ch->inst = inst - 1;
      xins_t *ins = &X.ins[inst - 1];
      int smp = ins->keymap ? ins->keymap[(note >= 1 && note <= 96) ? note - 1 : 0] : 0;
      ch->smp = ins->first + smp;
      if (note >= 1 && note <= 96) set_sample(ch, ch->smp, note);
    } else if (note >= 1 && note <= 96) {
      set_sample(ch, ch->smp, note);
    }

    if (note == 97) {
      ch->keyoff = 1;
    } else if (note >= 1 && note <= 96) {
      ch->note = note;
      reset_env(ch);
      if (is_tone) {
        ch->target_step = ch->step;       // the new note is the target
        ch->step = old_step;              // keep gliding from the old pitch
        ch->base_step = old_base;
        ch->pos = old_pos;
      } else {
        ch->base_step = ch->step;
        ch->target_step = ch->step;
      }
    }

    ch->vol_slide = 0;
    ch->pan_slide = 0;
    if (vol >= 0x10 && vol <= 0x50) {
      ch->vol = (uint8_t) (vol - 0x10);
    } else if (vol >= 0x60 && vol <= 0x6F) {
      ch->vol_slide = -(vol & 15);
    } else if (vol >= 0x70 && vol <= 0x7F) {
      ch->vol_slide = vol & 15;
    } else if (vol >= 0x80 && vol <= 0x8F) {
      int v = ch->vol - (vol & 15);         // fine volume slide down, once
      ch->vol = v < 0 ? 0 : (uint8_t) v;
    } else if (vol >= 0x90 && vol <= 0x9F) {
      int v = ch->vol + (vol & 15);         // fine volume slide up, once
      ch->vol = v > 64 ? 64 : (uint8_t) v;
    } else if (vol >= 0xA0 && vol <= 0xAF) {
      ch->vib_s = vol & 15;
    } else if (vol >= 0xB0 && vol <= 0xBF) {
      ch->vib_d = vol & 15;                 // vibrato depth
    } else if (vol >= 0xC0 && vol <= 0xCF) {
      ch->pan = (uint8_t) ((vol & 15) * 17);
    } else if (vol >= 0xD0 && vol <= 0xDF) {
      ch->pan_slide = -(vol & 15);
    } else if (vol >= 0xE0 && vol <= 0xEF) {
      ch->pan_slide = vol & 15;
    }

    switch (eff) {
      case 0x01: case 0x02: case 0x03: case 0x05:
        if (par) ch->porta_speed = par;          // the speed is remembered
        break;
      case 0x04: if (par) { ch->vib_s = par >> 4; ch->vib_d = par & 15; } break;
      case 0x08: ch->pan = (uint8_t) par; break;
      case 0x09: ch->pos = (uint32_t) par << 8 << 16; break;
      case 0x0A: break;
      case 0x0B: X.jump = par; break;
      case 0x0C: ch->vol = par > 64 ? 64 : (uint8_t) par; break;
      case 0x0D: X.brk = 1; X.brow = ((par >> 4) * 10) + (par & 15); break;
      case 0x0E:
        if ((par & 0xF0) == 0x60 && (par & 15) == 0) { /* pattern loop start: ignored */ }
        break;
      case 0x0F:
        if (par) {
          if (par < 32) X.speed = par;
          else X.bpm = par;
          uint32_t bpm = X.bpm ? X.bpm : 125;
          int spt = (int)(((uint64_t)120000 * X.rate) / (48000ull * bpm));
          if (spt < 1) spt = 1;
          if (spt > 4000) spt = 4000;
          X.spt = spt;
        }
        break;
      default: break;
    }
  }
}

static void chan_update_vol(xchan_t *ch, int c);

static void next_row(void) {
  X.row++;
  int pat = X.orders[X.order];
  if (pat >= X.npat) pat = 0;
  if (X.row >= X.pat[pat].rows || X.brk) {
    int o = X.jump >= 0 ? X.jump : X.order + 1;
    int r = X.brk ? X.brow : 0;
    X.jump = -1; X.brk = 0; X.brow = 0;
    X.order = o;
    X.row = r;
    if (X.order >= X.song || X.order < 0) { X.active = 0; X.ended = 1; return; }
  }
  X.tick = 0;
  parse_row();
  for (int c = 0; c < X.chn; c++) {
    X.ch[c].step = X.ch[c].base_step;
    X.ch[c].env++;
    chan_update_vol(&X.ch[c], c);
  }
}

static void chan_update_vol(xchan_t *ch, int c) {
  int ev = env_value(&X.ins[ch->inst], ch->env, ch->keyoff);
  int v = ch->vol * ev / 64;
  v = v * X.gvol / 64;
  v = v * ch->fade / 65536;
  ch->mix_vol = v;
  ch->lgain = v * (255 - ch->pan);
  ch->rgain = v * ch->pan;
  int vu = 0;
  if (ch->active && v && ch->data && ch->len) {
    uint32_t idx = ch->pos >> 16;
    if (idx >= ch->len) idx = ch->loop < ch->len ? ch->loop : 0;
    int s = (int8_t) ch->data[idx];
    vu = (s < 0 ? -s : s) * v >> 5;
    if (vu > 255) vu = 255;
  }
  X.vu[c] = (uint8_t) vu;
}

static void apply_tick(void) {
  for (int c = 0; c < X.chn; c++) {
    xchan_t *ch = &X.ch[c];
    if (!ch->active) continue;
    switch (ch->eff) {
      case 0x00:
        if (X.tick > 0 && ch->par) {
          int t = X.tick % 3;
          int n = t == 1 ? (ch->par >> 4) : (t == 2 ? (ch->par & 15) : 0);
          ch->step = (uint32_t) (((uint64_t) ch->base_step * semi_factor(n)) >> 16);
        }
        break;
      case 0x01:
        if (X.tick > 0 && ch->porta_speed) {
          ch->step = (uint32_t) (((uint64_t) ch->step * porta_factor(ch->porta_speed, 0)) >> 16);
          ch->base_step = ch->step;
        }
        break;
      case 0x02:
        if (X.tick > 0 && ch->porta_speed) {
          ch->step = (uint32_t) (((uint64_t) ch->step * porta_factor(ch->porta_speed, 1)) >> 16);
          ch->base_step = ch->step;
        }
        break;
      case 0x03:
      case 0x05:
        if (X.tick > 0 && ch->porta_speed) {
          if (ch->step < ch->target_step) {
            uint32_t n = (uint32_t) (((uint64_t) ch->step * porta_factor(ch->porta_speed, 0)) >> 16);
            ch->step = n > ch->target_step ? ch->target_step : n;
          } else if (ch->step > ch->target_step) {
            uint32_t n = (uint32_t) (((uint64_t) ch->step * porta_factor(ch->porta_speed, 1)) >> 16);
            ch->step = n < ch->target_step ? ch->target_step : n;
          }
          ch->base_step = ch->step;
        }
        if (ch->eff == 0x05 && X.tick > 0) vol_slide(ch, ch->par);
        break;
      case 0x04:
      case 0x06:
        if (X.tick > 0 && ch->vib_d) {
          ch->vib_p = (ch->vib_p + ch->vib_s) & 63;
          int32_t off = ((int32_t) vib_tab[ch->vib_p] * ch->vib_d * 256) >> 15;
          ch->step = (uint32_t) (((uint64_t) ch->base_step * (65536 + off)) >> 16);
        }
        if (ch->eff == 0x06 && X.tick > 0) vol_slide(ch, ch->par);
        break;
      case 0x0A:
        if (X.tick > 0) vol_slide(ch, ch->par);
        break;
      default:
        if (ch->vib_d && ch->volcol >= 0xB0 && ch->volcol <= 0xBF && X.tick > 0) {
          ch->vib_p = (ch->vib_p + ch->vib_s) & 63;
          int32_t off = ((int32_t) vib_tab[ch->vib_p] * ch->vib_d * 256) >> 15;
          ch->step = (uint32_t) (((uint64_t) ch->base_step * (65536 + off)) >> 16);
        }
        break;
    }
    if (X.tick > 0 && ch->vol_slide) {
      int v = ch->vol + ch->vol_slide;
      ch->vol = v < 0 ? 0 : (v > 64 ? 64 : (uint8_t) v);
    }
    if (X.tick > 0 && ch->pan_slide) {
      int p = ch->pan + ch->pan_slide * 8;
      ch->pan = p < 0 ? 0 : (p > 255 ? 255 : (uint8_t) p);
    }
    ch->env++;
    // the keyoff fadeout
    if (ch->keyoff) {
      int ins = ch->inst;
      int fade = (ins >= 0 && ins < X.nins) ? X.ins[ins].fade : 0;
      ch->fade -= fade;
      if (ch->fade < 0) ch->fade = 0;
    }
    chan_update_vol(ch, c);
  }
}

static void next_tick(void) {
  if (X.tick + 1 >= X.speed) {
    next_row();
  } else {
    X.tick++;
    apply_tick();
  }
  X.tleft = X.spt;
}


/* Minimal unsigned formatter: avoids pulling the whole printf machinery into the
 * firmware (it is linked on every page and was hurting the I-cache). */
static char *dbg_u(char *q, uint32_t v)
{
  char t[10];
  int n = 0;
  if (!v) { *q++ = '0'; return q; }
  while (v) { t[n++] = (char) ('0' + (v % 10u)); v /= 10u; }
  while (n) *q++ = t[--n];
  return q;
}

static int16_t fmout[XM_MIXBUF * 2];

/* Live diagnostics for the footer, same shape as the IT engine:
 * "<audible>/<channels>v <cpu>% r<mix share>" */
static char xm_dbg[32];
static uint32_t xm_dbg_us, xm_dbg_cycles, xm_dbg_pcycles, xm_dbg_ocycles, xm_dbg_tcycles;
static int16_t fmout_resampled[512 * 2];
static fm_voice_t fv[XM_CH];

/* Gain + clamp + FIFO write for one output frame. Kept out of the sample loops as an
 * inline function so the 48 kHz path has no per-sample branches. */
static inline void emit_sample(int32_t l32, int32_t r32, int32_t og) {
  int32_t l = (l32 * og) >> 6;
  int32_t r = (r32 * og) >> 6;
  if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
  if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
  int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
  if (al > X.peak_l) X.peak_l = al;
  if (ar > X.peak_r) X.peak_r = ar;
  X.last_l = l; X.last_r = r;
  mixer_write((int16_t) l, (int16_t) r);
}

static void mix(int n) {
  // Channel-major: for each channel keep pos/step/gains in registers and add a block into the
  // stereo accumulators. The accumulators are interleaved (L,R), and a block that does not reach
  // the sample end takes the branch-free path. Above 28 active voices the quietest ones are
  // skipped: a dense 32-channel section must not starve the 48 kHz FIFO.
  if (n > XM_MIXBUF) n = XM_MIXBUF;
  for (int i = 0; i < n; i++) { mixbuf[2 * i] = 0; mixbuf[2 * i + 1] = 0; }

  for (int c = 0; c < XM_CH; c++) mix_skip[c] = 0;
  if (X.chn > 28) {
    int order[XM_CH], cnt = 0;
    for (int c = 0; c < X.chn; c++) {
      xchan_t *p = &X.ch[c];
      if (p->active && (p->lgain | p->rgain)) order[cnt++] = c;
    }
    if (cnt > 28) {
      for (int i = 1; i < cnt; i++) {
        int v = order[i], g = X.ch[v].mix_vol, j = i - 1;
        while (j >= 0 && X.ch[order[j]].mix_vol < g) { order[j + 1] = order[j]; j--; }
        order[j + 1] = v;
      }
      for (int i = 28; i < cnt; i++) mix_skip[order[i]] = 1;
    }
  }
#ifdef XM_PROFILE
  int prof_active = 0;
  for (int c = 0; c < X.chn; c++) {
    xchan_t *p = &X.ch[c];
    if (p->active && (p->lgain | p->rgain) && !mix_skip[c]) prof_active++;
  }
  xm_prof_chan_samples += (uint64_t) prof_active * n;
  if ((uint32_t) prof_active > xm_prof_peak_chan) xm_prof_peak_chan = prof_active;
  xm_prof_blocks++;
  uint32_t sec = xm_prof_out_samples / 48000u;
  if (sec < XM_PROF_SEC) {
    xm_prof_sec_chan_samples[sec] += (uint64_t) prof_active * n;
    xm_prof_sec_samples[sec] += (uint32_t) n;
    if ((uint32_t) prof_active > xm_prof_sec_peak[sec]) xm_prof_sec_peak[sec] = prof_active;
  }
  xm_prof_out_samples += (uint32_t) n;
#endif

  for (int c = 0; c < X.chn; c++) {
    xchan_t *ch = &X.ch[c];
    int lg = ch->lgain, rg = ch->rgain;
    if (!ch->active || (lg | rg) == 0) continue;
    uint32_t pos = ch->pos, step = ch->step, end = ch->end, loop = ch->loop, llen = ch->llen;
    if (mix_skip[c]) {
      // A voice dropped by the limiter still has to keep its sample position for the next block.
      for (int i = 0; i < n; i++) {
        uint32_t over;
        uint32_t idx = pos >> 16;
        if (idx >= end) {
          if (!llen) { ch->active = 0; break; }
          over = idx - end;
          if (over >= llen) over %= llen;
          pos = ((loop + over) << 16) | (pos & 0xFFFF);
        }
        pos += step;
      }
      ch->pos = pos;
      continue;
    }
    const uint8_t *data = ch->data;
    uint64_t last = (uint64_t) pos + (uint64_t) step * (uint32_t) (n - 1);
    if ((last >> 16) < end) {
      int i = 0;
      if (X.interp) {
        for (; i + 1 < n; i += 2) {
          uint32_t idx0 = pos >> 16;
          uint32_t fr0 = (pos >> 8) & 0xFFu;
          int a0 = (int8_t) data[idx0];
          int b0 = (idx0 + 1 < end) ? (int8_t) data[idx0 + 1] : a0;
          int s = a0 + (((b0 - a0) * (int32_t) fr0) >> 8);
          mixbuf[2 * i] += (s * lg) >> 8;
          mixbuf[2 * i + 1] += (s * rg) >> 8;
          pos += step;
          uint32_t idx1 = pos >> 16;
          uint32_t fr1 = (pos >> 8) & 0xFFu;
          int a1 = (int8_t) data[idx1];
          int b1 = (idx1 + 1 < end) ? (int8_t) data[idx1 + 1] : a1;
          s = a1 + (((b1 - a1) * (int32_t) fr1) >> 8);
          mixbuf[2 * i + 2] += (s * lg) >> 8;
          mixbuf[2 * i + 3] += (s * rg) >> 8;
          pos += step;
        }
        if (i < n) {
          uint32_t idx0 = pos >> 16;
          uint32_t fr0 = (pos >> 8) & 0xFFu;
          int a0 = (int8_t) data[idx0];
          int b0 = (idx0 + 1 < end) ? (int8_t) data[idx0 + 1] : a0;
          int s = a0 + (((b0 - a0) * (int32_t) fr0) >> 8);
          mixbuf[2 * i] += (s * lg) >> 8;
          mixbuf[2 * i + 1] += (s * rg) >> 8;
          pos += step;
        }
      } else {
        for (; i + 1 < n; i += 2) {
          int s = (int8_t) data[pos >> 16];
          mixbuf[2 * i] += (s * lg) >> 8;
          mixbuf[2 * i + 1] += (s * rg) >> 8;
          pos += step;
          s = (int8_t) data[pos >> 16];
          mixbuf[2 * i + 2] += (s * lg) >> 8;
          mixbuf[2 * i + 3] += (s * rg) >> 8;
          pos += step;
        }
        if (i < n) {
          int s = (int8_t) data[pos >> 16];
          mixbuf[2 * i] += (s * lg) >> 8;
          mixbuf[2 * i + 1] += (s * rg) >> 8;
          pos += step;
        }
      }
    } else {
      if (X.interp) {
        for (int i = 0; i < n; i += 2) {
          uint32_t over;
          uint32_t idx = pos >> 16;
          if (idx >= end) {
            if (!llen) { ch->active = 0; break; }
            over = idx - end;
            if (over >= llen) over %= llen;
            pos = ((loop + over) << 16) | (pos & 0xFFFF);
            idx = loop + over;
          }
          uint32_t fr = (pos >> 8) & 0xFFu;
          int a = (int8_t) data[idx];
          int b = (idx + 1 < end) ? (int8_t) data[idx + 1] : a;
          int s = a + (((b - a) * (int32_t) fr) >> 8);
          mixbuf[2 * i] += (s * lg) >> 8;
          mixbuf[2 * i + 1] += (s * rg) >> 8;
          pos += step;
          if (i + 1 < n) {
            idx = pos >> 16;
            if (idx >= end) {
              if (!llen) { ch->active = 0; break; }
              over = idx - end;
              if (over >= llen) over %= llen;
              pos = ((loop + over) << 16) | (pos & 0xFFFF);
              idx = loop + over;
            }
            fr = (pos >> 8) & 0xFFu;
            a = (int8_t) data[idx];
            b = (idx + 1 < end) ? (int8_t) data[idx + 1] : a;
            s = a + (((b - a) * (int32_t) fr) >> 8);
            mixbuf[2 * i + 2] += (s * lg) >> 8;
            mixbuf[2 * i + 3] += (s * rg) >> 8;
            pos += step;
          }
        }
      } else {
        for (int i = 0; i < n; i += 2) {
          uint32_t over;
          uint32_t idx = pos >> 16;
          if (idx >= end) {
            if (!llen) { ch->active = 0; break; }
            over = idx - end;
            if (over >= llen) over %= llen;
            pos = ((loop + over) << 16) | (pos & 0xFFFF);
            idx = loop + over;
          }
          int s = (int8_t) data[idx];
          mixbuf[2 * i] += (s * lg) >> 8;
          mixbuf[2 * i + 1] += (s * rg) >> 8;
          pos += step;
          if (i + 1 < n) {
            idx = pos >> 16;
            if (idx >= end) {
              if (!llen) { ch->active = 0; break; }
              over = idx - end;
              if (over >= llen) over %= llen;
              pos = ((loop + over) << 16) | (pos & 0xFFFF);
              idx = loop + over;
            }
            s = (int8_t) data[idx];
            mixbuf[2 * i + 2] += (s * lg) >> 8;
            mixbuf[2 * i + 3] += (s * rg) >> 8;
            pos += step;
          }
        }
      }
    }
    ch->pos = pos;
  }
}

void fpga_xm_service(void) {
  if (!X.active || X.paused) return;
#ifdef IT_PROFILE
  uint32_t c0 = R_CYCLES;
#endif
  int free = audio_free();
  while (free > 2 && X.active) {
    int n = free > XM_CHUNK ? XM_CHUNK : free;
    if (X.rate && X.rate < 48000) {
      int lim = (int)(((int64_t)free * X.rate) / 48000) - 2;
      if (n > lim) n = lim;
    }
    if (n > X.tleft) n = X.tleft;
    if (n <= 0) break;
    
    int remaining = n;
    while (remaining > 0) {
      int chunk = remaining > XM_MIXBUF ? XM_MIXBUF : remaining;
      mix(chunk);
      
      const int32_t og = (X.chn > 8) ? (int32_t) ((X.vol * 3) >> 1) : (int32_t) X.vol;
    if (X.rate == 48000 || X.rate == 0) {
        /* No resampling: the mix buffer goes straight out (calling rs_frame per sample was
         * pure overhead at 48 kHz: a call and a branch for every sample). */
        for (int i = 0; i < chunk; i++)
          emit_sample(mixbuf[2 * i], mixbuf[2 * i + 1], og);
      } else {
        int out_count = 0;
        for (int i = 0; i < chunk && out_count < 512; i++) {
          int16_t l = (int16_t) mixbuf[2 * i];
          int16_t r = (int16_t) mixbuf[2 * i + 1];
          int nout = rs_frame(&X.rs, l, r, (int16_t(*)[2])&fmout_resampled[2 * out_count]);
          out_count += nout;
        }
        for (int i = 0; i < out_count; i++)
          emit_sample(fmout_resampled[2 * i], fmout_resampled[2 * i + 1], og);
      }
      
      remaining -= chunk;
    }
    
    free -= n;
    X.tleft -= n;
    if (X.tleft <= 0) next_tick();
  }

#ifdef IT_PROFILE
  xm_dbg_cycles += (uint32_t) (R_CYCLES - c0);
  if ((uint32_t) (R_US - xm_dbg_us) >= 1000000u) {
    uint32_t dt = (uint32_t) (R_US - xm_dbg_us);      /* the REAL window */
    xm_dbg_us = R_US;
    int voices = 0;
    for (int c = 0; c < X.chn; c++)
      if (X.ch[c].active && (X.ch[c].lgain | X.ch[c].rgain)) voices++;
    /* Percent of the CPU: 75 cycles per us at 75 MHz, divided by the real window;
     * otherwise a late service call makes the number exceed 100 %. */
    if (!dt) dt = 1;
    uint32_t pct = (uint32_t) ((uint64_t) xm_dbg_cycles * 100u / ((uint64_t) dt * 75u));
    {
      char *q = xm_dbg;
      q = dbg_u(q, (uint32_t) voices);
      *q++ = '/';
      q = dbg_u(q, (uint32_t) X.chn);
      *q++ = 'v'; *q++ = ' ';
      q = dbg_u(q, pct);
      *q++ = '%';
      *q = 0;
    }
    xm_dbg_cycles = 0;
    xm_dbg_pcycles = 0;
    xm_dbg_ocycles = 0;
    xm_dbg_tcycles = 0;
  }
#endif
}

const char *fpga_xm_debug(void) {
#ifdef IT_PROFILE
  return X.active ? xm_dbg : "";
#else
  return "";
#endif
}

static int parse_xm(uint32_t size) {
  if (size < 80 || memcmp(X.mod, "Extended Module: ", 17)) { set_error("XM: signature"); return -1; }
  if (X.mod[37] != 0x1A) { set_error("XM: 0x1a"); return -1; }
  uint32_t hs = le32(X.mod + 60);
  if (hs < 20 || 60 + hs > size) { set_error("XM: header"); return -1; }
  memset(X.pat, 0, sizeof(X.pat));
  memcpy(X.title, X.mod + 17, 20); X.title[20] = 0;
  for (int i = 19; i >= 0 && X.title[i] == ' '; i--) X.title[i] = 0;
  X.song = le16(X.mod + 64);
  X.restart = le16(X.mod + 66);
  X.chn = le16(X.mod + 68);
  X.npat = le16(X.mod + 70);
  X.nins = le16(X.mod + 72);
  X.linear = le16(X.mod + 74) & 1;
  X.speed = le16(X.mod + 76);
  X.bpm = le16(X.mod + 78);
  if (X.song < 1 || X.song > 256) X.song = 1;
  if (X.restart < 0 || X.restart >= X.song) X.restart = 0;
  if (X.chn < 1 || X.chn > XM_CH) X.chn = XM_CH;
  // Per-file rate based on channel count
  X.rate = X.chn <= 16 ? 48000 : 32000;
  // Linear interpolation is a quality win, but a dense 32-channel XM needs the nearest-sample
  // path to keep the 48 kHz FIFO alive. Keep interpolation for normal-width modules.
  X.interp = X.chn <= 16;
  if (X.npat > XM_PAT) X.npat = XM_PAT;
  if (X.nins > XM_INS) X.nins = XM_INS;
  if (80u + 256u > size) { set_error("XM: order table"); return -1; }
  for (int i = 0; i < 256; i++) X.orders[i] = X.mod[80 + i];

  uint32_t p = 60 + hs;
  for (int i = 0; i < X.npat; i++) {
    if (p + 9 > size) { set_error("XM: pattern"); return -1; }
    uint32_t hl = le32(X.mod + p);
    int rows = le16(X.mod + p + 5);
    uint32_t ps = le16(X.mod + p + 7);
    if (hl < 9 || p + hl + ps > size) { set_error("XM: pattern data"); return -1; }
    X.pat[i].d = X.mod + p + hl;
    X.pat[i].end = X.mod + p + hl + ps;
    X.pat[i].rows = rows;
    p += hl + ps;
  }

  X.nsmp = 0;
  memset(X.ins, 0, sizeof(X.ins));
  memset(X.smp, 0, sizeof(X.smp));
  for (int i = 0; i < X.nins; i++) {
    if (p + 29 > size) break;
    uint32_t isz = le32(X.mod + p);
    int ns = le16(X.mod + p + 27);
    if (isz < 29 || ns < 0) break;
    if (p + isz > size) break;
    xins_t *ins = &X.ins[i];
    ins->nsmp = 0;
    ins->keymap = 0;
    if (ns > 0 && p + 29 + 4 <= size) {
      uint32_t shs = le32(X.mod + p + 29);
      if (shs < 40) shs = 40;
      ins->keymap = X.mod + p + 33;
      ins->venv = X.mod + p + 129;
      ins->nvol = X.mod[p + 225];
      ins->vtype = X.mod[p + 233];
      ins->vsus = X.mod[p + 227];
      ins->vls = X.mod[p + 228];
      ins->vle = X.mod[p + 229];
      ins->fade = le16(X.mod + p + 239);
      ins->first = X.nsmp;
      uint32_t sh = p + isz;
      uint32_t data = sh + (uint32_t) ns * shs;
      if (data > size) { set_error("XM: sample headers"); return -1; }
      uint32_t dpos = data;
      for (int j = 0; j < ns; j++) {
        if (X.nsmp >= XM_SMP) break;
        const uint8_t *h = X.mod + sh + (uint32_t) j * shs;
        uint32_t ln = le32(h);
        uint32_t lps = le32(h + 4);
        uint32_t lpl = le32(h + 8);
        int16_t ft = (int8_t) h[13];
        uint8_t type = h[14];
        uint8_t bits = (type & 0x10) ? 16 : 8;
        uint32_t samples = bits == 16 ? ln / 2 : ln;
        uint32_t ls = bits == 16 ? lps / 2 : lps;
        uint32_t ll = bits == 16 ? lpl / 2 : lpl;
        if (dpos + ln > size) { set_error("XM: sample data"); return -1; }
        xsmp_t *s = &X.smp[X.nsmp];
        s->data = X.mod + dpos;
        s->len = samples;
        s->loop = ls;
        s->llen = (type & 3) && ll > 1 ? ll : 0;
        if (s->loop + s->llen > s->len) s->llen = 0;
        s->vol = h[12] > 64 ? 64 : h[12];
        s->pan = h[15] ? h[15] : 128;
        s->ft = ft;
        s->rel = (int8_t) h[16];
        sample_decode(s, (uint8_t *) (X.mod + dpos), ln, bits);
        X.nsmp++;
        ins->nsmp++;
        dpos += ln;
      }
      p = dpos;
      if (ins->nsmp == 0) p = sh + (uint32_t) ns * shs;
    } else {
      p += isz;
    }
  }

  memset(X.ch, 0, sizeof(X.ch));
  for (int c = 0; c < X.chn; c++) X.ch[c].pan = (c & 1) ? 192 : 64;
  X.order = 0; X.row = 0; X.tick = 0;
  if (!X.speed) X.speed = 6;
  if (!X.bpm) X.bpm = 125;
  X.spt = (int)(((uint64_t)120000 * X.rate) / (48000ull * X.bpm));
  X.tleft = X.spt;
  X.jump = -1; X.brk = 0; X.brow = 0;
  X.gvol = 64;
  X.vol = 64;
  X.paused = 0;
  X.peak_l = X.peak_r = 0;
  rs_init(&X.rs, X.rate);
  memset(X.vu, 0, sizeof(X.vu));
  parse_row();
  for (int c = 0; c < X.chn; c++) {
    X.ch[c].step = X.ch[c].base_step;
    X.ch[c].env = 0;
    chan_update_vol(&X.ch[c], c);
  }
  X.active = 1;
  X.ended = 0;
  xm_dbg[0] = 0;
  xm_dbg_us = xm_dbg_cycles = xm_dbg_pcycles = xm_dbg_ocycles = xm_dbg_tcycles = 0;
  X.err[0] = 0;
  return 0;
}

int fpga_xm_start(const play_src_t *src, uint32_t size, play_progress_t progress) {
  X.active = 0;
  if (!src || !size) { set_error("XM: empty"); return -1; }
  if (src->f) {
    if (size > SDRAM_FILE_MAX) { set_error("XM: over 32 MB"); return -1; }
    uint32_t t0 = R_US;
    while (!(R_SDRAM & 1)) {
      if ((uint32_t) (R_US - t0) > 200000u) { set_error("XM: SDRAM not ready"); return -1; }
    }
    uint8_t *dst = (uint8_t *) SDRAM_BASE;
    uint32_t off = 0;
    while (off < size) {
      uint32_t n = size - off;
      if (n > 4096) n = 4096;
      UINT got = 0;
      if (src_read((play_src_t *) src, off, dst + off, n, &got) != 0 || got == 0) {
        set_error("XM: read error");
        return -1;
      }
      off += got;
      if (progress && (off >> 15) != ((off - got) >> 15)) progress(off, size);
    }
    X.mod = dst;
  } else {
    X.mod = src->mem;
  }
  X.size = size;
  return parse_xm(size);
}

void fpga_xm_stop(void) {
  X.active = 0;
  X.ended = 0;
  X.paused = 0;
  memset(&X.rs, 0, sizeof(X.rs));
}
int fpga_xm_active(void) { return X.active; }
int fpga_xm_finished(void) { return X.ended; }
const char *fpga_xm_error(void) { return X.err; }
const char *fpga_xm_title(void) { return X.title; }

int fpga_xm_position(uint32_t *cur, uint32_t *total) {
  if (!X.active) return 0;
  *cur = (uint32_t) X.order * 64u + X.row;
  *total = (uint32_t) X.song * 64u;
  return 1;
}

void fpga_xm_info(fmt_info_t *o) {
  o->channels = X.chn;
  o->samples = X.nsmp;
  o->order = X.order;
  o->pattern = (X.order >= 0 && X.order < 256) ? X.orders[X.order] : 0;
  o->row = X.row;
  o->song_len = X.song;
  o->speed = X.speed;
  o->bpm = X.bpm;
  o->size = X.size;
}

void fpga_xm_audio(fmt_audio_t *o) {
  o->level_l = X.last_l;
  o->level_r = X.last_r;
  o->peak_l = X.peak_l;
  o->peak_r = X.peak_r;
  analysis_bands(o->bands, FMT_BANDS);
  memcpy(o->vu, X.vu, sizeof(o->vu));
  o->volume = X.vol;
  o->paused = X.paused;
  X.peak_l = X.peak_r = 0;
  memset(X.vu, 0, sizeof(X.vu));
}

int fpga_xm_get_volume(void) { return X.vol; }
int fpga_xm_set_volume(int v) {
  if (v < 0) v = 0;
  if (v > 64) v = 64;
  X.vol = v;
  return X.vol;
}
int fpga_xm_pause(int on) {
  if (on < 0) X.paused = !X.paused;
  else X.paused = on ? 1 : 0;
  if (X.paused) {
    memset(&X.rs, 0, sizeof(X.rs));
  }
  return X.paused;
}
