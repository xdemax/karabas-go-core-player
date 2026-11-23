// The shared spectrum analyzer of the FPGA engines. The mixer pushes the final mono sample;
// a skin calls analysis_bands() once per update (it reads the last 256 samples).
#pragma once
#include <stdint.h>

#define AN_N 512
extern int16_t an_buf[AN_N];
extern uint16_t an_wr;

static inline void analysis_push(int16_t l, int16_t r) {
  an_buf[an_wr & (AN_N - 1)] = (int16_t) (((int32_t) l + (int32_t) r) >> 1);
  an_wr = (an_wr + 1) & (AN_N - 1);
}

void analysis_bands(uint8_t *out, int n);
void analysis_set_enabled(int on);
