// A host test of the FPGA MOD player (src/sw/mod_fpga.c): a tiny 4-channel module built in memory,
// the parser, the mixer, the 48 kHz output. The audio FIFO is a stub (audio_free / audio_write).
//   host/run_mod_test.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../fpga.h"
#include "../audio.h"

// the host build has no MMIO; the big-file SDRAM path is not used with src.mem
static uint32_t hreg;
volatile uint32_t *host_reg(uint32_t off) { (void) off; hreg = 1; return &hreg; }

static int failures;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } while (0)

// ---- the FatFs calls src.c links (the test uses the memory source) ----
FRESULT f_lseek(FIL *f, FSIZE_t off) { (void) f; (void) off; return FR_OK; }
FRESULT f_read(FIL *f, void *buf, UINT n, UINT *got) { (void) f; (void) buf; (void) n; if (got) *got = 0; return FR_OK; }

// ---- the audio FIFO stub ----
static int free_slots = 256;
static int written;
static int32_t peak_l, peak_r;
int audio_free(void) { return free_slots; }
void audio_write(int16_t l, int16_t r) {
  if (l > peak_l) peak_l = l;
  if (l < -peak_l) peak_l = -l;
  if (r > peak_r) peak_r = r;
  if (r < -peak_r) peak_r = -r;
  written++;
}

// ---- a tiny MOD ----
static uint8_t mod[4096];
static uint32_t mod_size;

static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }

static void build_mod(const char *sig, int chn) {
  memset(mod, 0, sizeof(mod));
  memcpy(mod, "TEST MOD", 8);
  uint8_t *s0 = mod + 20;                 // sample 1
  memcpy(s0, "square", 6);
  wr16(s0 + 22, 128);                     // length, in words (256 bytes)
  s0[24] = 0;                             // finetune
  s0[25] = 64;                            // volume
  wr16(s0 + 26, 0);                       // loop start, in words
  wr16(s0 + 28, 128);                     // loop length, in words (256 bytes)
  mod[950] = 1;                           // song length
  mod[951] = 0;                           // restart
  mod[952] = 0;                           // order 0
  memcpy(mod + 1080, sig, 4);
  uint8_t *pat = mod + 1084;
  for (int r = 0; r < 64; r++)
    for (int c = 0; c < chn; c++) {
      uint8_t *p = pat + (r * chn + c) * 4;
      p[0] = 0x01;                        // sample 1, period high bits 0
      p[1] = (r == 0) ? 0xAC : 0xAC;      // period 428 (C-2): 0x1AC -> b0 low 0x1, b1 0xAC
      p[1] = 0xAC;
      p[2] = 0x10;                        // sample low 0, effect 0
      p[3] = 0x00;
    }
  uint32_t off = 1084 + 64 * chn * 4;
  for (uint32_t i = 0; i < 256; i++) mod[off + i] = (i & 32) ? 0x40 : 0xC0;
  mod_size = off + 256;
}

int main(void) {
  build_mod("M.K.", 4);
  // the period 428 = 0x1AC: b0 low nibble = 1, b1 = 0xAC
  build_mod("M.K.", 4);
  play_src_t src = { NULL, mod };
  int rc = fpga_mod_start(&src, mod_size, NULL);
  if (rc != 0) FAIL("start: %s", fpga_mod_error());
  if (!fpga_mod_active()) FAIL("not active");
  if (strcmp(fpga_mod_title(), "TEST MOD")) FAIL("title '%s'", fpga_mod_title());

  for (int i = 0; i < 20; i++) {
    free_slots = 256;
    fpga_mod_service();
  }
  if (written == 0) FAIL("no samples");
  if (peak_l == 0 && peak_r == 0) FAIL("the mix is silent");
  if (!fpga_mod_active()) FAIL("stopped too early");

  fpga_mod_stop();
  if (fpga_mod_active()) FAIL("still active after stop");

  // a wrong signature
  uint8_t bad[1200];
  memset(bad, 0, sizeof(bad));
  memcpy(bad + 1080, "XXXX", 4);
  play_src_t bs = { NULL, bad };
  if (fpga_mod_start(&bs, sizeof(bad), NULL) == 0) FAIL("accepted a bad signature");
  if (!fpga_mod_error()[0]) FAIL("no error text");

  printf("mod_test: %d samples, peak %d / %d\n", written, (int) peak_l, (int) peak_r);
  if (failures) { printf("mod_test: %d errors\n", failures); return 1; }
  printf("mod_test: 0 errors\n");
  return 0;
}
