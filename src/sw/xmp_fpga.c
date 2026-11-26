// S3M/IT through libxmp. It is a complete C tracker engine; its PCM goes to the common mixer.
#include <string.h>
#include "xmp_fpga.h"
#include "libxmp/include/xmp.h"
#include "mmio.h"
#include "audio.h"
#include "mixer.h"
#include "src.h"

#define XMP_RATE 48000

static xmp_context ctx;
static int active, ended, paused;
static uint8_t *mod_mem;
static char err[48], title[64];
static int16_t pcm[2048];
static int32_t last_l, last_r, peak_l, peak_r;
static uint8_t vu[FMT_CHANNELS];
static int vol = 64;
static struct xmp_frame_info fi;
static struct xmp_module_info mi;

static void set_error(const char *s) {
  int i = 0;
  for (; s && s[i] && i < (int) sizeof(err) - 1; i++) err[i] = s[i];
  err[i] = 0;
}

static void release(void) {
  if (ctx) {
    xmp_end_player(ctx);
    xmp_release_module(ctx);
    xmp_free_context(ctx);
    ctx = 0;
  }
  mod_mem = 0;
  active = ended = paused = 0;
}

int xmp_start(const play_src_t *src, uint32_t size, play_progress_t progress) {
  release();
  if (!src || !size) { set_error("empty module"); return -1; }
  if (size > SDRAM_FILE_MAX) { set_error("module over 32 MB"); return -1; }

  uint8_t *buf;
  if (src->f) {
    uint32_t t0 = R_US;
    while (!(R_SDRAM & 1)) {
      if ((uint32_t) (R_US - t0) > 200000u) { set_error("SDRAM not ready"); return -1; }
    }
    buf = (uint8_t *) SDRAM_BASE;
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
  mod_mem = buf;

  ctx = xmp_create_context();
  if (!ctx) { set_error("libxmp context"); return -1; }
  if (xmp_load_module_from_memory(ctx, buf, (long) size) != 0) {
    set_error("bad module");
    release();
    return -1;
  }
  if (xmp_start_player(ctx, XMP_RATE, 0) != 0) {
    set_error("libxmp start");
    release();
    return -1;
  }
  xmp_set_player(ctx, XMP_PLAYER_INTERP, XMP_INTERP_LINEAR);
  xmp_set_player(ctx, XMP_PLAYER_DSP, 0);
  xmp_set_player(ctx, XMP_PLAYER_VOLUME, vol * 100 / 64);
  xmp_get_module_info(ctx, &mi);
  title[0] = 0;
  if (mi.mod) {
    int i = 0;
    for (; mi.mod->name[i] && i < (int) sizeof(title) - 1; i++) title[i] = mi.mod->name[i];
    title[i] = 0;
  }
  if (!title[0]) strcpy(title, "tracker");
  active = 1;
  ended = paused = 0;
  last_l = last_r = peak_l = peak_r = 0;
  memset(vu, 0, sizeof(vu));
  return 0;
}

void xmp_service(void) {
  if (!active || paused) return;
  int free = audio_free();
  while (free > 0) {
    int n = free > 256 ? 256 : free;
    int rc = xmp_play_buffer(ctx, pcm, n * 4, 0);
    if (rc < 0) { active = 0; ended = 1; break; }
    for (int i = 0; i < n; i++) {
      int16_t l = pcm[i * 2], r = pcm[i * 2 + 1];
      int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
      if (al > peak_l) peak_l = al;
      if (ar > peak_r) peak_r = ar;
      last_l = l; last_r = r;
      mixer_write(l, r);
    }
    free -= n;
  }
}

int xmp_active(void) { return active; }
int xmp_finished(void) { return ended; }
void xmp_stop(void) { release(); }
const char *xmp_error(void) { return err; }
const char *xmp_title(void) { return title; }

int xmp_position(uint32_t *cur, uint32_t *total) {
  if (!active || !ctx) return 0;
  xmp_get_frame_info(ctx, &fi);
  *cur = (uint32_t) (fi.pos < 0 ? 0 : fi.pos);
  *total = mi.mod && mi.mod->len ? (uint32_t) mi.mod->len : 1;
  return 1;
}

void xmp_info(fmt_info_t *o) {
  if (!ctx || !mi.mod) return;
  xmp_get_frame_info(ctx, &fi);
  o->channels = mi.mod->chn;
  o->samples = mi.mod->smp;
  o->order = fi.pos;
  o->pattern = fi.pattern;
  o->row = fi.row;
  o->song_len = mi.mod->len;
  o->speed = fi.speed;
  o->bpm = fi.bpm;
  o->size = 0;
}

void xmp_audio(fmt_audio_t *o) {
  if (ctx && mi.mod) {
    xmp_get_frame_info(ctx, &fi);
    int n = mi.mod->chn;
    if (n > FMT_CHANNELS) n = FMT_CHANNELS;
    for (int c = 0; c < n; c++) {
      int v = fi.channel_info[c].volume;
      vu[c] = (uint8_t) (v > 255 ? 255 : v);
    }
  }
  o->level_l = last_l;
  o->level_r = last_r;
  o->peak_l = peak_l;
  o->peak_r = peak_r;
  memcpy(o->vu, vu, sizeof(o->vu));
  o->volume = vol;
  o->paused = paused;
  peak_l = peak_r = 0;
}

int xmp_get_volume(void) { return vol; }
int xmp_set_volume(int v) {
  if (v < 0) v = 0;
  if (v > 64) v = 64;
  vol = v;
  if (ctx) xmp_set_player(ctx, XMP_PLAYER_VOLUME, v * 100 / 64);
  return vol;
}
int xmp_pause(int on) {
  if (on < 0) paused = !paused; else paused = on ? 1 : 0;
  return paused;
}
