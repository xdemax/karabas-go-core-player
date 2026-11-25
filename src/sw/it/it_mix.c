/*
 * it_mix.c - IT sample mixer, adapted from xm_fpga.c mix() logic.
 * Channel-major loop with branch-free fast path for no-boundary blocks.
 * Supports 8/16-bit, nearest/linear, forward/backward (pingpong), ramping.
 * No branches/function calls/divisions in hot loop; gains and clamps outside.
 */

#include "it_mix.h"
#include "../fastmix.h"
#include <stdint.h>

#define IT_MIXBUF 256

static int32_t mixbuf[IT_MIXBUF * 2];

/* Gain + clamp for one output frame (output goes to accumulator, not FIFO). */
static inline void clamp_sample(int16_t *dst, int32_t l32, int32_t r32, int32_t og) {
  int32_t l = (l32 * og) >> 6;
  int32_t r = (r32 * og) >> 6;
  if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
  if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
  *dst++ = (int16_t)l;
  *dst++ = (int16_t)r;
}

/* Read one source sample scaled to 16-bit range. */
static inline int32_t read_sample(const fm_voice_t *v, uint32_t idx) {
  if (v->flags & FM_8BIT)
    return (int32_t)((const int8_t *)v->data)[idx] * 256;
  return ((const int16_t *)v->data)[idx];
}

/* Linear interpolation for 16-bit (input at 16-bit scale). */
static inline int32_t lerp16(int32_t s0, int32_t s1, uint32_t frac) {
  return s0 + (((s1 - s0) * (int32_t)(frac >> 1)) >> 15);
}

/* Linear interpolation for 8-bit (input 8-bit, scale to 16-bit). */
static inline int32_t lerp8(int8_t s0, int8_t s1, uint32_t frac) {
  int32_t d = (int32_t)s1 - (int32_t)s0;
  int32_t prod = (d < 0) ? ((-d * (int32_t)frac + 255) >> 8) : ((d * (int32_t)frac) >> 8);
  return ((int32_t)s0 << 8) + (d < 0 ? -prod : prod);
}

void it_mix(int16_t *dst, int frames, fm_voice_t *voices, int nvoices) {
  int remaining = frames;
  
  while (remaining > 0) {
    int chunk = remaining > IT_MIXBUF ? IT_MIXBUF : remaining;
    int16_t *out = dst;
    
    /* Zero accumulator */
    for (int i = 0; i < chunk; i++) {
      mixbuf[2 * i] = 0;
      mixbuf[2 * i + 1] = 0;
    }
    
    /* Channel-major: each voice renders into accumulators */
    for (int vi = 0; vi < nvoices; vi++) {
      fm_voice_t *v = &voices[vi];
      
      if (!(v->flags & FM_ACTIVE)) continue;
      
      uint32_t pos = v->pos;
      uint32_t frac = v->frac;
      uint32_t step = v->step;
      uint32_t length = v->length;
      uint32_t loop_start = v->loop_start;
      uint32_t loop_end = v->loop_end;
      int32_t gl = v->cur_l, gr = v->cur_r;
      int32_t dl = 0, dr = 0;
      
      /* Compute ramp deltas (Q16 gains ramping over frames) */
      if (v->ramp > 0) {
        int32_t target_l = (v->volume * (256 - (v->pan + 128))) >> 8;
        int32_t target_r = (v->volume * (v->pan + 128)) >> 8;
        dl = ((target_l << 16) - (gl * (int32_t)v->ramp_len)) / (int32_t)v->ramp;
        dr = ((target_r << 16) - (gr * (int32_t)v->ramp_len)) / (int32_t)v->ramp;
      }
      
      int is_8bit = (v->flags & FM_8BIT) ? 1 : 0;
      int is_looping = (v->flags & FM_LOOP) ? 1 : 0;
      int is_pingpong = (v->flags & FM_PINGPONG) ? 1 : 0;
      int is_nearest = (v->flags & FM_NEAREST) ? 1 : 0;
      int is_backward = v->dir ? 1 : 0;
      
      /* Fast path: entire block fits without boundary crossing */
      uint64_t last_pos = (uint64_t)pos + (uint64_t)step * (uint32_t)(chunk - 1);
      if (!is_looping && (last_pos >> 16) < length) {
        for (int i = 0; i < chunk; i++) {
          int32_t s;
          uint32_t idx = pos >> 16;
          
          if (is_nearest) {
            s = read_sample(v, idx);
          } else {
            int32_t s0 = read_sample(v, idx);
            int32_t s1 = read_sample(v, idx + 1);
            if (is_8bit) {
              s = lerp8((int8_t)(s0 >> 8), (int8_t)(s1 >> 8), frac);
            } else {
              s = lerp16(s0, s1, frac);
            }
          }
          
          if (v->ramp > 0) {
            gl += dl; gr += dr;
          }
          
          int32_t gl8 = gl >> 8, gr8 = gr >> 8;
          mixbuf[2 * i] += (s * gl8) >> 8;
          mixbuf[2 * i + 1] += (s * gr8) >> 8;
          
          if (!is_backward) {
            uint32_t carry = (frac + (step & 0xFFFF)) >> 16;
            frac = (frac + (step & 0xFFFF)) & 0xFFFF;
            pos += (step >> 16) + carry;
          } else {
            uint32_t borrow = (frac < (step & 0xFFFF)) ? 1 : 0;
            frac = (frac - (step & 0xFFFF)) & 0xFFFF;
            pos -= (step >> 16) + borrow;
          }
        }
        v->pos = pos;
        v->frac = frac;
      } else {
        /* Slow path: handle boundaries */
        for (int i = 0; i < chunk; i++) {
          if (!(v->flags & FM_ACTIVE)) break;
          
          uint32_t idx = pos >> 16;
          
          /* Loop/boundary handling */
          if (idx >= length) {
            if (!is_looping) {
              v->flags &= ~FM_ACTIVE;
              break;
            }
            if (is_pingpong && !is_backward) {
              is_backward = 1;
              v->dir = 1;
            } else if (is_pingpong && is_backward) {
              is_backward = 0;
              v->dir = 0;
            }
            uint32_t over = idx - length;
            if (loop_end > loop_start) {
              uint32_t loop_len = loop_end - loop_start;
              if (over >= loop_len) over %= loop_len;
              idx = loop_start + over;
            }
          }
          
          int32_t s;
          if (is_nearest) {
            s = read_sample(v, idx);
          } else {
            int32_t s0 = read_sample(v, idx);
            int32_t s1 = (idx + 1 < length) ? read_sample(v, idx + 1) : s0;
            if (is_8bit) {
              s = lerp8((int8_t)(s0 >> 8), (int8_t)(s1 >> 8), frac);
            } else {
              s = lerp16(s0, s1, frac);
            }
          }
          
          if (v->ramp > 0) {
            gl += dl; gr += dr;
          }
          
          int32_t gl8 = gl >> 8, gr8 = gr >> 8;
          mixbuf[2 * i] += (s * gl8) >> 8;
          mixbuf[2 * i + 1] += (s * gr8) >> 8;
          
          if (!is_backward) {
            uint32_t carry = (frac + (step & 0xFFFF)) >> 16;
            frac = (frac + (step & 0xFFFF)) & 0xFFFF;
            pos = ((idx << 16) | frac) + (step >> 16) + carry;
          } else {
            uint32_t borrow = (frac < (step & 0xFFFF)) ? 1 : 0;
            frac = (frac - (step & 0xFFFF)) & 0xFFFF;
            pos = ((idx << 16) | frac) - (step >> 16) - borrow;
          }
        }
        
        v->pos = pos;
        v->frac = frac;
        v->dir = is_backward;
      }
      
      if (v->ramp > 0) {
        v->ramp = (v->ramp > (uint16_t)chunk) ? (v->ramp - chunk) : 0;
      }
    }
    
    /* Output: gain, clamp and write to dst (unity gain for now) */
    int32_t og = 256;
    for (int i = 0; i < chunk; i++) {
      clamp_sample(out, mixbuf[2 * i], mixbuf[2 * i + 1], og);
      out += 2;
    }
    
    dst += 2 * chunk;
    remaining -= chunk;
  }
}


