/* Header-only synthetic IT builder for player tests. */
#ifndef ITBUILD_H
#define ITBUILD_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define ITB_NONE_NOTE 0xF0
#define ITB_NONE_VOL  0xFF

typedef struct { uint16_t row; uint8_t ch, note, inst, vol, cmd, param; } itb_cell_t;
typedef struct { uint16_t rows; const itb_cell_t *cells; unsigned n; } itb_pat_t;

typedef struct {
    uint16_t flags;              /* IT_F_* */
    uint8_t  speed, tempo;
    int      instmode;
    uint8_t  nna, dct, dca;
    uint16_t fadeout;
    int      venv_n;             /* volume envelope: nodes (tick,value) */
    uint16_t venv_t[8]; uint8_t venv_v[8];
    uint8_t  venv_flags, venv_lpb, venv_lpe, venv_slb, venv_sle;
    uint8_t  orders[16]; unsigned norders;
    itb_pat_t pats[8]; unsigned npats;
} itb_cfg_t;

static void itb_p16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void itb_p32(uint8_t *p, uint32_t v) { itb_p16(p, v & 0xFFFF); itb_p16(p + 2, v >> 16); }

/* one looping 8-bit triangle sample (64 frames, c5=8363, loop whole), optional 2nd sample */
static size_t itb_build(uint8_t *f, size_t cap, const itb_cfg_t *c)
{
    unsigned i, k, ninst = c->instmode ? 1 : 0, nsmp = 2;
    size_t tab, pos, off_i = 0, off_s[2], off_p[8], dp;
    memset(f, 0, cap);
    memcpy(f, "IMPM", 4); strcpy((char *)f + 4, "itb");
    itb_p16(f + 0x20, c->norders + 1); itb_p16(f + 0x22, ninst); itb_p16(f + 0x24, nsmp); itb_p16(f + 0x26, c->npats);
    itb_p16(f + 0x28, 0x0214); itb_p16(f + 0x2A, 0x0214);
    itb_p16(f + 0x2C, c->flags | (c->instmode ? 4u : 0u));
    f[0x30] = 128; f[0x31] = 128; f[0x32] = c->speed ? c->speed : 6; f[0x33] = c->tempo ? c->tempo : 125; f[0x34] = 128;
    for (i = 0; i < 64; i++) { f[0x40 + i] = 32; f[0x80 + i] = 64; }
    for (i = 0; i < c->norders; i++) f[0xC0 + i] = c->orders[i];
    f[0xC0 + c->norders] = 255;
    tab = 0xC0 + c->norders + 1;
    pos = tab + 4u * (ninst + nsmp + c->npats);
    if (ninst) {
        uint8_t *p;
        off_i = pos; pos += 554; p = f + off_i;
        memcpy(p, "IMPI", 4);
        p[0x11] = c->nna; p[0x12] = c->dct; p[0x13] = c->dca; itb_p16(p + 0x14, c->fadeout);
        p[0x18] = 128; p[0x19] = 0x80;
        for (i = 0; i < 120; i++) { p[0x40 + i * 2] = (uint8_t)i; p[0x41 + i * 2] = 1; }
        if (c->venv_n) {
            uint8_t *e = p + 0x130;
            e[0] = c->venv_flags; e[1] = (uint8_t)c->venv_n; e[2] = c->venv_lpb; e[3] = c->venv_lpe; e[4] = c->venv_slb; e[5] = c->venv_sle;
            for (k = 0; k < (unsigned)c->venv_n; k++) { e[6 + k * 3] = c->venv_v[k]; itb_p16(e + 7 + k * 3, c->venv_t[k]); }
        }
    }
    for (k = 0; k < 2; k++) {
        off_s[k] = pos; pos += 80;
    }
    dp = pos;
    for (k = 0; k < 2; k++) {
        uint8_t *p = f + off_s[k];
        memcpy(p, "IMPS", 4);
        p[0x11] = 64; p[0x12] = 1 | 0x10; p[0x13] = 64; p[0x2E] = 1;
        itb_p32(p + 0x30, 64); itb_p32(p + 0x34, 0); itb_p32(p + 0x38, 64);
        itb_p32(p + 0x3C, 8363); itb_p32(p + 0x48, (uint32_t)dp);
        for (i = 0; i < 64; i++) f[dp + i] = (uint8_t)(int8_t)(i < 32 ? i * 4 - 64 : 192 - i * 4);
        dp += 64;
    }
    pos = dp;
    for (k = 0; k < c->npats; k++) {
        const itb_pat_t *pt = &c->pats[k];
        uint8_t *p = f + pos + 8; size_t n = 0; unsigned r, j;
        for (r = 0; r < pt->rows; r++) {
            for (j = 0; j < pt->n; j++) {
                const itb_cell_t *ce = &pt->cells[j];
                uint8_t mask = 0;
                if (ce->row != r) continue;
                if (ce->note != ITB_NONE_NOTE) mask |= 1;
                if (ce->inst) mask |= 2;
                if (ce->vol != ITB_NONE_VOL) mask |= 4;
                if (ce->cmd) mask |= 8;
                p[n++] = (uint8_t)((ce->ch + 1) | 0x80); p[n++] = mask;
                if (mask & 1) p[n++] = ce->note;
                if (mask & 2) p[n++] = ce->inst;
                if (mask & 4) p[n++] = ce->vol;
                if (mask & 8) { p[n++] = ce->cmd; p[n++] = ce->param; }
            }
            p[n++] = 0;
        }
        itb_p16(f + pos, (unsigned)n); itb_p16(f + pos + 2, pt->rows);
        off_p[k] = pos; pos += 8 + n;
    }
    if (ninst) itb_p32(f + tab, (uint32_t)off_i);
    for (k = 0; k < 2; k++) itb_p32(f + tab + 4u * ninst + 4u * k, (uint32_t)off_s[k]);
    for (k = 0; k < c->npats; k++) itb_p32(f + tab + 4u * (ninst + nsmp) + 4u * k, (uint32_t)off_p[k]);
    return pos;
}
#endif
