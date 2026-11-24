// Full IT parser/replayer, using fastmix. MikMod remains available as the old path in fmt_it.
#include <string.h>
#include "it_fpga.h"
#include "it/it_parser.h"
#include "it/it_player.h"
#include "mmio.h"
#include "audio.h"
#include "mixer.h"
#include "src.h"

#define IT_META_BYTES (64u * 1024u)
#define IT_OUT_CHUNK  256

static uint8_t it_meta[IT_META_BYTES];
static it_module_t it_mod;
static it_player_t it_pl;
static uint8_t *it_file;
static uint32_t it_file_size, it_file_used;
static int it_active, it_ended, it_paused;
static int it_volume = 64;
static char it_err[48];
static int16_t it_out[IT_OUT_CHUNK * 2];
static int32_t it_last_l, it_last_r, it_peak_l, it_peak_r;
static uint8_t it_vu[FMT_CHANNELS];

static void it_error(const char *s) {
  int i = 0;
  for (; s && s[i] && i < (int) sizeof(it_err) - 1; i++) it_err[i] = s[i];
  it_err[i] = 0;
}

static void it_release(void) {
  it_active = it_ended = it_paused = 0;
  it_file = 0;
  it_file_size = it_file_used = 0;
  memset(it_vu, 0, sizeof(it_vu));
  it_last_l = it_last_r = it_peak_l = it_peak_r = 0;
}

int it_fpga_start(const play_src_t *src, uint32_t size, play_progress_t progress) {
  it_release();
  if (!src || !size) { it_error("empty IT"); return -1; }
  if (size > SDRAM_FILE_MAX) { it_error("IT over 32 MB"); return -1; }

  uint8_t *buf;
  if (src->f) {
    uint32_t t0 = R_US;
    while (!(R_SDRAM & 1)) {
      if ((uint32_t)(R_US - t0) > 200000u) { it_error("SDRAM not ready"); return -1; }
    }
    buf = (uint8_t *) SDRAM_BASE;
    uint32_t off = 0;
    while (off < size) {
      uint32_t n = size - off;
      if (n > 4096) n = 4096;
      UINT got = 0;
      if (src_read((play_src_t *) src, off, buf + off, n, &got) != 0 || got == 0) {
        it_error("read error"); return -1;
      }
      off += got;
      if (progress && (off >> 15) != ((off - got) >> 15)) progress(off, size);
    }
  } else {
    buf = (uint8_t *) src->mem;
  }
  it_file = buf;
  it_file_size = size;
  it_file_used = buf == (uint8_t *) SDRAM_BASE ? ((size + 15u) & ~15u) : 0;

  uint8_t *pcm_base = (uint8_t *) SDRAM_BASE + it_file_used;
  uint32_t pcm_size = SDRAM_FILE_MAX - it_file_used;
  it_arena_t meta, pcm;
  it_arena_init(&meta, it_meta, sizeof(it_meta));
  it_arena_init(&pcm, pcm_base, pcm_size);

  it_diag_t diag;
  int rc = it_load(buf, size, &meta, &pcm, IT_LOAD_NOZEROCOPY, &diag, &it_mod);
  if (rc != IT_OK) { it_error(it_err_str(rc)); return -1; }

  it_player_cfg_t cfg;
  it_player_cfg_default(&cfg);
  cfg.mixrate = 48000;
  cfg.flags = 0;                 // stop at the end of the song
  cfg.out_gain = 256;            // calibrated later on target
  it_player_init(&it_pl, &it_mod, &cfg);
  fm_reset();
  fm_set_max_mixed(32);

  it_active = 1;
  it_ended = 0;
  it_paused = 0;
  it_error("");
  return 0;
}

void it_fpga_service(void) {
  if (!it_active || it_paused) return;
  int free = audio_free();
  if (free <= 0) return;
  int n = free > IT_OUT_CHUNK ? IT_OUT_CHUNK : free;
  it_player_render(&it_pl, it_out, n);
  int v = it_volume;
  for (int i = 0; i < n; i++) {
    int32_t l = (int32_t)it_out[2 * i] * v / 64;
    int32_t r = (int32_t)it_out[2 * i + 1] * v / 64;
    int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
    if (al > it_peak_l) it_peak_l = al;
    if (ar > it_peak_r) it_peak_r = ar;
    it_last_l = l; it_last_r = r;
    mixer_write((int16_t)l, (int16_t)r);
  }
  if (it_pl.finished) {
    it_active = 0;
    it_ended = 1;
  }
  for (int c = 0; c < FMT_CHANNELS; c++) it_vu[c] = 0;
  for (int i = 0; i < it_pl.nv_hi; i++) {
    const it_voice_t *x = &it_pl.vox[i];
    if (x->state == IT_VS_FREE || x->state == IT_VS_DYING || x->out_vol <= 0) continue;
    if (x->host < FMT_CHANNELS) {
      int val = x->out_vol > 255 ? 255 : x->out_vol;
      if (val > it_vu[x->host]) it_vu[x->host] = (uint8_t)val;
    }
  }
}

int it_fpga_active(void) { return it_active; }
int it_fpga_finished(void) { return it_ended; }
void it_fpga_stop(void) { it_release(); }
const char *it_fpga_error(void) { return it_err; }

const char *it_fpga_title(void) {
  static char t[27];
  int i = 0;
  for (; i < 26 && it_mod.title[i]; i++) t[i] = it_mod.title[i];
  t[i] = 0;
  return t[0] ? t : "IT";
}

int it_fpga_position(uint32_t *cur, uint32_t *total) {
  if (!it_active) return 0;
  *cur = it_pl.ord;
  *total = it_mod.song_len ? it_mod.song_len : 1;
  return 1;
}

void it_fpga_info(fmt_info_t *o) {
  if (!it_active) return;
  o->channels = it_mod.used_channels;
  o->samples = it_mod.smpnum;
  o->order = it_pl.ord;
  o->pattern = it_pl.pat;
  o->row = it_pl.row;
  o->song_len = it_mod.song_len;
  o->speed = it_pl.speed;
  o->bpm = it_pl.tempo;
  o->size = it_file_size;
}

void it_fpga_audio(fmt_audio_t *o) {
  o->level_l = it_last_l;
  o->level_r = it_last_r;
  o->peak_l = it_peak_l;
  o->peak_r = it_peak_r;
  memcpy(o->vu, it_vu, sizeof(o->vu));
  o->volume = it_volume;
  o->paused = it_paused;
  it_peak_l = it_peak_r = 0;
}

int it_fpga_get_volume(void) { return it_volume; }
int it_fpga_set_volume(int v) {
  if (v < 0) v = 0;
  if (v > 64) v = 64;
  it_volume = v;
  return v;
}
int it_fpga_pause(int on) {
  if (on < 0) it_paused = !it_paused; else it_paused = on ? 1 : 0;
  return it_paused;
}
