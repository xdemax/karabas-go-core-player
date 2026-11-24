/* it_parser.c - see it_parser.h. Integer only, bounds-checked everywhere. */
#include "it_parser.h"
#include <string.h>

#define U16(p) ((uint16_t)((p)[0] | ((p)[1] << 8)))
#define U32(p) ((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) | \
                ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))

#define IT_MAX_SAMPLE_LEN 0x4000000u   /* 64M samples: overflow guard only */
#define IT_HDR_SIZE       0xC0u
#define IT_INS_SIZE       554u
#define IT_SMP_SIZE       80u
#define IT_PAT_HDR        8u

/* ======================================================================= */
/* strings                                                                 */
/* ======================================================================= */
const char *it_err_str(int c)
{
    switch (c) {
    case IT_OK:             return "ok";
    case IT_ERR_ARGS:       return "bad arguments";
    case IT_ERR_TOO_SMALL:  return "file too small";
    case IT_ERR_MAGIC:      return "bad magic (not IMPM/IMPI/IMPS)";
    case IT_ERR_VERSION:    return "unsupported Cmwt";
    case IT_ERR_COUNTS:     return "counts out of range";
    case IT_ERR_TRUNC:      return "header/offset table beyond end of file";
    case IT_ERR_NOMEM:      return "arena exhausted";
    case IT_ERR_INSTR:      return "damaged instrument";
    case IT_ERR_SAMPLE:     return "damaged sample header";
    case IT_ERR_SAMPLE_DATA:return "sample data out of file";
    case IT_ERR_COMPRESS:   return "damaged compressed sample";
    case IT_ERR_PATTERN:    return "damaged pattern";
    }
    return "?";
}

const char *it_warn_str(int c)
{
    switch (c) {
    case IT_W_CMWT_UNKNOWN:    return "Cmwt outside 0x100..0x217";
    case IT_W_ORDERS_CLAMPED:  return "order list clamped";
    case IT_W_ORDER_BAD_PAT:   return "order refers to missing pattern";
    case IT_W_HISTORY_BAD:     return "edit history outside file";
    case IT_W_MIDICFG_BAD:     return "embedded MIDI config outside file";
    case IT_W_MESSAGE_BAD:     return "message outside file";
    case IT_W_INSTR_SKIPPED:   return "instrument skipped";
    case IT_W_ENV_FIXED:       return "envelope repaired";
    case IT_W_SAMPLE_SKIPPED:  return "sample skipped";
    case IT_W_SAMPLE_TRUNC:    return "sample data truncated";
    case IT_W_LOOP_FIXED:      return "loop repaired/disabled";
    case IT_W_C5SPEED_ZERO:    return "C5Speed 0 -> 8363";
    case IT_W_IT215_MISMATCH:  return "IT214/215 flag disagrees with Cmwt";
    case IT_W_PATTERN_SKIPPED: return "pattern header unusable";
    case IT_W_PATTERN_TRUNC:   return "pattern truncated";
    case IT_W_PATTERN_ROWS:    return "pattern rows outside 32..200";
    case IT_W_BAD_COMMAND:     return "effect command > Z";
    }
    return "?";
}

/* ======================================================================= */
/* arena                                                                   */
/* ======================================================================= */
void it_arena_init(it_arena_t *a, void *mem, size_t size)
{
    a->base = (uint8_t *)mem;
    a->size = size;
    a->used = 0;
}

static void *aalloc(it_arena_t *a, size_t n)
{
    uintptr_t cur = (uintptr_t)a->base + a->used;
    uintptr_t al  = (cur + 7u) & ~(uintptr_t)7u;
    size_t pad = (size_t)(al - cur);
    if (a->used > a->size) return NULL;
    size_t left = a->size - a->used;
    if (left < pad || left - pad < n) return NULL;
    a->used += pad + n;
    return (void *)al;
}

static void *aalloc0(it_arena_t *a, size_t n)
{
    void *p = aalloc(a, n);
    if (p && n) memset(p, 0, n);
    return p;
}

/* ======================================================================= */
/* context + diagnostics                                                   */
/* ======================================================================= */
typedef struct {
    const uint8_t *f;
    size_t         size;
    it_arena_t    *meta, *pcm;
    uint32_t       flags;
    it_diag_t     *dg;
    it_module_t   *m;
} ctx_t;

static int fail(ctx_t *c, int code, int pat, int row, int ch, int idx, uint32_t off)
{
    if (c->dg) {
        c->dg->err = code; c->dg->pattern = pat; c->dg->row = row;
        c->dg->channel = ch; c->dg->index = idx; c->dg->offset = off;
    }
    return code;
}

static void warn(ctx_t *c, int code, int idx, int row, int ch, uint32_t off)
{
    it_diag_t *d = c->dg;
    if (!d) return;
    d->nwarn_total++;
    if (d->nwarn < IT_MAX_WARN) {
        it_warn_t *w = &d->warn[d->nwarn++];
        w->code = (uint16_t)code; w->index = (int16_t)idx;
        w->row = (int16_t)row; w->ch = (int16_t)ch; w->offset = off;
    }
}

#define STRICT(c) (((c)->flags & IT_LOAD_STRICT) != 0)

static void copy_name(char *dst, const uint8_t *src, unsigned n)
{
    unsigned i;
    for (i = 0; i < n && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = 0;
}

/* ======================================================================= */
/* IT214 / IT215 decompression                                             */
/* ======================================================================= */
typedef struct { const uint8_t *p, *end; uint32_t buf; int nbits; } bits_t;

static int rd_bits(bits_t *b, int n, uint32_t *out)
{
    while (b->nbits < n) {
        if (b->p >= b->end) return -1;
        b->buf |= (uint32_t)*b->p++ << b->nbits;
        b->nbits += 8;
    }
    *out = b->buf & ((1u << n) - 1u);
    b->buf >>= n;
    b->nbits -= n;
    return 0;
}

/* Decompress n samples (8 or 16 bit) from src. *used = bytes consumed. */
static int it_decompress(const uint8_t *src, size_t srclen, void *dst,
                         uint32_t n, int is16, int it215, size_t *used)
{
    const int      w0     = is16 ? 17 : 9;
    const int      cbits  = is16 ? 4 : 3;
    const uint32_t blkmax = is16 ? 0x4000u : 0x8000u;
    const int      top    = is16 ? 16 : 8;
    const uint32_t span   = is16 ? 16u : 8u;
    const uint32_t full   = is16 ? 0xFFFFu : 0xFFu;
    size_t   pos  = 0;
    uint32_t done = 0;

    while (done < n) {
        if (srclen - pos < 2) return -1;
        uint32_t clen = U16(src + pos);
        pos += 2;
        if (clen > srclen - pos) return -1;

        uint32_t blk = n - done;
        if (blk > blkmax) blk = blkmax;

        bits_t b; b.p = src + pos; b.end = src + pos + clen; b.buf = 0; b.nbits = 0;
        int width = w0;
        int32_t d1 = 0, d2 = 0;
        uint32_t k = 0;

        while (k < blk) {
            uint32_t v;
            if (rd_bits(&b, width, &v)) return -1;

            if (width < 7) {
                if (v == (1u << (width - 1))) {
                    uint32_t t;
                    if (rd_bits(&b, cbits, &t)) return -1;
                    t += 1;
                    width = (t < (uint32_t)width) ? (int)t : (int)t + 1;
                    continue;
                }
            } else if (width < w0) {
                uint32_t border = (full >> (w0 - width)) - span / 2u;
                if (v > border && v <= border + span) {
                    v -= border;
                    width = (v < (uint32_t)width) ? (int)v : (int)v + 1;
                    continue;
                }
            } else if (width == w0) {
                if (v & (1u << (w0 - 1))) {
                    width = (int)((v + 1u) & 0xFFu);
                    if (width < 1 || width > w0) return -1;
                    continue;
                }
            } else {
                return -1;
            }

            int sw = width > top ? top : width;
            uint32_t m = 1u << (sw - 1);
            int32_t val = (int32_t)((v ^ m) - m);

            if (is16) {
                d1 = (int16_t)(d1 + val);
                d2 = (int16_t)(d2 + d1);
                ((int16_t *)dst)[done + k] = (int16_t)(it215 ? d2 : d1);
            } else {
                d1 = (int8_t)(d1 + val);
                d2 = (int8_t)(d2 + d1);
                ((int8_t *)dst)[done + k] = (int8_t)(it215 ? d2 : d1);
            }
            k++;
        }
        pos  += clen;
        done += blk;
    }
    *used = pos;
    return 0;
}

/* ======================================================================= */
/* envelopes                                                               */
/* ======================================================================= */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void env_finish(ctx_t *c, it_env_t *e, int idx, int kind /*0 vol,1 pan,2 pitch*/)
{
    int fixed = 0;
    unsigned i;
    (void)kind;
    if (e->num > IT_ENV_NODES) { e->num = IT_ENV_NODES; fixed = 1; }
    for (i = 1; i < e->num; i++)
        if (e->node[i].tick < e->node[i - 1].tick) { e->node[i].tick = e->node[i - 1].tick; fixed = 1; }
    if (e->num == 0) { e->flags &= (uint8_t)~IT_ENV_ON; e->lpb = e->lpe = e->slb = e->sle = 0; }
    else {
        if ((e->flags & IT_ENV_LOOP) && (e->lpb > e->lpe || e->lpe >= e->num)) {
            e->flags &= (uint8_t)~IT_ENV_LOOP; fixed = 1;
        }
        if ((e->flags & IT_ENV_SUSLOOP) && (e->slb > e->sle || e->sle >= e->num)) {
            e->flags &= (uint8_t)~IT_ENV_SUSLOOP; fixed = 1;
        }
    }
    if (fixed) warn(c, IT_W_ENV_FIXED, idx, kind, 0, 0);
}

static void env_new(ctx_t *c, it_env_t *e, const uint8_t *p, int idx, int kind)
{
    unsigned i;
    int filter = (kind == 2) && (p[0] & IT_ENV_FILTER);
    e->flags = p[0]; e->num = p[1];
    e->lpb = p[2]; e->lpe = p[3]; e->slb = p[4]; e->sle = p[5];
    for (i = 0; i < IT_ENV_NODES; i++) {
        int raw = (int8_t)p[6 + i * 3];
        int v;
        if (kind == 0)      v = clampi((int)p[6 + i * 3], 0, 64);
        else if (filter)    v = clampi(raw + 32, 0, 64);
        else                v = clampi(raw, -32, 32);
        e->node[i].value = (int16_t)v;
        e->node[i].tick  = U16(p + 7 + i * 3);
    }
    if (kind != 2) e->flags &= (uint8_t)~IT_ENV_FILTER;
    env_finish(c, e, idx, kind);
}

/* ======================================================================= */
/* instruments                                                             */
/* ======================================================================= */
static int load_instrument(ctx_t *c, unsigned n /*0-based*/, uint32_t off)
{
    it_instrument_t *in = &c->m->ins[n];
    const uint8_t *p;
    unsigned i;
    int idx = (int)n + 1;

    if (off == 0) return 0;
    if ((uint64_t)off + IT_INS_SIZE > c->size || memcmp(c->f + off, "IMPI", 4) != 0) {
        if (STRICT(c)) return fail(c, IT_ERR_INSTR, -1, 0, 0, idx, off);
        warn(c, IT_W_INSTR_SKIPPED, idx, 0, 0, off);
        return 0;
    }
    p = c->f + off;
    in->present = 1;
    copy_name(in->dosname, p + 4, 12);
    copy_name(in->name, p + 0x20, 26);
    for (i = 0; i < 120; i++) {
        uint8_t note = p[0x40 + i * 2], smp = p[0x41 + i * 2];
        in->kb_note[i] = note > 119 ? (uint8_t)i : note;
        in->kb_smp[i]  = smp;
    }

    if (!c->m->old_instruments) {
        in->nna = p[0x11]; in->dct = p[0x12]; in->dca = p[0x13];
        in->fadeout = U16(p + 0x14);
        in->pps = (int8_t)p[0x16]; in->ppc = p[0x17];
        in->gbv = p[0x18] > 128 ? 128 : p[0x18];
        in->dfp = p[0x19]; in->rv = p[0x1A]; in->rp = p[0x1B];
        in->trkvers = U16(p + 0x1C);
        in->ifc = p[0x3A]; in->ifr = p[0x3B];
        in->mch = p[0x3C]; in->mpr = p[0x3D]; in->mbank = U16(p + 0x3E);
        env_new(c, &in->vol,   p + 0x130, idx, 0);
        env_new(c, &in->pan,   p + 0x182, idx, 1);
        env_new(c, &in->pitch, p + 0x1D4, idx, 2);
    } else {
        /* pre-2.00 layout (see docs: verify against real old files) */
        unsigned k;
        it_env_t *e = &in->vol;
        in->old_format = 1;
        in->nna = p[0x1A];
        in->dct = p[0x1B] ? IT_DCT_NOTE : IT_DCT_OFF;
        in->dca = IT_DCA_CUT;
        in->fadeout = U16(p + 0x18);
        in->gbv = 128; in->dfp = 0x80; in->ppc = 60;
        in->trkvers = U16(p + 0x1C);
        e->flags = (uint8_t)(p[0x11] & 0x07);
        e->lpb = p[0x12]; e->lpe = p[0x13]; e->slb = p[0x14]; e->sle = p[0x15];
        for (k = 0; k < IT_ENV_NODES; k++) {
            uint8_t tick = p[0x1F8 + k * 2], val = p[0x1F9 + k * 2];
            if (tick == 0xFF) break;
            e->node[k].tick  = tick;
            e->node[k].value = (int16_t)clampi(val, 0, 64);
        }
        e->num = (uint8_t)k;
        env_finish(c, e, idx, 0);
    }
    return 0;
}

/* ======================================================================= */
/* samples                                                                 */
/* ======================================================================= */
static void fix_loop(ctx_t *c, int idx, uint32_t len, uint32_t *b, uint32_t *e,
                     uint8_t on, uint8_t pp, uint8_t *mode)
{
    *mode = IT_LOOP_NONE;
    if (!on) return;
    if (*e > len) { *e = len; warn(c, IT_W_LOOP_FIXED, idx, 0, 0, 0); }
    if (*b >= *e) { warn(c, IT_W_LOOP_FIXED, idx, 1, 0, 0); return; }
    *mode = pp ? IT_LOOP_PINGPONG : IT_LOOP_FWD;
}

static int load_sample(ctx_t *c, unsigned n /*0-based*/, uint32_t off)
{
    it_sample_t *s = &c->m->smp[n];
    const uint8_t *p;
    int idx = (int)n + 1;
    uint32_t ptr;
    unsigned bps, ch, k;
    uint64_t need;
    size_t mark_pcm;
    it_arena_t *pa = c->pcm ? c->pcm : c->meta;

    if (off == 0) return 0;
    if ((uint64_t)off + IT_SMP_SIZE > c->size || memcmp(c->f + off, "IMPS", 4) != 0) {
        if (STRICT(c)) return fail(c, IT_ERR_SAMPLE, -1, 0, 0, idx, off);
        warn(c, IT_W_SAMPLE_SKIPPED, idx, 0, 0, off);
        return 0;
    }
    p = c->f + off;
    copy_name(s->dosname, p + 4, 12);
    copy_name(s->name, p + 0x14, 26);
    s->gvl = p[0x11] > 64 ? 64 : p[0x11];
    s->flags = p[0x12];
    s->vol = p[0x13] > 64 ? 64 : p[0x13];
    s->cvt = p[0x2E];
    s->dfp = p[0x2F];
    s->length = U32(p + 0x30);
    s->loop_begin = U32(p + 0x34); s->loop_end = U32(p + 0x38);
    s->c5speed = U32(p + 0x3C);
    s->sus_begin = U32(p + 0x40);  s->sus_end = U32(p + 0x44);
    ptr = U32(p + 0x48);
    s->data_off = ptr;
    s->vis = p[0x4C]; s->vid = p[0x4D]; s->vir = p[0x4E]; s->vit = p[0x4F];
    if (s->c5speed == 0) { s->c5speed = 8363; warn(c, IT_W_C5SPEED_ZERO, idx, 0, 0, off); }

    if (!(s->flags & IT_SMP_DATA) || s->length == 0) return 0;
    if (s->length > IT_MAX_SAMPLE_LEN) {
        if (STRICT(c)) return fail(c, IT_ERR_SAMPLE, -1, 0, 0, idx, off);
        warn(c, IT_W_SAMPLE_SKIPPED, idx, 1, 0, off);
        return 0;
    }

    s->bits = (s->flags & IT_SMP_16BIT) ? 16 : 8;
    s->channels = (s->flags & IT_SMP_STEREO) ? 2 : 1;
    bps = s->bits / 8u; ch = s->channels;
    mark_pcm = pa->used;

    if (s->flags & IT_SMP_COMPRESSED) {
        int it215 = (s->cvt & IT_CVT_DELTA) != 0;
        const uint8_t *src; size_t srclen, pos = 0;
        void *buf[2] = { NULL, NULL };
        if (ptr >= c->size) goto data_bad;
        src = c->f + ptr; srclen = c->size - ptr;
        if (c->m->cmwt >= 0x215 && !it215 && c->m->cmwt <= 0x217) warn(c, IT_W_IT215_MISMATCH, idx, 0, 0, off);
        for (k = 0; k < ch; k++) {
            size_t used;
            buf[k] = aalloc(pa, (size_t)s->length * bps);
            if (!buf[k]) { pa->used = mark_pcm; return fail(c, IT_ERR_NOMEM, -1, 0, 0, idx, off); }
            if (it_decompress(src + pos, srclen - pos, buf[k], s->length, s->bits == 16, it215, &used)) {
                pa->used = mark_pcm;
                if (STRICT(c)) return fail(c, IT_ERR_COMPRESS, -1, 0, 0, idx, ptr);
                warn(c, IT_W_SAMPLE_SKIPPED, idx, 2, 0, ptr);
                goto drop;
            }
            pos += used;
        }
        s->pcm = buf[0]; s->pcm_r = buf[1];
    } else {
        need = (uint64_t)s->length * bps * ch;
        if ((uint64_t)ptr + need > c->size) {
            if (ch == 1 && ptr < c->size && !STRICT(c)) {
                s->length = (uint32_t)((c->size - ptr) / bps);
                warn(c, IT_W_SAMPLE_TRUNC, idx, 0, 0, ptr);
                if (s->length == 0) goto drop;
            } else goto data_bad;
        }
        {
            int zc = !(c->flags & IT_LOAD_NOZEROCOPY) && (s->cvt & IT_CVT_SIGNED) &&
                     !(s->cvt & (IT_CVT_DELTA | IT_CVT_BIGENDIAN));
            for (k = 0; k < ch; k++) {
                const uint8_t *src = c->f + ptr + (size_t)k * s->length * bps;
                const void *out;
                if (zc && (bps == 1 || (((uintptr_t)src & 1u) == 0))) {
                    out = src;
                } else {
                    uint32_t i;
                    void *buf = aalloc(pa, (size_t)s->length * bps);
                    if (!buf) { pa->used = mark_pcm; return fail(c, IT_ERR_NOMEM, -1, 0, 0, idx, off); }
                    if (bps == 1) {
                        int8_t *o = (int8_t *)buf; int acc = 0;
                        for (i = 0; i < s->length; i++) {
                            int v = src[i];
                            if (s->cvt & IT_CVT_DELTA) { acc = (int8_t)(acc + v); v = acc; }
                            if (!(s->cvt & IT_CVT_SIGNED)) v ^= 0x80;
                            o[i] = (int8_t)v;
                        }
                    } else {
                        int16_t *o = (int16_t *)buf; int acc = 0;
                        for (i = 0; i < s->length; i++) {
                            int v = (s->cvt & IT_CVT_BIGENDIAN) ? ((src[i * 2] << 8) | src[i * 2 + 1])
                                                                : (src[i * 2] | (src[i * 2 + 1] << 8));
                            v = (int16_t)v;
                            if (s->cvt & IT_CVT_DELTA) { acc = (int16_t)(acc + v); v = acc; }
                            if (!(s->cvt & IT_CVT_SIGNED)) v ^= 0x8000;
                            o[i] = (int16_t)v;
                        }
                    }
                    out = buf;
                }
                if (k == 0) s->pcm = out; else s->pcm_r = out;
            }
        }
    }

    fix_loop(c, idx, s->length, &s->loop_begin, &s->loop_end,
             (s->flags & IT_SMP_LOOP) != 0, (s->flags & IT_SMP_PINGPONG) != 0, &s->loop_mode);
    fix_loop(c, idx, s->length, &s->sus_begin, &s->sus_end,
             (s->flags & IT_SMP_SUSLOOP) != 0, (s->flags & IT_SMP_SUSPINGPONG) != 0, &s->sus_mode);
    return 0;

data_bad:
    pa->used = mark_pcm;
    if (STRICT(c)) return fail(c, IT_ERR_SAMPLE_DATA, -1, 0, 0, idx, ptr);
    warn(c, IT_W_SAMPLE_SKIPPED, idx, 3, 0, ptr);
drop:
    s->pcm = s->pcm_r = NULL; s->bits = 0; s->channels = 0; s->length = 0;
    return 0;
}

/* ======================================================================= */
/* pattern decoder                                                         */
/* ======================================================================= */
void it_patdec_init(it_patdec_t *d, const it_pattern_t *pat)
{
    unsigned i;
    d->p = pat->data; d->end = pat->data ? pat->data + pat->len : pat->data;
    d->row = 0; d->rows = pat->rows; d->valid = pat->valid_rows;
    d->empty = pat->data == NULL; d->last_ch = 0;
    for (i = 0; i < IT_MAX_CHANNELS; i++) {
        d->lastmask[i] = d->lastnote[i] = d->lastinst[i] = 0;
        d->lastvol[i] = d->lastcmd[i] = d->lastparam[i] = 0;
    }
}

int it_patdec_row(it_patdec_t *d, it_cell_t cells[IT_MAX_CHANNELS], uint32_t present[2])
{
    present[0] = present[1] = 0;
    if (d->row >= d->rows) return IT_ROW_END;
    if (d->empty || d->row >= d->valid) { d->row++; return IT_ROW_OK; }

    for (;;) {
        unsigned ch, mask;
        uint8_t fl = 0;
        it_cell_t *c;
        if (d->p >= d->end) goto bad;
        {
            unsigned cv = *d->p++;
            if (cv == 0) break;
            ch = (cv - 1u) & 63u;
            d->last_ch = (uint8_t)ch;
            if (cv & 0x80u) {
                if (d->p >= d->end) goto bad;
                d->lastmask[ch] = *d->p++;
            }
        }
        mask = d->lastmask[ch];
        if (mask & 0x01u) { if (d->p >= d->end) goto bad; d->lastnote[ch] = *d->p++; }
        if (mask & 0x02u) { if (d->p >= d->end) goto bad; d->lastinst[ch] = *d->p++; }
        if (mask & 0x04u) { if (d->p >= d->end) goto bad; d->lastvol[ch]  = *d->p++; }
        if (mask & 0x08u) {
            if (d->end - d->p < 2) goto bad;
            d->lastcmd[ch] = d->p[0]; d->lastparam[ch] = d->p[1]; d->p += 2;
        }
        c = &cells[ch];
        if (mask & 0x11u) { c->note = d->lastnote[ch]; fl |= IT_CF_NOTE; }
        if (mask & 0x22u) { c->inst = d->lastinst[ch]; fl |= IT_CF_INST; }
        if (mask & 0x44u) { c->vol  = d->lastvol[ch];  fl |= IT_CF_VOL; }
        if (mask & 0x88u) { c->cmd = d->lastcmd[ch]; c->param = d->lastparam[ch]; fl |= IT_CF_CMD; }
        if (fl) { c->flags = fl; present[ch >> 5] |= 1u << (ch & 31u); }
    }
    d->row++;
    return IT_ROW_OK;
bad:
    d->valid = d->row;      /* later rows come back empty */
    d->row++;
    return IT_ROW_CORRUPT;
}

void it_patdec_seek(it_patdec_t *d, const it_pattern_t *pat, unsigned row)
{
    it_cell_t scratch[IT_MAX_CHANNELS];
    uint32_t pr[2];
    it_patdec_init(d, pat);
    while (d->row < row && d->row < d->rows) it_patdec_row(d, scratch, pr);
}

/* parse-time scan: validates the pattern and gathers statistics */
static void scan_pattern(ctx_t *c, unsigned pn, it_pattern_t *pt, uint32_t file_off,
                         uint32_t *chan_or)
{
    it_patdec_t d;
    it_cell_t cells[IT_MAX_CHANNELS];
    uint32_t pr[2];
    int bad_cmd_row = -1, bad_cmd_ch = 0;
    int rc = IT_ROW_OK;

    pt->valid_rows = pt->rows;
    it_patdec_init(&d, pt);
    d.valid = pt->rows;
    while (d.row < d.rows) {
        unsigned w, b;
        unsigned row = d.row;
        rc = it_patdec_row(&d, cells, pr);
        if (rc != IT_ROW_OK) { pt->valid_rows = (uint16_t)row; break; }
        for (w = 0; w < 2; w++) {
            uint32_t bits = pr[w];
            chan_or[w] |= bits;
            for (b = 0; bits; b++, bits >>= 1) {
                if (!(bits & 1u)) continue;
                const it_cell_t *cc = &cells[w * 32 + b];
                if ((cc->flags & IT_CF_CMD) && cc->cmd > 26 && bad_cmd_row < 0) {
                    bad_cmd_row = (int)row; bad_cmd_ch = (int)(w * 32 + b);
                }
            }
        }
    }
    if (rc != IT_ROW_OK)
        warn(c, IT_W_PATTERN_TRUNC, (int)pn, pt->valid_rows, d.last_ch, file_off);
    if (bad_cmd_row >= 0)
        warn(c, IT_W_BAD_COMMAND, (int)pn, bad_cmd_row, bad_cmd_ch, file_off);
}

static int load_pattern(ctx_t *c, unsigned n, uint32_t off, uint32_t *chan_or)
{
    it_pattern_t *pt = &c->m->pat[n];
    uint32_t len, rows, avail;

    pt->data = NULL; pt->len = 0; pt->rows = 64; pt->valid_rows = 64;
    if (off == 0) return 0;                       /* empty 64-row pattern */

    if ((uint64_t)off + IT_PAT_HDR > c->size) {
        if (STRICT(c)) return fail(c, IT_ERR_PATTERN, (int)n, 0, 0, 0, off);
        warn(c, IT_W_PATTERN_SKIPPED, (int)n, 0, 0, off);
        return 0;
    }
    len = U16(c->f + off); rows = U16(c->f + off + 2);
    if (rows < 1 || rows > IT_MAX_ROWS) {
        if (STRICT(c)) return fail(c, IT_ERR_PATTERN, (int)n, 0, 0, 0, off);
        warn(c, IT_W_PATTERN_SKIPPED, (int)n, 0, 0, off);
        return 0;
    }
    if (rows < 32 || rows > 200) warn(c, IT_W_PATTERN_ROWS, (int)n, (int)rows, 0, off);
    avail = (uint32_t)(c->size - off - IT_PAT_HDR);
    if (len > avail) {
        if (STRICT(c)) return fail(c, IT_ERR_PATTERN, (int)n, 0, 0, 0, off);
        len = avail;
        warn(c, IT_W_PATTERN_TRUNC, (int)n, -1, 0, off);
    }
    pt->rows = (uint16_t)rows;
    if (len == 0) { pt->valid_rows = pt->rows; return 0; }   /* all empty rows */
    pt->data = c->f + off + IT_PAT_HDR;
    pt->len  = (uint16_t)len;

    scan_pattern(c, n, pt, off, chan_or);
    if (pt->valid_rows < pt->rows && STRICT(c))
        return fail(c, IT_ERR_PATTERN, (int)n, pt->valid_rows, 0, 0, off);
    return 0;
}

/* ======================================================================= */
/* top-level loader                                                        */
/* ======================================================================= */
int it_load(const uint8_t *f, size_t size, it_arena_t *meta, it_arena_t *pcm,
            uint32_t flags, it_diag_t *dg, it_module_t *m)
{
    ctx_t c;
    unsigned i, ordnum;
    size_t tab, tabend, tail;
    const uint8_t *offs;
    uint32_t chan_or[2] = { 0, 0 };
    int rc;

    if (dg) memset(dg, 0, sizeof *dg), dg->pattern = -1;
    if (!f || !meta || !m) return IT_ERR_ARGS;
    memset(m, 0, sizeof *m);
    c.f = f; c.size = size; c.meta = meta; c.pcm = pcm; c.flags = flags; c.dg = dg; c.m = m;

    if (size < IT_HDR_SIZE) return fail(&c, IT_ERR_TOO_SMALL, -1, 0, 0, 0, 0);
    if (memcmp(f, "IMPM", 4) != 0) return fail(&c, IT_ERR_MAGIC, -1, 0, 0, 0, 0);

    m->file = f; m->file_size = size;
    copy_name(m->title, f + 4, 26);
    m->ordnum = U16(f + 0x20); m->insnum = U16(f + 0x22);
    m->smpnum = U16(f + 0x24); m->patnum = U16(f + 0x26);
    m->cwt = U16(f + 0x28);    m->cmwt = U16(f + 0x2A);
    m->flags = U16(f + 0x2C);  m->special = U16(f + 0x2E);
    m->gv = f[0x30] > 128 ? 128 : f[0x30];
    m->mv = f[0x31] > 128 ? 128 : f[0x31];
    m->speed = f[0x32] ? f[0x32] : 6;
    m->tempo = f[0x33] < 32 ? 125 : f[0x33];
    m->sep = f[0x34] > 128 ? 128 : f[0x34];
    m->pwd = f[0x35];
    memcpy(m->chn_pan, f + 0x40, 64);
    memcpy(m->chn_vol, f + 0x80, 64);
    for (i = 0; i < 64; i++) if (m->chn_vol[i] > 64) m->chn_vol[i] = 64;

    if (m->cmwt < 0x100) return fail(&c, IT_ERR_VERSION, -1, 0, 0, 0, 0x2A);
    if (m->cmwt > 0x217) warn(&c, IT_W_CMWT_UNKNOWN, 0, 0, 0, 0x2A);
    m->old_instruments = m->cmwt < 0x200;

    if (m->insnum > IT_MAX_INSTR || m->smpnum > IT_MAX_SAMPLES ||
        m->patnum > IT_MAX_PATTERNS || m->ordnum > 1024)
        return fail(&c, IT_ERR_COUNTS, -1, 0, 0, 0, 0x20);

    ordnum = m->ordnum;
    tab = IT_HDR_SIZE + ordnum;
    tabend = tab + 4u * ((size_t)m->insnum + m->smpnum + m->patnum);
    if (tabend > size) return fail(&c, IT_ERR_TRUNC, -1, 0, 0, 0, (uint32_t)tab);

    /* orders */
    {
        unsigned keep = ordnum > IT_MAX_ORDERS ? IT_MAX_ORDERS : ordnum;
        if (keep != ordnum) warn(&c, IT_W_ORDERS_CLAMPED, 0, 0, 0, IT_HDR_SIZE);
        m->orders = (uint8_t *)aalloc0(meta, keep ? keep : 1);
        m->ins = (it_instrument_t *)aalloc0(meta, sizeof(it_instrument_t) * (m->insnum ? m->insnum : 1u));
        m->smp = (it_sample_t *)aalloc0(meta, sizeof(it_sample_t) * (m->smpnum ? m->smpnum : 1u));
        m->pat = (it_pattern_t *)aalloc0(meta, sizeof(it_pattern_t) * (m->patnum ? m->patnum : 1u));
        if (!m->orders || !m->ins || !m->smp || !m->pat) return fail(&c, IT_ERR_NOMEM, -1, 0, 0, 0, 0);
        memcpy(m->orders, f + IT_HDR_SIZE, keep);
        m->song_len = (uint16_t)keep;
        for (i = 0; i < keep; i++) if (m->orders[i] == 255) { m->song_len = (uint16_t)i; break; }
        m->ordnum = (uint16_t)keep;
    }

    /* message, history, embedded MIDI config */
    if ((m->special & IT_S_MESSAGE) && U16(f + 0x36)) {
        uint32_t mo = U32(f + 0x38), ml = U16(f + 0x36);
        if (mo < size) {
            m->message = f + mo;
            m->message_len = (uint16_t)((size - mo) < ml ? (size - mo) : ml);
        } else warn(&c, IT_W_MESSAGE_BAD, 0, 0, 0, mo);
    }
    tail = tabend;
    if (m->special & IT_S_HISTORY) {
        if (tail + 2 <= size) {
            size_t hl = 2u + 8u * (size_t)U16(f + tail);
            if (tail + hl <= size) tail += hl; else { warn(&c, IT_W_HISTORY_BAD, 0, 0, 0, (uint32_t)tail); tail = size + 1; }
        } else { warn(&c, IT_W_HISTORY_BAD, 0, 0, 0, (uint32_t)tail); tail = size + 1; }
    }
    if (m->special & IT_S_MIDICFG) {
        if (tail <= size && size - tail >= IT_MIDI_CFG_SIZE) m->midi_cfg = f + tail;
        else warn(&c, IT_W_MIDICFG_BAD, 0, 0, 0, (uint32_t)tail);
    }

    offs = f + tab;
    for (i = 0; i < m->insnum; i++)
        if ((rc = load_instrument(&c, i, U32(offs + 4u * i)))) return rc;
    offs += 4u * m->insnum;
    for (i = 0; i < m->smpnum; i++)
        if ((rc = load_sample(&c, i, U32(offs + 4u * i)))) return rc;
    offs += 4u * m->smpnum;
    for (i = 0; i < m->patnum; i++)
        if ((rc = load_pattern(&c, i, U32(offs + 4u * i), chan_or))) return rc;

    for (i = 0; i < m->ordnum; i++) {
        uint8_t o = m->orders[i];
        if (o == 255) break;
        if (o < 254 && o >= m->patnum) warn(&c, IT_W_ORDER_BAD_PAT, (int)i, 0, 0, 0);
    }
    {
        unsigned hi = 0, b;
        for (b = 0; b < 64; b++) if (chan_or[b >> 5] & (1u << (b & 31))) hi = b + 1;
        m->used_channels = (uint8_t)hi;
    }
    return IT_OK;
}
