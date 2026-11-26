// Scream Tracker 1/2 modules (.stm): native parser and replayer.
// Reference: https://gist.github.com/cs127/c0f5dffc9f4db2221a66fd9ffc1e3edb
// and the openmpt123 render of the same file (the acceptance reference).
#include <string.h>
#include "stm_fpga.h"
#include "mixer.h"
#include "audio.h"
#include "analysis.h"
#include "mmio.h"

#define STM_OUT_CHUNK 256
#define STM_ROWS      64
#define STM_CHANNELS  4
#define STM_SAMPLES   32

typedef struct {
  const uint8_t *data;
  uint32_t len, loop_start, loop_len;
  uint8_t vol, loop;
  uint16_t c2spd;
} stm_sample_t;

typedef struct {
  uint32_t note, pos, per, per_target, per_base, c2spd, step;
  int smp, vol, active, keyoff, eff, par;
  int porta_speed, vol_slide, vib_pos, vib_speed, vib_depth;
  int vol_cur, end_ramp;
} stm_voice_t;

typedef struct {
  const uint8_t *file;
  uint32_t size;
  char title[21];
  int npat, ordnum, order, row, tick, speed, fac;
  uint32_t spt;
  uint8_t orders[128];
  stm_sample_t smp[STM_SAMPLES];
  const uint8_t *pat[128];
  stm_voice_t vox[STM_CHANNELS];
  int gvol, next_order, ended, active, paused;
  int32_t last_l, last_r, peak_l, peak_r;
  uint8_t vu[FMT_CHANNELS];
  int vol;
  uint32_t sample_acc;
} stm_module_t;

static stm_module_t S;
static int stm_active, stm_ended, stm_paused;
static int stm_volume = 64;
static char stm_err[48];
static int16_t stm_out[STM_OUT_CHUNK * 2];

static void stm_error(const char *s) {
  int i = 0;
  for (; s && s[i] && i < (int) sizeof(stm_err) - 1; i++) stm_err[i] = s[i];
  stm_err[i] = 0;
}

static uint8_t r8(const uint8_t *p, uint32_t o, uint32_t max) { return o < max ? p[o] : 0; }
static uint16_t r16(const uint8_t *p, uint32_t o, uint32_t max) {
  return (uint16_t) (o + 1 < max ? (p[o] | (p[o + 1] << 8)) : 0);
}
static uint32_t r32(const uint8_t *p, uint32_t o, uint32_t max) {
  return (uint32_t) r16(p, o, max) | ((uint32_t) r16(p, o + 2, max) << 16);
}

/* STM middle C is octave 2; the Amiga period table from the S3M engine is reused
 * with C-4 = 48, so C-2 becomes index 24. */
static const uint16_t stm_amiga[24] = {
  0x6b00,0x6800,0x6500,0x6220,0x5f50,0x5c80,0x5a00,0x5740,
  0x54d0,0x5260,0x5010,0x4dc0,0x4b90,0x4960,0x4750,0x4540,
  0x4350,0x4160,0x3f90,0x3dc0,0x3c10,0x3a40,0x38b0,0x3700
};
static const uint8_t stm_vibtab[128] = {
    0,  6, 13, 19, 25, 31, 37, 44, 50, 56, 62, 68, 74, 80, 86, 92,
   98,103,109,115,120,126,131,136,142,147,152,157,162,167,171,176,
  180,185,189,193,197,201,205,208,212,215,219,222,225,228,231,233,
  236,238,240,242,244,246,247,249,250,251,252,253,254,254,255,255,
  255,255,255,254,254,253,252,251,250,249,247,246,244,242,240,238,
  236,233,231,228,225,222,219,215,212,208,205,201,197,193,189,185,
  180,176,171,167,162,157,152,147,142,136,131,126,120,115,109,103,
   98, 92, 86, 80, 74, 68, 62, 56, 50, 44, 37, 31, 25, 19, 13,  6
};

static uint32_t stm_step_from_period(uint32_t per) {
  if (!per) per = 1;
  uint32_t rate = (uint32_t) ((8363ull * 1712ull) / per);
  uint64_t step = ((uint64_t) rate << 16) / 48000u;
  return step > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t) step;
}

static uint32_t stm_period_at(int n12, uint32_t c2spd) {
  /* n12 is the note index with C-4 = 48 */
  if (n12 < 0) n12 = 0;
  int n2 = n12 * 2;
  int o = n2 / 24;
  uint32_t p = stm_amiga[n2 % 24];
  uint32_t per = (uint32_t) (((uint64_t) 8363u * p) >> (o > 15 ? 15 : o));
  per /= (c2spd ? c2spd : 8363u);
  return per ? per : 1;
}

static void stm_apply_period(stm_voice_t *v) { v->step = stm_step_from_period(v->per); }

/* tempo: one byte, two digits. v2.21+ uses hex digits, older versions decimal. */
static void stm_set_tempo(stm_module_t *m, uint8_t t) {
  int hex = 1;
  if (m->file && r8(m->file, 0x1E, m->size) == 2 && r8(m->file, 0x1F, m->size) < 21) hex = 0;
  if (!hex) {
    m->speed = t / 10;
    m->fac = t % 10;
  } else {
    m->speed = t >> 4;
    m->fac = t & 15;
  }
  if (m->speed < 1) m->speed = 6;
  static const uint8_t fc[16] = {140, 50, 25, 15, 10, 7, 6, 4, 3, 3, 2, 2, 2, 2, 1, 1};
  int denom = 50 - (fc[m->speed & 15] * m->fac) / 16;
  if (denom < 1) denom = 1;
  m->spt = (uint32_t) (48000 / denom);
  if (!m->spt) m->spt = 1;
}

static void stm_note_on(stm_module_t *m, int ch, int note, int ins, int vol) {
  stm_voice_t *v = &m->vox[ch];
  if (ins > 0 && ins < STM_SAMPLES) {
    v->smp = ins;
    v->vol = m->smp[ins].vol;
  }
  if (vol >= 0 && vol <= 64) v->vol = vol;
  if (note >= 0 && note < 250) {
    uint32_t c2spd = (v->smp > 0 && v->smp < STM_SAMPLES) ? m->smp[v->smp].c2spd : 8363;
    /* C-2 is 24; the period helper is in C-4=48 units */
    v->note = (uint32_t) note;
    v->c2spd = c2spd;
    v->pos = 0;
    v->per = v->per_target = v->per_base = stm_period_at(note + 24, c2spd);
    stm_apply_period(v);
    v->vib_pos = 0;
    v->end_ramp = 0;
    v->vol_cur = 0;
    v->active = 1;
    v->keyoff = 0;
  }
}

static void stm_tick_effects(stm_module_t *m) {
  int tick = m->tick;
  for (int ch = 0; ch < STM_CHANNELS; ch++) {
    stm_voice_t *v = &m->vox[ch];
    if (!v->active) continue;
    int eff = v->eff, par = v->par;
    if (!eff) continue;

    if (eff == 4 || eff == 11 || eff == 15) {            /* D, K(part), O(part) */
      if (tick) {
        int up = par >> 4, dn = par & 15;
        if (up) v->vol += up; else if (dn) v->vol -= dn;
        if (v->vol < 0) v->vol = 0; else if (v->vol > 64) v->vol = 64;
      }
      if (eff == 4) continue;
    }
    if (eff == 5) {                                       /* E: pitch down */
      if (tick) { v->per += (uint32_t) par << 2; v->per_base = v->per; stm_apply_period(v); }
    } else if (eff == 6) {                                /* F: pitch up */
      if (tick) { uint32_t d = (uint32_t) par << 2; v->per = v->per > d ? v->per - d : 1;
                  v->per_base = v->per; stm_apply_period(v); }
    } else if (eff == 7 || eff == 15) {                   /* G, O(part) */
      if (tick) {
        uint32_t speed = (uint32_t) (par ? par : 1) << 2;
        if (v->per > v->per_target) v->per = (v->per - v->per_target > speed) ? v->per - speed : v->per_target;
        else if (v->per < v->per_target) v->per = (v->per_target - v->per > speed) ? v->per + speed : v->per_target;
        v->per_base = v->per;
        stm_apply_period(v);
      }
    } else if (eff == 8 || eff == 11) {                   /* H, K(part): vibrato */
      if (tick) {
        if (par >> 4) v->vib_speed = par >> 4;
        if (par & 15) v->vib_depth = par & 15;
        int amp = stm_vibtab[v->vib_pos & 0x7F];
        if ((int8_t) (v->vib_pos & 0xFF) < 0) amp = -amp;
        int32_t d = ((int32_t) amp * v->vib_depth) >> 7;
        d <<= 2;
        int32_t np = (int32_t) v->per_base + d;
        v->per = np > 0 ? (uint32_t) np : 1;
        stm_apply_period(v);
        v->vib_pos += v->vib_speed ? v->vib_speed : 1;
      }
    } else if (eff == 10) {                               /* J: arpeggio */
      int x = par >> 4, y = par & 15;
      int t = tick % 3;
      v->per = stm_period_at((int) v->note + 24 + (t == 1 ? x : t == 2 ? y : 0), v->c2spd);
      stm_apply_period(v);
    }
  }
}

static int stm_pattern_scan(const uint8_t *p, uint32_t len, uint32_t *used) {
  uint32_t pos = 0;
  for (int row = 0; row < STM_ROWS; row++)
    for (int ch = 0; ch < STM_CHANNELS; ch++) {
      if (pos >= len) return -1;
      uint8_t b = p[pos++];
      if (b >= 251 && b <= 253) continue;   /* compressed event */
      if (pos + 3 > len) return -1;
      pos += 3;
    }
  *used = pos;
  return 0;
}

int stm_fpga_start(const play_src_t *src, uint32_t size, play_progress_t progress) {
  stm_fpga_stop();
  if (!src || size < 0x490) { stm_error("STM too small"); return -1; }
  const uint8_t *buf;
  if (src->f) {
    buf = (const uint8_t *) SDRAM_BASE;
    uint32_t off = 0;
    while (off < size) {
      uint32_t n = size - off;
      if (n > 4096) n = 4096;
      UINT got = 0;
      if (src_read((play_src_t *) src, off, (void *) (buf + off), n, &got) != 0 || got == 0) {
        stm_error("read error"); return -1;
      }
      off += got;
      if (progress && ((off >> 15) != ((off - got) >> 15) || off == size)) progress(off, size);
    }
  } else {
    buf = src->mem;
  }
  S.file = buf;
  S.size = size;

  if (r8(buf, 0x1D, size) != 2) { stm_error("STM: not a module"); return -1; }
  if (r8(buf, 0x1E, size) != 2) { stm_error("STM v1 not supported"); return -1; }
  S.npat = r8(buf, 0x21, size);
  if (S.npat < 1 || S.npat > 64) { stm_error("STM: bad pattern count"); return -1; }
  S.gvol = r8(buf, 0x22, size);
  if (S.gvol == 0x58 || S.gvol > 64) S.gvol = 64;
  memcpy(S.title, buf, 20); S.title[20] = 0;

  memset(S.smp, 0, sizeof S.smp);
  for (int i = 1; i < STM_SAMPLES; i++) {
    uint32_t h = 0x30 + (uint32_t) (i - 1) * 32;
    stm_sample_t *s = &S.smp[i];
    uint32_t pos = (uint32_t) r16(buf, h + 0x0E, size) * 16u;
    s->len = r16(buf, h + 0x10, size);
    s->loop_start = r16(buf, h + 0x12, size);
    uint32_t le = r16(buf, h + 0x14, size);
    s->vol = r8(buf, h + 0x16, size);
    if (s->vol > 64) s->vol = 64;
    s->c2spd = r16(buf, h + 0x18, size);
    if (s->c2spd < 100) s->c2spd = 8448;
    if (s->len && pos + s->len <= size) {
      s->data = buf + pos;
      if (le > s->loop_start && le != 0xFFFF) {
        s->loop = 1;
        s->loop_len = le - s->loop_start;
        if (s->loop_start + s->loop_len > s->len) s->loop_len = s->len - s->loop_start;
      }
    }
  }

  uint32_t orders_at = 0x410;
  memset(S.orders, 99, sizeof S.orders);
  S.ordnum = 0;
  for (int i = 0; i < 128; i++) {
    uint8_t o = r8(buf, orders_at + i, size);
    S.orders[i] = o;
    if (o < 98) S.ordnum = i + 1;
    if (o >= 98) { for (int j = i; j < 128; j++) S.orders[j] = o; break; }
  }
  if (!S.ordnum) { stm_error("STM: no orders"); return -1; }

  uint32_t pos = orders_at + 128;
  for (int p = 0; p < S.npat; p++) {
    uint32_t used = 0;
    if (stm_pattern_scan(buf + pos, size - pos, &used) != 0) { stm_error("STM: pattern truncated"); return -1; }
    S.pat[p] = buf + pos;
    pos += used;
  }

  stm_set_tempo(&S, r8(buf, 0x20, size));
  S.order = 0;
  S.row = 0;
  S.tick = S.speed > 0 ? S.speed - 1 : 0;
  S.next_order = -1;
  S.ended = 0;
  S.active = 1;
  S.paused = 0;
  S.sample_acc = 0;
  for (int i = 0; i < STM_CHANNELS; i++) S.vox[i].smp = 0;
  stm_error("");
  stm_active = 1;
  stm_ended = 0;
  return 0;
}

static uint32_t stm_row_offset(const uint8_t *p, uint32_t max, int row) {
  uint32_t pos = 0;
  for (int r = 0; r < row; r++)
    for (int ch = 0; ch < STM_CHANNELS; ch++) {
      if (pos >= max) return pos;
      uint8_t b = p[pos++];
      if (b >= 251 && b <= 253) continue;
      pos += 3;
    }
  return pos;
}

void stm_fpga_service(void) {
  if (!stm_active || stm_paused) return;
  int free = audio_free();
  if (free <= 0) return;
  int n = free > STM_OUT_CHUNK ? STM_OUT_CHUNK : free;
  int out_count = 0;

  for (int i = 0; i < n; i++) {
    S.sample_acc++;
    if (S.sample_acc >= S.spt) {
      S.sample_acc = 0;
      S.tick++;
      if (S.tick >= S.speed) S.tick = 0;

      if (S.tick == 0) {
        for (int c = 0; c < STM_CHANNELS; c++) S.vox[c].eff = 0;

        uint8_t onum = S.orders[S.order];
        if (onum < S.npat && S.pat[onum]) {
          const uint8_t *p = S.pat[onum];
          uint32_t max = S.size - (uint32_t) (p - S.file);
          uint32_t pos = stm_row_offset(p, max, S.row);

          for (int ch = 0; ch < STM_CHANNELS; ch++) {
            if (pos >= max) break;
            uint8_t b0 = p[pos++];
            uint8_t note = b0, ins = 0, vol = 65, eff = 0, par = 0;
            if (b0 == 252) { note = 255; }
            else if (b0 == 253) { note = 254; }
            else if (b0 == 251) { note = 0; ins = 0; vol = 0; }
            else {
              if (pos + 3 > max) break;
              uint8_t b1 = p[pos++], b2 = p[pos++], b3 = p[pos++];
              ins = b1 >> 3;
              vol = (uint8_t) (((b2 >> 4) << 3) | (b1 & 7));
              eff = b2 & 15;
              par = b3;
            }
            stm_voice_t *v = &S.vox[ch];
            if (note == 254) {
              v->active = 0;                         /* ^^^ */
            } else if (note < 250) {
              int n12 = ((note >> 4) & 0x0F) * 12 + (note & 0x0F);
              if ((eff == 7 || eff == 15) && v->active && v->smp > 0) {
                /* portamento: slide to the new note instead of retriggering */
                uint32_t c2 = S.smp[v->smp].c2spd;
                v->per_target = stm_period_at(n12 + 24, c2);
              } else {
                stm_note_on(&S, ch, n12, ins, vol <= 64 ? (int) vol : -1);
              }
            } else if (ins > 0 && ins < STM_SAMPLES) {
              v->smp = ins;
              v->vol = S.smp[ins].vol;
            }
            if (vol <= 64) v->vol = vol;
            if (eff) {
              v->eff = eff;
              v->par = par;
              if (eff == 1) stm_set_tempo(&S, par);
              else if (eff == 2) S.next_order = par;
              else if (eff == 3) S.next_order = S.order + 1;
            }
          }
        }
        stm_tick_effects(&S);

        if (S.next_order >= 0) {
          S.order = (uint8_t) S.next_order;
          S.row = 0;
          S.next_order = -1;
        } else {
          S.row++;
          if (S.row >= STM_ROWS) { S.row = 0; S.order++; }
        }
        if (S.order >= 128 || S.orders[S.order] >= 98) {
          S.ended = 1; stm_active = 0; stm_ended = 1;
        }
      } else {
        stm_tick_effects(&S);
      }
    }

    int32_t mix_l = 0, mix_r = 0;
    for (int ch = 0; ch < STM_CHANNELS; ch++) {
      stm_voice_t *v = &S.vox[ch];
      if (!v->active || v->smp <= 0 || v->smp >= STM_SAMPLES) continue;
      const stm_sample_t *sm = &S.smp[v->smp];
      if (!sm->data || !sm->len) continue;
      int looping = sm->loop && sm->loop_len > 1;
      uint32_t stop = looping ? sm->loop_start + sm->loop_len : sm->len;
      if (looping && stop > sm->len) stop = sm->len;

      uint32_t pos_int = v->pos >> 16;
      if (pos_int >= stop) {
        if (looping) { v->pos = sm->loop_start << 16; pos_int = sm->loop_start; }
        else if (v->end_ramp == 0) { v->end_ramp = 64; pos_int = stop ? stop - 1 : 0; }
        else if (v->end_ramp <= 1) { v->active = 0; continue; }
        else pos_int = stop ? stop - 1 : 0;
      }
      uint32_t nxt = pos_int + 1;
      if (nxt >= stop) nxt = looping ? sm->loop_start : pos_int;
      int s0 = (int8_t) sm->data[pos_int];
      int s1 = (int8_t) sm->data[nxt];
      int fr = (int) ((v->pos >> 8) & 0xFF);
      int32_t val = (((s0 * (256 - fr)) + (s1 * fr)) >> 8) << 8;

      int vc = v->vol_cur, tgt = v->vol << 8;
      if (vc < 0) vc = 0;
      vc += (tgt - vc) >> 4;
      v->vol_cur = vc;
      int vol = ((vc >> 8) * S.gvol) >> 6;
      if (v->end_ramp) { vol = vol * v->end_ramp / 64; v->end_ramp--; }
      val = (val * vol) >> 8;

      mix_l += val;
      mix_r += val;
      v->pos += v->step;
    }

    mix_l = (mix_l * 196) >> 8;
    mix_r = (mix_r * 196) >> 8;
    if (mix_l > 32767) mix_l = 32767; else if (mix_l < -32768) mix_l = -32768;
    if (mix_r > 32767) mix_r = 32767; else if (mix_r < -32768) mix_r = -32768;
    stm_out[2 * out_count] = (int16_t) mix_l;
    stm_out[2 * out_count + 1] = (int16_t) mix_r;
    out_count++;
  }

  for (int i = 0; i < out_count; i++) {
    int32_t l = stm_out[2 * i], r = stm_out[2 * i + 1];
    int v = stm_volume;
    l = (l * v) >> 6; r = (r * v) >> 6;
    int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
    if (al > S.peak_l) S.peak_l = al;
    if (ar > S.peak_r) S.peak_r = ar;
    S.last_l = l; S.last_r = r;
    mixer_write((int16_t) l, (int16_t) r);
  }
  memset(S.vu, 0, sizeof S.vu);
  for (int ch = 0; ch < STM_CHANNELS; ch++)
    if (S.vox[ch].active) S.vu[ch] = (uint8_t) (S.vox[ch].vol * 4 > 255 ? 255 : S.vox[ch].vol * 4);
}

int stm_fpga_active(void) { return stm_active; }
int stm_fpga_finished(void) { return stm_ended; }
void stm_fpga_stop(void) { stm_active = 0; stm_ended = 0; S.ended = 1; S.active = 0; }
const char *stm_fpga_error(void) { return stm_err; }
const char *stm_fpga_title(void) { return S.title; }
int stm_fpga_position(uint32_t *cur, uint32_t *total) {
  if (!stm_active && !stm_ended) return 0;
  if (cur) *cur = (uint32_t) S.order * STM_ROWS + (uint32_t) S.row;
  if (total) *total = (uint32_t) (S.ordnum ? S.ordnum : 1) * STM_ROWS;
  return 1;
}
void stm_fpga_info(fmt_info_t *o) {
  o->channels = STM_CHANNELS;
  int ns = 0;
  for (int i = 1; i < STM_SAMPLES; i++) if (S.smp[i].len) ns++;
  o->samples = ns;
  o->order = S.order; o->pattern = S.orders[S.order];
  o->row = S.row; o->song_len = S.ordnum;
  o->speed = S.speed; o->bpm = 125; o->size = S.size;
}
void stm_fpga_audio(fmt_audio_t *o) {
  o->level_l = S.last_l; o->level_r = S.last_r;
  o->peak_l = S.peak_l; o->peak_r = S.peak_r;
  o->volume = stm_volume; o->paused = stm_paused;
  memcpy(o->vu, S.vu, sizeof o->vu);
  analysis_bands(o->bands, FMT_BANDS);
}
int stm_fpga_get_volume(void) { return stm_volume; }
int stm_fpga_set_volume(int v) { if (v < 0) v = 0; if (v > 64) v = 64; stm_volume = v; return v; }
int stm_fpga_pause(int on) { if (on < 0) stm_paused = !stm_paused; else stm_paused = on ? 1 : 0; return stm_paused; }
