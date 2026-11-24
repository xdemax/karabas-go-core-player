/*
 * it_trace - drive the replayer tick by tick and print channel/voice state.
 *   it_trace file.it [max_ticks] [-c N]   (N = only host channel N)
 * Output columns (one line per audible voice per tick):
 *   T ord pat row tick | ch kind note pitch(lf|period) freq(Hz) vol pan
 * Compare freq/pitch per tick against libxmp/OpenMPT for the same channel.
 */
#include "../it_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *path = NULL; unsigned maxt = 600, only = 999, i; int a;
    FILE *fp; long sz; uint8_t *buf; it_arena_t meta, pcm; it_module_t m; it_diag_t dg; static it_player_t pl;
    it_player_cfg_t pc;
    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-c") && a + 1 < argc) only = (unsigned)atoi(argv[++a]);
        else if (!path) path = argv[a]; else maxt = (unsigned)atoi(argv[a]);
    }
    if (!path) { fprintf(stderr, "usage: it_trace file.it [ticks] [-c ch]\n"); return 2; }
    fp = fopen(path, "rb"); if (!fp) { perror(path); return 2; }
    fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) return 2;
    fclose(fp);
    it_arena_init(&meta, malloc(16u << 20), 16u << 20);
    it_arena_init(&pcm, malloc(256u << 20), 256u << 20);
    if (it_load(buf, (size_t)sz, &meta, &pcm, 0, &dg, &m)) { printf("load failed: %s\n", it_err_str(dg.err)); return 1; }
    it_player_cfg_default(&pc); pc.flags = 0;
    it_player_init(&pl, &m, &pc);
    for (unsigned t = 0; t < maxt && !pl.finished; t++) {
        unsigned ord = pl.ord, pat = pl.pat, row = pl.row, tick = pl.tick;
        it_player_tick(&pl);
        for (i = 0; i < FM_MAX_VOICES; i++) {
            const it_voice_t *v = &pl.vox[i];
            if (v->state == IT_VS_FREE || v->state == IT_VS_DYING) continue;
            if (only != 999 && v->host != only) continue;
            printf("%5u %3u %3u %3u %2u | ch%02u %s n%03u %6d %7u Hz vol%3d pan%4d\n", t, ord, pat, row, tick,
                   v->host, v->state == IT_VS_MASTER ? "M " : "BG", v->note, v->pitch + v->mod_lf, v->freq, v->out_vol, v->out_pan);
        }
    }
    return 0;
}
