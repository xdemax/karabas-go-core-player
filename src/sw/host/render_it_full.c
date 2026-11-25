// Full IT path on the host: it_fpga_start/service + a FIFO that drains at 48 kHz.
// This is the missing harness: changes in it_fpga.c (resampler, FIFO budget, render-ahead)
// can now be validated without flashing - underruns and the WAV show what the device hears.
//
//   cc -O2 -std=c99 -I. -o /tmp/itfull render_it_full.c it_fpga.c it/it_parser.c it/it_player.c \
//      fastmix.c resample.c analysis.c
//   /tmp/itfull song.it out.wav [seconds]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include "fpga.h"
#include "audio.h"
#include "mixer.h"
#include "it_fpga.h"
#include "it/it_parser.h"

/* ---- simulated FPGA ------------------------------------------------------- */
static uint32_t regs[64];
static uint32_t cycles_now, us_now;
uint32_t host_cycles(void) { return cycles_now; }

volatile uint32_t *host_reg(uint32_t off) { return &regs[(off >> 2) & 63]; }

#define REG_US     (0x40 >> 2)
#define REG_CYC    (0x0c >> 2)

/* FIFO */
static int16_t fifo[AUDIO_CAP * 2];
static int fifo_rd, fifo_wr, fifo_n;
static uint32_t stat_under, stat_drop;

int audio_free(void) { return AUDIO_CAP - 1 - fifo_n; }
uint16_t audio_under(void) { return (uint16_t) stat_under; }
uint16_t audio_drop(void) { return (uint16_t) stat_drop; }
void audio_reset_stats(void) { stat_under = stat_drop = 0; }
static uint32_t nwritten;
uint32_t audio_written(void) { return nwritten; }
int audio_rate(void) { return 48000; }
int audio_set_rate(int hz) { (void) hz; return 48000; }
int audio_rate_test(void) { return 48000; }
void audio_write(int16_t l, int16_t r) {
  if (fifo_n >= AUDIO_CAP - 1) { stat_drop++; return; }
  fifo[fifo_wr * 2] = l; fifo[fifo_wr * 2 + 1] = r;
  fifo_wr = (fifo_wr + 1) % AUDIO_CAP; fifo_n++; nwritten++;
}

void mixer_write(int16_t l, int16_t r) { audio_write(l, r); }
void mixer_write_noana(int16_t l, int16_t r) { audio_write(l, r); }
void mixer_write_it(int16_t l, int16_t r) { audio_write(l, r); }
void mixer_reset(void) { }

/* it_fpga_start reads the file through src_read only when src.f is set; we pass memory. */
int src_read(play_src_t *s, uint32_t off, void *buf, uint32_t n, unsigned *got) {
  (void) s; (void) off; (void) buf; (void) n;
  if (got) *got = 0;
  return -1;
}

/* ---- file loading --------------------------------------------------------- */
static uint8_t *load(const char *path, uint32_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror("open"); return NULL; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *d = malloc((size_t) n);
  if (!d || fread(d, 1, (size_t) n, f) != (size_t) n) { fclose(f); return NULL; }
  fclose(f);
  *size = (uint32_t) n;
  return d;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s song.it out.wav [seconds]\n", argv[0]); return 2; }
  /* it_fpga/it_load use the hardcoded device SDRAM base: map it in this process so the
   * full path (arenas, sample decoding) works unchanged. */
  { void *p = mmap((void *) 0x10000000u, 32u << 20, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap sdram"); return 1; } }

  uint32_t size = 0;
  uint8_t *buf = load(argv[1], &size);
  if (!buf) return 1;
  int secs = argc > 3 ? atoi(argv[3]) : 30;

  play_src_t src = { NULL, buf };
  if (it_fpga_start(&src, size, NULL) != 0) {
    fprintf(stderr, "it_fpga_start failed: %s\n", it_fpga_error());
    return 1;
  }
  /* give the engine a head start like the real FIFO does */
  for (int i = 0; i < 512; i++) it_fpga_service();

  FILE *out = fopen(argv[2], "wb");
  if (!out) { perror("wav"); return 1; }
  { char h[44] = {0};
    memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVE", 4); memcpy(h + 12, "fmt ", 4);
    uint32_t sz = 16; memcpy(h + 16, &sz, 4);
    uint16_t pcm = 1, chn = 2; uint32_t sr = 48000; uint16_t bps = 16;
    uint16_t align = 4; uint32_t brate = sr * align;
    memcpy(h + 20, &pcm, 2); memcpy(h + 22, &chn, 2); memcpy(h + 24, &sr, 4);
    memcpy(h + 28, &brate, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bps, 2);
    memcpy(h + 36, "data", 4);
    fwrite(h, 1, 44, out); }

  long total = (long) secs * 48000;
  long clipped = 0;
  for (long t = 0; t < total; t++) {
    int16_t l = 0, r = 0;
    if (fifo_n > 0) {
      l = fifo[fifo_rd * 2]; r = fifo[fifo_rd * 2 + 1];
      fifo_rd = (fifo_rd + 1) % AUDIO_CAP; fifo_n--;
    } else {
      stat_under++;
    }
    if (l >= 32767 || l <= -32768 || r >= 32767 || r <= -32768) clipped++;
    fwrite(&l, 2, 1, out); fwrite(&r, 2, 1, out);
    us_now += 21;                       /* ~20.833 us per 48 kHz frame */
    cycles_now += 1563;                 /* 75 MHz */
    regs[REG_US] = us_now;
    regs[REG_CYC] = cycles_now;
    it_fpga_service();
    if (it_fpga_finished()) { /* keep draining silence to the end */ }
  }
  long data = total * 4;
  fseek(out, 4, SEEK_SET); uint32_t riff = (uint32_t) (36 + data); fwrite(&riff, 4, 1, out);
  fseek(out, 40, SEEK_SET); uint32_t ds = (uint32_t) data; fwrite(&ds, 4, 1, out);
  fclose(out);
  fprintf(stderr, "frames=%ld under=%u drop=%u clipped=%ld written=%u\n",
          total, stat_under, stat_drop, clipped, nwritten);
  return 0;
}
