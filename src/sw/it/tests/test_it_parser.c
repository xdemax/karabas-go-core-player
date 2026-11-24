#define _GNU_SOURCE
/*
 * Host tests for it_parser. Build with -fsanitize=address,undefined.
 *  1. IT214/IT215 decompressor against a reference bit-writer (all width paths)
 *  2. synthetic module: header, order list, instrument, 3 samples, pattern
 *  3. pattern decoder: mask reuse, empty rows, corruption
 *  4. every truncation length, strict and lenient
 *  5. random byte/offset corruption (fuzz), full decode of survivors
 */
#include "../it_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(x) do { g_checks++; if (!(x)) { g_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

/* ---- reference bit writer (LSB first) --------------------------------- */
typedef struct { uint8_t *p; size_t n; uint32_t acc; int nb; } bw_t;
static void bw_put(bw_t *w, uint32_t v, int n)
{
    w->acc |= v << w->nb; w->nb += n;
    while (w->nb >= 8) { w->p[w->n++] = (uint8_t)w->acc; w->acc >>= 8; w->nb -= 8; }
}
static void bw_flush(bw_t *w) { if (w->nb) { w->p[w->n++] = (uint8_t)w->acc; w->acc = 0; w->nb = 0; } }

/*
 * Encode 8-bit signal x[0..n) as ONE block, exercising: width 9 raw,
 * 9->5 switch (escape), 5-bit values, 5->9 escape in the <7 branch,
 * 9->8, 8-bit values, 8->9 in the 7..8 branch. d = deltas for IT214,
 * second differences for IT215.
 */
static size_t enc8(const int8_t *x, unsigned n, int it215, uint8_t *out)
{
    bw_t w = { out + 2, 0, 0, 0 };
    int width = 9, d1 = 0, prev = 0;
    unsigned i;
    for (i = 0; i < n; i++) {
        int dlt, v;
        if (it215) { int dd = x[i] - prev; v = (int8_t)(dd - d1); d1 = dd; prev = x[i]; v = (int8_t)v; }
        else       { v = (int8_t)(x[i] - prev); prev = x[i]; }
        dlt = v;
        /* schedule of width switches by position */
        if (i == n / 5 && width == 9)      { bw_put(&w, 0x100u | 4u, 9); width = 5; }
        else if (i == 2 * n / 5 && width == 5) { bw_put(&w, 16u, 5); bw_put(&w, 7u, 3); width = 9; }
        else if (i == 3 * n / 5 && width == 9) { bw_put(&w, 0x100u | 7u, 9); width = 8; }
        else if (i == 4 * n / 5 && width == 8) { bw_put(&w, 131u, 8); width = 9; }
        /* only use narrow widths when the value fits */
        if (width == 5 && (dlt < -15 || dlt > 15)) { /* leave 5-bit mode */
            bw_put(&w, 16u, 5); bw_put(&w, 7u, 3); width = 9;
        }
        if (width == 8 && ((dlt >= 124 && dlt <= 127) || (dlt >= -128 && dlt <= -125))) {
            bw_put(&w, 131u, 8); width = 9;
        }
        bw_put(&w, (uint32_t)dlt & ((1u << (width > 8 ? 8 : width)) - 1u), width);
    }
    bw_flush(&w);
    put16(out, (unsigned)w.n);
    return w.n + 2;
}

static size_t enc16(const int16_t *x, unsigned n, int it215, uint8_t *out)
{
    bw_t w = { out + 2, 0, 0, 0 };
    int width = 17, d1 = 0, prev = 0;
    unsigned i;
    for (i = 0; i < n; i++) {
        int v;
        if (it215) { int dd = (int16_t)(x[i] - prev); v = (int16_t)(dd - d1); d1 = dd; prev = x[i]; }
        else       { v = (int16_t)(x[i] - prev); prev = x[i]; }
        if (i == n / 3 && width == 17)     { bw_put(&w, 0x10000u | 11u, 17); width = 12; }
        if (width == 12 && (v < -1000 || v > 1000)) { bw_put(&w, 2055u, 12); width = 17; }
        else if (i == 2 * n / 3 && width == 12)     { bw_put(&w, 2055u, 12); width = 17; }
        bw_put(&w, (uint32_t)v & ((1u << (width > 16 ? 16 : width)) - 1u), width);
    }
    bw_flush(&w);
    put16(out, (unsigned)w.n);
    return w.n + 2;
}

static void test_decompress_via_module(void);

/* ---- synthetic module -------------------------------------------------- */
#define SMP_N 300
static int8_t  ref8[SMP_N];
static int16_t ref16[SMP_N];

static size_t build_it(uint8_t *f, size_t cap, int it215_flavour)
{
    size_t tab, pos, off_ins, off_s[3], off_p;
    unsigned i;
    memset(f, 0, cap);
    memcpy(f, "IMPM", 4); strcpy((char *)f + 4, "synthetic test");
    put16(f + 0x20, 3); put16(f + 0x22, 1); put16(f + 0x24, 3); put16(f + 0x26, 1);
    put16(f + 0x28, 0x0214); put16(f + 0x2A, 0x0214); put16(f + 0x2C, IT_F_INSTR | IT_F_LINEAR | IT_F_STEREO);
    f[0x30] = 128; f[0x31] = 48; f[0x32] = 6; f[0x33] = 125; f[0x34] = 128;
    for (i = 0; i < 64; i++) { f[0x40 + i] = 32; f[0x80 + i] = 64; }
    f[0xC0] = 0; f[0xC1] = 254; f[0xC2] = 255;
    tab = 0xC3; pos = tab + 4u * 5;

    /* instrument */
    off_ins = pos; pos += 554;
    memcpy(f + off_ins, "IMPI", 4);
    f[off_ins + 0x11] = IT_NNA_FADE; f[off_ins + 0x12] = IT_DCT_NOTE; f[off_ins + 0x13] = IT_DCA_FADE;
    put16(f + off_ins + 0x14, 256); f[off_ins + 0x18] = 128; f[off_ins + 0x19] = 0x80 | 32;
    strcpy((char *)f + off_ins + 0x20, "test inst");
    for (i = 0; i < 120; i++) { f[off_ins + 0x40 + i * 2] = (uint8_t)i; f[off_ins + 0x41 + i * 2] = 1; }
    {   /* volume envelope: 3 nodes, sustain loop 1..1 */
        uint8_t *e = f + off_ins + 0x130;
        e[0] = IT_ENV_ON | IT_ENV_SUSLOOP; e[1] = 3; e[4] = 1; e[5] = 1;
        e[6] = 64; put16(e + 7, 0); e[9] = 32; put16(e + 10, 10); e[12] = 0; put16(e + 13, 20);
    }
    {   /* pitch envelope marked as filter envelope: raw -32..+32 -> 0..64 */
        uint8_t *e = f + off_ins + 0x1D4;
        e[0] = IT_ENV_ON | IT_ENV_FILTER; e[1] = 2;
        e[6] = (uint8_t)-32; put16(e + 7, 0); e[9] = 32; put16(e + 10, 16);
    }

    for (i = 0; i < SMP_N; i++) {
        ref8[i]  = (int8_t)((((i % 20) < 10 ? (i % 20) : (20 - (i % 20))) * 2) - 10 + (i > 150 ? 60 : 0));
        ref16[i] = (int16_t)(((int)(i % 50) * 20) - 500 + (i > 200 ? 8000 : 0));
    }
    /* sample 1: 8-bit signed uncompressed, forward loop (zero-copy path) */
    off_s[0] = pos; pos += 80;
    /* sample 2: 16-bit compressed */
    off_s[1] = pos; pos += 80;
    /* sample 3: 8-bit unsigned + delta, stereo */
    off_s[2] = pos; pos += 80;

    {   size_t dp = pos;
        /* s1 data */
        memcpy(f + off_s[0], "IMPS", 4);
        f[off_s[0] + 0x11] = 64; f[off_s[0] + 0x12] = IT_SMP_DATA | IT_SMP_LOOP | IT_SMP_PINGPONG;
        f[off_s[0] + 0x13] = 64; strcpy((char *)f + off_s[0] + 0x14, "s1 8bit");
        f[off_s[0] + 0x2E] = IT_CVT_SIGNED; put32(f + off_s[0] + 0x30, SMP_N);
        put32(f + off_s[0] + 0x34, 50); put32(f + off_s[0] + 0x38, 250);
        put32(f + off_s[0] + 0x3C, 8363); put32(f + off_s[0] + 0x48, (uint32_t)dp);
        for (i = 0; i < SMP_N; i++) f[dp + i] = (uint8_t)ref8[i];
        dp += SMP_N;
        /* s2 */
        memcpy(f + off_s[1], "IMPS", 4);
        f[off_s[1] + 0x11] = 64; f[off_s[1] + 0x12] = IT_SMP_DATA | IT_SMP_16BIT | IT_SMP_COMPRESSED | IT_SMP_LOOP;
        f[off_s[1] + 0x13] = 32; strcpy((char *)f + off_s[1] + 0x14, "s2 16bit comp");
        f[off_s[1] + 0x2E] = IT_CVT_SIGNED | (it215_flavour ? IT_CVT_DELTA : 0);
        put32(f + off_s[1] + 0x30, SMP_N);
        put32(f + off_s[1] + 0x34, 0); put32(f + off_s[1] + 0x38, 9999);   /* end clamped */
        put32(f + off_s[1] + 0x3C, 22050); put32(f + off_s[1] + 0x48, (uint32_t)dp);
        dp += enc16(ref16, SMP_N, it215_flavour, f + dp);
        /* s3: stereo unsigned delta (left = ref8 as unsigned, right = reversed) */
        memcpy(f + off_s[2], "IMPS", 4);
        f[off_s[2] + 0x11] = 64; f[off_s[2] + 0x12] = IT_SMP_DATA | IT_SMP_STEREO;
        f[off_s[2] + 0x13] = 64; strcpy((char *)f + off_s[2] + 0x14, "s3 stereo");
        f[off_s[2] + 0x2E] = IT_CVT_DELTA; put32(f + off_s[2] + 0x30, 100);
        put32(f + off_s[2] + 0x3C, 0);                                     /* -> 8363 + warning */
        put32(f + off_s[2] + 0x48, (uint32_t)dp);
        {   int acc = 0, k;
            for (k = 0; k < 100; k++) { int target = (uint8_t)(ref8[k] ^ 0x80); f[dp + k] = (uint8_t)(target - acc); acc = target; }
            dp += 100; acc = 0;
            for (k = 0; k < 100; k++) { int target = (uint8_t)(ref8[99 - k] ^ 0x80); f[dp + k] = (uint8_t)(target - acc); acc = target; }
            dp += 100;
        }
        pos = dp;
    }

    /* pattern: 32 rows */
    off_p = pos;
    {
        uint8_t *p = f + off_p + 8; size_t n = 0; unsigned r;
        /* row 0: ch0 mask 0x0F note/inst/vol/cmd ; ch1 mask 0x01 note */
        p[n++] = 0x81; p[n++] = 0x0F; p[n++] = 60; p[n++] = 1; p[n++] = 64; p[n++] = 1; p[n++] = 0x06;
        p[n++] = 0x82; p[n++] = 0x01; p[n++] = 48;
        p[n++] = 0;
        /* row 1: ch0 reuses last mask (0x0F -> data follows again) */
        p[n++] = 0x01; p[n++] = 61; p[n++] = 1; p[n++] = 70; p[n++] = 4; p[n++] = 0x12;
        p[n++] = 0;
        /* row 2: ch0 new mask 0xF0 -> all "use last" */
        p[n++] = 0x81; p[n++] = 0xF0;
        p[n++] = 0xC4 /* ch 3 */; p[n++] = 0x01; p[n++] = 255;
        p[n++] = 0;
        for (r = 3; r < 32; r++) p[n++] = 0;
        put16(f + off_p, (unsigned)n); put16(f + off_p + 2, 32);
        pos = off_p + 8 + n;
    }
    put32(f + tab, (uint32_t)off_ins);
    for (i = 0; i < 3; i++) put32(f + tab + 4 + 4 * i, (uint32_t)off_s[i]);
    put32(f + tab + 16, (uint32_t)off_p);
    return pos;
}

static uint8_t   g_file[8192];
static uint8_t   g_meta[1 << 20], g_pcm[1 << 20];

static int load(const uint8_t *f, size_t n, uint32_t fl, it_module_t *m, it_diag_t *dg)
{
    it_arena_t a, b;
    it_arena_init(&a, g_meta, sizeof g_meta);
    it_arena_init(&b, g_pcm, sizeof g_pcm);
    return it_load(f, n, &a, &b, fl, dg, m);
}

static void check_module(const it_module_t *m, int zerocopy)
{
    unsigned i;
    const it_sample_t *s1 = &m->smp[0], *s2 = &m->smp[1], *s3 = &m->smp[2];
    CHECK(!strcmp(m->title, "synthetic test"));
    CHECK(m->ordnum == 3 && m->song_len == 2 && m->orders[1] == 254);
    CHECK(m->gv == 128 && m->mv == 48 && m->speed == 6 && m->tempo == 125);
    CHECK(m->cmwt == 0x214 && !m->old_instruments);
    CHECK(m->ins[0].present && !strcmp(m->ins[0].name, "test inst"));
    CHECK(m->ins[0].nna == IT_NNA_FADE && m->ins[0].dct == IT_DCT_NOTE && m->ins[0].fadeout == 256);
    CHECK(m->ins[0].dfp == (0x80 | 32) && m->ins[0].kb_smp[60] == 1 && m->ins[0].kb_note[60] == 60);
    CHECK(m->ins[0].vol.num == 3 && (m->ins[0].vol.flags & IT_ENV_SUSLOOP) && m->ins[0].vol.node[1].tick == 10);
    CHECK(m->ins[0].vol.node[1].value == 32);
    CHECK((m->ins[0].pitch.flags & IT_ENV_FILTER) && m->ins[0].pitch.node[0].value == 0 && m->ins[0].pitch.node[1].value == 64);

    CHECK(s1->bits == 8 && s1->channels == 1 && s1->length == SMP_N && s1->loop_mode == IT_LOOP_PINGPONG);
    CHECK(s1->loop_begin == 50 && s1->loop_end == 250);
    CHECK(s1->pcm != NULL);
    if (zerocopy) CHECK((const uint8_t *)s1->pcm >= m->file && (const uint8_t *)s1->pcm < m->file + m->file_size);
    for (i = 0; i < SMP_N; i++) if (((const int8_t *)s1->pcm)[i] != ref8[i]) { CHECK(0); break; }

    CHECK(s2->bits == 16 && s2->length == SMP_N && s2->loop_mode == IT_LOOP_FWD && s2->loop_end == SMP_N);
    CHECK(s2->pcm != NULL);
    for (i = 0; s2->pcm && i < SMP_N; i++) if (((const int16_t *)s2->pcm)[i] != ref16[i]) { CHECK(0); printf("  s2 mismatch at %u\n", i); break; }

    CHECK(s3->channels == 2 && s3->bits == 8 && s3->length == 100 && s3->c5speed == 8363);
    for (i = 0; s3->pcm && s3->pcm_r && i < 100; i++) {
        if (((const int8_t *)s3->pcm)[i] != ref8[i] || ((const int8_t *)s3->pcm_r)[i] != ref8[99 - i]) { CHECK(0); printf("  s3 mismatch at %u\n", i); break; }
    }
    CHECK(m->pat[0].rows == 32 && m->pat[0].valid_rows == 32);
    CHECK(m->used_channels == 4);
}

static void test_synthetic(void)
{
    it_module_t m; it_diag_t dg; int flav;
    for (flav = 0; flav < 2; flav++) {
        size_t n = build_it(g_file, sizeof g_file, flav);
        CHECK(load(g_file, n, IT_LOAD_STRICT, &m, &dg) == IT_OK);
        check_module(&m, 1);
        CHECK(load(g_file, n, 0, &m, &dg) == IT_OK);
        check_module(&m, 1);
        CHECK(load(g_file, n, IT_LOAD_NOZEROCOPY, &m, &dg) == IT_OK);
        CHECK((const uint8_t *)m.smp[0].pcm < m.file || (const uint8_t *)m.smp[0].pcm >= m.file + m.file_size);
        check_module(&m, 0);
    }
}

static void test_patdec(void)
{
    it_module_t m; it_diag_t dg; it_patdec_t d; it_cell_t c[64]; uint32_t pr[2];
    size_t n = build_it(g_file, sizeof g_file, 1);
    CHECK(load(g_file, n, IT_LOAD_STRICT, &m, &dg) == IT_OK);
    it_patdec_init(&d, &m.pat[0]);
    CHECK(it_patdec_row(&d, c, pr) == IT_ROW_OK);
    CHECK(pr[0] == 3 && c[0].note == 60 && c[0].inst == 1 && c[0].vol == 64 && c[0].cmd == 1 && c[0].param == 6);
    CHECK(c[1].flags == IT_CF_NOTE && c[1].note == 48);
    CHECK(it_patdec_row(&d, c, pr) == IT_ROW_OK);
    CHECK(pr[0] == 1 && c[0].note == 61 && c[0].vol == 70 && c[0].param == 0x12);
    CHECK(it_patdec_row(&d, c, pr) == IT_ROW_OK);
    CHECK(pr[0] == 9 && c[0].note == 61 && c[0].inst == 1 && c[0].cmd == 4 && c[0].param == 0x12); /* last values */
    CHECK(c[3].note == 255);
    {   unsigned rows = 3;
        while (it_patdec_row(&d, c, pr) == IT_ROW_OK) { CHECK(pr[0] == 0 && pr[1] == 0); rows++; }
        CHECK(rows == 32);
    }
    CHECK(it_patdec_row(&d, c, pr) == IT_ROW_END);
    it_patdec_seek(&d, &m.pat[0], 2);
    CHECK(it_patdec_row(&d, c, pr) == IT_ROW_OK && c[3].note == 255);
}

static void test_corrupt_pattern(void)
{
    it_module_t m; it_diag_t dg; size_t n = build_it(g_file, sizeof g_file, 1);
    uint8_t *pat = (uint8_t *)memmem(g_file, n, "\x81\x0F\x3C", 3);
    CHECK(pat != NULL);
    if (!pat) return;
    /* chop pattern length so that row 2 is cut mid-way */
    {   size_t hdr = (size_t)(pat - g_file) - 8;
        put16(g_file + hdr, 20);
        CHECK(load(g_file, n, IT_LOAD_STRICT, &m, &dg) == IT_ERR_PATTERN);
        CHECK(dg.pattern == 0 && dg.row == 2);
        CHECK(load(g_file, n, 0, &m, &dg) == IT_OK);
        CHECK(m.pat[0].valid_rows == 2 && dg.nwarn >= 1);
        {   it_patdec_t d; it_cell_t c[64]; uint32_t pr[2]; unsigned rows = 0;
            it_patdec_init(&d, &m.pat[0]);
            while (it_patdec_row(&d, c, pr) == IT_ROW_OK) rows++;
            CHECK(rows == 32);              /* truncated rows come back empty */
        }
    }
}

static void test_truncation(void)
{
    it_module_t m; it_diag_t dg; size_t n = build_it(g_file, sizeof g_file, 1), len;
    unsigned ok_lenient = 0;
    for (len = 0; len < n; len++) {
        uint8_t *copy = (uint8_t *)malloc(len ? len : 1);   /* exact size: ASAN sees overreads */
        int r1, r2;
        memcpy(copy, g_file, len);
        r1 = load(copy, len, IT_LOAD_STRICT, &m, &dg);
        CHECK(r1 != IT_OK);                    /* a cut file is never valid in strict mode */
        r2 = load(copy, len, 0, &m, &dg);
        if (r2 == IT_OK) {
            unsigned i;
            ok_lenient++;
            for (i = 0; i < m.patnum; i++) {
                it_patdec_t d; it_cell_t c[64]; uint32_t pr[2];
                it_patdec_init(&d, &m.pat[i]);
                while (it_patdec_row(&d, c, pr) != IT_ROW_END) {}
            }
            for (i = 0; i < m.smpnum; i++) {
                const it_sample_t *s = &m.smp[i]; volatile int sum = 0; uint32_t k;
                for (k = 0; s->pcm && k < s->length; k++) sum += s->bits == 8 ? ((const int8_t *)s->pcm)[k] : ((const int16_t *)s->pcm)[k];
                for (k = 0; s->pcm_r && k < s->length; k++) sum += ((const int8_t *)s->pcm_r)[k];
            }
        }
        free(copy);
    }
    printf("truncation: %zu lengths, %u loaded leniently\n", n, ok_lenient);
}

static uint32_t rng_s = 12345;
static uint32_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s; }

static void test_fuzz(void)
{
    it_module_t m; it_diag_t dg; size_t n = build_it(g_file, sizeof g_file, 1);
    uint8_t *work = (uint8_t *)malloc(n);
    unsigned it, ok = 0, rej = 0;
    for (it = 0; it < 30000; it++) {
        unsigned k, flips = 1 + rnd() % 6;
        memcpy(work, g_file, n);
        for (k = 0; k < flips; k++) {
            size_t pos = (rnd() & 3) ? (rnd() % n) : (0xC0 + rnd() % 0x20);   /* bias to header/tables */
            work[pos] = (uint8_t)rnd();
        }
        if (load(work, n, (it & 1) ? IT_LOAD_STRICT : 0, &m, &dg) == IT_OK) {
            unsigned i;
            ok++;
            for (i = 0; i < m.patnum; i++) {
                it_patdec_t d; it_cell_t c[64]; uint32_t pr[2];
                it_patdec_init(&d, &m.pat[i]);
                while (it_patdec_row(&d, c, pr) != IT_ROW_END) {}
            }
            for (i = 0; i < m.smpnum; i++) {
                const it_sample_t *s = &m.smp[i]; volatile int sum = 0; uint32_t q;
                for (q = 0; s->pcm && q < s->length; q++) sum += s->bits == 8 ? ((const int8_t *)s->pcm)[q] : ((const int16_t *)s->pcm)[q];
            }
        } else rej++;
    }
    printf("fuzz: %u loaded, %u rejected\n", ok, rej);
    free(work);
}

/* standalone: decompress random signals through both flavours */
static void test_decompress_via_module(void)
{
    /* covered by build_it() sample 2 (16-bit) in both IT214/IT215 flavours;
       8-bit path is exercised here directly through a one-sample module. */
    it_module_t m; it_diag_t dg; int flav;
    for (flav = 0; flav < 2; flav++) {
        size_t n = build_it(g_file, sizeof g_file, flav), off;
        /* find sample 1 header and turn it into an 8-bit compressed sample whose data is appended */
        const uint8_t *tab = g_file + 0xC3;
        uint8_t blk[2048]; size_t bl; unsigned i;
        off = (size_t)(tab[8] | (tab[9] << 8));
        bl = enc8(ref8, SMP_N, flav, blk);
        memcpy(g_file + n, blk, bl);
        g_file[off + 0x12] = IT_SMP_DATA | IT_SMP_COMPRESSED;
        g_file[off + 0x2E] = IT_CVT_SIGNED | (flav ? IT_CVT_DELTA : 0);
        put32(g_file + off + 0x48, (uint32_t)n);
        CHECK(load(g_file, n + bl, IT_LOAD_STRICT, &m, &dg) == IT_OK);
        CHECK(m.smp[0].pcm != NULL && m.smp[0].bits == 8);
        for (i = 0; m.smp[0].pcm && i < SMP_N; i++)
            if (((const int8_t *)m.smp[0].pcm)[i] != ref8[i]) { CHECK(0); printf("  8bit comp (it%d) mismatch at %u\n", flav ? 215 : 214, i); break; }
    }
}

int main(void)
{
    if (getenv("IT_WRITE")) {   /* dump the synthetic module for tools/it_dump */
        size_t n = build_it(g_file, sizeof g_file, 1);
        FILE *fp = fopen(getenv("IT_WRITE"), "wb");
        if (fp) { fwrite(g_file, 1, n, fp); fclose(fp); }
        return 0;
    }
    test_synthetic();
    test_decompress_via_module();
    test_patdec();
    test_corrupt_pattern();
    test_truncation();
    test_fuzz();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
