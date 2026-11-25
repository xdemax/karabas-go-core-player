// Full IT parser/replayer, using fastmix. MikMod remains available as the old path in fmt_it.
#include <string.h>
#include <stdio.h>
#include "it_fpga.h"
#include "it/it_parser.h"
#include "it/it_player.h"
#include "mmio.h"
#include "audio.h"
#include "mixer.h"
#include "src.h"
#include "resample.h"

#define IT_META_BYTES (64u * 1024u)
#define IT_OUT_CHUNK  256
#define IT_OUT_MAX    512

static uint8_t it_meta[IT_META_BYTES];
static it_module_t it_mod;
static it_player_t it_pl;
static uint8_t *it_file;
static uint32_t it_file_size, it_file_used;
static int it_active, it_ended, it_paused;
static int it_volume = 64;
static char it_err[48];
static int16_t it_out[IT_OUT_CHUNK * 2];
static int16_t it_out_resampled[IT_OUT_MAX * 2];
static uint32_t it_mixrate;
static uint32_t it_total_ms;      /* real song length, computed once at load */
static uint32_t it_elapsed_ms;    /* wall-clock playback time, for the progress bar */
static uint32_t it_elapsed_rem;
static rs_t rs;
static int32_t it_last_l, it_last_r, it_peak_l, it_peak_r;
static uint8_t it_vu[FMT_CHANNELS];
static char it_dbg[32];
static uint32_t it_dbg_us, it_dbg_cycles, it_dbg_pcycles, it_dbg_cps;
static int it_dbg_voices, it_dbg_alloc;

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
  memset(&rs, 0, sizeof(rs));
  it_dbg[0] = 0;
  it_dbg_us = it_dbg_cycles = it_dbg_pcycles = it_dbg_cps = 0;
  it_dbg_voices = it_dbg_alloc = 0;
}

/* Walk the song once without mixing: run ticks until the end or until an order repeats
 * (a real loop). This gives the true length in ms instead of "orders / total orders",
 * which made the progress bar jump when the song uses order jumps. */
static it_player_t it_sim;
static uint32_t it_probe_duration(const it_player_t *src, uint32_t rate)
{
  uint8_t seen[32];
  uint64_t frames = 0;
  int guard = 0, last = -1;

  if (!rate) rate = 48000;
  memset(seen, 0, sizeof(seen));
  memcpy(&it_sim, src, sizeof(it_sim));
  while (!it_sim.finished && guard++ < 60000) {
    int o = it_sim.ord;
    if (o != last) {                       /* an order spans many ticks */
      if (o >= 0 && o < 256) {
        uint8_t bit = (uint8_t) (1u << (o & 7));
        if (seen[o >> 3] & bit) break;     /* back to an order we already played: loop */
        seen[o >> 3] |= bit;
      }
      last = o;
    }
    it_player_tick(&it_sim);
    frames += it_player_tick_frames(&it_sim);
  }
  return (uint32_t) (frames * 1000ull / rate);
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

  #define IT_CH_LIMIT 64

  it_player_cfg_t cfg;
  it_player_cfg_default(&cfg);
  uint32_t rate = it_mod.used_channels <= 16 ? 48000 : 24000;
  it_mixrate = rate;
  cfg.mixrate = rate;
  cfg.flags = (rate < 48000) ? IT_CFG_NEAREST : 0;
  // Calibrated against libopenmpt (openmpt123) on En-trance.it: RMS within 10 % of the
  // reference and no clipping. 256 was 2.6x too hot and clipped (audible distortion).
  cfg.out_gain = 112;
  it_player_init(&it_pl, &it_mod, &cfg);
  it_total_ms = it_probe_duration(&it_pl, it_mixrate);
  it_elapsed_ms = 0;
  it_elapsed_rem = 0;
  fm_reset();
  fm_set_max_mixed(32);
  rs_init(&rs, it_mixrate);

  it_active = 1;
  it_ended = 0;
  it_paused = 0;
  it_error("");
  return 0;
}

void it_fpga_service(void) {
  if (!it_active || it_paused) return;
  int free = audio_free();
  if (free <= 2) return;
  int n = free > IT_OUT_CHUNK ? IT_OUT_CHUNK : free;
  if (it_mixrate && it_mixrate < 48000) {
    int lim = (int)(((int64_t)free * it_mixrate) / 48000) - 2;
    if (n > lim) n = lim;
  }
  if (n <= 0) return;
#ifdef IT_PROFILE
  uint32_t c0 = R_CYCLES;
#endif
  it_player_render(&it_pl, it_out, n);
#ifdef IT_PROFILE
  uint32_t c1 = R_CYCLES;
#endif
  int v = it_volume;
  
  int out_count = 0;
  for (int i = 0; i < n && out_count < IT_OUT_MAX; i++) {
    int32_t t = (int32_t)it_out[2 * i] * v;
    int32_t l = (t + ((t >> 31) & 63)) >> 6;
    t = (int32_t)it_out[2 * i + 1] * v;
    int32_t r = (t + ((t >> 31) & 63)) >> 6;
    
    int nout = rs_frame(&rs, (int16_t)l, (int16_t)r, (int16_t(*)[2])&it_out_resampled[2 * out_count]);
    out_count += nout;
  }

  for (int i = 0; i < out_count; i++) {
    int32_t l = it_out_resampled[2 * i];
    int32_t r = it_out_resampled[2 * i + 1];
    int32_t al = l < 0 ? -l : l;
    int32_t ar = r < 0 ? -r : r;
    if (al > it_peak_l) it_peak_l = al;
    if (ar > it_peak_r) it_peak_r = ar;
    it_last_l = l;
    it_last_r = r;
    mixer_write_it((int16_t)l, (int16_t)r);
  }
  {
    uint32_t rem = it_elapsed_rem + (uint32_t) ((uint64_t) n * 1000u);
    it_elapsed_ms += rem / (it_mixrate ? it_mixrate : 48000u);
    it_elapsed_rem = rem % (it_mixrate ? it_mixrate : 48000u);
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
#ifdef IT_PROFILE
  uint32_t c2 = R_CYCLES;
  it_dbg_pcycles += (uint32_t)(c1 - c0);
  it_dbg_cycles += (uint32_t)(c2 - c0);
  if ((uint32_t)(R_US - it_dbg_us) >= 1000000u) {
    uint32_t dt = (uint32_t) (R_US - it_dbg_us);      // the REAL window
    it_dbg_us = R_US;
    it_dbg_cps = it_dbg_cycles;
    /* Percent of the CPU over the real window (see the XM engine for the reasoning). */
    if (!dt) dt = 1;
    uint32_t pct = (uint32_t) ((uint64_t) it_dbg_cps * 100u / ((uint64_t) dt * 75u));
    uint32_t rpct = it_dbg_cps ?
        (uint32_t)((uint64_t)it_dbg_pcycles * 100u / it_dbg_cps) : 0;
    it_dbg_voices = it_player_active_voices(&it_pl);
    it_dbg_alloc = it_pl.nv_hi;
    snprintf(it_dbg, sizeof(it_dbg), "%d/%dv %lu%% r%lu",
             it_dbg_voices, it_dbg_alloc,
             (unsigned long)pct, (unsigned long)rpct);
    it_dbg_cycles = 0;
    it_dbg_pcycles = 0;
  }
#endif
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
  /* Both in seconds: the bar and the remaining time are then monotonic even when the
   * song jumps between orders. */
  *cur = it_elapsed_ms / 1000u;
  *total = it_total_ms / 1000u;
  if (!*total) *total = 1;
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
  if (it_paused) {
    memset(&rs, 0, sizeof(rs));
    rs_init(&rs, it_mixrate);
  }
  return it_paused;
}
const char *it_fpga_debug(void) {
#ifdef IT_PROFILE
  return it_active ? it_dbg : "";
#else
  return "";
#endif
}
