#include "resample.h"
#include <string.h>

void rs_init(rs_t *r, uint32_t rate) {
  memset(r, 0, sizeof(rs_t));
  r->rate = rate;
  if (rate != 0 && rate != 48000) {
    r->step = (rate << 16) / 48000;
  }
}

int rs_frame(rs_t *r, int16_t l, int16_t r, int16_t out[2][2]) {
  if (r->rate == 48000 || r->rate == 0) {
    out[0][0] = l;
    out[0][1] = r;
    return 1;
  }

  int32_t li = (int32_t)l;
  int32_t ri = (int32_t)r;

  if (!r->init) {
    r->prev_l = li;
    r->prev_r = ri;
    r->phase = 0;
    r->init = 1;
    return 0;
  }

  int out_count = 0;
  while (r->phase < 65536u && out_count < 2) {
    int32_t out_l = r->prev_l + (int32_t)(((int64_t)(li - r->prev_l) * (int32_t)r->phase) >> 16);
    int32_t out_r = r->prev_r + (int32_t)(((int64_t)(ri - r->prev_r) * (int32_t)r->phase) >> 16);
    out[out_count][0] = (int16_t)out_l;
    out[out_count][1] = (int16_t)out_r;
    out_count++;
    r->phase += r->step;
  }
  r->phase -= 65536u;
  r->prev_l = li;
  r->prev_r = ri;
  return out_count;
}
