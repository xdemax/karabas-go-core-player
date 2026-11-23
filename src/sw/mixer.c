// The one output mixer of the player: every format feeds its stereo samples here.
// It owns the output low-pass filter and the single spectrum analyzer feed; the
// format engines only produce samples.
#include <stdint.h>
#include "mixer.h"
#include "audio.h"
#include "analysis.h"

// Two-pole low-pass, 12 kHz at 48 kHz, Q=0.707 (RBJ, Q15 coefficients).
static int32_t x1l, x2l, y1l, y2l;
static int32_t x1r, x2r, y1r, y2r;

static inline int16_t lp(int32_t x, int32_t *x1, int32_t *x2, int32_t *y1, int32_t *y2) {
  int32_t y = (9598 * x + 19196 * *x1 + 9598 * *x2 - 5623 * *y2) >> 15;
  *x2 = *x1; *x1 = x;
  *y2 = *y1; *y1 = y;
  if (y > 32767) y = 32767; else if (y < -32768) y = -32768;
  return (int16_t) y;
}

void mixer_reset(void) {
  x1l = x2l = y1l = y2l = 0;
  x1r = x2r = y1r = y2r = 0;
}

void mixer_write(int16_t l, int16_t r) {
  int16_t fl = lp(l, &x1l, &x2l, &y1l, &y2l);
  int16_t fr = lp(r, &x1r, &x2r, &y1r, &y2r);
  analysis_push(fl, fr);
  audio_write(fl, fr);
}
