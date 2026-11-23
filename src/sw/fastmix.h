/*
 * fastmix.h - compact fixed-point mixer for tracker voices (C99, no FPU).
 *
 * Output: interleaved stereo int16. Internal sum: int32, saturation only
 * at the output. fm_mix() OVERWRITES dst (it does not add to it).
 *
 * Differences from the TZ draft (all deliberate):
 *  - position is split into `pos` (integer frame index) + `frac` (0..65535)
 *    instead of one 16.16 uint32: a 16.16 uint32 caps samples at 65535 frames,
 *    which is too short for XM/IT samples. `step` stays 16.16.
 *  - `voices` is non-const: pos/frac/dir/ramp/gains are advanced in place.
 *  - added cur_l/cur_r (current gains, Q16) and dir (ping-pong direction).
 */
#ifndef FASTMIX_H
#define FASTMIX_H

#include <stdint.h>

#define FM_MAX_VOICES 64

enum {
  FM_8BIT     = 1u << 0,
  FM_LOOP     = 1u << 1,
  FM_PINGPONG = 1u << 2,   /* only with FM_LOOP */
  FM_ACTIVE   = 1u << 3
};

typedef struct {
  const void *data;        /* signed 8-bit or signed 16-bit, mono */
  uint32_t length;         /* frames */
  uint32_t loop_start;
  uint32_t loop_end;       /* exclusive; clamped to length */

  uint32_t pos;            /* integer source frame index */
  uint32_t frac;           /* 0..65535 */
  uint32_t step;           /* 16.16 source frames per output frame */
  int32_t  volume;         /* 0..256 (Q8) */
  int32_t  pan;            /* -128 (left) .. 127 (right) */

  int32_t  cur_l, cur_r;   /* current gains, Q16 (managed by the mixer) */
  uint16_t ramp;           /* frames left until target gain is reached */
  uint16_t ramp_len;
  uint8_t  flags;
  uint8_t  dir;            /* 0 forward, 1 backward (ping-pong state) */
  uint8_t  reserved[2];
} fm_voice_t;

/* Resets global mixer state (max mixed voices = 32). */
void fm_reset(void);

/* Max voices actually mixed per block; quietest (by volume) are skipped
 * (their position still advances). 1..FM_MAX_VOICES, default 32. */
void fm_set_max_mixed(int n);

/* Zero a voice. */
void fm_voice_init(fm_voice_t *v);

/* Set target volume/pan; reaches it after `ramp_frames` output frames
 * (0 = immediately). Current gain is kept, so there is no click. */
void fm_voice_set_gain(fm_voice_t *v, int32_t volume, int32_t pan,
                       uint16_t ramp_frames);

/* Mix `frames` output frames into dst (interleaved stereo int16). */
void fm_mix(int16_t *dst, int frames, fm_voice_t *voices, int nvoices);

#endif
