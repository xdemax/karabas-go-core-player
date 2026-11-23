// The host test of the FPGA XM player: a synthetic 2-channel module, the parser, the delta sample
// decoder, the mixer, the loop, pause/volume, and the error path.
//   host/run_xm_test.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../fpga.h"
#include "../audio.h"

// the host build has no MMIO; the big-file SDRAM path is not used with src.mem
static uint32_t hreg;
volatile uint32_t *host_reg(uint32_t off) { (void) off; hreg = 1; return &hreg; }

static int errors;
#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); errors++; } while (0)

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24); }

static int16_t out[4096];
static int outn;
static int paused_now;

int audio_free(void) { return paused_now ? 0 : 256; }
void audio_write(int16_t l, int16_t r) {
  if (outn < (int) (sizeof(out) / sizeof(out[0])) / 2) { out[outn * 2] = l; out[outn * 2 + 1] = r; }
  outn++;
}
int src_read(play_src_t *s, uint32_t off, void *buf, uint32_t n, UINT *got) {
  (void) s; (void) off; (void) buf; (void) n;
  if (got) *got = 0;
  return -1;
}

static uint32_t build_xm(uint8_t *b) {
  uint32_t p = 0;
  memcpy(b + p, "Extended Module: ", 17); p += 17;
  memcpy(b + p, "xtest", 5); memset(b + p + 5, 0, 15); p += 20;
  b[p++] = 0x1A;
  memset(b + p, ' ', 20); p += 20;
  w16(b + p, 0x0104); p += 2;
  w32(b + p, 276); p += 4;
  w16(b + p, 2); p += 2;              // song length
  w16(b + p, 0); p += 2;              // restart
  w16(b + p, 2); p += 2;              // channels
  w16(b + p, 1); p += 2;              // patterns
  w16(b + p, 1); p += 2;              // instruments
  w16(b + p, 1); p += 2;              // linear
  w16(b + p, 6); p += 2;              // speed
  w16(b + p, 125); p += 2;            // bpm
  memset(b + p, 0, 256); p += 256;

  // one pattern, one row, two cells (C-4 and G-4)
  w32(b + p, 9); p += 4;
  b[p++] = 0;
  w16(b + p, 1); p += 2;
  w16(b + p, 10); p += 2;
  b[p++] = 49; b[p++] = 1; b[p++] = 0; b[p++] = 0; b[p++] = 0;
  b[p++] = 56; b[p++] = 1; b[p++] = 0; b[p++] = 0; b[p++] = 0;

  // sample: a 64-byte square wave (32 positive, 32 negative), delta encoded, forward loop 0..32
  uint8_t sample[64];
  for (int i = 0; i < 64; i++) sample[i] = (i < 32) ? 0 : (uint8_t) -200;
  sample[0] = 100;

  // instrument header 263 bytes
  uint32_t ins = p;
  memset(b + p, 0, 263); p += 263;
  w32(b + ins, 263);
  memcpy(b + ins + 4, "xtest inst", 10);
  w16(b + ins + 27, 1);
  w32(b + ins + 29, 40);
  // no volume envelope: nvol = 0

  uint32_t sh = p;
  memset(b + p, 0, 40); p += 40;
  w32(b + sh, 64);                    // length in bytes (8-bit)
  w32(b + sh + 4, 0);                 // loop start
  w32(b + sh + 8, 32);                // loop length
  b[sh + 12] = 64;                    // volume
  b[sh + 13] = 0;                     // finetune
  b[sh + 14] = 1;                     // 8-bit, forward loop
  b[sh + 15] = 128;                   // pan
  b[sh + 16] = 0;                     // relative note

  memcpy(b + p, sample, 64); p += 64;
  return p;
}

int main(void) {
  static uint8_t buf[2048];
  uint32_t n = build_xm(buf);
  play_src_t s = { NULL, buf };

  if (fpga_xm_start(&s, n, NULL) != 0) { FAIL("start: %s", fpga_xm_error()); return 1; }
  if (!fpga_xm_active()) FAIL("not active");
  if (strcmp(fpga_xm_title(), "xtest")) FAIL("title '%s'", fpga_xm_title());

  fmt_info_t info;
  memset(&info, 0, sizeof info);
  fpga_xm_info(&info);
  if (info.channels != 2) FAIL("channels %d", info.channels);
  if (info.samples != 1) FAIL("samples %d", info.samples);
  if (info.speed != 6 || info.bpm != 125) FAIL("speed/bpm %d/%d", info.speed, info.bpm);

  uint32_t cur = 0, total = 0;
  if (!fpga_xm_position(&cur, &total) || total != 128) FAIL("position");

  if (fpga_xm_set_volume(32) != 32 || fpga_xm_get_volume() != 32) FAIL("volume");
  fpga_xm_set_volume(64);

  // pause must stop the mixing
  fpga_xm_pause(1);
  int before = outn;
  fpga_xm_service();
  if (outn != before) FAIL("pause wrote %d samples", outn - before);
  fpga_xm_pause(0);

  for (int i = 0; i < 8; i++) fpga_xm_service();
  if (outn < 1000) FAIL("only %d samples", outn);
  int nz = 0, peak = 0;
  for (int i = 0; i < outn && i < 2048; i++) {
    int a = out[i * 2] < 0 ? -out[i * 2] : out[i * 2];
    if (a) nz++;
    if (a > peak) peak = a;
  }
  if (nz < 900) FAIL("only %d nonzero samples", nz);
  if (peak < 1000) FAIL("peak %d", peak);
  // the loop must still produce sound after the first 32 samples
  int late = 0;
  for (int i = 500; i < 1000 && i < outn; i++) if (out[i * 2]) late++;
  if (late < 400) FAIL("the loop is silent (%d/500)", late);

  int guard = 0;
  while (!fpga_xm_finished() && guard++ < 10000) fpga_xm_service();
  if (!fpga_xm_finished()) FAIL("the song never reached its end");

  fpga_xm_stop();
  if (fpga_xm_active()) FAIL("still active after stop");

  uint8_t bad[40];
  memset(bad, 0, sizeof bad);
  play_src_t bs = { NULL, bad };
  if (fpga_xm_start(&bs, sizeof bad, NULL) == 0) FAIL("the bad module was accepted");
  else if (!fpga_xm_error()[0]) FAIL("no error text");

  printf("xm_test: %d samples, peak %d, %d errors\n", outn, peak, errors);
  return errors ? 1 : 0;
}
