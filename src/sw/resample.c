#include "resample.h"
#include <string.h>

void rs_init(rs_t *rs, uint32_t rate) {
  memset(rs, 0, sizeof(rs_t));
  rs->rate = rate;
  if (rate != 0 && rate != 48000) {
    rs->step = (rate << 16) / 48000;
  }
}

int rs_frame(rs_t *rs, int16_t l, int16_t r, int16_t out[2][2]) {
  if (rs->rate == 48000 || rs->rate == 0) {
    out[0][0] = l;
    out[0][1] = r;
    return 1;
  }

  int32_t li = (int32_t)l;
  int32_t ri = (int32_t)r;

  if (!rs->init) {
    rs->prev_l = li;
    rs->prev_r = ri;
    rs->phase = 0;
    rs->init = 1;
    return 0;
  }

  int out_count = 0;
  while (rs->phase < 65536u && out_count < 2) {
    int32_t out_l = rs->prev_l + (int32_t)(((int64_t)(li - rs->prev_l) * (int32_t)rs->phase) >> 16);
    int32_t out_r = rs->prev_r + (int32_t)(((int64_t)(ri - rs->prev_r) * (int32_t)rs->phase) >> 16);
    out[out_count][0] = (int16_t)out_l;
    out[out_count][1] = (int16_t)out_r;
    out_count++;
    rs->phase += rs->step;
  }
  rs->phase -= 65536u;
  rs->prev_l = li;
  rs->prev_r = ri;
  return out_count;
}
