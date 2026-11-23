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
 */
#include "fastmix.h"

#define FM_CHUNK 256

#if defined(__GNUC__)
#define FM_AI static inline __attribute__((always_inline))
#else
#define FM_AI static inline
#endif

static int32_t acc[FM_CHUNK * 2];
static int max_mixed = 32;

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

/* ------------------------------------------------------------------ */
/* kernels: n frames, no boundary inside                              */

FM_AI void kern(int32_t *a, const void *d, int is8, int bwd, int ramping,
                uint32_t *ppos, uint32_t *pfrac, uint32_t step, int n,
                int32_t *pgl, int32_t *pgr, int32_t dl, int32_t dr)
{
  const int16_t *p16 = (const int16_t *)d;
  const int8_t *p8 = (const int8_t *)d;
  uint32_t pos = *ppos;
  int32_t frac = (int32_t)*pfrac;
  const int32_t si = (int32_t)(step >> 16);
  const int32_t sf = (int32_t)(step & 0xFFFF);
  int32_t gl = *pgl, gr = *pgr;
  const int32_t gl8 = gl >> 8, gr8 = gr >> 8;

  while (n-- > 0) {
    int32_t s;
    if (is8) {
      int32_t s0 = p8[pos], s1 = p8[pos + 1];
      s = s0 * 256 + (((s1 - s0) * frac) >> 8);
    } else {
      s = lerp16(p16[pos], p16[pos + 1], frac);
    }
    if (ramping) {
      gl += dl;
      gr += dr;
      a[0] += s * (gl >> 8);
      a[1] += s * (gr >> 8);
    } else {
      a[0] += s * gl8;
      a[1] += s * gr8;
    }
    a += 2;
    if (!bwd) {
      frac += sf;
      pos += (uint32_t)si + (uint32_t)(frac >> 16);
      frac &= 0xFFFF;
    } else {
      frac -= sf;
      pos -= (uint32_t)si;
      if (frac < 0) { frac += 65536; pos--; }
    }
  }
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
  int key = ((v->flags & FM_8BIT) ? 4 : 0) | (v->dir ? 2 : 0) | (ramping ? 1 : 0);
#define K(E, B, R) kern(a, d, E, B, R, ppos, pfrac, st, n, gl, gr, dl, dr)
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
