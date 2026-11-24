/*
 * it_dump - print parsed IT state as text, for A/B comparison with
 * OpenMPT / libxmp (see docs/AB_PLAN.md).
 *
 *   it_dump file.it            header, orders, instruments, samples, warnings
 *   it_dump -p N file.it       also pattern N rows (-P: all used patterns)
 *   it_dump -s file.it         per-sample FNV-1a of decoded PCM
 *   it_dump -S file.it         strict load
 *
 * Pattern cell format (same column layout as OpenMPT's text view):
 *   NNN II vXX EXX     note  instrument  volume-column  effect
 */
#include "../it_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *NN[12] = { "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-" };

static void fmt_note(char *o, unsigned n)
{
    if (n < 120)       sprintf(o, "%s%u", NN[n % 12], n / 12);
    else if (n == 254) strcpy(o, "^^^");
    else if (n == 255) strcpy(o, "===");
    else               strcpy(o, "~~~");
}

static void fmt_vol(char *o, unsigned v)
{
    if (v <= 64)        sprintf(o, "v%02u", v);
    else if (v <= 74)   sprintf(o, "a%02u", v - 65);
    else if (v <= 84)   sprintf(o, "b%02u", v - 75);
    else if (v <= 94)   sprintf(o, "c%02u", v - 85);
    else if (v <= 104)  sprintf(o, "d%02u", v - 95);
    else if (v <= 114)  sprintf(o, "e%02u", v - 105);
    else if (v <= 124)  sprintf(o, "f%02u", v - 115);
    else if (v >= 128 && v <= 192) sprintf(o, "p%02u", v - 128);
    else if (v >= 193 && v <= 202) sprintf(o, "g%02u", v - 193);
    else if (v >= 203 && v <= 212) sprintf(o, "h%02u", v - 203);
    else                sprintf(o, "?%02X", v);
}

static uint32_t fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p; uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

static void dump_env(const char *nm, const it_env_t *e)
{
    unsigned i;
    if (!(e->flags & IT_ENV_ON) && !e->num) return;
    printf("    %s env: flags=%02X num=%u loop=%u..%u sus=%u..%u :", nm, e->flags, e->num,
           e->lpb, e->lpe, e->slb, e->sle);
    for (i = 0; i < e->num; i++) printf(" (%u,%d)", e->node[i].tick, e->node[i].value);
    printf("\n");
}

static void dump_pattern(const it_module_t *m, unsigned pn)
{
    it_patdec_t d; it_cell_t cells[IT_MAX_CHANNELS]; uint32_t pr[2];
    const it_pattern_t *pt = &m->pat[pn];
    unsigned row = 0, ch;
    it_patdec_init(&d, pt);
    printf("\nPattern %u: rows=%u valid=%u len=%u\n", pn, pt->rows, pt->valid_rows, pt->len);
    while (it_patdec_row(&d, cells, pr) != IT_ROW_END) {
        printf("%03u", row++);
        for (ch = 0; ch < m->used_channels; ch++) {
            char n[8] = "...", v[8] = "...", e[8] = "...", ii[8] = "..";
            if (pr[ch >> 5] & (1u << (ch & 31))) {
                const it_cell_t *c = &cells[ch];
                if (c->flags & IT_CF_NOTE) fmt_note(n, c->note);
                if (c->flags & IT_CF_INST) sprintf(ii, "%02u", c->inst);
                if (c->flags & IT_CF_VOL)  fmt_vol(v, c->vol);
                if (c->flags & IT_CF_CMD)  {
                    if (c->cmd >= 1 && c->cmd <= 26) sprintf(e, "%c%02X", 'A' + c->cmd - 1, c->param);
                    else sprintf(e, "?%02X", c->param);
                }
            }
            printf(" |%s %s %s %s", n, ii, v, e);
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    const char *path = NULL; int pat = -1, all = 0, hash = 0, strict = 0, i;
    FILE *fp; long sz; uint8_t *buf; it_arena_t meta, pcm; it_module_t m; it_diag_t dg;
    int rc;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) pat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-P")) all = 1;
        else if (!strcmp(argv[i], "-s")) hash = 1;
        else if (!strcmp(argv[i], "-S")) strict = 1;
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: it_dump [-p N|-P] [-s] [-S] file.it\n"); return 2; }
    fp = fopen(path, "rb");
    if (!fp) { perror(path); return 2; }
    fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { fprintf(stderr, "read failed\n"); return 2; }
    fclose(fp);

    it_arena_init(&meta, malloc(16u << 20), 16u << 20);
    it_arena_init(&pcm, malloc(256u << 20), 256u << 20);
    rc = it_load(buf, (size_t)sz, &meta, &pcm, strict ? IT_LOAD_STRICT : 0, &dg, &m);
    if (rc) {
        printf("LOAD FAILED: %s (pattern=%d row=%d ch=%d index=%d offset=0x%X)\n",
               it_err_str(rc), dg.pattern, dg.row, dg.channel, dg.index, dg.offset);
        return 1;
    }

    printf("title      : %s\n", m.title);
    printf("Cwt/Cmwt   : %04X / %04X%s\n", m.cwt, m.cmwt, m.old_instruments ? " (old instruments)" : "");
    printf("flags      : %04X (%s%s%s%s%s) special=%04X\n", m.flags,
           m.flags & IT_F_STEREO ? "stereo " : "mono ", m.flags & IT_F_INSTR ? "instr " : "sample ",
           m.flags & IT_F_LINEAR ? "linear " : "amiga ", m.flags & IT_F_OLDEFF ? "oldeff " : "",
           m.flags & IT_F_COMPATGXX ? "compatG" : "", m.special);
    printf("GV/MV/IS/IT: %u/%u/%u/%u sep=%u pwd=%u\n", m.gv, m.mv, m.speed, m.tempo, m.sep, m.pwd);
    printf("orders     : %u (song_len %u):", m.ordnum, m.song_len);
    for (i = 0; i < m.ordnum; i++) printf(" %u", m.orders[i]);
    printf("\ncounts     : ins=%u smp=%u pat=%u used_channels=%u midi_cfg=%s\n",
           m.insnum, m.smpnum, m.patnum, m.used_channels, m.midi_cfg ? "yes" : "no");

    for (i = 0; i < m.insnum; i++) {
        const it_instrument_t *n = &m.ins[i];
        unsigned k, used = 0;
        if (!n->present) continue;
        for (k = 0; k < 120; k++) if (n->kb_smp[k]) used++;
        printf("INS %02d \"%s\" nna=%u dct=%u dca=%u fade=%u gbv=%u dfp=%02X rv=%u rp=%u pps=%d ppc=%u ifc=%02X ifr=%02X mapped=%u\n",
               i + 1, n->name, n->nna, n->dct, n->dca, n->fadeout, n->gbv, n->dfp, n->rv, n->rp,
               n->pps, n->ppc, n->ifc, n->ifr, used);
        dump_env("vol", &n->vol); dump_env("pan", &n->pan); dump_env("pitch", &n->pitch);
    }
    for (i = 0; i < m.smpnum; i++) {
        const it_sample_t *s = &m.smp[i];
        if (!s->flags && !s->length && !s->name[0]) continue;
        printf("SMP %02d \"%s\" len=%u %ubit x%u c5=%u vol=%u gvl=%u dfp=%02X cvt=%02X flg=%02X loop=%u[%u,%u) sus=%u[%u,%u) vib=%u/%u/%u/%u",
               i + 1, s->name, s->length, s->bits, s->channels, s->c5speed, s->vol, s->gvl, s->dfp,
               s->cvt, s->flags, s->loop_mode, s->loop_begin, s->loop_end, s->sus_mode,
               s->sus_begin, s->sus_end, s->vis, s->vid, s->vir, s->vit);
        if (hash && s->pcm) {
            size_t bytes = (size_t)s->length * (s->bits / 8u);
            printf(" fnv=%08X", fnv(s->pcm, bytes));
            if (s->pcm_r) printf("/%08X", fnv(s->pcm_r, bytes));
        }
        printf("\n");
    }
    for (i = 0; i < (int)dg.nwarn; i++)
        printf("WARN %s (index=%d row=%d ch=%d offset=0x%X)\n", it_warn_str(dg.warn[i].code),
               dg.warn[i].index, dg.warn[i].row, dg.warn[i].ch, dg.warn[i].offset);
    if (dg.nwarn_total > dg.nwarn) printf("WARN ... %u more\n", dg.nwarn_total - dg.nwarn);
    printf("arena      : meta %zu bytes, pcm %zu bytes\n", meta.used, pcm.used);

    if (all) { for (i = 0; i < m.patnum; i++) if (m.pat[i].data) dump_pattern(&m, (unsigned)i); }
    else if (pat >= 0 && pat < m.patnum) dump_pattern(&m, (unsigned)pat);
    return 0;
}
