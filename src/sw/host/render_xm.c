// Render an XM with the same xm_fpga.c mixer to a 48 kHz stereo WAV (host diagnostic).
//   cc -O2 -DHOST -I. -Ifatfs -o host/render_xm host/render_xm.c xm_fpga.c
//   ./host/render_xm song.xm out.wav [seconds]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../fpga.h"
#include "../audio.h"

// the host build has no MMIO; the big-file SDRAM path is not used with src.mem
static uint32_t hreg;
volatile uint32_t *host_reg(uint32_t off) { (void) off; hreg = 1; return &hreg; }

static FILE *out;
static uint32_t written;
static uint32_t total;
#ifdef XM_PROFILE
extern uint64_t xm_prof_chan_samples;
extern uint32_t xm_prof_peak_chan, xm_prof_blocks;
extern uint64_t xm_prof_sec_chan_samples[];
extern uint32_t xm_prof_sec_samples[], xm_prof_sec_peak[];
#endif

int audio_free(void) { return 2047; }
void audio_write(int16_t l, int16_t r) {
  if (written >= total) return;
  int16_t b[2] = { l, r };
  fwrite(b, 2, 2, out);
  written++;
}
int src_read(play_src_t *s, uint32_t off, void *buf, uint32_t n, UINT *got) {
  (void) s; (void) off; (void) buf; (void) n; if (got) *got = 0; return -1;
}

static void wr32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void wr16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s song.xm out.wav [seconds]\n", argv[0]); return 2; }
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
  if (!out) { perror(argv[2]); return 1; }
  uint8_t h[44];
  memset(h, 0, sizeof h);
  memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); wr32(h + 16, 16); wr16(h + 20, 1); wr16(h + 22, 2);
  wr32(h + 24, 48000); wr32(h + 28, 48000 * 4); wr16(h + 32, 4); wr16(h + 34, 16);
  memcpy(h + 36, "data", 4);
  fwrite(h, 1, 44, out);

  play_src_t src = { NULL, buf };
  if (fpga_xm_start(&src, (uint32_t) n, NULL) != 0) {
    fprintf(stderr, "xm: %s\n", fpga_xm_error());
    return 1;
  }
  while (written < total) {
    uint32_t before = written;
    fpga_xm_service();
    if (written == before) break;   // no progress (should not happen)
  }

  long data = (long) written * 4;
  fseek(out, 4, SEEK_SET);  uint32_t riff = (uint32_t) (36 + data); fwrite(&riff, 4, 1, out);
  fseek(out, 40, SEEK_SET); uint32_t ds = (uint32_t) data; fwrite(&ds, 4, 1, out);
  fclose(out);
  fprintf(stderr, "rendered %u samples, '%s'\n", written, fpga_xm_title());
#ifdef XM_PROFILE
  if (written) {
    fprintf(stderr, "mix: avg %.2f active channels, peak %u, %.2f channel-samples/output-sample\n",
            (double) xm_prof_chan_samples / written, xm_prof_peak_chan,
            (double) xm_prof_chan_samples / written);
    for (uint32_t s = 0; s < (written + 47999u) / 48000u; s++) {
      if (!xm_prof_sec_samples[s]) break;
      fprintf(stderr, "sec %2u: avg %5.2f peak %2u\n", s,
              (double) xm_prof_sec_chan_samples[s] / xm_prof_sec_samples[s],
              xm_prof_sec_peak[s]);
    }
  }
#endif
  return 0;
}
