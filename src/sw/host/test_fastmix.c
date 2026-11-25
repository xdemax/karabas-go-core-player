/* Host tests for fastmix. Float is used ONLY here, as a reference. */
#include "../fastmix.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
  printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define GUARD 16
#define SENT 30000

/* sample buffer with sentinel guard frames before/after; reading them
 * (beyond the declared length) would be caught by the bound checks */
static int16_t *mk16(const int16_t *src, int len)
{
  int16_t *b = malloc((len + 2 * GUARD) * sizeof(int16_t));
  int i;
  for (i = 0; i < len + 2 * GUARD; i++) b[i] = SENT;
  for (i = 0; i < len; i++) b[GUARD + i] = src[i];
  return b + GUARD;
}

static void vinit(fm_voice_t *v, const void *d, uint32_t len, uint8_t fl,
                  double step, int vol, int pan)
{
  fm_voice_init(v);
  v->data = d; v->length = len; v->flags = (uint8_t)(fl | FM_ACTIVE);
  v->step = (uint32_t)(step * 65536.0 + 0.5);
  fm_voice_set_gain(v, vol, pan, 0);
}

/* 1. sine 1 kHz, 16 and 8 bit, step 1:1, forward loop */
static void test_sine(void)
{
  int16_t s16[48]; int8_t s8[48];
  int16_t out[480 * 2];
  fm_voice_t v;
  int i, zc;
  for (i = 0; i < 48; i++) {
    double x = sin(2 * M_PI * i / 48.0);
    s16[i] = (int16_t)lrint(x * 30000);
    s8[i] = (int8_t)lrint(x * 120);
  }
  vinit(&v, s16, 48, FM_LOOP, 1.0, 256, -128);
  v.loop_start = 0; v.loop_end = 48;
  fm_mix(out, 480, &v, 1);
  zc = 0;
  for (i = 0; i < 480; i++) {
    CHECK(out[2 * i] == s16[i % 48], "sine16 i=%d %d!=%d", i, out[2 * i], s16[i % 48]);
    CHECK(out[2 * i + 1] == 0, "sine16 right not silent");
    if (i && out[2 * i - 2] < 0 && out[2 * i] >= 0) zc++;
  }
  CHECK(zc == 9 || zc == 10, "sine 1kHz crossings=%d", zc);
  vinit(&v, s8, 48, FM_LOOP | FM_8BIT, 1.0, 256, -128);
  v.loop_start = 0; v.loop_end = 48;
  fm_mix(out, 480, &v, 1);
  for (i = 0; i < 480; i++)
    CHECK(out[2 * i] == s8[i % 48] * 256, "sine8 i=%d", i);
}

/* 2. linear interpolation vs float reference (non-looped) */
static void test_interp(void)
{
  static const double steps[] = { 0.37, 1.0, 1.91, 3.3, 0.05, 7.77 };
  int16_t src[4000], *buf;
  int t, i;
  srand(1);
  for (i = 0; i < 4000; i++) src[i] = (int16_t)((rand() % 65536) - 32768);
  buf = mk16(src, 4000);
  for (t = 0; t < 6; t++) {
    fm_voice_t v; int16_t out[1000 * 2]; double pos = 0; int maxerr = 0;
    double qstep = (double)(uint32_t)(steps[t] * 65536.0 + 0.5) / 65536.0;
    int vol = 200, pan = -30;
    int gl = (((vol * (127 - pan) * 257 + 32768) >> 16));
    int gr = (((vol * (pan + 128) * 257 + 32768) >> 16));
    int n = 1000;
    vinit(&v, buf, 4000, 0, steps[t], vol, pan);
    if (steps[t] * n > 3990) n = (int)(3990 / steps[t]);
    fm_mix(out, n, &v, 1);
    for (i = 0; i < n; i++) {
      int ip = (int)pos; double fr = pos - ip;
      double s = src[ip] + (src[ip + 1] - src[ip]) * fr;
      double rl = floor(s * gl / 256.0), rr = floor(s * gr / 256.0);
      int el = abs((int)rl - out[2 * i]), er = abs((int)rr - out[2 * i + 1]);
      if (el > maxerr) maxerr = el;
      if (er > maxerr) maxerr = er;
      pos += qstep;
    }
    CHECK(maxerr <= 3, "interp step %.2f maxerr=%d", steps[t], maxerr);
  }
  free(buf - GUARD);
}

/* 3. forward loop: exact index sequence, no reads past loop_end */
static void test_fwd_loop(void)
{
  int16_t src[100], *buf; fm_voice_t v; int16_t out[300 * 2]; int i;
  for (i = 0; i < 100; i++) src[i] = (int16_t)(i * 10);
  buf = mk16(src, 100);          /* data past loop_end is real data (<1000) */
  vinit(&v, buf, 100, FM_LOOP, 1.0, 256, -128);
  v.loop_start = 20; v.loop_end = 60;
  fm_mix(out, 300, &v, 1);
  for (i = 0; i < 300; i++) {
    int idx = i < 60 ? i : 20 + (i - 20) % 40;
    CHECK(out[2 * i] == idx * 10, "fwd loop i=%d got %d want %d", i, out[2 * i], idx * 10);
  }
  /* fractional step: all outputs stay inside the loop-region value range */
  vinit(&v, buf, 100, FM_LOOP, 1.37, 256, -128);
  v.loop_start = 20; v.loop_end = 60; v.pos = 25;
  fm_mix(out, 300, &v, 1);
  for (i = 0; i < 300; i++)
    CHECK(out[2 * i] >= 190 && out[2 * i] <= 590, "fwd loop frac i=%d out=%d", i, out[2 * i]);
  /* no-loop: sample ends, voice deactivates, trailing silence */
  vinit(&v, buf, 100, 0, 1.0, 256, -128);
  fm_mix(out, 150, &v, 1);
  CHECK(!(v.flags & FM_ACTIVE), "voice should stop");
  CHECK(out[2 * 120] == 0, "silence after end");
  free(buf - GUARD);
}

/* 4. ping-pong */
static void test_pingpong(void)
{
  int16_t src[40], *buf; fm_voice_t v; int16_t out[200 * 2]; int i;
  for (i = 0; i < 40; i++) src[i] = (int16_t)(i * 100);
  buf = mk16(src, 40);
  vinit(&v, buf, 40, FM_LOOP | FM_PINGPONG, 1.0, 256, -128);
  v.loop_start = 10; v.loop_end = 20; v.pos = 10;
  fm_mix(out, 200, &v, 1);
  for (i = 0; i < 200; i++)
    CHECK(out[2 * i] >= 990 && out[2 * i] <= 1900, "pp range i=%d out=%d", i, out[2 * i]);
  for (i = 0; i < 10; i++) CHECK(out[2 * i] == (10 + i) * 100, "pp fwd i=%d", i);
  /* descending run after the turn */
  for (i = 12; i < 19; i++)
    CHECK(out[2 * i] < out[2 * (i - 1)] + 2, "pp descending i=%d", i);
  /* periodic (tolerate sub-LSB turn-around drift) */
  for (i = 40; i < 180; i++)
    CHECK(abs(out[2 * i] - out[2 * (i + 20)]) <= 6, "pp periodic i=%d", i);
  /* big step + short loop must not read out of bounds (guard sentinels) */
  vinit(&v, buf, 40, FM_LOOP | FM_PINGPONG, 13.37, 256, -128);
  v.loop_start = 10; v.loop_end = 14; v.pos = 10;
  fm_mix(out, 200, &v, 1);
  for (i = 0; i < 200; i++)
    CHECK(out[2 * i] >= 990 && out[2 * i] <= 1400, "pp big step i=%d out=%d", i, out[2 * i]);
  free(buf - GUARD);
}

/* 5. ramp: monotonic, bounded slope, reaches target */
static void test_ramp(void)
{
  int16_t dc[8], out[300 * 2]; fm_voice_t v; int i, maxd = 0;
  for (i = 0; i < 8; i++) dc[i] = 20000;
  vinit(&v, dc, 8, FM_LOOP, 1.0, 0, -128);
  v.loop_start = 0; v.loop_end = 8;
  fm_voice_set_gain(&v, 256, -128, 100);
  fm_mix(out, 300, &v, 1);
  for (i = 1; i < 300; i++) {
    int d = out[2 * i] - out[2 * i - 2];
    CHECK(d >= 0, "ramp up monotonic i=%d", i);
    if (d > maxd) maxd = d;
  }
  CHECK(maxd <= 20000 / 100 + 20000 / 256 + 2, "ramp slope %d", maxd);
  CHECK(out[2 * 100] == 20000 && out[2 * 299] == 20000, "ramp target");
  fm_voice_set_gain(&v, 0, -128, 50);
  fm_mix(out, 100, &v, 1);
  for (i = 1; i < 100; i++)
    CHECK(out[2 * i] <= out[2 * i - 2], "ramp down monotonic i=%d", i);
  CHECK(out[2 * 50] == 0 && out[2 * 99] == 0, "ramp down target");
}

/* 6. 64 voices: saturation, no wrap; drop of quiet voices */
static void test_many(void)
{
  int16_t pos_dc[8], neg_dc[8], out[64 * 2]; fm_voice_t vs[64]; int i;
  for (i = 0; i < 8; i++) { pos_dc[i] = 30000; neg_dc[i] = -30000; }
  fm_set_max_mixed(64);
  for (i = 0; i < 64; i++) {
    vinit(&vs[i], pos_dc, 8, FM_LOOP, 1.0, 256, -128);
    vs[i].loop_end = 8;
  }
  fm_mix(out, 64, vs, 64);
  for (i = 0; i < 64; i++)
    CHECK(out[2 * i] == 32767 && out[2 * i + 1] == 0, "sat+ i=%d out=%d", i, out[2 * i]);
  for (i = 0; i < 64; i++) vs[i].data = neg_dc;
  fm_mix(out, 64, vs, 64);
  for (i = 0; i < 64; i++) CHECK(out[2 * i] == -32768, "sat- i=%d", i);

  /* limiter: 40 voices, only the 32 loudest are mixed; all positions move */
  {
    static int16_t dc[1000]; fm_voice_t w[40]; int16_t o[64 * 2];
    for (i = 0; i < 1000; i++) dc[i] = 100;
    fm_reset();                      /* max_mixed = 32 */
    for (i = 0; i < 40; i++) {
      vinit(&w[i], dc, 1000, 0, 1.0, i < 32 ? 256 : 16, -128);
    }
    fm_mix(o, 64, w, 40);
    CHECK(o[0] == 3200 && o[2 * 63] == 3200, "limiter sum %d", o[0]);
    for (i = 0; i < 40; i++) CHECK(w[i].pos == 64, "dropped voice must advance, v%d pos=%u", i, w[i].pos);
  }
}

/* 7. block sizes: identical results whatever the chunking; 8. mix of all */
static void test_blocks_one(int with_ramp)
{
  static const int sizes[] = { 1, 255, 256, 512 };
  int16_t a16[300], b16[200]; int8_t c8[150];
  fm_voice_t base[6], v1[6], v2[6];
  int16_t ref[1024 * 2], o[1024 * 2];
  int s, i, k;
  srand(7);
  for (i = 0; i < 300; i++) a16[i] = (int16_t)(rand() % 20000 - 10000);
  for (i = 0; i < 200; i++) b16[i] = (int16_t)(rand() % 20000 - 10000);
  for (i = 0; i < 150; i++) c8[i] = (int8_t)(rand() % 200 - 100);
  vinit(&base[0], a16, 300, FM_LOOP, 0.71, 200, -50);              base[0].loop_start = 40; base[0].loop_end = 290;
  vinit(&base[1], b16, 200, FM_LOOP | FM_PINGPONG, 1.33, 256, 60); base[1].loop_start = 10; base[1].loop_end = 150;
  vinit(&base[2], c8, 150, FM_LOOP | FM_8BIT, 2.9, 180, 0);        base[2].loop_start = 0;  base[2].loop_end = 150;
  vinit(&base[3], a16, 300, 0, 0.25, 256, 127);
  vinit(&base[4], c8, 150, FM_8BIT | FM_LOOP | FM_PINGPONG, 0.4, 90, -128); base[4].loop_start = 5; base[4].loop_end = 60;
  vinit(&base[5], b16, 200, FM_LOOP, 5.5, 0, 0);                  base[5].loop_start = 0;  base[5].loop_end = 200;
  if (with_ramp) {
    fm_voice_set_gain(&base[2], 256, 100, 300);   /* ramp spanning chunks */
    fm_voice_set_gain(&base[5], 128, 0, 77);
  }
  memcpy(v1, base, sizeof base);
  fm_mix(ref, 1024, v1, 6);
  for (s = 0; s < 4; s++) {
    int done = 0;
    memcpy(v2, base, sizeof base);
    while (done < 1024) {
      int n = sizes[s]; if (done + n > 1024) n = 1024 - done;
      fm_mix(o + done * 2, n, v2, 6);
      done += n;
    }
    {
      int maxd = 0;
      for (i = 0; i < 1024 * 2; i++) {
        int d = abs(ref[i] - o[i]); if (d > maxd) maxd = d;
      }
      CHECK(with_ramp ? maxd <= 200 : maxd == 0,
            "chunking %d ramp=%d maxdiff=%d", sizes[s], with_ramp, maxd);
    }
    for (k = 0; k < 6; k++)
      CHECK(v2[k].flags == v1[k].flags &&
            ((v1[k].flags & FM_ACTIVE) == 0 ||
             (v2[k].pos == v1[k].pos && v2[k].frac == v1[k].frac && v2[k].dir == v1[k].dir)),
            "state differs after chunking %d voice %d", sizes[s], k);
  }
}

static void test_blocks(void)
{
  test_blocks_one(0);
  test_blocks_one(1);
}

/* 9. nearest-neighbor (no interpolation): step 0.5, data {0,1000,-1000,500} */
static void test_nearest(void)
{
  int16_t src[4] = { 0, 1000, -1000, 500 };
  int16_t *buf = mk16(src, 4);
  fm_voice_t v;
  int16_t out[16 * 2];
  int i;
  
  vinit(&v, buf, 4, 0, 0.5, 256, -128);
  fm_mix(out, 16, &v, 1);
  
  /* Expected: each src sample repeated twice (step 0.5 → 2 frames per sample) */
  int expected[16] = { 0, 0, 1000, 1000, -1000, -1000, 500, 500, 0, 0, 0, 0, 0, 0, 0, 0 };
  for (i = 0; i < 16; i++) {
    if (i < 8) {
      CHECK(out[2*i] == expected[i], "nearest i=%d got %d want %d", i, out[2*i], expected[i]);
    } else {
      CHECK(out[2*i] == 0, "nearest silence i=%d", i);
    }
    CHECK(out[2*i + 1] == 0, "nearest right silent i=%d", i);
  }
  
  /* Position/length tracking */
  CHECK(!(v.flags & FM_ACTIVE), "voice stopped after end");
  
  free(buf - GUARD);
}

int main(void)
{
  fm_reset();
  test_sine(); fm_reset();
  test_interp(); fm_reset();
  test_fwd_loop(); fm_reset();
  test_pingpong(); fm_reset();
  test_ramp(); fm_reset();
  test_many(); fm_reset();
  test_blocks(); fm_reset();
  test_nearest();
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
