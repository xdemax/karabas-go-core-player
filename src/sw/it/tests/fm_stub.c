/* Minimal host stand-in for fastmix.c: nearest-neighbour, same voice semantics
 * as documented in fastmix.h. Only for tests; the real fastmix.c is used on target. */
#include "../../fastmix.h"
#include <string.h>

void fm_reset(void) {}
void fm_set_max_mixed(int n) { (void)n; }
void fm_voice_init(fm_voice_t *v) { memset(v, 0, sizeof *v); }

void fm_voice_set_gain(fm_voice_t *v, int32_t volume, int32_t pan, uint16_t ramp_frames)
{
    v->volume = volume; v->pan = pan;
    v->ramp = ramp_frames; v->ramp_len = ramp_frames;
    {
        int32_t l = volume * (128 - pan) / 256 * 256, r = volume * (128 + pan) / 256 * 256;
        if (ramp_frames == 0) { v->cur_l = l; v->cur_r = r; }
        else { v->cur_l = l; v->cur_r = r; v->ramp = 0; }   /* stub: instant gain, ramp counted as done */
    }
}

void fm_mix(int16_t *dst, int frames, fm_voice_t *voices, int nvoices)
{
    int i, k;
    memset(dst, 0, (size_t)frames * 4);
    for (i = 0; i < nvoices; i++) {
        fm_voice_t *v = &voices[i];
        if (!(v->flags & FM_ACTIVE)) continue;
        for (k = 0; k < frames; k++) {
            int32_t s;
            if (v->pos >= v->length) { if (!(v->flags & FM_LOOP)) { v->flags &= (uint8_t)~FM_ACTIVE; break; } }
            s = (v->flags & FM_8BIT) ? ((const int8_t *)v->data)[v->pos] * 256 : ((const int16_t *)v->data)[v->pos];
            {
                int32_t l = dst[2 * k] + (s * v->cur_l >> 24), r = dst[2 * k + 1] + (s * v->cur_r >> 24);
                dst[2 * k] = (int16_t)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
                dst[2 * k + 1] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
            }
            v->frac += v->step;
            v->pos += v->frac >> 16; v->frac &= 0xFFFF;
            if ((v->flags & FM_LOOP) && v->pos >= v->loop_end) v->pos = v->loop_start + (v->pos - v->loop_end) % (v->loop_end - v->loop_start ? v->loop_end - v->loop_start : 1);
        }
    }
}
