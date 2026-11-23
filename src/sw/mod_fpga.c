// ProTracker MOD player on the VexRiscv: parse the module in the SRAM, mix 48 kHz stereo into the
// audio FIFO of menu_soc. The engine of the FPGA (no ESP32). Nearest sample, hard L/R panning (as
// ProTracker: 0/3 left, 1/2 right), the common effects of the 4/6/8 channel modules.
#include <string.h>
#include "fpga.h"
#include "audio.h"
#include "mixer.h"
#include "analysis.h"
#include "mmio.h"

#define MOD_MAX_CH 8
#define MIX_CHUNK 256

static const uint16_t periods[36] = {
  856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
  428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
  214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113
};

// one half of the ProTracker vibrato sine, 0..255; the sign comes from bit 5 of vib_pos
static const uint8_t vib_tab[32] = {
  0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
  255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24
};

typedef struct {
  const int8_t *data;
  uint32_t len, loop_start, loop_len;
  uint8_t vol;
  int8_t finetune;
} mod_sample_t;

typedef struct {
  const int8_t *data;
  uint32_t len, loop_start, loop_len;
  uint32_t pos, step;
  uint16_t period, target;
  uint8_t note;              // 0..35
  uint8_t vol;               // 0..64
  uint8_t pan;               // 0 left, 1 right
  uint8_t active;
  uint8_t inst;              // 1..31
  uint8_t eff, par;
  uint8_t vib_pos, vib_speed, vib_depth;
  uint8_t arp_x, arp_y;
  uint8_t port_speed, porta_up, porta_dn;
  int8_t finetune;
} mod_chan_t;

typedef struct {
  const uint8_t *mod;
  uint32_t size;
  char title[21];
  int chn;
  int song_len, restart;
  uint8_t orders[128];
  const uint8_t *pat;
  int num_pat;
  mod_sample_t smp[31];
  mod_chan_t ch[MOD_MAX_CH];
  int order, row, tick, speed, tempo;
  int tick_left, samples_per_tick;
  int pos_jump, pat_break, break_row;
  int loop_row, loop_count, loop_jump;
  int active, ended;
  char err[48];
} mod_t;

static mod_t M;

static int32_t s_last_l, s_last_r, s_peak_l, s_peak_r;
static uint8_t s_vu[FMT_CHANNELS];
static int s_vol = 64;
static int s_paused;

static uint16_t be16(const uint8_t *p) { return (uint16_t) ((p[0] << 8) | p[1]); }

static void set_error(const char *s) {
  int i = 0;
  for (; s[i] && i < (int) sizeof(M.err) - 1; i++) M.err[i] = s[i];
  M.err[i] = 0;
}

// 2^(finetune / 96) in 16.16, finetune -8..7 (one step = 1/8 semitone)
static const uint32_t ft_mul[16] = {
  61858, 62306, 62757, 63212, 63670, 64132, 64596, 65065,
  65536, 66011, 66489, 66971, 67456, 67945, 68438, 68933
};

static uint32_t step_for(uint16_t period, int finetune) {
  if (!period) return 0;
  // PAL Amiga C-2: 3546895 / 428 = 8287 Hz (NTSC would be 8363)
  uint64_t n = (uint64_t) 8287 * 428 * 65536;
  uint32_t base = (uint32_t) (n / ((uint64_t) period * 48000));
  if (finetune < -8) finetune = -8;
  if (finetune > 7) finetune = 7;
  return (uint32_t) (((uint64_t) base * ft_mul[finetune + 8]) >> 16);
}

static int note_of(uint16_t period) {
  int best = 0;
  for (int i = 0; i < 36; i++) {
    if (periods[i] == period) return i;
    if (periods[i] > period) best = i;
  }
  return best;
}

static void set_sample(mod_chan_t *c, int inst) {
  if (inst < 1 || inst > 31) return;
  mod_sample_t *s = &M.smp[inst - 1];
  c->data = s->data;
  c->len = s->len;
  c->loop_start = s->loop_start;
  c->loop_len = s->loop_len;
  c->vol = s->vol;
  c->inst = (uint8_t) inst;
  c->finetune = s->finetune;
}

static void parse_row(void) {
  int pat_idx = M.orders[M.order];
  if (pat_idx >= M.num_pat) pat_idx = 0;
  for (int c = 0; c < M.chn; c++) {
    mod_chan_t *ch = &M.ch[c];
    const uint8_t *p = M.pat + ((pat_idx * 64 + M.row) * M.chn + c) * 4;
    uint8_t b0 = p[0], b1 = p[1], b2 = p[2], b3 = p[3];
    int inst = (b0 & 0xF0) | (b2 >> 4);
    uint16_t period = (uint16_t) (((b0 & 0x0F) << 8) | b1);
    ch->eff = b2 & 0x0F;
    ch->par = b3;

    if (inst) set_sample(ch, inst);
    if (period) {
      if (ch->eff == 0x3 || ch->eff == 0x5) {
        ch->target = period;
      } else {
        ch->period = period;
        ch->target = 0;
        ch->note = (uint8_t) note_of(period);
        ch->pos = 0;
        ch->active = ch->data && ch->len ? 1 : 0;
        ch->vib_pos = 0;
      }
    }

    switch (ch->eff) {
      case 0x0: ch->arp_x = b3 >> 4; ch->arp_y = b3 & 0x0F; break;
      case 0x1: if (b3) ch->porta_up = b3; break;
      case 0x2: if (b3) ch->porta_dn = b3; break;
      case 0x3:
        if (period) ch->target = period;
        if (b3) ch->port_speed = b3;
        break;
      case 0x4:
        if (b3 >> 4) ch->vib_speed = b3 >> 4;
        if (b3 & 0x0F) ch->vib_depth = b3 & 0x0F;
        break;
      case 0x5: if (period) ch->target = period; break;
      case 0x6: break;                         // vibrato + volume slide (the vibrato state is 4xy)
      case 0x9: ch->pos = (uint32_t) b3 << 8 << 16; break;
      case 0xA: break;
      case 0xB: M.pos_jump = b3; break;
      case 0xC: ch->vol = b3 > 64 ? 64 : b3; break;
      case 0xD:
        M.pat_break = 1;
        M.break_row = ((b3 >> 4) * 10) + (b3 & 0x0F);
        break;
      case 0xE:
        if ((b3 & 0xF0) == 0x60) {
          if ((b3 & 0x0F) == 0) { M.loop_row = M.row; M.loop_count = -1; }
          else M.loop_jump = b3 & 0x0F;
        }
        break;
      case 0xF:
        if (b3) {
          if (b3 < 32) M.speed = b3;
          else M.tempo = b3;
          int spt = 120000 / (M.tempo ? M.tempo : 125);
          if (spt < 1) spt = 1;
          if (spt > 4000) spt = 4000;
          M.samples_per_tick = spt;
        }
        break;
      default: break;
    }
  }
}

static void apply_tick(void) {
  for (int c = 0; c < M.chn; c++) {
    mod_chan_t *ch = &M.ch[c];
    int per = ch->period;
    switch (ch->eff) {
      case 0x0:
        if (M.tick % 3 == 1) {
          int n = ch->note + ch->arp_x; if (n > 35) n = 35;
          per = periods[n];
        } else if (M.tick % 3 == 2) {
          int n = ch->note + ch->arp_y; if (n > 35) n = 35;
          per = periods[n];
        }
        break;
      case 0x1:
        if (M.tick > 0 && ch->porta_up) {
          per -= ch->porta_up;
          if (per < 113) per = 113;
          ch->period = (uint16_t) per;
        }
        break;
      case 0x2:
        if (M.tick > 0 && ch->porta_dn) {
          per += ch->porta_dn;
          if (per > 1712) per = 1712;
          ch->period = (uint16_t) per;
        }
        break;
      case 0x3:
        if (M.tick > 0 && ch->target && ch->port_speed) {
          if (per < ch->target) { per += ch->port_speed; if (per > ch->target) per = ch->target; }
          else if (per > ch->target) { per -= ch->port_speed; if (per < ch->target) per = ch->target; }
          ch->period = (uint16_t) per;
        }
        break;
      case 0x4:
        if (M.tick > 0 && ch->vib_depth) {
          ch->vib_pos = (uint8_t) ((ch->vib_pos + ch->vib_speed) & 63);
          int d = (vib_tab[ch->vib_pos & 31] * ch->vib_depth) >> 7;
          per += (ch->vib_pos & 32) ? -d : d;
          if (per < 113) per = 113;
          if (per > 1712) per = 1712;
        }
        break;
      case 0x5:
        if (M.tick > 0 && ch->target && ch->port_speed) {
          if (per < ch->target) { per += ch->port_speed; if (per > ch->target) per = ch->target; }
          else if (per > ch->target) { per -= ch->port_speed; if (per < ch->target) per = ch->target; }
          ch->period = (uint16_t) per;
        }
        if (M.tick > 0) {
          int v = ch->vol + (ch->par >> 4) - (ch->par & 0x0F);
          if (v < 0) v = 0; else if (v > 64) v = 64;
          ch->vol = (uint8_t) v;
        }
        break;
      case 0x6:
        if (M.tick > 0 && ch->vib_depth) {
          ch->vib_pos = (uint8_t) ((ch->vib_pos + ch->vib_speed) & 63);
          int d = (vib_tab[ch->vib_pos & 31] * ch->vib_depth) >> 7;
          per += (ch->vib_pos & 32) ? -d : d;
          if (per < 113) per = 113;
          if (per > 1712) per = 1712;
        }
        if (M.tick > 0) {
          int v = ch->vol + (ch->par >> 4) - (ch->par & 0x0F);
          if (v < 0) v = 0; else if (v > 64) v = 64;
          ch->vol = (uint8_t) v;
        }
        break;
      case 0xA:
        if (M.tick > 0) {
          int v = ch->vol + (ch->par >> 4) - (ch->par & 0x0F);
          if (v < 0) v = 0; else if (v > 64) v = 64;
          ch->vol = (uint8_t) v;
        }
        break;
      default: break;
    }
    ch->step = step_for((uint16_t) per, ch->finetune);
  }
}

static void next_row(void) {
  if (M.loop_jump) {
    int x = M.loop_jump;
    M.loop_jump = 0;
    if (M.loop_count < 0) M.loop_count = x;
    if (M.loop_count > 0) {
      M.loop_count--;
      M.row = M.loop_row;
      parse_row();
      apply_tick();
      return;
    }
  }

  M.row++;
  if (M.row >= 64 || M.pat_break) {
    int o = M.pos_jump >= 0 ? M.pos_jump : M.order + 1;
    int r = (M.pat_break && M.break_row > 0) ? M.break_row : 0;
    M.pos_jump = -1;
    M.pat_break = 0;
    M.break_row = 0;
    M.order = o;
    M.row = r;
    if (M.order >= M.song_len || M.order < 0) { M.active = 0; M.ended = 1; return; }
    M.loop_row = 0;
    M.loop_count = -1;
    M.loop_jump = 0;
  }
  parse_row();
  apply_tick();
}

static void next_tick(void) {
  if (M.tick + 1 >= M.speed) {
    M.tick = 0;
    next_row();
  } else {
    M.tick++;
    apply_tick();
  }
  M.tick_left = M.samples_per_tick;
}

static void mix(int n) {
  int shift = M.chn > 4 ? 1 : 0;
  for (int i = 0; i < n; i++) {
    int32_t l = 0, r = 0;
    for (int c = 0; c < M.chn; c++) {
      mod_chan_t *ch = &M.ch[c];
      if (!ch->active || !ch->step) continue;
      uint32_t idx = ch->pos >> 16;
      if (idx >= ch->len) {
        if (ch->loop_len > 2) { ch->pos = ch->loop_start << 16; idx = ch->loop_start; }
        else { ch->active = 0; continue; }
      }
      // Linear interpolation (the libxmp-style 16.16 phase): the native mixer used nearest
      // samples before, which is the main reason MOD sounded rough next to the C engines.
      uint32_t frac = (ch->pos >> 8) & 0xFFu;
      uint32_t end = (ch->loop_len > 2) ? (ch->loop_start + ch->loop_len) : ch->len;
      int s0 = ((const int8_t *) ch->data)[idx];
      int s1 = (idx + 1 < end) ? ((const int8_t *) ch->data)[idx + 1] : s0;
      int s = s0 + (((s1 - s0) * (int32_t) frac) >> 8);
      int v = s * ch->vol;
      {
        int av = v < 0 ? -v : v;
        av >>= 5;
        if (av > 255) av = 255;
        if (av > (int) s_vu[c]) s_vu[c] = (uint8_t) av;
      }
      if (ch->pan) r += v; else l += v;
      ch->pos += ch->step;
      if ((ch->pos >> 16) >= end) {
        if (ch->loop_len > 2) {
          while ((ch->pos >> 16) >= end) ch->pos -= ch->loop_len << 16;
        } else {
          ch->active = 0;
        }
      }
    }
    l >>= shift;
    r >>= shift;
    l = l * s_vol / 64;
    r = r * s_vol / 64;
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    {
      int32_t al = l < 0 ? -l : l;
      int32_t ar = r < 0 ? -r : r;
      if (al > s_peak_l) s_peak_l = al;
      if (ar > s_peak_r) s_peak_r = ar;
    }
    s_last_l = l; s_last_r = r;
    mixer_write((int16_t) l, (int16_t) r);
  }
}

void fpga_mod_service(void) {
  if (!M.active || s_paused) return;
  int free = audio_free();
  while (free > 0 && M.active) {
    int n = free > MIX_CHUNK ? MIX_CHUNK : free;
    if (n > M.tick_left) n = M.tick_left;
    if (n <= 0) break;
    mix(n);
    free -= n;
    M.tick_left -= n;
    if (M.tick_left <= 0) next_tick();
  }
}

void fpga_mod_info(fmt_info_t *o) {
  o->channels = M.chn;
  int n = 0;
  for (int i = 0; i < 31; i++) if (M.smp[i].len) n++;
  o->samples = n;
  o->order = M.order;
  o->pattern = (M.order >= 0 && M.order < 128) ? M.orders[M.order] : 0;
  o->row = M.row;
  o->song_len = M.song_len;
  o->speed = M.speed;
  o->bpm = M.tempo;
  o->size = M.size;
}

void fpga_mod_audio(fmt_audio_t *o) {
  o->level_l = s_last_l;
  o->level_r = s_last_r;
  o->peak_l = s_peak_l;
  o->peak_r = s_peak_r;
  analysis_bands(o->bands, FMT_BANDS);
  for (int i = 0; i < FMT_CHANNELS; i++) o->vu[i] = s_vu[i];
  o->volume = s_vol;
  o->paused = s_paused;
  s_peak_l = s_peak_r = 0;
  for (int i = 0; i < FMT_CHANNELS; i++) s_vu[i] = 0;
}

int fpga_mod_get_volume(void) { return s_vol; }

int fpga_mod_set_volume(int v) {
  if (v < 0) v = 0;
  if (v > 64) v = 64;
  s_vol = v;
  return s_vol;
}

int fpga_mod_pause(int on) {
  if (on < 0) s_paused = !s_paused;
  else s_paused = on ? 1 : 0;
  return s_paused;
}

static int parse_mod(uint32_t size) {
  if (size < 1084) { set_error("MOD: too short"); return -1; }
  const uint8_t *p = M.mod;
  memcpy(M.title, p, 20);
  M.title[20] = 0;
  for (int i = 19; i >= 0 && M.title[i] == ' '; i--) M.title[i] = 0;

  int chn = 0;
  const uint8_t *sig = p + 1080;
  if (!memcmp(sig, "M.K.", 4) || !memcmp(sig, "M!K!", 4) || !memcmp(sig, "4CHN", 4) ||
      !memcmp(sig, "N.T.", 4) || !memcmp(sig, "FLT4", 4)) chn = 4;
  else if (!memcmp(sig, "6CHN", 4)) chn = 6;
  else if (!memcmp(sig, "8CHN", 4) || !memcmp(sig, "OCTA", 4) || !memcmp(sig, "CD81", 4)) chn = 8;
  else { set_error("MOD: unknown signature"); return -1; }
  M.chn = chn;

  M.song_len = p[950];
  M.restart = p[951];
  if (M.song_len < 1 || M.song_len > 128) M.song_len = 1;
  if (M.restart < 0 || M.restart >= M.song_len) M.restart = 0;
  for (int i = 0; i < 128; i++) M.orders[i] = p[952 + i];

  M.num_pat = 1;
  for (int i = 0; i < M.song_len; i++) if (M.orders[i] + 1 > M.num_pat) M.num_pat = M.orders[i] + 1;
  M.pat = p + 1084;
  uint32_t data_off = 1084 + (uint32_t) M.num_pat * 64 * chn * 4;
  if (data_off > size) { set_error("MOD: bad patterns"); return -1; }

  uint32_t off = data_off;
  for (int i = 0; i < 31; i++) {
    const uint8_t *s = p + 20 + i * 30;
    uint32_t len = (uint32_t) be16(s + 22) * 2;          // the MOD header is in words
    uint32_t loop_start = (uint32_t) be16(s + 26) * 2;
    uint32_t loop_len = (uint32_t) be16(s + 28) * 2;
    uint8_t vol = s[25] > 64 ? 64 : s[25];
    int8_t ft = (int8_t) (s[24] & 0x0F);
    if (ft >= 8) ft -= 16;
    M.smp[i].finetune = ft;
    M.smp[i].len = len;
    M.smp[i].loop_start = loop_start;
    if (loop_len > 2 && loop_start < len) {
      if (loop_start + loop_len > len) loop_len = len - loop_start;   // PT truncates the loop
    } else loop_len = 0;
    M.smp[i].loop_len = loop_len;
    M.smp[i].vol = vol;
    M.smp[i].data = (const int8_t *) (p + off);
    off += len;
    if (off > size) { set_error("MOD: bad sample data"); return -1; }
  }
  M.size = size;

  memset(M.ch, 0, sizeof(M.ch));
  for (int c = 0; c < chn; c++) M.ch[c].pan = ((c & 3) == 0 || (c & 3) == 3) ? 0 : 1;
  M.order = 0;
  M.row = 0;
  M.tick = 0;
  M.speed = 6;
  M.tempo = 125;
  M.samples_per_tick = 120000 / M.tempo;
  M.tick_left = M.samples_per_tick;
  M.pos_jump = -1;
  M.pat_break = 0;
  M.break_row = 0;
  M.loop_row = 0;
  M.loop_count = -1;
  M.loop_jump = 0;
  M.err[0] = 0;
  parse_row();
  apply_tick();
  M.active = 1;
  M.ended = 0;
  return 0;
}

int fpga_mod_start(const play_src_t *src, uint32_t size, play_progress_t progress) {
  M.active = 0;
  s_paused = 0;
  s_last_l = s_last_r = s_peak_l = s_peak_r = 0;
  for (int i = 0; i < FMT_CHANNELS; i++) s_vu[i] = 0;
  if (!src || !size) { set_error("MOD: empty"); return -1; }
  if (src->f) {
    if (size > SDRAM_FILE_MAX) { set_error("MOD: over 32 MB"); return -1; }
    uint32_t t0 = R_US;
    while (!(R_SDRAM & 1)) {
      if ((uint32_t) (R_US - t0) > 200000u) { set_error("MOD: SDRAM not ready"); return -1; }
    }
    uint8_t *dst = (uint8_t *) SDRAM_BASE;
    uint32_t off = 0;
    while (off < size) {
      uint32_t n = size - off;
      if (n > 4096) n = 4096;
      UINT got = 0;
      if (src_read((play_src_t *) src, off, dst + off, n, &got) != 0 || got == 0) {
        set_error("MOD: read error");
        return -1;
      }
      off += got;
      if (progress && (off >> 15) != ((off - got) >> 15)) progress(off, size);
    }
    M.mod = dst;
  } else {
    M.mod = src->mem;
  }
  return parse_mod(size);
}

void fpga_mod_stop(void) { M.active = 0; M.ended = 0; s_paused = 0; }
int fpga_mod_active(void) { return M.active; }
int fpga_mod_finished(void) { return M.ended; }
const char *fpga_mod_error(void) { return M.err; }
const char *fpga_mod_title(void) { return M.title; }
int fpga_mod_position(uint32_t *cur, uint32_t *total) {
  if (!M.active) return 0;
  *cur = (uint32_t) M.order * 64u + M.row;
  *total = (uint32_t) M.song_len * 64u;
  return 1;
}

