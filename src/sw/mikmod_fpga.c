// S3M and IT through MikMod: one complete tracker engine, not a per-effect port.
#include <string.h>
#include "mikmod_fpga.h"
#include "mikmod.h"
#include "mikmod/mmio/mikmod_mem.h"
#include "mmio.h"
#include "audio.h"
#include "mixer.h"
extern UWORD md_bpm;               // the internal tempo that VC_WriteBytes uses

#define MIKMOD_MAXCH 64

static int active, ended, paused, inited, kind = MIKMOD_S3M;
static MODULE *mod;
static uint8_t *module_mem;
static char err[48], title[64];
static uint8_t vu[FMT_CHANNELS];
static int32_t last_l, last_r, peak_l, peak_r;
static int vol = 64;
static int16_t pcm[2048];

static void set_error(const char *s) {
  int i = 0;
  for (; s && s[i] && i < (int) sizeof(err) - 1; i++) err[i] = s[i];
  err[i] = 0;
}

static int init(void) {
  if (inited) return 0;
  MikMod_RegisterDriver(&drv_nos);
  MikMod_RegisterLoader(&load_s3m);
  MikMod_RegisterLoader(&load_it);
  md_mode = DMODE_16BITS | DMODE_STEREO | DMODE_SOFT_MUSIC | DMODE_INTERP;
  md_mixfreq = 48000;
  md_musicvolume = 128;
  md_sndfxvolume = 0;
  if (MikMod_Init("") != 0) { set_error("MikMod init"); return -1; }
  inited = 1;
  return 0;
}

static void release(void) {
  if (mod) { Player_Stop(); Player_Free(mod); mod = 0; }
  if (inited) { MikMod_Exit(); inited = 0; }
  module_mem = 0;
  active = ended = paused = 0;
}

int mikmod_start(int k, const play_src_t *src, uint32_t size, play_progress_t progress) {
  release();
  if (!src || !size) { set_error("empty module"); return -1; }
  if (size > SDRAM_FILE_MAX) { set_error("module over 32 MB"); return -1; }

  uint8_t *buf;
  if (src->f) {
    buf = (uint8_t *) LD_FILE_BASE;
    if (size > LD_FILE_MAX) buf = (uint8_t *) SDRAM_BASE;
    if (size > LD_FILE_MAX || !(R_SDRAM & 1)) {
      uint32_t t0 = R_US;
      while (!(R_SDRAM & 1)) {
        if ((uint32_t) (R_US - t0) > 200000u) { set_error("SDRAM not ready"); return -1; }
      }
    }
    uint32_t off = 0;
    while (off < size) {
      uint32_t n = size - off;
      if (n > 4096) n = 4096;
      UINT got = 0;
      if (src_read((play_src_t *) src, off, buf + off, n, &got) != 0 || got == 0) {
        set_error("read error"); return -1;
      }
      off += got;
      if (progress && ((off >> 15) != ((off - got) >> 15) || off == size)) progress(off, size);
    }
  } else {
    buf = (uint8_t *) src->mem;
  }
  module_mem = buf;
#ifndef HOST
  if (!(R_SDRAM & 1)) {
    uint32_t t0 = R_US;
    while (!(R_SDRAM & 1)) {
      if ((uint32_t) (R_US - t0) > 200000u) { set_error("SDRAM not ready"); return -1; }
    }
  }
#endif
  // All MikMod data (module structures and samples) lives in SDRAM. For a big module the file
  // itself occupies the start of the SDRAM window; the arena starts after it.
  uint8_t *arena = (uint8_t *) SDRAM_BASE;
  uint32_t arena_size = SDRAM_FILE_MAX;
  if (buf >= (uint8_t *) SDRAM_BASE && buf < (uint8_t *) SDRAM_BASE + SDRAM_FILE_MAX) {
    uint32_t used = (uint32_t) (buf - (uint8_t *) SDRAM_BASE) + size;
    used = (used + 15u) & ~15u;
    if (used >= SDRAM_FILE_MAX) { set_error("no SDRAM for samples"); return -1; }
    arena = (uint8_t *) SDRAM_BASE + used;
    arena_size = SDRAM_FILE_MAX - used;
  }
  mikmod_mem_set(arena, arena_size);
  if (init() != 0) return -1;
  mod = Player_LoadMem((CHAR *) buf, (int) size, MIKMOD_MAXCH, 0);
  if (!mod) { set_error("bad module"); return -1; }
  mod->loop = 0;                 // a playlist track must finish at its end
  mod->wrap = 0;
  Player_Start(mod);
  md_bpm = mod->bpm;             // the first VC_WriteBytes tick must use this module's tempo
  md_musicvolume = (UBYTE) (vol * 2);
  kind = k;
  title[0] = 0;
  if (mod->songname) {
    int i = 0;
    for (; mod->songname[i] && i < (int) sizeof(title) - 1; i++) title[i] = mod->songname[i];
    title[i] = 0;
  }
  if (!title[0]) strcpy(title, k == MIKMOD_IT ? "IT" : "S3M");
  active = 1;
  ended = 0;
  paused = 0;
  last_l = last_r = peak_l = peak_r = 0;
  memset(vu, 0, sizeof(vu));
  return 0;
}

void mikmod_service(void) {
  if (!active || paused) return;
  int free = audio_free();
  while (free > 0) {
    int n = free > 256 ? 256 : free;
    int bytes = n * 4;
    VC_WriteBytes((SBYTE *) pcm, (ULONG) bytes);
    for (int i = 0; i < n; i++) {
      int16_t l = pcm[i * 2];
      int16_t r = pcm[i * 2 + 1];
      int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
      if (al > peak_l) peak_l = al;
      if (ar > peak_r) peak_r = ar;
      last_l = l; last_r = r;
      mixer_write(l, r);
    }
    free -= n;
    if (!Player_Active()) { active = 0; ended = 1; break; }
  }
  if (mod) {
    int n = mod->numchn;
    if (n > FMT_CHANNELS) n = FMT_CHANNELS;
    for (int c = 0; c < n; c++) {
      UWORD v = Voice_GetVolume((SBYTE) c);
      vu[c] = (uint8_t) (v > 255 ? 255 : v);
    }
  }
}

int mikmod_active(void) { return active; }
int mikmod_finished(void) { return ended; }
void mikmod_stop(void) { release(); }
const char *mikmod_error(void) { return err; }
const char *mikmod_title(void) { return title; }
int mikmod_position(uint32_t *cur, uint32_t *total) {
  if (!active || !mod) return 0;
  *cur = (uint32_t) (mod->sngpos < 0 ? 0 : mod->sngpos);
  *total = mod->numpos ? mod->numpos : 1;
  return 1;
}
void mikmod_info(fmt_info_t *o) {
  if (!mod) return;
  o->channels = mod->numchn;
  o->samples = mod->numsmp;
  o->order = mod->sngpos;
  o->pattern = mod->sngpos >= 0 && mod->sngpos < mod->numpos ? mod->positions[mod->sngpos] : 0;
  o->row = mod->patpos;
  o->song_len = mod->numpos;
  o->speed = mod->sngspd;
  o->bpm = mod->bpm;
  o->size = 0;
}
void mikmod_audio(fmt_audio_t *o) {
  o->level_l = last_l;
  o->level_r = last_r;
  o->peak_l = peak_l;
  o->peak_r = peak_r;
  memcpy(o->vu, vu, sizeof(o->vu));
  o->volume = vol;
  o->paused = paused;
  peak_l = peak_r = 0;
}
int mikmod_get_volume(void) { return vol; }
int mikmod_set_volume(int v) {
  if (v < 0) v = 0;
  if (v > 64) v = 64;
  vol = v;
  md_musicvolume = (UBYTE) (v * 2);
  return vol;
}
int mikmod_pause(int on) {
  if (on < 0) paused = !paused; else paused = on ? 1 : 0;
  return paused;
}
