// The FPGA AY engine: the PT3 / PT2 / STC replayers (ay_players.cpp) write the YM2149
// registers at 50 Hz. On the board the two chips are the tsconf YM2149 pair in RTL; this file
// only parses/replays the module, sends the register writes and keeps the analysis display alive.
// The host build still uses the small software model below so its WAV renderers work.
#include <string.h>
#include "fpga.h"
#include "audio.h"
#include "mixer.h"
#include "analysis.h"
#include "mmio.h"
#include "ay/ay_engine.h"
#include "ay/ay_emu.h"

#define AY_CLK     1773400u
#define AY_TICK    960u          // 50 Hz at 48000
#ifndef HOST
#define AY_HW      1
#endif
#define AY_CHUNK   256

extern int ayp_detect(int fmt, const uint8_t *data, uint32_t size);
extern int ayp_start(int fmt, const uint8_t *data, uint32_t size);
extern void ayp_tick(void);
extern const uint8_t *ayp_regs(int chip);
extern uint16_t ayp_written(int chip);
extern void ayp_clear_written(int chip);
extern int ayp_channels(void);
extern uint32_t ayp_ticks(void);
extern uint32_t ayp_length(void);
extern const char *ayp_title(void);
extern const char *ayp_error(void);
extern void ayp_stop(void);

static const uint8_t ay_vol[32] = {
  0, 1, 1, 2, 2, 3, 3, 4, 6, 7, 9, 10, 12, 14, 17, 19,
  23, 27, 32, 37, 44, 53, 62, 71, 84, 102, 119, 136, 161, 192, 224, 255
};

typedef struct {
  uint32_t tphase[3], tstep[3];
  uint32_t nphase, nstep, lfsr;
  uint32_t ephase, estep;
  int evol, einc, ehold, shape, last_shape;
  int tone[3], noise;
  uint8_t regs[14];
} ay_t;

static ay_t ay[2];
static int active, ended, paused, nchips;
static int engine_mode;
static AYChipStruct ayemu[2];
static int vol = 64;
static uint32_t tick_left, ticks, next_us;
static int32_t last_l, last_r, peak_l, peak_r;
static uint8_t vu[FMT_CHANNELS];
static uint32_t pt3_size;
static char title[64], err[48];

static void apply_ay_regs(void) {
  for (int c = 0; c < 2; c++) {
    const uint8_t *r = ayp_regs(c);
    for (int i = 0; i < 14; i++) ay_out(&ayemu[c], i, r[i]);
  }
}

static void set_error(const char *s) {
  int i = 0;
  for (; s && s[i] && i < (int) sizeof(err) - 1; i++) err[i] = s[i];
  err[i] = 0;
}

#ifdef AY_HW
// The RTL pair of tsconf YM2149s. One MMIO write carries chip / register / data; the RTL
// turns it into the YM2149 address and data bus cycles. bit 31 resets both chips.
static void ay_hw_reg(int chip, int reg, int data) {
  MMIO(0xc0) = ((uint32_t) (chip & 1) << 12) | ((uint32_t) (reg & 15) << 8) | (uint32_t) (data & 0xff);
  // The RTL write sequencer needs four clocks for the address and the data bus cycle; a
  // tracker burst of 14 registers must not overrun it. This is still only ~1 ms per second.
  for (volatile int i = 0; i < 32; i++) ;
}
static void ay_hw_reset(void) { MMIO(0xc0) = 1u << 31; }
static void ay_hw_sync(int chip, int all) {
  uint16_t mask = all ? 0x3fffu : ayp_written(chip);
  for (int i = 0; i < 14; i++)
    if (mask & (1u << i)) ay_hw_reg(chip, i, ayp_regs(chip)[i]);
  ayp_clear_written(chip);
}
static void ay_hw_ctl(int on, int volume) {
  MMIO(0xc4) = (on ? 1u : 0u) | ((uint32_t) (volume & 0x7f) << 8);
}
#endif

static void ay_apply(ay_t *a, const uint8_t *r, int chip) {
  (void) chip;
  memcpy(a->regs, r, 14);
  for (int c = 0; c < 3; c++) {
    uint32_t p = a->regs[c * 2] | ((uint32_t) a->regs[c * 2 + 1] << 8);
    if (!p) p = 1;
    a->tstep[c] = (uint32_t) (((uint64_t) AY_CLK << 16) / (8u * p * 48000u));
  }
  uint32_t np = a->regs[6] & 31;
  if (!np) np = 1;
  a->nstep = (uint32_t) (((uint64_t) AY_CLK << 16) / (16u * np * 48000u));
  uint32_t ep = a->regs[11] | ((uint32_t) a->regs[12] << 8);
  if (!ep) ep = 1;
  a->estep = (uint32_t) (((uint64_t) AY_CLK << 16) / (8u * ep * 48000u));

  if (a->regs[13] != 0xff && a->regs[13] != a->last_shape) {
    a->last_shape = a->regs[13];
    a->shape = a->regs[13] & 15;
    a->ehold = 0;
    if (a->shape & 4) { a->evol = 0; a->einc = 1; }
    else { a->evol = 31; a->einc = -1; }
  }
}

static void env_step(ay_t *a) {
  if (!a->ehold) {
    a->evol += a->einc;
    if (a->evol < 0) a->evol = 0;
    if (a->evol > 31) a->evol = 31;
  }
  int sh = a->shape;
  if (!(sh & 8)) {
    if (a->einc < 0 && a->evol <= 1) a->ehold = 1;
    else if (a->einc > 0 && a->evol >= 31) a->ehold = 1;
  } else if (sh & 1) {
    if (a->einc < 0 && ((sh & 2) ? a->evol == 0 : a->evol == 1)) a->ehold = 1;
    else if (a->einc > 0 && ((sh & 2) ? a->evol == 31 : a->evol == 30)) a->ehold = 1;
  } else if (sh & 2) {
    if (a->einc < 0) {
      if (a->evol <= 1) a->ehold = 1;
      if (a->evol == 0) { a->ehold = 0; a->einc = 1; }
    } else {
      if (a->evol >= 30) a->ehold = 1;
      if (a->evol == 31) { a->ehold = 0; a->einc = -1; }
    }
  }
}

static void ay_chip(ay_t *a, int base, int *out_l, int *out_r) {
  for (int c = 0; c < 3; c++) {
    uint32_t p = a->tphase[c] + a->tstep[c];
    int t = p >> 16;
    a->tphase[c] = p & 0xFFFF;
    if (t & 1) a->tone[c] ^= 1;
  }
  uint32_t np = a->nphase + a->nstep;
  int nt = np >> 16;
  a->nphase = np & 0xFFFF;
  while (nt-- > 0) {
    uint32_t bit = (a->lfsr ^ (a->lfsr >> 3)) & 1;   // the AY noise polynomial, not a generic LFSR
    a->lfsr = (a->lfsr >> 1) | (bit << 16);
  }
  a->noise = (int) (a->lfsr & 1);

  uint32_t ep = a->ephase + a->estep;
  int et = ep >> 16;
  a->ephase = ep & 0xFFFF;
  while (et-- > 0) env_step(a);

  int mixer = a->regs[7];
  int sum_l = 0, sum_r = 0;
  for (int c = 0; c < 3; c++) {
    int rv = a->regs[8 + c];
    int v;
    if (rv & 0x10) v = a->evol;
    else { v = rv & 15; v = (v << 1) | (v >> 3); }
    int level = ay_vol[v & 31];
    // The AY mixer is AND, not OR: a disabled source reads as 1. This matters when both
    // tone and noise are on at once, and it is what the Spectrum players expect.
    int tone_on = !(mixer & (1 << c));
    int noise_on = !(mixer & (8 << c));
    int out = (tone_on ? a->tone[c] : 1) & (noise_on ? a->noise : 1);
    int s = out ? level : -level;
    // The RTL pair places A and B on the left, B and C on the right (menu_soc):
    //   left = 2*A + B, right = 2*C + B for each YM2149.
    if (c == 0) sum_l += s * 2;
    else if (c == 1) { sum_l += s; sum_r += s; }
    else sum_r += s * 2;
    vu[base + c] = (uint8_t) level;
  }
  *out_l = sum_l;
  *out_r = sum_r;
}

static void ay_mix(int n, int emit) {
  for (int i = 0; i < n; i++) {
    int sum_l = 0, sum_r = 0;
    for (int c = 0; c < nchips; c++) {
      int l, r;
      ay_chip(&ay[c], c * 3, &l, &r);
      sum_l += l;
      sum_r += r;
    }
    int32_t sl = sum_l * 32;
    int32_t sr = sum_r * 32;
    sl = sl * vol / 64;
    sr = sr * vol / 64;
    if (sl > 32767) sl = 32767; else if (sl < -32768) sl = -32768;
    if (sr > 32767) sr = 32767; else if (sr < -32768) sr = -32768;
    int32_t al = sl < 0 ? -sl : sl;
    int32_t ar = sr < 0 ? -sr : sr;
    if (al > peak_l) peak_l = al;
    if (ar > peak_r) peak_r = ar;
    last_l = sl; last_r = sr;
    if (emit) mixer_write((int16_t) sl, (int16_t) sr);
  }
}

void pt3_service(void) {
  if (!active || paused) return;
  int free = audio_free();
  while (free > 0 && active) {
    if (!tick_left) {
      uint32_t length = engine_mode ? (uint32_t) ay_engine_length() : ayp_length();
      if (length && ticks >= length) { active = 0; ended = 1; break; }
      if (engine_mode) ay_engine_tick(); else ayp_tick();
      ticks++;
      apply_ay_regs();
      tick_left = AY_TICK;
    }
    int n = free > AY_CHUNK ? AY_CHUNK : free;
    if (n > (int) tick_left) n = (int) tick_left;
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      ay_tick(&ayemu[0], 1);
      ay_tick(&ayemu[1], 1);
      int32_t l = (int32_t) ayemu[0].out[0] * 2 + ayemu[0].out[1] +
                  (int32_t) ayemu[1].out[0] * 2 + ayemu[1].out[1];
      int32_t r = (int32_t) ayemu[0].out[2] * 2 + ayemu[0].out[1] +
                  (int32_t) ayemu[1].out[2] * 2 + ayemu[1].out[1];
      l = (l * vol) / 64;
      r = (r * vol) / 64;
      if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
      if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
      int32_t al = l < 0 ? -l : l;
      int32_t ar = r < 0 ? -r : r;
      if (al > peak_l) peak_l = al;
      if (ar > peak_r) peak_r = ar;
      last_l = l; last_r = r;
      mixer_write((int16_t) l, (int16_t) r);
      for (int c = 0; c < 3; c++) {
        int v = ayp_regs(0)[8 + c] & 15;
        vu[c] = (uint8_t) (v * 16);
        v = ayp_regs(1)[8 + c] & 15;
        vu[3 + c] = (uint8_t) (v * 16);
      }
    }
    free -= n;
    tick_left -= (uint32_t) n;
  }
}

static int start_engine(int fmt, const play_src_t *src, uint32_t size, play_progress_t progress) {
  active = 0; ended = 0; paused = 0; tick_left = 0; ticks = 0; pt3_size = size;
  last_l = last_r = peak_l = peak_r = 0;
  memset(vu, 0, sizeof(vu));
  err[0] = 0;
  if (!src || size < 16) { set_error("AY: too short"); return -1; }

  const uint8_t *d = (const uint8_t *) LD_FILE_BASE;
  if (src->f) {
    uint8_t *dst = (uint8_t *) LD_FILE_BASE;
    if (size > LD_FILE_MAX) { set_error("AY: over 2.5 MB"); return -1; }
    uint32_t off = 0;
    while (off < size) {
      uint32_t n = size - off;
      if (n > 4096) n = 4096;
      UINT got = 0;
      if (src_read((play_src_t *) src, off, dst + off, n, &got) != 0 || got == 0) {
        set_error("AY: read error");
        return -1;
      }
      off += got;
      if (progress && (off >> 15) != ((off - got) >> 15)) progress(off, size);
    }
    d = dst;
  } else {
    d = src->mem;
  }

  if (ay_engine_start(fmt, d, size) != 0) { set_error("AY: bad module"); return -1; }
  nchips = ay_engine_channels() > 3 ? 2 : 1;
  if (nchips > 2) nchips = 2;
  engine_mode = 1;
  memset(title, 0, sizeof(title));
  const char *t = ay_engine_title();
  for (int n = 0; t && t[n] && n < (int) sizeof(title) - 1; n++) title[n] = t[n];
  ay_init(&ayemu[0]);
  ay_init(&ayemu[1]);
  ay_engine_tick();
  ticks = 1;
  apply_ay_regs();
  tick_left = 0;
  active = 1;
  return 0;
}

static int start_kind(int kind, const play_src_t *src, uint32_t size, play_progress_t progress) {
  active = 0; ended = 0; paused = 0; tick_left = 0; ticks = 0; pt3_size = size;
  last_l = last_r = peak_l = peak_r = 0;
  memset(vu, 0, sizeof(vu));
  err[0] = 0;
  if (!src || size < 80) { set_error("AY: too short"); return -1; }

  const uint8_t *d = (const uint8_t *) LD_FILE_BASE;
  if (src->f) {
    uint8_t *dst = (uint8_t *) LD_FILE_BASE;
    if (size > LD_FILE_MAX) { set_error("AY: over 2.5 MB"); return -1; }
    uint32_t off = 0;
    while (off < size) {
      uint32_t n = size - off;
      if (n > 4096) n = 4096;
      UINT got = 0;
      if (src_read((play_src_t *) src, off, dst + off, n, &got) != 0 || got == 0) {
        set_error("AY: read error");
        return -1;
      }
      off += got;
      if (progress && (off >> 15) != ((off - got) >> 15)) progress(off, size);
    }
    d = dst;
  } else {
    d = src->mem;
  }

  if (!ayp_detect(kind, d, size)) { set_error("AY: header"); return -1; }
  if (ayp_start(kind, d, size) != 0) { set_error(ayp_error()); return -1; }
  nchips = ayp_channels() > 3 ? 2 : 1;
  if (nchips > 2) nchips = 2;
  for (int c = 0; c < 2; c++) {
    memset(&ay[c], 0, sizeof(ay[c]));
    ay[c].lfsr = 0x1FFFF;
    ay[c].last_shape = 0xff;
    ay[c].shape = 0;
  }
  memset(title, 0, sizeof(title));
  const char *t = ayp_title();
  int n = 0;
  for (; t && t[n] && n < (int) sizeof(title) - 1; n++) title[n] = t[n];
#ifdef AY_HW
  ay_hw_ctl(0, vol);
  ay_hw_reset();
  for (volatile int i = 0; i < 2000; i++) ;   // let the RTL reset pulse finish
  ayp_tick();
  ticks = 1;
  for (int c = 0; c < nchips; c++) { ay_apply(&ay[c], ayp_regs(c), c); ay_hw_sync(c, 1); }
  ay_mix((int) AY_TICK, 0);
  next_us = R_US + 20000u;
  ay_hw_ctl(1, vol);
#else
  tick_left = 0;
#endif
  active = 1;
  return 0;
}

int pt3_start(const play_src_t *src, uint32_t size, play_progress_t progress) { return start_engine(AYF_PT3, src, size, progress); }
int pt2_start(const play_src_t *src, uint32_t size, play_progress_t progress) { return start_engine(AYF_PT2, src, size, progress); }
int stc_start(const play_src_t *src, uint32_t size, play_progress_t progress) { return start_engine(AYF_STC, src, size, progress); }
int pt1_start(const play_src_t *src, uint32_t size, play_progress_t progress) { return start_engine(AYF_PT1, src, size, progress); }
int asc_start(const play_src_t *src, uint32_t size, play_progress_t progress) { return start_engine(AYF_ASC, src, size, progress); }
int psc_start(const play_src_t *src, uint32_t size, play_progress_t progress) { return start_engine(AYF_PSC, src, size, progress); }
int sqt_start(const play_src_t *src, uint32_t size, play_progress_t progress) { return start_engine(AYF_SQT, src, size, progress); }
int stp_start(const play_src_t *src, uint32_t size, play_progress_t progress) { return start_engine(AYF_STP, src, size, progress); }
int vtx_start(const play_src_t *src, uint32_t size, play_progress_t progress) { return start_engine(AYF_VTX, src, size, progress); }
int psg_start(const play_src_t *src, uint32_t size, play_progress_t progress) { return start_engine(AYF_PSG, src, size, progress); }

void pt3_stop(void) {
  if (engine_mode) ay_engine_stop();
  else if (active) ayp_stop();
  engine_mode = 0;
  active = 0; ended = 0; paused = 0;
}
int pt3_active(void) { return active; }
int pt3_finished(void) { return ended; }
const char *pt3_error(void) { return err; }
const char *pt3_title(void) { return title; }

int pt3_position(uint32_t *cur, uint32_t *total) {
  if (!active) return 0;
  *cur = ticks;
  *total = engine_mode ? (uint32_t) ay_engine_length() : ayp_length();
  return 1;
}

void pt3_info(fmt_info_t *o) {
  o->channels = engine_mode ? ay_engine_channels() : ayp_channels();
  o->samples = 0;
  o->order = 0;
  o->pattern = 0;
  o->row = 0;
  o->song_len = engine_mode ? (ay_engine_length() / 50) : (int) (ayp_length() / 50u);
  o->speed = 1;
  o->bpm = 50;
  o->size = pt3_size;
}

void pt3_audio(fmt_audio_t *o) {
  o->level_l = last_l;
  o->level_r = last_r;
  o->peak_l = peak_l;
  o->peak_r = peak_r;
  analysis_bands(o->bands, FMT_BANDS);
  memcpy(o->vu, vu, sizeof(o->vu));
  o->volume = vol;
  o->paused = paused;
  peak_l = peak_r = 0;
  memset(vu, 0, sizeof(vu));
}

int pt3_get_volume(void) { return vol; }
int pt3_set_volume(int v) {
  if (v < 0) v = 0; if (v > 64) v = 64; vol = v;
#ifdef AY_HW
  if (active) ay_hw_ctl(paused ? 0 : 1, vol);
#endif
  return vol;
}
int pt3_pause(int on) {
  if (on < 0) paused = !paused; else paused = on ? 1 : 0;
#ifdef AY_HW
  if (active) {
    if (paused) ay_hw_ctl(0, vol);
    else { next_us = R_US + 20000u; ay_hw_ctl(1, vol); }
  }
#endif
  return paused;
}
