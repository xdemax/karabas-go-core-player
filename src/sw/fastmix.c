/*
 * fastmix.c - fixed-point tracker voice mixer. See fastmix.h.
 *
 * Structure:
 *  - channel-major: each voice is rendered into an int32 stereo accumulator
 *    (one chunk of up to FM_CHUNK frames), then saturated once.
 *  - the inner kernels run on a segment where no boundary can be hit, so
 *    there is no loop/end test, no '%' and no sample-edge branch per sample.
 *    The segment length is computed outside the loop (int64 is used only
 *    there). Frames sitting on a sample/loop edge go through slow_frame().
 *  - format (8/16), direction and ramping are compile-time constants of
 *    each kernel instance (always_inline + switch once per segment).
 *  - no float, no malloc, no memcpy, no function pointers per sample.
 *
 * Custom CPU instructions (opcode 0x0b, funct3=0):
 *  - SETF (funct7=0): hidden_frac = rs1 & 0xFFFF
 *  - LERP16 (funct7=1): rd = s0 + (((s1-s0) * (hidden_frac >> 1)) >> 15); hidden_s = rd
 *  - MAC (funct7=2): rd = rs1 + (hidden_s * (rs2 >> 8)), 32-bit wrap
 *  - GETF (funct7=3): rd = hidden_frac (zero-extend)
 *  - ADV (funct7=4): t = hidden_frac + (rs2 & 0xFFFF); hidden_frac = t & 0xFFFF; rd = rs1 + (rs2 >> 16) + (t >> 16)
 *  - ADVB (funct7=5): borrow = hidden_frac < (rs2 & 0xFFFF); hidden_frac = (hidden_frac - rs2) & 0xFFFF; rd = rs1 - (rs2 >> 16) - borrow
 *  - LERP8 (funct7=6): rd = s0*256 + (((s1-s0) * hidden_frac) >> 8); hidden_s = rd
 *
 * Optimizations (compile-time opt-in, default off):
 *  - FM_OPT_UNROLL: unroll main loop 2x (fwd and bwd iterations) with
 *    const/restrict pointers to enable better ILP and scheduling (default: on, disable with -DFM_OPT_UNROLL=0).
 */
#include "fastmix.h"

#define FM_CHUNK 256

/* Optimizations: set to 1 to enable. */
#ifndef FM_OPT_UNROLL
#define FM_OPT_UNROLL 1
#endif

/* Custom CPU support: set to 1 to use, 0 for C fallback. */
#ifndef FM_CUSTOM_CPU
#define FM_CUSTOM_CPU 0
#endif

#if defined(__GNUC__)
#define FM_AI static inline __attribute__((always_inline))
#else
#define FM_AI static inline
#endif

static int32_t acc[FM_CHUNK * 2];
static int max_mixed = 32;

/* ------------------------------------------------------------------ */
/* custom CPU instruction macros                                      */

#if FM_CUSTOM_CPU

/* Hidden state lives in the custom CPU; in C we just emit the insn. */
#define FM_SETF(frac) \
  asm volatile (".insn r 0x0b, 0, 0, x0, %0, x0" : : "r"(frac))

#define FM_LERP16(rd, s0, s1) \
  asm volatile (".insn r 0x0b, 0, 1, %0, %1, %2" : "=r"(rd) : "r"(s0), "r"(s1))

#define FM_MAC(rd, acc, gain) \
  asm volatile (".insn r 0x0b, 0, 2, %0, %1, %2" : "=r"(rd) : "r"(acc), "r"(gain))

#define FM_GETF(rd) \
  asm volatile (".insn r 0x0b, 0, 3, %0, x0, x0" : "=r"(rd))

#define FM_ADV(rd, pos, step) \
  asm volatile (".insn r 0x0b, 0, 4, %0, %1, %2" : "=r"(rd) : "r"(pos), "r"(step))

#define FM_ADVB(rd, pos, step) \
  asm volatile (".insn r 0x0b, 0, 5, %0, %1, %2" : "=r"(rd) : "r"(pos), "r"(step))

#define FM_LERP8(rd, s0, s1) \
  asm volatile (".insn r 0x0b, 0, 6, %0, %1, %2" : "=r"(rd) : "r"(s0), "r"(s1))

#else

/* C fallback: hidden state in static variables. */
static uint32_t fm_hidden_frac;
static int32_t fm_hidden_s;

#define FM_SETF(frac) \
  (fm_hidden_frac = (frac) & 0xFFFFu)

#define FM_LERP16(rd, s0, s1) do { \
  int32_t _s0 = (s0), _s1 = (s1); \
  int32_t _diff = _s1 - _s0; \
  (rd) = _s0 + (((_diff * (fm_hidden_frac >> 1)) >> 15)); \
  fm_hidden_s = (rd); \
} while (0)

#define FM_MAC(rd, acc, gain) do { \
  int32_t _gain = (gain) >> 8; \
  (rd) = (int32_t)((uint32_t)(acc) + (uint32_t)(fm_hidden_s * _gain)); \
} while (0)

#define FM_GETF(rd) \
  ((rd) = fm_hidden_frac)

#define FM_ADV(rd, pos, step) do { \
  uint32_t _t = fm_hidden_frac + ((step) & 0xFFFFu); \
  fm_hidden_frac = _t & 0xFFFFu; \
  (rd) = (uint32_t)(pos) + ((step) >> 16) + (_t >> 16); \
} while (0)

#define FM_ADVB(rd, pos, step) do { \
  uint32_t _sf = (step) & 0xFFFFu; \
  uint32_t _borrow = (fm_hidden_frac < _sf) ? 1u : 0u; \
  fm_hidden_frac = (fm_hidden_frac - _sf) & 0xFFFFu; \
  (rd) = (uint32_t)(pos) - ((step) >> 16) - _borrow; \
} while (0)

#define FM_LERP8(rd, s0, s1) do { \
  int32_t _s0 = (int32_t)(int8_t)(s0); \
  int32_t _s1 = (int32_t)(int8_t)(s1); \
  int32_t _diff = _s1 - _s0; \
  (rd) = _s0 * 256 + (((_diff * (int32_t)fm_hidden_frac) >> 8)); \
  fm_hidden_s = (rd); \
} while (0)

#endif

/* ------------------------------------------------------------------ */
/* helpers                                                            */

/* read one source frame scaled to the 16-bit range */
static int32_t rd(const fm_voice_t *v, uint32_t i)
{
  if (v->flags & FM_8BIT)
    return (int32_t)((const int8_t *)v->data)[i] * 256;
  return ((const int16_t *)v->data)[i];
}

/* linear interpolation; (s1-s0) is 17 bits signed, frac>>1 is 15 bits,
 * so the product always fits in int32. */
FM_AI int32_t lerp16(int32_t s0, int32_t s1, int32_t frac)
{
  return s0 + (((s1 - s0) * (frac >> 1)) >> 15);
}

/* Advance frac and pos forward (no-branch version for rv32im). */
FM_AI void advance_pos_fwd(uint32_t *ppos, uint32_t *pfrac, uint32_t si, uint32_t sf)
{
  uint32_t carry = (*pfrac + sf) >> 16;
  *pfrac = (*pfrac + sf) & 0xFFFF;
  *ppos += si + carry;
}

/* Advance frac and pos backward. */
FM_AI void advance_pos_bwd(uint32_t *ppos, uint32_t *pfrac, uint32_t si, uint32_t sf)
{
  uint32_t borrow = (*pfrac < sf) ? 1 : 0;
  *pfrac = (*pfrac - sf) & 0xFFFF;
  *ppos -= si + borrow;
}

/* ------------------------------------------------------------------ */
/* kernels: n frames, no boundary inside                              */

FM_AI void kern(int32_t *a, const void *d, int is8, int bwd, int ramping, int nearest,
                uint32_t *ppos, uint32_t *pfrac, uint32_t step, int n,
                int32_t *pgl, int32_t *pgr, int32_t dl, int32_t dr)
{
  const int16_t * const p16 = (const int16_t *)d;
  const int8_t * const p8 = (const int8_t *)d;
  uint32_t pos = *ppos;
  int32_t frac = (int32_t)*pfrac;
  const int32_t si = (int32_t)(step >> 16);
  const int32_t sf = (int32_t)(step & 0xFFFF);
  int32_t gl = *pgl, gr = *pgr;
  const int32_t gl8 = gl >> 8, gr8 = gr >> 8;

  FM_SETF(frac);

#if FM_OPT_UNROLL
  int32_t *ap = a;
  int i = 0;
  /* Process pairs of iterations unrolled */
  for (; i + 1 < n; i += 2) {
    int32_t s;
    
    /* Iteration 1 */
    if (nearest) {
      if (is8) {
        s = p8[pos] * 256;
      } else {
        s = p16[pos];
      }
    } else {
      if (is8) {
        int32_t s0 = p8[pos], s1 = p8[pos + 1];
        FM_LERP8(s, s0, s1);
      } else {
        int32_t s0 = p16[pos], s1 = p16[pos + 1];
        FM_LERP16(s, s0, s1);
      }
    }
    
    if (ramping) {
      gl += dl;
      gr += dr;
    }
    int32_t acc0 = ap[0];
    FM_MAC(acc0, acc0, gl);
    ap[0] = acc0;
    int32_t acc1 = ap[1];
    FM_MAC(acc1, acc1, gr);
    ap[1] = acc1;
    
    ap += 2;
    if (!bwd) {
      FM_ADV(pos, pos, step);
    } else {
      FM_ADVB(pos, pos, step);
    }

    /* Iteration 2 */
    if (nearest) {
      if (is8) {
        s = p8[pos] * 256;
      } else {
        s = p16[pos];
      }
    } else {
      if (is8) {
        int32_t s0 = p8[pos], s1 = p8[pos + 1];
        FM_LERP8(s, s0, s1);
      } else {
        int32_t s0 = p16[pos], s1 = p16[pos + 1];
        FM_LERP16(s, s0, s1);
      }
    }
    
    if (ramping) {
      gl += dl;
      gr += dr;
    }
    acc0 = ap[0];
    FM_MAC(acc0, acc0, gl);
    ap[0] = acc0;
    acc1 = ap[1];
    FM_MAC(acc1, acc1, gr);
    ap[1] = acc1;
    
    ap += 2;
    if (!bwd) {
      FM_ADV(pos, pos, step);
    } else {
      FM_ADVB(pos, pos, step);
    }
  }
  
  /* Handle remaining frame if n is odd */
  if (i < n) {
    int32_t s;
    if (nearest) {
      if (is8) {
        s = p8[pos] * 256;
      } else {
        s = p16[pos];
      }
    } else {
      if (is8) {
        int32_t s0 = p8[pos], s1 = p8[pos + 1];
        FM_LERP8(s, s0, s1);
      } else {
        int32_t s0 = p16[pos], s1 = p16[pos + 1];
        FM_LERP16(s, s0, s1);
      }
    }
    
    if (ramping) {
      gl += dl;
      gr += dr;
    }
    int32_t acc0 = ap[0];
    FM_MAC(acc0, acc0, gl);
    ap[0] = acc0;
    int32_t acc1 = ap[1];
    FM_MAC(acc1, acc1, gr);
    ap[1] = acc1;
    
    if (!bwd) {
      FM_ADV(pos, pos, step);
    } else {
      FM_ADVB(pos, pos, step);
    }
  }
#else
  while (n-- > 0) {
    int32_t s;
    if (nearest) {
      if (is8) {
        s = p8[pos] * 256;
      } else {
        s = p16[pos];
      }
    } else {
      if (is8) {
        int32_t s0 = p8[pos], s1 = p8[pos + 1];
        FM_LERP8(s, s0, s1);
      } else {
        int32_t s0 = p16[pos], s1 = p16[pos + 1];
        FM_LERP16(s, s0, s1);
      }
    }

    if (ramping) {
      gl += dl;
      gr += dr;
    }
    
    int32_t acc0 = a[0];
    FM_MAC(acc0, acc0, gl);
    a[0] = acc0;
    int32_t acc1 = a[1];
    FM_MAC(acc1, acc1, gr);
    a[1] = acc1;
    
    a += 2;

    if (!bwd) {
      FM_ADV(pos, pos, step);
    } else {
      FM_ADVB(pos, pos, step);
    }
  }
#endif

  FM_GETF(frac);
  *ppos = pos;
  *pfrac = (uint32_t)frac;
  *pgl = gl;
  *pgr = gr;
}

static void kernel(int32_t *a, const fm_voice_t *v, int ramping,
                   uint32_t *ppos, uint32_t *pfrac, int n,
                   int32_t *gl, int32_t *gr, int32_t dl, int32_t dr)
{
  const void *d = v->data;
  uint32_t st = v->step;
  int nearest = (v->flags & FM_NEAREST) ? 1 : 0;
  int key = ((v->flags & FM_8BIT) ? 4 : 0) | (v->dir ? 2 : 0) | (ramping ? 1 : 0);
#define K(E, B, R) kern(a, d, E, B, R, nearest, ppos, pfrac, st, n, gl, gr, dl, dr)
  switch (key) {
  case 0: K(0, 0, 0); break;
  case 1: K(0, 0, 1); break;
  case 2: K(0, 1, 0); break;
  case 3: K(0, 1, 1); break;
  case 4: K(1, 0, 0); break;
  case 5: K(1, 0, 1); break;
  case 6: K(1, 1, 0); break;
  default: K(1, 1, 1); break;
  }
#undef K
}

/* ------------------------------------------------------------------ */
/* boundary handling (outside the inner loop)                         */

typedef struct { uint32_t ls, end; int loop, pp; } fm_range_t;

static void setup(fm_voice_t *v, fm_range_t *r)
{
  r->end = v->length;
  r->ls = 0;
  r->loop = r->pp = 0;
  if ((v->flags & FM_LOOP) && v->loop_end > v->loop_start) {
    uint32_t e = v->loop_end > v->length ? v->length : v->loop_end;
    if (v->loop_start < e) {
      r->ls = v->loop_start;
      r->end = e;
      r->loop = 1;
      r->pp = (v->flags & FM_PINGPONG) ? 1 : 0;
    }
  }
  if (!r->pp) v->dir = 0;
}

static int64_t getP(const fm_voice_t *v)
{
  return (int64_t)(int32_t)v->pos * 65536 + (int64_t)v->frac;
}

static void putP(fm_voice_t *v, int64_t P)
{
  v->pos = (uint32_t)(P >> 16);
  v->frac = (uint32_t)(P & 0xFFFF);
}

/* Bring position into the readable range [0,end) (wrap / reflect).
 * Returns 0 if the voice ran past the end of a non-looping sample. */
static int norm(fm_voice_t *v, const fm_range_t *r)
{
  int64_t P = getP(v);
  int64_t E = (int64_t)r->end * 65536;
  int64_t LS, L, per;

  if (!r->loop)
    return !(P >= E || P < 0);

  LS = (int64_t)r->ls * 65536;
  L = E - LS;

  if (!r->pp) {
    if (P >= E) { P = LS + (P - E) % L; putP(v, P); }
    return 1;
  }
  per = 2 * L;
  if (v->dir == 0 && P >= E + per) P -= ((P - E) / per) * per;
  if (v->dir == 1 && P < LS - per) P += ((LS - P) / per) * per;
  for (;;) {
    if (v->dir == 0) {
      if (P < E) break;
      P = 2 * E - P - 1; v->dir = 1;
    } else {
      if (P >= LS) break;
      P = 2 * LS - P - 1; v->dir = 0;
    }
  }
  putP(v, P);
  return 1;
}

/* one frame at the very edge: next sample is chosen explicitly */
static void slow_frame(fm_voice_t *v, int32_t *a, const fm_range_t *r,
                       int32_t *gl, int32_t *gr, int32_t dl, int32_t dr,
                       int ramping)
{
  int32_t s0 = rd(v, v->pos), s1;
  uint32_t i1 = v->pos + 1;
  int64_t P;
  if (i1 < r->end) s1 = rd(v, i1);
  else if (r->loop && !r->pp) s1 = rd(v, r->ls);
  else s1 = s0;
  {
    int32_t s = lerp16(s0, s1, (int32_t)v->frac);
    if (ramping) { *gl += dl; *gr += dr; }
    a[0] += s * (*gl >> 8);
    a[1] += s * (*gr >> 8);
  }
  P = getP(v);
  P += v->dir ? -(int64_t)v->step : (int64_t)v->step;
  putP(v, P);
}

/* render n frames with the given gains (ramping or constant) */
static void render(fm_voice_t *v, int32_t *a, int n, int32_t *gl, int32_t *gr,
                   int32_t dl, int32_t dr, int ramping)
{
  fm_range_t r;
  setup(v, &r);
  while (n > 0 && (v->flags & FM_ACTIVE)) {
    int64_t cnt;
    if (!norm(v, &r)) { v->flags &= (uint8_t)~FM_ACTIVE; break; }
    if (v->pos + 1 >= r.end) {          /* sitting on the last frame */
      slow_frame(v, a, &r, gl, gr, dl, dr, ramping);
      a += 2; n--;
      continue;
    }

    if (v->step == 0) {
      cnt = n;
    } else if (v->dir == 0) {
      int64_t L = (int64_t)(r.end - 1) * 65536;
      cnt = (L - getP(v) + v->step - 1) / v->step;
    } else {
      cnt = (getP(v) - (int64_t)r.ls * 65536) / v->step + 1;
    }
    if (cnt > n) cnt = n;
    {
      uint32_t pos = v->pos, frac = v->frac;
      kernel(a, v, ramping, &pos, &frac, (int)cnt, gl, gr, dl, dr);
      v->pos = pos; v->frac = frac;
    }
    n -= (int)cnt;
    a += 2 * cnt;
  }
}

/* move a voice forward without mixing (silent / dropped voices) */
static void advance_voice(fm_voice_t *v, int n)
{
  fm_range_t r;
  int64_t P, d;
  setup(v, &r);
  d = (int64_t)v->step * n;
  P = getP(v) + (v->dir ? -d : d);
  putP(v, P);
  if (!norm(v, &r)) v->flags &= (uint8_t)~FM_ACTIVE;
}

/* ------------------------------------------------------------------ */

static void mix_voice(fm_voice_t *v, int32_t *a, int n)
{
  int32_t vol = v->volume, pan = v->pan;
  int32_t tl, tr, gl, gr;
  if (vol < 0) vol = 0; else if (vol > 256) vol = 256;
  if (pan < -128) pan = -128; else if (pan > 127) pan = 127;
  /* gain = vol * (127-pan)/255 etc., rounded, Q8 -> Q16 */
  tl = ((vol * (127 - pan) * 257 + 32768) >> 16) << 8;
  tr = ((vol * (pan + 128) * 257 + 32768) >> 16) << 8;

  if (v->ramp == 0) { v->cur_l = tl; v->cur_r = tr; }
  if (tl == 0 && tr == 0 && v->cur_l == 0 && v->cur_r == 0 && v->ramp == 0) {
    advance_voice(v, n);
    return;
  }
  gl = v->cur_l; gr = v->cur_r;

  if (v->ramp) {
    int rs = v->ramp < n ? v->ramp : n;
    int32_t dl = (tl - gl) / (int32_t)v->ramp;
    int32_t dr = (tr - gr) / (int32_t)v->ramp;
    render(v, a, rs, &gl, &gr, dl, dr, 1);
    v->ramp = (uint16_t)(v->ramp - rs);
    if (v->ramp == 0) { gl = tl; gr = tr; }
    a += 2 * rs;
    n -= rs;
    if (!(v->flags & FM_ACTIVE)) { v->cur_l = gl; v->cur_r = gr; return; }
  }
  if (n > 0) render(v, a, n, &gl, &gr, 0, 0, 0);
  v->cur_l = gl; v->cur_r = gr;
}

void fm_reset(void) { max_mixed = 32; }

void fm_set_max_mixed(int n)
{
  if (n < 1) n = 1;
  if (n > FM_MAX_VOICES) n = FM_MAX_VOICES;
  max_mixed = n;
}

void fm_voice_init(fm_voice_t *v)
{
  uint8_t *p = (uint8_t *)v;
  unsigned i;
  for (i = 0; i < sizeof(*v); i++) p[i] = 0;
}

void fm_voice_set_gain(fm_voice_t *v, int32_t volume, int32_t pan,
                       uint16_t ramp_frames)
{
  v->volume = volume;
  v->pan = pan;
  v->ramp = v->ramp_len = ramp_frames;
}

void fm_mix(int16_t *dst, int frames, fm_voice_t *voices, int nvoices)
{
  uint8_t order[FM_MAX_VOICES];
  uint8_t mixed[FM_MAX_VOICES];
  int na = 0, nm, i, k;

  if (nvoices > FM_MAX_VOICES) nvoices = FM_MAX_VOICES;
  for (i = 0; i < nvoices; i++) {
    mixed[i] = 0;
    if (voices[i].flags & FM_ACTIVE) order[na++] = (uint8_t)i;
  }
  nm = na;
  if (na > max_mixed) {            /* keep the loudest, stable by index */
    for (i = 1; i < na; i++) {
      uint8_t x = order[i];
      int32_t vx = voices[x].volume;
      k = i - 1;
      while (k >= 0 && voices[order[k]].volume < vx) {
        order[k + 1] = order[k];
        k--;
      }
      order[k + 1] = x;
    }
    nm = max_mixed;
  }
  for (k = 0; k < nm; k++) mixed[order[k]] = 1;

  while (frames > 0) {
    int n = frames < FM_CHUNK ? frames : FM_CHUNK;
    int cnt = n * 2;
    for (i = 0; i < cnt; i++) acc[i] = 0;
    for (i = 0; i < nvoices; i++) {
      fm_voice_t *v = &voices[i];
      if (!(v->flags & FM_ACTIVE)) continue;
      if (mixed[i]) mix_voice(v, acc, n);
      else advance_voice(v, n);
    }
    for (i = 0; i < cnt; i++) {
      int32_t x = acc[i] >> 8;
      if (x > 32767) x = 32767; else if (x < -32768) x = -32768;
      dst[i] = (int16_t)x;
    }
    dst += cnt;
    frames -= n;
  }
}

static void kernel(int32_t *a, const fm_voice_t *v, int ramping,
                   uint32_t *ppos, uint32_t *pfrac, int n,
                   int32_t *gl, int32_t *gr, int32_t dl, int32_t dr)
{
  const void *d = v->data;
  uint32_t st = v->step;
  int nearest = (v->flags & FM_NEAREST) ? 1 : 0;
  int key = ((v->flags & FM_8BIT) ? 4 : 0) | (v->dir ? 2 : 0) | (ramping ? 1 : 0);
#define K(E, B, R) kern(a, d, E, B, R, nearest, ppos, pfrac, st, n, gl, gr, dl, dr)
  switch (key) {
  case 0: K(0, 0, 0); break;
  case 1: K(0, 0, 1); break;
  case 2: K(0, 1, 0); break;
  case 3: K(0, 1, 1); break;
  case 4: K(1, 0, 0); break;
  case 5: K(1, 0, 1); break;
  case 6: K(1, 1, 0); break;
  default: K(1, 1, 1); break;
  }
#undef K
}

/* ------------------------------------------------------------------ */
/* boundary handling (outside the inner loop)                         */

typedef struct { uint32_t ls, end; int loop, pp; } fm_range_t;

static void setup(fm_voice_t *v, fm_range_t *r)
{
  r->end = v->length;
  r->ls = 0;
  r->loop = r->pp = 0;
  if ((v->flags & FM_LOOP) && v->loop_end > v->loop_start) {
    uint32_t e = v->loop_end > v->length ? v->length : v->loop_end;
    if (v->loop_start < e) {
      r->ls = v->loop_start;
      r->end = e;
      r->loop = 1;
      r->pp = (v->flags & FM_PINGPONG) ? 1 : 0;
    }
  }
  if (!r->pp) v->dir = 0;
}

static int64_t getP(const fm_voice_t *v)
{
  return (int64_t)(int32_t)v->pos * 65536 + (int64_t)v->frac;
}

static void putP(fm_voice_t *v, int64_t P)
{
  v->pos = (uint32_t)(P >> 16);
  v->frac = (uint32_t)(P & 0xFFFF);
}

/* Bring position into the readable range [0,end) (wrap / reflect).
 * Returns 0 if the voice ran past the end of a non-looping sample. */
static int norm(fm_voice_t *v, const fm_range_t *r)
{
  int64_t P = getP(v);
  int64_t E = (int64_t)r->end * 65536;
  int64_t LS, L, per;

  if (!r->loop)
    return !(P >= E || P < 0);

  LS = (int64_t)r->ls * 65536;
  L = E - LS;

  if (!r->pp) {
    if (P >= E) { P = LS + (P - E) % L; putP(v, P); }
    return 1;
  }
  per = 2 * L;
  if (v->dir == 0 && P >= E + per) P -= ((P - E) / per) * per;
  if (v->dir == 1 && P < LS - per) P += ((LS - P) / per) * per;
  for (;;) {
    if (v->dir == 0) {
      if (P < E) break;
      P = 2 * E - P - 1; v->dir = 1;
    } else {
      if (P >= LS) break;
      P = 2 * LS - P - 1; v->dir = 0;
    }
  }
  putP(v, P);
  return 1;
}

/* one frame at the very edge: next sample is chosen explicitly */
static void slow_frame(fm_voice_t *v, int32_t *a, const fm_range_t *r,
                       int32_t *gl, int32_t *gr, int32_t dl, int32_t dr,
                       int ramping)
{
  int32_t s0 = rd(v, v->pos), s1;
  uint32_t i1 = v->pos + 1;
  int64_t P;
  if (i1 < r->end) s1 = rd(v, i1);
  else if (r->loop && !r->pp) s1 = rd(v, r->ls);
  else s1 = s0;
  {
    int32_t s = lerp16(s0, s1, (int32_t)v->frac);
    if (ramping) { *gl += dl; *gr += dr; }
    a[0] += s * (*gl >> 8);
    a[1] += s * (*gr >> 8);
  }
  P = getP(v);
  P += v->dir ? -(int64_t)v->step : (int64_t)v->step;
  putP(v, P);
}

/* render n frames with the given gains (ramping or constant) */
static void render(fm_voice_t *v, int32_t *a, int n, int32_t *gl, int32_t *gr,
                   int32_t dl, int32_t dr, int ramping)
{
  fm_range_t r;
  setup(v, &r);
  while (n > 0 && (v->flags & FM_ACTIVE)) {
    int64_t cnt;
    if (!norm(v, &r)) { v->flags &= (uint8_t)~FM_ACTIVE; break; }
    if (v->pos + 1 >= r.end) {          /* sitting on the last frame */
      slow_frame(v, a, &r, gl, gr, dl, dr, ramping);
      a += 2; n--;
      continue;
    }

    if (v->step == 0) {
      cnt = n;
    } else if (v->dir == 0) {
      int64_t L = (int64_t)(r.end - 1) * 65536;
      cnt = (L - getP(v) + v->step - 1) / v->step;
    } else {
      cnt = (getP(v) - (int64_t)r.ls * 65536) / v->step + 1;
    }
    if (cnt > n) cnt = n;
    {
      uint32_t pos = v->pos, frac = v->frac;
      kernel(a, v, ramping, &pos, &frac, (int)cnt, gl, gr, dl, dr);
      v->pos = pos; v->frac = frac;
    }
    n -= (int)cnt;
    a += 2 * cnt;
  }
}

/* move a voice forward without mixing (silent / dropped voices) */
static void advance_voice(fm_voice_t *v, int n)
{
  fm_range_t r;
  int64_t P, d;
  setup(v, &r);
  d = (int64_t)v->step * n;
  P = getP(v) + (v->dir ? -d : d);
  putP(v, P);
  if (!norm(v, &r)) v->flags &= (uint8_t)~FM_ACTIVE;
}

/* ------------------------------------------------------------------ */

static void mix_voice(fm_voice_t *v, int32_t *a, int n)
{
  int32_t vol = v->volume, pan = v->pan;
  int32_t tl, tr, gl, gr;
  if (vol < 0) vol = 0; else if (vol > 256) vol = 256;
  if (pan < -128) pan = -128; else if (pan > 127) pan = 127;
  /* gain = vol * (127-pan)/255 etc., rounded, Q8 -> Q16 */
  tl = ((vol * (127 - pan) * 257 + 32768) >> 16) << 8;
  tr = ((vol * (pan + 128) * 257 + 32768) >> 16) << 8;

  if (v->ramp == 0) { v->cur_l = tl; v->cur_r = tr; }
  if (tl == 0 && tr == 0 && v->cur_l == 0 && v->cur_r == 0 && v->ramp == 0) {
    advance_voice(v, n);
    return;
  }
  gl = v->cur_l; gr = v->cur_r;

  if (v->ramp) {
    int rs = v->ramp < n ? v->ramp : n;
    int32_t dl = (tl - gl) / (int32_t)v->ramp;
    int32_t dr = (tr - gr) / (int32_t)v->ramp;
    render(v, a, rs, &gl, &gr, dl, dr, 1);
    v->ramp = (uint16_t)(v->ramp - rs);
    if (v->ramp == 0) { gl = tl; gr = tr; }
    a += 2 * rs;
    n -= rs;
    if (!(v->flags & FM_ACTIVE)) { v->cur_l = gl; v->cur_r = gr; return; }
  }
  if (n > 0) render(v, a, n, &gl, &gr, 0, 0, 0);
  v->cur_l = gl; v->cur_r = gr;
}

void fm_reset(void) { max_mixed = 32; }

void fm_set_max_mixed(int n)
{
  if (n < 1) n = 1;
  if (n > FM_MAX_VOICES) n = FM_MAX_VOICES;
  max_mixed = n;
}

void fm_voice_init(fm_voice_t *v)
{
  uint8_t *p = (uint8_t *)v;
  unsigned i;
  for (i = 0; i < sizeof(*v); i++) p[i] = 0;
}

void fm_voice_set_gain(fm_voice_t *v, int32_t volume, int32_t pan,
                       uint16_t ramp_frames)
{
  v->volume = volume;
  v->pan = pan;
  v->ramp = v->ramp_len = ramp_frames;
}

void fm_mix(int16_t *dst, int frames, fm_voice_t *voices, int nvoices)
{
  uint8_t order[FM_MAX_VOICES];
  uint8_t mixed[FM_MAX_VOICES];
  int na = 0, nm, i, k;

  if (nvoices > FM_MAX_VOICES) nvoices = FM_MAX_VOICES;
  for (i = 0; i < nvoices; i++) {
    mixed[i] = 0;
    if (voices[i].flags & FM_ACTIVE) order[na++] = (uint8_t)i;
  }
  nm = na;
  if (na > max_mixed) {            /* keep the loudest, stable by index */
    for (i = 1; i < na; i++) {
      uint8_t x = order[i];
      int32_t vx = voices[x].volume;
      k = i - 1;
      while (k >= 0 && voices[order[k]].volume < vx) {
        order[k + 1] = order[k];
        k--;
      }
      order[k + 1] = x;
    }
    nm = max_mixed;
  }
  for (k = 0; k < nm; k++) mixed[order[k]] = 1;

  while (frames > 0) {
    int n = frames < FM_CHUNK ? frames : FM_CHUNK;
    int cnt = n * 2;
    for (i = 0; i < cnt; i++) acc[i] = 0;
    for (i = 0; i < nvoices; i++) {
      fm_voice_t *v = &voices[i];
      if (!(v->flags & FM_ACTIVE)) continue;
      if (mixed[i]) mix_voice(v, acc, n);
      else advance_voice(v, n);
    }
    for (i = 0; i < cnt; i++) {
      int32_t x = acc[i] >> 8;
      if (x > 32767) x = 32767; else if (x < -32768) x = -32768;
      dst[i] = (int16_t)x;
    }
    dst += cnt;
    frames -= n;
  }
}
