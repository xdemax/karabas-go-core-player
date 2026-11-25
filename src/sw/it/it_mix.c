/*
 * it_mix.c - Ultra-compact, cache-friendly IT mixer for VexRiscv RV32IM.
 * Single-pass stereo accumulation + gain, specialized kernels for 8 flag combinations,
 * ramping via increments (Q8), no divisions in hot path.
 */
#include "it_mix.h"
#include <string.h>

#define IT_MIX_FRAMES 256

/* Accumulator: stereo int32, fits in D-cache with working samples */
static int32_t it_mix_acc[IT_MIX_FRAMES * 2];

/* Attributes for GCC always-inline */
#if defined(__GNUC__)
#define IT_AI static inline __attribute__((always_inline))
#else
#define IT_AI static inline
#endif

/* ===== Sample fetch (direct, no calls in loop body) ===== */
IT_AI int32_t it_mix_rd16(const int16_t *data, uint32_t idx)
{
	return (int32_t)data[idx];
}

IT_AI int32_t it_mix_rd8(const int8_t *data, uint32_t idx)
{
	return (int32_t)data[idx] * 256;
}

/* ===== Linear interpolation (Q16 source samples to Q16 output) ===== */
IT_AI int32_t it_mix_lerp16(int32_t s0, int32_t s1, uint32_t frac)
{
	/* frac >> 1 is 15-bit; (s1-s0) is 17-bit signed => product fits int32 */
	return s0 + (((s1 - s0) * (int32_t)(frac >> 1)) >> 15);
}

IT_AI int32_t it_mix_lerp8(int32_t s0, int32_t s1, uint32_t frac)
{
	/* both args already Q16 (s0, s1 have been *256); frac>>0 is 16-bit */
	return s0 + (((s1 - s0) * (int32_t)frac) >> 8);
}

/* ===== Main kernel macro (16 combinations: 8/16 × loop/no × nearest/lerp × fwd/bwd) ===== */
#define IT_MIX_KERN(IS8, LOOP, NEAREST, BWD) \
static void it_mix_k_##IS8##_##LOOP##_##NEAREST##_##BWD( \
	int32_t *buf, int n, \
	const void *data, uint32_t len, uint32_t loop_start, uint32_t loop_end, \
	uint32_t *ppos, uint32_t *pfrac, uint32_t step, \
	int32_t gl, int32_t gr, int32_t dl, int32_t dr, int ramping) \
{ \
	uint32_t pos = *ppos, frac = *pfrac; \
	const int32_t gl8 = gl >> 8, gr8 = gr >> 8; \
	int i = 0; \
	\
	/* Process pairs for better ILP */ \
	for (; i + 1 < n; i += 2) { \
		int32_t s; \
		\
		/* Sample 1 */ \
		if (NEAREST) { \
			s = (IS8) ? it_mix_rd8((const int8_t *)data, pos) : it_mix_rd16((const int16_t *)data, pos); \
		} else { \
			uint32_t nx = pos + (BWD ? (uint32_t)-1 : 1); \
			if (LOOP) { \
				if (BWD && (int32_t)nx < (int32_t)loop_start) nx = loop_end - 1; \
				else if (!BWD && nx >= loop_end) nx = loop_start; \
			} \
			int32_t s0 = (IS8) ? it_mix_rd8((const int8_t *)data, pos) : it_mix_rd16((const int16_t *)data, pos); \
			int32_t s1 = (IS8) ? it_mix_rd8((const int8_t *)data, nx) : it_mix_rd16((const int16_t *)data, nx); \
			s = (IS8) ? it_mix_lerp8(s0, s1, BWD ? (65536u - frac) : frac) : it_mix_lerp16(s0, s1, BWD ? (65536u - frac) : frac); \
		} \
		\
		/* Ramp gains */ \
		if (ramping) { gl += dl; gr += dr; } \
		int32_t tgl = (ramping) ? (gl >> 8) : gl8; \
		int32_t tgr = (ramping) ? (gr >> 8) : gr8; \
		\
		/* Accumulate */ \
		buf[2*i] += (s * tgl) >> 8; \
		buf[2*i+1] += (s * tgr) >> 8; \
		\
		/* Advance position */ \
		if (BWD) { \
			uint32_t sf = step & 0xFFFFu; \
			uint32_t borrow = (frac < sf) ? 1u : 0u; \
			frac = (frac - sf) & 0xFFFFu; \
			pos -= (step >> 16) + borrow; \
			if (LOOP && (int32_t)pos < (int32_t)loop_start) pos = loop_end - (loop_start - pos); \
		} else { \
			uint32_t carry = (frac + (step & 0xFFFFu)) >> 16; \
			frac = (frac + (step & 0xFFFFu)) & 0xFFFFu; \
			pos += (step >> 16) + carry; \
			if (LOOP && pos >= loop_end) pos = loop_start + (pos - loop_end); \
		} \
		\
		/* Sample 2 */ \
		if (NEAREST) { \
			s = (IS8) ? it_mix_rd8((const int8_t *)data, pos) : it_mix_rd16((const int16_t *)data, pos); \
		} else { \
			uint32_t nx = pos + (BWD ? (uint32_t)-1 : 1); \
			if (LOOP) { \
				if (BWD && (int32_t)nx < (int32_t)loop_start) nx = loop_end - 1; \
				else if (!BWD && nx >= loop_end) nx = loop_start; \
			} \
			int32_t s0 = (IS8) ? it_mix_rd8((const int8_t *)data, pos) : it_mix_rd16((const int16_t *)data, pos); \
			int32_t s1 = (IS8) ? it_mix_rd8((const int8_t *)data, nx) : it_mix_rd16((const int16_t *)data, nx); \
			s = (IS8) ? it_mix_lerp8(s0, s1, BWD ? (65536u - frac) : frac) : it_mix_lerp16(s0, s1, BWD ? (65536u - frac) : frac); \
		} \
		\
		/* Ramp gains */ \
		if (ramping) { gl += dl; gr += dr; } \
		tgl = (ramping) ? (gl >> 8) : gl8; \
		tgr = (ramping) ? (gr >> 8) : gr8; \
		\
		/* Accumulate */ \
		buf[2*i+2] += (s * tgl) >> 8; \
		buf[2*i+3] += (s * tgr) >> 8; \
		\
		/* Advance position */ \
		if (BWD) { \
			uint32_t sf = step & 0xFFFFu; \
			uint32_t borrow = (frac < sf) ? 1u : 0u; \
			frac = (frac - sf) & 0xFFFFu; \
			pos -= (step >> 16) + borrow; \
			if (LOOP && (int32_t)pos < (int32_t)loop_start) pos = loop_end - (loop_start - pos); \
		} else { \
			uint32_t carry = (frac + (step & 0xFFFFu)) >> 16; \
			frac = (frac + (step & 0xFFFFu)) & 0xFFFFu; \
			pos += (step >> 16) + carry; \
			if (LOOP && pos >= loop_end) pos = loop_start + (pos - loop_end); \
		} \
	} \
	\
	/* Handle odd frame */ \
	if (i < n) { \
		int32_t s; \
		if (NEAREST) { \
			s = (IS8) ? it_mix_rd8((const int8_t *)data, pos) : it_mix_rd16((const int16_t *)data, pos); \
		} else { \
			uint32_t nx = pos + (BWD ? (uint32_t)-1 : 1); \
			if (LOOP) { \
				if (BWD && (int32_t)nx < (int32_t)loop_start) nx = loop_end - 1; \
				else if (!BWD && nx >= loop_end) nx = loop_start; \
			} \
			int32_t s0 = (IS8) ? it_mix_rd8((const int8_t *)data, pos) : it_mix_rd16((const int16_t *)data, pos); \
			int32_t s1 = (IS8) ? it_mix_rd8((const int8_t *)data, nx) : it_mix_rd16((const int16_t *)data, nx); \
			s = (IS8) ? it_mix_lerp8(s0, s1, BWD ? (65536u - frac) : frac) : it_mix_lerp16(s0, s1, BWD ? (65536u - frac) : frac); \
		} \
		if (ramping) { gl += dl; gr += dr; } \
		int32_t tgl = (ramping) ? (gl >> 8) : gl8; \
		int32_t tgr = (ramping) ? (gr >> 8) : gr8; \
		buf[2*i] += (s * tgl) >> 8; \
		buf[2*i+1] += (s * tgr) >> 8; \
		if (BWD) { \
			uint32_t sf = step & 0xFFFFu; \
			uint32_t borrow = (frac < sf) ? 1u : 0u; \
			frac = (frac - sf) & 0xFFFFu; \
			pos -= (step >> 16) + borrow; \
		} else { \
			uint32_t carry = (frac + (step & 0xFFFFu)) >> 16; \
			frac = (frac + (step & 0xFFFFu)) & 0xFFFFu; \
			pos += (step >> 16) + carry; \
		} \
	} \
	\
	*ppos = pos; *pfrac = frac; \
}

/* Generate 16 specialized kernels */
IT_MIX_KERN(0, 0, 0, 0)  /* 16-bit, no-loop, lerp, forward */
IT_MIX_KERN(0, 0, 0, 1)  /* 16-bit, no-loop, lerp, backward */
IT_MIX_KERN(0, 0, 1, 0)  /* 16-bit, no-loop, nearest, forward */
IT_MIX_KERN(0, 0, 1, 1)  /* 16-bit, no-loop, nearest, backward */
IT_MIX_KERN(0, 1, 0, 0)  /* 16-bit, loop, lerp, forward */
IT_MIX_KERN(0, 1, 0, 1)  /* 16-bit, loop, lerp, backward */
IT_MIX_KERN(0, 1, 1, 0)  /* 16-bit, loop, nearest, forward */
IT_MIX_KERN(0, 1, 1, 1)  /* 16-bit, loop, nearest, backward */
IT_MIX_KERN(1, 0, 0, 0)  /* 8-bit, no-loop, lerp, forward */
IT_MIX_KERN(1, 0, 0, 1)  /* 8-bit, no-loop, lerp, backward */
IT_MIX_KERN(1, 0, 1, 0)  /* 8-bit, no-loop, nearest, forward */
IT_MIX_KERN(1, 0, 1, 1)  /* 8-bit, no-loop, nearest, backward */
IT_MIX_KERN(1, 1, 0, 0)  /* 8-bit, loop, lerp, forward */
IT_MIX_KERN(1, 1, 0, 1)  /* 8-bit, loop, lerp, backward */
IT_MIX_KERN(1, 1, 1, 0)  /* 8-bit, loop, nearest, forward */
IT_MIX_KERN(1, 1, 1, 1)  /* 8-bit, loop, nearest, backward */

/* Dispatch helper: select kernel based on flags and direction */
#define IT_MIX_DISPATCH(IS8, LOOP, NEAREST, BWD, BUF, N, DATA, LEN, LS, LE, POS, FRAC, STEP, GL, GR, DL, DR, RAMP) \
	it_mix_k_##IS8##_##LOOP##_##NEAREST##_##BWD(BUF, N, DATA, LEN, LS, LE, POS, FRAC, STEP, GL, GR, DL, DR, RAMP)

/* ===== One-pass voice prep + mix + write ===== */
void it_mix_prepare(it_mix_voice_t *v)
{
	/* Compute target gains (Q16) from volume/pan once per tick, not per frame */
	if (v->volume < 0) v->volume = 0;
	else if (v->volume > 256) v->volume = 256;
	
	if (v->pan < -128) v->pan = -128;
	else if (v->pan > 127) v->pan = 127;
	
	/* Stereo panning: Q8 × (127-pan)/128 and (pan+128)/128 */
	int32_t vol = (int32_t)v->volume;
	int32_t pan = (int32_t)v->pan;
	v->tgt_gl = ((vol * (127 - pan) * 257 + 32768) >> 16) << 8;
	v->tgt_gr = ((vol * (pan + 128) * 257 + 32768) >> 16) << 8;
}

void it_mix_block(int16_t *dst, int frames, it_mix_voice_t *voices, int nv)
{
	int32_t *ap;
	int i, j, cnt;
	int16_t *out;

	while (frames > 0) {
		int n = frames < IT_MIX_FRAMES ? frames : IT_MIX_FRAMES;
		cnt = n * 2;

		/* Clear accumulator with memset (faster than loop on most archs) */
		memset(it_mix_acc, 0, (size_t)cnt * sizeof(int32_t));

		/* Mix all voices into accumulator */
		for (i = 0; i < nv; i++) {
			it_mix_voice_t *v = &voices[i];
			if (!(v->flags & IT_MIX_ACTIVE)) continue;

			int32_t tgl = v->tgt_gl, tgr = v->tgt_gr;

			/* Skip if silent */
			if (tgl == 0 && tgr == 0 && v->cur_gl == 0 && v->cur_gr == 0 && v->ramp == 0) {
				/* Advance position without mixing */
				uint32_t pos = v->pos, frac = v->frac;
				int bwd = (v->flags & IT_MIX_BACKWARD) != 0;
				for (j = 0; j < n; j++) {
					if (bwd) {
						uint32_t sf = v->step & 0xFFFFu;
						uint32_t borrow = (frac < sf) ? 1u : 0u;
						frac = (frac - sf) & 0xFFFFu;
						pos -= (v->step >> 16) + borrow;
						if ((v->flags & IT_MIX_LOOP) && (int32_t)pos < (int32_t)v->loop_start) {
							pos = v->loop_end - (v->loop_start - pos);
						}
					} else {
						uint32_t carry = (frac + (v->step & 0xFFFFu)) >> 16;
						frac = (frac + (v->step & 0xFFFFu)) & 0xFFFFu;
						pos += (v->step >> 16) + carry;
						if ((v->flags & IT_MIX_LOOP) && pos >= v->loop_end) {
							pos = v->loop_start + (pos - v->loop_end);
						}
					}
				}
				v->pos = pos;
				v->frac = frac;
				continue;
			}

			/* Initialize or use current gain */
			if (v->ramp == 0) {
				v->cur_gl = tgl;
				v->cur_gr = tgr;
			}

			int32_t gl = v->cur_gl, gr = v->cur_gr;
			int ramping = (v->ramp > 0) ? 1 : 0;
			int32_t dl = 0, dr = 0;

			if (ramping) {
				/* Ramping: compute increment in Q8 */
				int32_t ramp_frames = (int32_t)v->ramp;
				int32_t mix_frames = (int32_t)n;
				if (ramp_frames > mix_frames) ramp_frames = mix_frames;

				dl = (tgl - gl) / ramp_frames;
				dr = (tgr - gr) / ramp_frames;

				int kern_frames = ramp_frames;
				int is8 = (v->flags & IT_MIX_8BIT) != 0;
				int loop = (v->flags & IT_MIX_LOOP) != 0;
				int nearest = (v->flags & IT_MIX_NEAREST) != 0;
				int bwd = (v->flags & IT_MIX_BACKWARD) != 0;

				/* Dispatch to specialized kernel */
				if (is8) {
					if (loop) {
						if (nearest) {
							if (bwd) it_mix_k_1_1_1_1(it_mix_acc, kern_frames, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
							else it_mix_k_1_1_1_0(it_mix_acc, kern_frames, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
						} else {
							if (bwd) it_mix_k_1_1_0_1(it_mix_acc, kern_frames, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
							else it_mix_k_1_1_0_0(it_mix_acc, kern_frames, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
						}
					} else {
						if (nearest) {
							if (bwd) it_mix_k_1_0_1_1(it_mix_acc, kern_frames, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
							else it_mix_k_1_0_1_0(it_mix_acc, kern_frames, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
						} else {
							if (bwd) it_mix_k_1_0_0_1(it_mix_acc, kern_frames, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
							else it_mix_k_1_0_0_0(it_mix_acc, kern_frames, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
						}
					}
				} else {
					if (loop) {
						if (nearest) {
							if (bwd) it_mix_k_0_1_1_1(it_mix_acc, kern_frames, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
							else it_mix_k_0_1_1_0(it_mix_acc, kern_frames, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
						} else {
							if (bwd) it_mix_k_0_1_0_1(it_mix_acc, kern_frames, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
							else it_mix_k_0_1_0_0(it_mix_acc, kern_frames, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
						}
					} else {
						if (nearest) {
							if (bwd) it_mix_k_0_0_1_1(it_mix_acc, kern_frames, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
							else it_mix_k_0_0_1_0(it_mix_acc, kern_frames, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
						} else {
							if (bwd) it_mix_k_0_0_0_1(it_mix_acc, kern_frames, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
							else it_mix_k_0_0_0_0(it_mix_acc, kern_frames, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										gl, gr, dl, dr, 1);
						}
					}
				}

				v->ramp -= (uint16_t)ramp_frames;
				gl = v->cur_gl + dl * ramp_frames;
				gr = v->cur_gr + dr * ramp_frames;

				/* Mix rest of block at target gain (no ramping) */
				if (ramp_frames < n) {
					int rest = n - ramp_frames;
					int is8 = (v->flags & IT_MIX_8BIT) != 0;
					int loop = (v->flags & IT_MIX_LOOP) != 0;
					int nearest = (v->flags & IT_MIX_NEAREST) != 0;
					int bwd = (v->flags & IT_MIX_BACKWARD) != 0;

					if (is8) {
						if (loop) {
							if (nearest) {
								if (bwd) it_mix_k_1_1_1_1(it_mix_acc, rest, v->data, v->length,
											v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
								else it_mix_k_1_1_1_0(it_mix_acc, rest, v->data, v->length,
											v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
							} else {
								if (bwd) it_mix_k_1_1_0_1(it_mix_acc, rest, v->data, v->length,
											v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
								else it_mix_k_1_1_0_0(it_mix_acc, rest, v->data, v->length,
											v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
							}
						} else {
							if (nearest) {
								if (bwd) it_mix_k_1_0_1_1(it_mix_acc, rest, v->data, v->length,
											0, v->length, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
								else it_mix_k_1_0_1_0(it_mix_acc, rest, v->data, v->length,
											0, v->length, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
							} else {
								if (bwd) it_mix_k_1_0_0_1(it_mix_acc, rest, v->data, v->length,
											0, v->length, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
								else it_mix_k_1_0_0_0(it_mix_acc, rest, v->data, v->length,
											0, v->length, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
							}
						}
					} else {
						if (loop) {
							if (nearest) {
								if (bwd) it_mix_k_0_1_1_1(it_mix_acc, rest, v->data, v->length,
											v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
								else it_mix_k_0_1_1_0(it_mix_acc, rest, v->data, v->length,
											v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
							} else {
								if (bwd) it_mix_k_0_1_0_1(it_mix_acc, rest, v->data, v->length,
											v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
								else it_mix_k_0_1_0_0(it_mix_acc, rest, v->data, v->length,
											v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
							}
						} else {
							if (nearest) {
								if (bwd) it_mix_k_0_0_1_1(it_mix_acc, rest, v->data, v->length,
											0, v->length, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
								else it_mix_k_0_0_1_0(it_mix_acc, rest, v->data, v->length,
											0, v->length, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
							} else {
								if (bwd) it_mix_k_0_0_0_1(it_mix_acc, rest, v->data, v->length,
											0, v->length, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
								else it_mix_k_0_0_0_0(it_mix_acc, rest, v->data, v->length,
											0, v->length, &v->pos, &v->frac, v->step,
											tgl, tgr, 0, 0, 0);
							}
						}
					}
				}
			} else {
				/* No ramping: use current/target gain directly */
				int32_t use_gl = gl, use_gr = gr;
				int is8 = (v->flags & IT_MIX_8BIT) != 0;
				int loop = (v->flags & IT_MIX_LOOP) != 0;
				int nearest = (v->flags & IT_MIX_NEAREST) != 0;
				int bwd = (v->flags & IT_MIX_BACKWARD) != 0;

				if (is8) {
					if (loop) {
						if (nearest) {
							if (bwd) it_mix_k_1_1_1_1(it_mix_acc, n, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
							else it_mix_k_1_1_1_0(it_mix_acc, n, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
						} else {
							if (bwd) it_mix_k_1_1_0_1(it_mix_acc, n, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
							else it_mix_k_1_1_0_0(it_mix_acc, n, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
						}
					} else {
						if (nearest) {
							if (bwd) it_mix_k_1_0_1_1(it_mix_acc, n, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
							else it_mix_k_1_0_1_0(it_mix_acc, n, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
						} else {
							if (bwd) it_mix_k_1_0_0_1(it_mix_acc, n, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
							else it_mix_k_1_0_0_0(it_mix_acc, n, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
						}
					}
				} else {
					if (loop) {
						if (nearest) {
							if (bwd) it_mix_k_0_1_1_1(it_mix_acc, n, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
							else it_mix_k_0_1_1_0(it_mix_acc, n, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
						} else {
							if (bwd) it_mix_k_0_1_0_1(it_mix_acc, n, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
							else it_mix_k_0_1_0_0(it_mix_acc, n, v->data, v->length,
										v->loop_start, v->loop_end, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
						}
					} else {
						if (nearest) {
							if (bwd) it_mix_k_0_0_1_1(it_mix_acc, n, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
							else it_mix_k_0_0_1_0(it_mix_acc, n, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
						} else {
							if (bwd) it_mix_k_0_0_0_1(it_mix_acc, n, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
							else it_mix_k_0_0_0_0(it_mix_acc, n, v->data, v->length,
										0, v->length, &v->pos, &v->frac, v->step,
										use_gl, use_gr, 0, 0, 0);
						}
					}
				}
			}

			v->cur_gl = gl;
			v->cur_gr = gr;
		}

		/* One-pass: saturate and write to output (Q8 → Q0 with clamp) */
		ap = it_mix_acc;
		out = dst;
		for (j = 0; j < cnt; j++) {
			int32_t x = ap[j] >> 8;
			if (x > 32767) x = 32767;
			else if (x < -32768) x = -32768;
			out[j] = (int16_t)x;
		}

		dst += cnt;
		frames -= n;
	}
}

/* ===== Wrapper functions for fastmix compatibility ===== */
void fm_voice_init(it_mix_voice_t *v)
{
	v->data = NULL;
	v->length = 0;
	v->loop_start = 0;
	v->loop_end = 0;
	v->pos = 0;
	v->frac = 0;
	v->step = 0;
	v->volume = 0;
	v->pan = 0;
	v->cur_gl = 0;
	v->cur_gr = 0;
	v->tgt_gl = 0;
	v->tgt_gr = 0;
	v->ramp = 0;
	v->ramp_len = 0;
	v->flags = 0;
	v->dir = 0;
	v->reserved[0] = 0;
	v->reserved[1] = 0;
}

void fm_voice_set_gain(it_mix_voice_t *v, int32_t volume, int32_t pan,
                       uint16_t ramp_frames)
{
	v->volume = volume;
	v->pan = pan;
	v->ramp = ramp_frames;
	it_mix_prepare(v);
}
