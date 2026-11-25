#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <stdint.h>

typedef struct {
  uint32_t rate;
  uint32_t step;
  uint32_t phase;
  int32_t prev_l;
  int32_t prev_r;
  int init;
} rs_t;

void rs_init(rs_t *rs, uint32_t rate);
int rs_frame(rs_t *rs, int16_t l, int16_t r, int16_t out[2][2]);

#endif
