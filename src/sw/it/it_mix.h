/*
 * it_mix.h - Ultra-compact IT mixer interface for VexRiscv.
 * Minimal API: prepare (once per tick) + block (per render chunk).
 * Voice structure is binary-compatible with fm_voice_t for easy integration.
 */
#ifndef IT_MIX_H
#define IT_MIX_H

#include <stdint.h>

#define FM_MAX_VOICES  64

/* Voice flags */
enum {
	IT_MIX_ACTIVE    = 1u << 0,
	IT_MIX_8BIT      = 1u << 1,
	IT_MIX_LOOP      = 1u << 2,
	IT_MIX_NEAREST   = 1u << 3,
	IT_MIX_BACKWARD  = 1u << 4,  /* pingpong/backward playback */
	
	/* Aliases for fastmix compatibility */
	FM_ACTIVE = IT_MIX_ACTIVE,
	FM_8BIT = IT_MIX_8BIT,
	FM_LOOP = IT_MIX_LOOP,
	FM_NEAREST = IT_MIX_NEAREST,
	FM_PINGPONG = IT_MIX_BACKWARD,
};

typedef struct {
	const void *data;        /* signed 8/16-bit mono samples */
	uint32_t length;         /* total frames */
	uint32_t loop_start;
	uint32_t loop_end;       /* exclusive, clamped to length */

	uint32_t pos;            /* integer frame index */
	uint32_t frac;           /* fractional pos, 16.16 fixed */
	uint32_t step;           /* 16.16 source frames per output frame */

	int32_t volume;          /* 0..256 */
	int32_t pan;             /* -128 (L) .. 127 (R) */
	
	/* Gains: used by old fastmix interface (cur_l/cur_r) and new mixer (cur_gl/cur_gr) */
	int32_t cur_gl, cur_gr;  /* current gains Q16 (managed by mixer) - NEW NAMES */
	uint16_t ramp;           /* frames until target is reached */
	uint16_t ramp_len;       /* unused, kept for alignment with fm_voice_t */
	
	uint8_t flags;           /* IT_MIX_ACTIVE | IT_MIX_8BIT | IT_MIX_LOOP | IT_MIX_NEAREST | IT_MIX_BACKWARD */
	uint8_t dir;             /* backward direction flag (unused, for fm_voice_t compat) */
	uint8_t reserved[2];
	
	/* New fields (not in fm_voice_t, stored after alignment) */
	int32_t tgt_gl, tgt_gr;  /* target gains Q16 (computed in prepare) */
} it_mix_voice_t;

/* Convenience macros for old code compatibility */
#define cur_l cur_gl
#define cur_r cur_gr

/* Alias for fastmix compatibility */
typedef it_mix_voice_t fm_voice_t;

/* Prepare all voices: compute target gains once per tick.
   Call this before rendering a block. */
void it_mix_prepare(it_mix_voice_t *v);

/* Mix n frames from voices (nv count) into stereo int16 dst.
   Accumulation Q8 internal, saturated to int16 on output.
   Overwrites dst (does not add to it). */
void it_mix_block(int16_t *dst, int frames, it_mix_voice_t *voices, int nv);

/* fastmix compatibility wrappers */
void fm_voice_init(it_mix_voice_t *v);
void fm_voice_set_gain(it_mix_voice_t *v, int32_t volume, int32_t pan,
                       uint16_t ramp_frames);

#endif
