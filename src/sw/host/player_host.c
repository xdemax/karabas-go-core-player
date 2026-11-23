// The host build of the player screen (no CPU, no cards, no ESP): a fake list and the frame as
// menu_compose makes it -> host/out/frame.ppm / .png, for a look at the layout.
//   host/run_player_host.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../mmio.h"
#include "../ui.h"
#include "../fs.h"
#include "../fmt.h"
#include "../src.h"
#include "../sd1.h"
#include "../player.h"

uint8_t host_fb[2][0x80000];
static uint32_t regs[0x1000];
static uint32_t now_ms = 1000;
static int fb_shown = 0;

volatile uint32_t *host_reg(uint32_t off) {
  static uint32_t tmp;
  if (off == 0x1c) { tmp = now_ms++; return &tmp; }
  if (off == 0x40) { tmp = now_ms * 1000; return &tmp; }
  if (off == 0x50) { tmp = 0; return &tmp; }        // SPI: never busy
  return &regs[off / 4];
}


// ---- the fake list ----

static const struct { const char *name; uint32_t size; int dir; } demo[] = {
  { "mods", 0, 1 }, { "MEGAMIX.MOD", 143 * 1024, 0 }, { "second_reality.xm", 312 * 1024, 0 },
  { "tracker tune.s3m", 87 * 1024, 0 }, { "A_VERY_LONG_TRACKER_NAME_TEST.XM", 1024 * 1024, 0 },
  { "old", 0, 1 }, { "demo.mod", 64 * 1024, 0 },
};

int fs_mount(void) { return 0; }
int fs_list(const char *path, fs_entry_t *out, int max, fs_keep_t keep) {
  (void) path;
  int n = 0;
  for (unsigned i = 0; i < sizeof(demo) / sizeof(demo[0]) && n < max; i++) {
    if (keep && !keep(demo[i].name, demo[i].dir)) continue;
    size_t len = strlen(demo[i].name) + 1;
    out[n].name = malloc(len);
    if (!out[n].name) return -3;
    memcpy(out[n].name, demo[i].name, len);
    out[n].path = NULL;
    out[n].size = demo[i].size;
    out[n].dir = (uint8_t) demo[i].dir;
    n++;
  }
  return n;
}
int fs_list_alloc(const char *path, fs_entry_t **out, fs_keep_t keep, fs_idle_t idle) {
  (void) idle;
  fs_entry_t *a = malloc(64 * sizeof(fs_entry_t));
  if (!a) return -3;
  int n = fs_list(path, a, 64, keep);
  if (n < 0) { free(a); return n; }
  *out = a;
  return n;
}
void fs_free_names(fs_entry_t *list, int n) { for (int i = 0; i < n; i++) { free(list[i].name); list[i].name = NULL; free(list[i].path); list[i].path = NULL; } }
void fs_free_list(fs_entry_t *list, int n) { if (!list) return; fs_free_names(list, n); free(list); }
int fs_open(const char *path, FIL *f) { (void) path; (void) f; return -1; }
int playlist_supported(const char *name) { (void) name; return 0; }
int playlist_load(const char *path, fs_entry_t **out) { (void) path; if (out) *out = NULL; return -2; }
int fs_dir_exists(const char *path) { (void) path; return 1; }
void fs_path_up(char *path) { int n = (int) strlen(path); while (n > 1 && path[n - 1] != '/') n--; if (n > 1) n--; path[n] = 0; if (!path[0]) { path[0] = '/'; path[1] = 0; } }

// ---- the format registry (a fake module) and the SD1 loader ----

static int fake_play(const play_src_t *s, uint32_t n, play_progress_t p) { (void) s; (void) n; (void) p; return 0; }
static int fake_detect(const uint8_t *h, uint32_t n) { (void) h; (void) n; return 0; }
static const char *fake_error(void) { return "demo"; }
static const fmt_module_t fake = { "demo", "mod", fake_detect, fake_play, fake_error };

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
const fmt_module_t *fmt_by_ext(const char *name) {
  if (!name) return NULL;
  const char *d = strrchr(name, '.');
  if (!d) return NULL;
  if (!strcasecmp(d, ".mod") || !strcasecmp(d, ".xm") || !strcasecmp(d, ".s3m")) return &fake;
  return NULL;
}
const fmt_module_t *fmt_by_head(const uint8_t *h, uint32_t n) { (void) h; (void) n; return NULL; }
int fmt_supported(const char *name) { return fmt_by_ext(name) != NULL; }
int fmt_supported_list(char *out, int max) {
  const char *s = "mod, xm, pt3, s3m";
  int n = 0;
  while (s[n] && n < max - 1) { out[n] = s[n]; n++; }
  if (max > 0) out[n] = 0;
  return n;
}
int audio_rate_test(void) { return 48000; }
int audio_rate(void) { return 48000; }
uint32_t audio_written(void) { return 0; }
uint16_t audio_drop(void) { return 0; }
uint16_t audio_under(void) { return 0; }
void audio_reset_stats(void) {}
void fmt_init(void) {}
void fmt_service(void) {}
int fmt_active(void) { return 0; }
void fmt_stop(void) {}
const char *fmt_error(void) { return "demo"; }
const char *fmt_title(void) { return "Demo module"; }
int fmt_position(uint32_t *cur, uint32_t *total) { *cur = 7; *total = 20; return 1; }
void fmt_info(fmt_info_t *o) {
  memset(o, 0, sizeof(*o));
  o->channels = 4; o->samples = 12; o->order = 3; o->pattern = 3; o->row = 12;
  o->song_len = 8; o->speed = 4; o->bpm = 125; o->size = 143 * 1024;
}
void fmt_set_analysis(int on) { (void) on; }
int settings_load(char *dir, int dir_cap, char *file, int file_cap, int *volume) {
  if (dir_cap > 0) { dir[0] = '/'; dir[1] = 0; }
  if (file_cap > 0) file[0] = 0;
  *volume = 64;
  return 0;
}
int settings_save(const char *dir, const char *file, int volume) { (void) dir; (void) file; (void) volume; return 0; }
void fmt_audio(fmt_audio_t *o) {
  memset(o, 0, sizeof(*o));
  o->level_l = 12000; o->level_r = 9000; o->peak_l = 26000; o->peak_r = 19000;
  o->volume = 60;
  for (int i = 0; i < FMT_BANDS; i++) o->bands[i] = (uint8_t) (30 + (i * 53) % 200);
  for (int i = 0; i < 4; i++) o->vu[i] = (uint8_t) (220 - i * 40);
}
int fmt_volume(void) { return 60; }
int fmt_set_volume(int v) { return v; }
int fmt_pause(int on) { (void) on; return 0; }
const char *fmt_engine_name(void) { return "FPGA"; }
const char *fmt_engine_version(void) { return "1.0"; }

int sd1_ready(void) { return 0; }
const uint8_t *sd1_data(void) { return NULL; }
uint32_t sd1_size(void) { return 0; }
uint32_t sd1_ext(void) { return 0; }
void sd1_ack(void) {}

void spi_div(uint16_t d) { (void) d; }
void spi_cs(uint8_t m) { (void) m; }
uint8_t spi_xfer(uint8_t b) { (void) b; return 0; }
void spi_send(const uint8_t *p, int n) { (void) p; (void) n; }
void spi_recv(uint8_t *p, int n) { (void) p; (void) n; }
void spi_fill(uint8_t b, int n) { (void) b; (void) n; }
int spi_busy(void) { return 0; }

int rpc_enabled(void) { return 1; }
int rpc_call(uint8_t svc, uint8_t fn, const void *in, uint16_t in_len, void *out, uint16_t out_max, uint16_t *out_len, uint32_t timeout_ms) {
  (void) svc; (void) fn; (void) in; (void) in_len; (void) out; (void) out_max; (void) timeout_ms;
  if (out_len) *out_len = 0;
  return 3;
}
void rpc_launch(const char *file) { (void) file; }

// ---- the picture ----

static void dump(const char *name) {
  int b = fb_shown;
  uint32_t top = regs[0x18 / 4], bot = regs[0x20 / 4];
  char path[256];
  snprintf(path, sizeof(path), "host/out/%s.ppm", name);
  FILE *o = fopen(path, "wb");
  if (!o) { perror(path); exit(1); }
  fprintf(o, "P6 800 600 255\n");
  for (int y = 0; y < 600; y++) {
    int t = y >= 450 ? 256 : y * 256 / 450;
    for (int x = 0; x < 800; x++) {
      uint8_t idx = host_fb[b][y * 800 + x];
      uint32_t e = regs[(0x400 + 0x400 * b) / 4 + idx];
      if (idx == 0) e = 0;
      int a = e >> 28;
      if (a == 15) a = 16;
      for (int s = 16; s >= 0; s -= 8) {
        int g = (int) ((top >> s) & 255) + (((int) ((bot >> s) & 255) - (int) ((top >> s) & 255)) * t >> 8);
        int c = (e >> s) & 255;
        fputc((c * a + g * (16 - a)) / 16, o);
      }
    }
  }
  fclose(o);
  printf("frame %s (buffer %d)\n", name, b);
}

int main(void) {
  player_run();
  fb_shown = regs[0x04 / 4] & 1;
  dump("frame");
  return 0;
}
