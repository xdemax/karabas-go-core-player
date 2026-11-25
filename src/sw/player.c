// The player core: the file list, the keys, the playback, the now-playing state. It knows no
// format and no skin: the formats are fmt_module_t (formats/*.c), the screen is a ui_skin_t
// (skin_*.c). The view model is player_view_t (ui_skin.h).
#include <string.h>
#include <stdlib.h>
#include "mmio.h"
#include "ui.h"
#include "ui_skin.h"
#include "fs.h"
#include "browser.h"
#include "playlist.h"
#include "it_fpga.h"
#ifndef PLAYER_BUILD
#define PLAYER_BUILD "?"
#endif
#include "fmt.h"
#include "src.h"
#include "audio.h"
#include "spi.h"

#define ROWS 14
#define HEAVY_CHANNELS 24       // the analyzer gives up above this; keep the mixer fed instead

static browser_t br;
static char msg[64] = "ready";
static int playing = 0;
static int playing_sd1 = 0;
enum { PLAY_CTX_DIR = 0, PLAY_CTX_PLAYLIST = 1 };
static int play_ctx;

static int np_on;
static char np_fmt[8], np_title[48];
static char fmt_list[96];
static uint32_t np_start, np_cur, np_total, np_us;
static uint32_t nav_t;
static int view_ch;
static int spectrum_on = 1;
static int volume_setting = 64;
static int osd_raw = -1;
static char mix_msg[64];
static uint32_t mix_t0, mix_w0;

static const int osd_vol[16] = { 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64 };

static player_view_t V;

static void view_fill(int audio) {
  fmt_info_t fi;
  fmt_audio_t fa;
  if (audio) {
    fmt_info(&fi);
    spectrum_on = fi.channels < HEAVY_CHANNELS;
    fmt_set_analysis(spectrum_on);
    fmt_audio(&fa);
  }

  V.path = br.path;
  V.ent = br.ent;
  V.nent = br.nent;
  V.sel = br.sel;
  V.top = br.top;
  V.playing = playing;
  V.now = np_on;
  V.status = msg;
  V.mix = mix_msg;
  V.format = np_fmt;
  V.title = np_title;
  fmt_supported_list(fmt_list, sizeof(fmt_list));
  V.formats = fmt_list;
  V.skin = skin_name();
  V.elapsed = np_on ? (uint32_t) ((R_US - np_start) / 1000000u) : 0;
  V.cur = np_cur;
  V.total_pos = np_total;
  V.has_pos = (np_total != 0);
  V.total = (V.has_pos && np_cur) ? (uint32_t) ((uint64_t) V.elapsed * np_total / np_cur) : 0;

  V.engine = fmt_engine_name();
  V.version = fmt_engine_version();
  V.build = PLAYER_BUILD;
  V.debug = it_fpga_debug();
  if (!V.debug || !V.debug[0]) V.debug = audio_rate_debug();
  V.aud_rate = (uint32_t) audio_rate();
  V.size = (br.sel >= 0 && br.sel < br.nent) ? br.ent[br.sel].size : 0;
  V.loading = (np_on && !playing) ? 1 : 0;
  V.error = msg;

  if (!audio) {
    V.frame++;
    V.fb = (int) (R_FB & 1);
    V.idle = playing ? fmt_service : NULL;
    return;
  }

  view_ch = fi.channels;
  V.channels = fi.channels;
  V.samples = fi.samples;
  V.order = fi.order;
  V.pattern = fi.pattern;
  V.row = fi.row;
  V.song_len = fi.song_len;
  V.speed = fi.speed;
  V.bpm = fi.bpm;

  V.level_l = fa.level_l; V.level_r = fa.level_r;
  V.peak_l = fa.peak_l; V.peak_r = fa.peak_r;
  V.spectrum = spectrum_on;
  memcpy(V.bands, fa.bands, sizeof(V.bands));
  memcpy(V.vu, fa.vu, sizeof(V.vu));
  V.volume = playing ? fa.volume : volume_setting;
  V.paused = fa.paused;

  V.frame++;
  V.fb = (int) (R_FB & 1);
  V.idle = playing ? fmt_service : NULL;
}

static void draw_view(void) {
  view_fill(1);
  skin_current()->draw(&V);
}

static void update_view(void) {
  view_fill(1);
  skin_current()->update(&V);
}

// While a list key repeats, only the list changes: no audio analysis, no player panel.
static void update_list_view(void) {
  view_fill(0);
  if (skin_current()->list) skin_current()->list(&V);
  else skin_current()->update(&V);
}

static void status(const char *s) {
  int i = 0;
  for (; s[i] && i < (int) sizeof(msg) - 1; i++) msg[i] = s[i];
  msg[i] = 0;
}

static int keep(const char *name, int dir) {
  return dir || fmt_supported(name) || playlist_supported(name);
}

static void list(void) {
  int n = browser_reload(&br, keep, playing ? fmt_service : NULL);
  if (n < 0) {
    status(n == -3 ? "SD2: too many files (RAM)" : "SD2: no card or no FAT");
    return;
  }
  if (br.sel >= br.top + ROWS) br.top = br.sel - ROWS + 1;
}

static void np_copy(const fmt_module_t *m, const char *name) {
  int i = 0;
  for (; m->id[i] && i < (int) sizeof(np_fmt) - 1; i++) np_fmt[i] = m->id[i];
  np_fmt[i] = 0;
  const char *t = fmt_title();
  if (!t || !t[0]) t = name;
  for (i = 0; t && t[i] && i < (int) sizeof(np_title) - 1; i++) np_title[i] = t[i];
  np_title[i] = 0;
}

static void np_begin(const fmt_module_t *m, const char *name) {
  np_on = 1; np_start = R_US; np_cur = np_total = np_us = 0;
  np_copy(m, name);
}

static void np_set(const fmt_module_t *m, const char *name) {
  np_on = 1; np_start = R_US; np_cur = np_total = np_us = 0;
  np_copy(m, name);
}

static void playing_status(const fmt_module_t *m, int sd1, const char *name) {
  char b[32], *q = b;
  q = str_cpy(b, sd1 ? "playing SD1 " : "playing ");
  str_cpy(q, m->id);
  status(b);
  np_set(m, name);
}

static void progress(uint32_t done, uint32_t total) {
  np_cur = done; np_total = total;
  if (np_on) {
    view_fill(1);
    skin_current()->progress(&V);
  }
}

static void play_selected(void) {
  const fs_entry_t *e = browser_selected(&br);
  if (!e) return;
  if (e->dir || playlist_supported(e->name)) {
    int n = browser_enter(&br, keep, playing ? fmt_service : NULL);
    if (n < 0) {
      status(n == -3 ? "SD2: too many files (RAM)" : "SD2: can't open the directory/playlist");
      draw_view();
      return;
    }
    if (n == BROWSER_ENTER_PLAYLIST) {
      if (br.nent == 0) { status("empty playlist"); draw_view(); return; }
      play_ctx = PLAY_CTX_PLAYLIST;
      play_selected();                 // the playlist starts with its first track
      return;
    }
    if (n == BROWSER_ENTER_DIR) update_list_view();
    return;
  }

  char full[FS_PATH_MAX + FS_NAME_MAX + 2];
  if (browser_selected_path(&br, full, sizeof(full)) != 0) { status("SD2: path too long"); draw_view(); return; }
  const fmt_module_t *m = fmt_by_ext(e->name);
  if (!m) { status("no player for this format"); draw_view(); return; }
  FIL f;
  if (fs_open(full, &f) != 0) { status("can't open the file"); draw_view(); return; }
  uint32_t file_size = e->size ? e->size : (uint32_t) f_size(&f);
  play_ctx = br.playlist ? PLAY_CTX_PLAYLIST : PLAY_CTX_DIR;
  fmt_stop();
  playing = 0;
  status("sending...");
  np_begin(m, e->name);
  draw_view();
  play_src_t src = { &f, NULL };
  playing_sd1 = 0;
  int rc = m->play(&src, file_size, progress);
  f_close(&f);
  if (rc == 0) {
    playing = 1; audio_reset_stats();
    fmt_set_volume(volume_setting);
    mix_t0 = R_US; mix_w0 = audio_written(); mix_msg[0] = 0;
    playing_status(m, 0, e->name);
  } else {
    np_on = 0;
    status(m->error ? m->error() : fmt_error());
  }
  update_view();
}

// the next playable file of the SD2 list (the current directory). It does not wrap: after the last
// file the player stops.
static int next_track(void) {
  if (playing_sd1) return 0;
  for (int i = br.sel + 1; i < br.nent; i++) {
    if (br.ent[i].dir || !fmt_supported(br.ent[i].name)) continue;
    br.sel = i;
    if (br.sel < br.top) br.top = br.sel;
    if (br.sel >= br.top + ROWS) br.top = br.sel - ROWS + 1;
    play_selected();
    return 1;
  }
  return 0;
}

// F12: a soft reset of the core program: stop the engine, reset the list state and jump to the
// reset vector. crt0 clears .bss, main -> player_run re-initializes the SPI/SD/GFX/audio/formats.
static void core_reset(void) __attribute__((noreturn));
static void core_reset(void) {
  fmt_stop();
  browser_free(&br);
  browser_init(&br);
  msg[0] = 0;
  playing = 0;
  play_ctx = PLAY_CTX_DIR;
  np_on = 0;
  void (*reset_vector)(void) = (void *) 0;
  reset_vector();
  for (;;) ;
}

static void key(key_ev_t *e) {
  int nav = 0;
  {
    skin_event_t se;
    se.type = SKIN_EV_KEY;
    se.key = e->key;
    se.x = se.y = se.buttons = se.wheel = 0;
    if (skin_current()->event) skin_current()->event(&se);
  }
  switch (e->key) {
    case K_UP: if (br.sel > 0) br.sel--; nav = 1; break;
    case K_DOWN: if (br.sel + 1 < br.nent) br.sel++; nav = 1; break;
    case K_LEFT:
    case K_PGUP: br.sel -= ROWS; if (br.sel < 0) br.sel = 0; nav = 1; break;
    case K_RIGHT:
    case K_PGDN: br.sel += ROWS; if (br.sel >= br.nent) br.sel = br.nent ? br.nent - 1 : 0; nav = 1; break;
    case K_HOME: br.sel = 0; nav = 1; break;
    case K_END: br.sel = br.nent ? br.nent - 1 : 0; nav = 1; break;
    case K_TAB: skin_next(); status(skin_name()); draw_view(); return;
    case K_SPACE: {
      int p = fmt_pause(-1);
      status(p ? "paused" : "playing");
      update_view();
      return;
    }
    case K_MINUS: case K_EQUAL: {
      int v = volume_setting + (e->key == K_EQUAL ? 4 : -4);
      if (v < 0) v = 0;
      if (v > 64) v = 64;
      volume_setting = v;
      fmt_set_volume(v);
      char b[20], *q = str_cpy(b, "volume ");
      u2s(q, (uint32_t) v, 0);
      status(b);
      update_view();
      return;
    }
    case K_F12: core_reset();
    case K_ENTER: case K_KPENTER: play_selected(); return;
    case K_ESC:
      if (playing) { fmt_stop(); playing = 0; playing_sd1 = 0; np_on = 0; status("stopped"); draw_view(); }
      return;
    case K_BKSP: {
      int n = browser_up(&br, keep, playing ? fmt_service : NULL);
      if (n < 0) status(n == -3 ? "SD2: too many files (RAM)" : "SD2: can't open the directory");
      else if (n > 0) update_list_view();
      return;
    }
    default: return;
  }
  if (br.sel < br.top) br.top = br.sel;
  if (br.sel >= br.top + ROWS) br.top = br.sel - ROWS + 1;
  if (nav) { nav_t = R_US; update_list_view(); } else draw_view();
}

void player_run(void) {
  spi_div(2);
  spi_cs(0xF);
  gfx_init();
  if (skin_current()->init) skin_current()->init();
  browser_init(&br);
  fs_mount();
  list();
  draw_view();
  spi_div(2);
  fmt_init();
  int raw = (int) (R_SW & 0xFF);
  if (raw < 16) { osd_raw = raw; volume_setting = osd_vol[raw]; }
  const char *ver = fmt_engine_version();
  if (ver && ver[0] && strcmp(ver, "0.0.0")) {
    char b[64], *q = str_cpy(b, fmt_engine_name());
    *q++ = ' ';
    q = str_cpy(q, ver);
    int ar = audio_rate_test();
    q = str_cpy(q, "  AUD ");
    if (ar > 0) u2s(q, (uint32_t) ar, 0); else str_cpy(q, "?");
    status(b);
  } else status("FPGA: not ready");
  draw_view();
#ifdef HOST
  // a demo of the now-playing panel for the host screenshot (never in the firmware)
  np_on = 1;
  str_cpy(np_fmt, "mod");
  str_cpy(np_title, "BrillSmurf");
  np_start = R_US - 84000000u;
  np_cur = 40; np_total = 100;
  draw_view();
  update_view();          // the live 200 ms refresh must keep the list
  return;
#endif
  uint32_t list_t = 0;
  for (;;) {
    // F12 is a recovery key. Check the raw keyboard report too, so a short press is not lost
    // while an audio engine is busy filling the FIFO.
    {
      uint32_t k0 = R_KBD0, k1 = R_KBD1;
      uint8_t keys[6] = { (uint8_t) (k0 >> 8), (uint8_t) (k0 >> 16), (uint8_t) (k0 >> 24),
                          (uint8_t) k1, (uint8_t) (k1 >> 8), (uint8_t) (k1 >> 16) };
      for (int i = 0; i < 6; i++) if (keys[i] == K_F12) core_reset();
    }
    fmt_service();
    if (playing && fmt_volume() != volume_setting) fmt_set_volume(volume_setting);
    raw = (int) (R_SW & 0xFF);
    if (raw != osd_raw && raw < 16) {
      osd_raw = raw;
      volume_setting = osd_vol[raw];
      if (playing) fmt_set_volume(volume_setting);
    }
    if ((uint32_t) (R_US - mix_t0) >= 1000000u) {
      mix_t0 = R_US;
      uint32_t wr = audio_written() - mix_w0;
      mix_w0 = audio_written();
      char b[64], *q = str_cpy(b, "MIX ");
      q = u2s(q, wr, 0);
      q = str_cpy(q, " U");
      q = u2s(q, audio_under(), 0);
      q = str_cpy(q, " R");
      q = str_cpy(q, audio_rate_debug() + 1);   // "asked/fpga", drop the leading 'R'
      str_ncpy(mix_msg, b, sizeof(mix_msg));
    }
    // While a list key is held the player panel refreshes slowly (5 Hz); after the key is
    // released it goes back to 30 Hz, or 20 Hz for a dense 24+ channel module. The spectrum
    // analyzer is the expensive part of the UI and the mixer must keep the audio FIFO fed.
    uint32_t panel_us = view_ch >= 24 ? 100000u : 33000u;
    if (np_on && (uint32_t) (R_US - np_us) >= panel_us &&
        ((uint32_t) (R_US - nav_t) >= 150000u || (uint32_t) (R_US - np_us) >= 200000u)) {
      np_us = R_US;
      uint32_t c, t;
      if (fmt_position(&c, &t)) { np_cur = c; np_total = t; }
      update_view();
    } else if (!np_on && br.nent && (uint32_t) (R_US - list_t) >= 50000u) {
      // Keep the long selected name moving while the player is idle.
      list_t = R_US;
      update_list_view();
    }
    if (playing && !fmt_active()) {
      int ended = fmt_finished();
      int stop_after_playlist = play_ctx == PLAY_CTX_PLAYLIST && !br.playlist;
      playing = 0;
      if (ended && !stop_after_playlist && next_track()) {
        // play_selected() started the next file and drew the new state
      } else {
        playing_sd1 = 0;
        np_on = 0;
        status(ended ? (play_ctx == PLAY_CTX_PLAYLIST ? "end of playlist" : "end of list") : "stopped");
        draw_view();
      }
    }
    key_ev_t e;
    if (key_next(&e)) key(&e);
  }
}
