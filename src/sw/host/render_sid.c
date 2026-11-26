// Render a SID with sid_fpga.c to a 48 kHz stereo WAV (host diagnostic).
//   cc -O2 -DHOST -I. -Ifatfs -o /tmp/sidrend host/render_sid.c sid_fpga.c sid_cpu.c mixer.c analysis.c
//   /tmp/sidrend song.sid out.wav [seconds]
//
// There is no SID in the host build, so this bench stands in for the FPGA tap: voice 1 of
// the register writes drives a square wave and the samples go into the same 1024-entry ring
// the RTL fills. The audio is therefore only a rough check of the notes and the rhythm; the
// exact 8580 sound is verified in the Verilator bench.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "../fpga.h"
#include "../audio.h"
#include "../sid_fpga.h"

#define TAP_MASK 1023
static uint32_t tap_ring[TAP_MASK + 1];
static uint32_t tap_wr, tap_rd;
static uint16_t v1_freq;
static uint8_t v1_ctrl;
static uint32_t v1_phase;
static uint32_t last_sid_wr;

static volatile uint32_t host_regs[64];
static uint32_t sid_reg_count;
static uint32_t written;

static uint32_t gen_sample(void) {
  uint32_t wr = host_regs[0xc8 >> 2];
  if (wr != last_sid_wr) {
    last_sid_wr = wr;
    uint32_t reg = (wr >> 8) & 0x1f;
    uint8_t val = (uint8_t) wr;
    if (reg == 0) v1_freq = (uint16_t) ((v1_freq & 0xff00) | val);
    else if (reg == 1) v1_freq = (uint16_t) ((v1_freq & 0x00ff) | (val << 8));
    else if (reg == 4) v1_ctrl = val;
  }
  int16_t s = 0;
  if (v1_ctrl & 1) {
    v1_phase += (uint32_t) (((uint64_t) v1_freq * 0x100000000ull) / 48000ull);
    s = (v1_phase & 0x80000000u) ? 6000 : -6000;
  }
  return (uint32_t) (uint16_t) s << 16 | (uint16_t) s;
}

static void tap_push(void) {
  if (tap_wr - tap_rd > TAP_MASK) return;       /* overrun: the oldest samples stay */
  tap_ring[tap_wr & TAP_MASK] = gen_sample();
  tap_wr++;
}

volatile uint32_t *host_reg(uint32_t off) {
  off &= 0xfc;
  if (off == 0x40) {          /* R_US: the virtual clock of the bench */
    static uint32_t us;
    us = (uint32_t) (((uint64_t) written * 1000000u) / 48000u);
    return &us;
  }
  if (off == 0xc8) sid_reg_count++;
  if (off == 0xdc) host_regs[0xdc >> 2] = tap_wr - tap_rd;
  if (off == 0xd8 && tap_wr != tap_rd) {
    host_regs[0xd8 >> 2] = tap_ring[tap_rd & TAP_MASK];
    tap_rd++;
  }
  return &host_regs[(off >> 2) & 63];
}

static FILE *out, *regs;
static uint32_t written;
static uint32_t total;

void sid_host_reg_write(uint32_t reg, uint32_t val) {
  if (!regs) return;
  uint32_t rec[2] = { written, (reg & 0x1f) | ((val & 0xff) << 8) };
  fwrite(rec, 4, 2, regs);
}

int audio_free(void) { return 2047; }

void audio_write(int16_t l, int16_t r) {
  if (written < total) {
    int16_t b[2] = { l, r };
    fwrite(b, 2, 2, out);
    written++;
  }
  tap_push();                    /* one new tap sample per emitted sample */
}

int src_read(play_src_t *s, uint32_t off, void *buf, uint32_t n, UINT *got) {
  (void) s; (void) off; (void) buf; (void) n; if (got) *got = 0; return -1;
}

static void wr32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void wr16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s song.sid out.wav [seconds]\n", argv[0]); return 2; }
  int secs = argc > 3 ? atoi(argv[3]) : 10;
  if (secs < 1) secs = 1;
  total = (uint32_t) secs * 48000u;

  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc((size_t) n);
  if (!buf || fread(buf, 1, (size_t) n, f) != (size_t) n) { perror("read"); return 1; }
  fclose(f);

  out = fopen(argv[2], "wb");
  { char rp[512]; snprintf(rp, sizeof rp, "%s.regs", argv[2]); regs = fopen(rp, "wb"); }
  if (!out) { perror(argv[2]); return 1; }
  uint8_t h[44];
  memset(h, 0, sizeof h);
  memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); wr32(h + 16, 16); wr16(h + 20, 1); wr16(h + 22, 2);
  wr32(h + 24, 48000); wr32(h + 28, 48000 * 4); wr16(h + 32, 4); wr16(h + 34, 16);
  memcpy(h + 36, "data", 4);
  fwrite(h, 1, 44, out);

  play_src_t src = { NULL, buf };
  if (sid_fpga_start(&src, (uint32_t) n, NULL) != 0) {
    fprintf(stderr, "sid: %s\n", sid_fpga_error());
    return 1;
  }
  for (int i = 0; i < 256; i++) tap_push();      /* prime the ring */

  sid_reg_count = 0;
  while (written < total && sid_fpga_active()) {
    uint32_t before = written;
    sid_fpga_service();
    if (written == before) break;
  }

  long data = (long) written * 4;
  fseek(out, 4, SEEK_SET);  uint32_t riff = (uint32_t) (36 + data); fwrite(&riff, 4, 1, out);
  fseek(out, 40, SEEK_SET); uint32_t ds = (uint32_t) data; fwrite(&ds, 4, 1, out);
  fclose(out);
  if (regs) fclose(regs);

  fprintf(stderr, "rendered %u samples, '%s'\n", written, sid_fpga_title());
  fprintf(stderr, "SID register writes: %u\n", sid_reg_count);

  return 0;
}
