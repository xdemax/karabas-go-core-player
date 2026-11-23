/*
 * bench_fastmix.c
 *
 * Host: build with -O3; prints ns per voice-frame (relative numbers only,
 * a PC is not a VexRiscv).
 * Target: build with -DFM_BENCH_TARGET and define BENCH_CYCLES() to read the
 * project's cycle counter (R_CYCLES on Karabas Go); prints cycles per block,
 * cycles per voice-frame, and Mcycles/s at 48 kHz (= % of 75 MHz * 0.75...).
 *   budget (TZ): 4v < 10, 16v < 30, 32v < 55 Mcycles/s.
 */
#include "../fastmix.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef FM_BENCH_TARGET
  /* e.g. #define BENCH_CYCLES() R_CYCLES  -- provided by the firmware */
  #define BENCH_UNIT "cycles"
  #define BENCH_PER_SEC 75000000.0
#else
  #include <time.h>
  static unsigned long long BENCH_CYCLES(void)
  {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
  }
  #define BENCH_UNIT "ns"
  #define BENCH_PER_SEC 1e9
#endif

#define BLOCK 256
#define ITER  2000

static int16_t s16[8192];
static int8_t  s8[8192];

static void setup(fm_voice_t *v, int n, int is8, int pp)
{
  int i;
  for (i = 0; i < n; i++) {
    fm_voice_init(&v[i]);
    v[i].data = is8 ? (const void *)s8 : (const void *)s16;
    v[i].length = 8192;
    v[i].loop_start = 100; v[i].loop_end = 8000;
    v[i].flags = (uint8_t)(FM_ACTIVE | FM_LOOP | (is8 ? FM_8BIT : 0) | (pp ? FM_PINGPONG : 0));
    v[i].step = 20000 + (uint32_t)i * 1500;         /* 0.3 .. ~1.5 */
    v[i].pos = 200;
    fm_voice_set_gain(&v[i], 128 + (i * 5) % 128, (i * 37) % 256 - 128, 0);
  }
}

int main(void)
{
  static const int counts[] = { 4, 16, 32, 64 };
  int16_t out[BLOCK * 2];
  fm_voice_t v[FM_MAX_VOICES];
  int c, i, mode;
  for (i = 0; i < 8192; i++) { s16[i] = (int16_t)((i * 7919) & 0x7FFF) - 16384; s8[i] = (int8_t)(s16[i] >> 8); }

  printf("%-18s %5s %14s %14s %14s\n", "case", "vox", "unit/block", "unit/vframe", "M/s @48k");
  for (mode = 0; mode < 3; mode++) {
    const char *name = mode == 0 ? "16bit linear fwd" : mode == 1 ? "8bit linear fwd" : "16bit pingpong";
    for (c = 0; c < 4; c++) {
      int n = counts[c];
      unsigned long long t0, t1;
      fm_reset(); fm_set_max_mixed(64);
      setup(v, n, mode == 1, mode == 2);
      fm_mix(out, BLOCK, v, n);                       /* warm up */
      t0 = BENCH_CYCLES();
      for (i = 0; i < ITER; i++) fm_mix(out, BLOCK, v, n);
      t1 = BENCH_CYCLES();
      {
        double per_block = (double)(t1 - t0) / ITER;
        double per_vf = per_block / (BLOCK * n);
        double mps = per_block / BLOCK * 48000.0 / 1e6;
        printf("%-18s %5d %14.0f %14.2f %14.2f\n", name, n, per_block, per_vf, mps);
      }
    }
  }
  printf("(unit = %s; M/s column is meaningful for cycles on target only)\n", BENCH_UNIT);
  return 0;
}
