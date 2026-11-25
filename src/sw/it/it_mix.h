/*
 * it_mix.h - IT sample mixer interface.
 * Reuses fastmix.h fm_voice_t structure; renders frames into int16 output.
 */

#ifndef IT_MIX_H
#define IT_MIX_H

#include <stdint.h>
#include "../fastmix.h"

/* Mix `frames` output frames into dst (interleaved stereo int16).
 * Input voices array is updated in place (pos, frac, dir, ramp advance). */
void it_mix(int16_t *dst, int frames, fm_voice_t *voices, int nvoices);

#endif
