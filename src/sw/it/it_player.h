/*
 * it_player.h - Impulse Tracker replayer, integer-only, no libc in the tick path.
 *
 * The player owns an array of fm_voice_t (fastmix) and fills it every tick.
 * Call it_player_render() to get PCM (it calls fm_mix() in tick-sized chunks),
 * or it_player_tick() alone to drive the engine without audio (trace/tests).
 *
 * Pitch model: linear-slide modules keep pitch as `lf` = 1/768 octave units
 * relative to the sample's C5 (lf = (note-60)*64). Amiga-slide modules keep an
 * Amiga-style period (IT units, x4). Arpeggio/vibrato/envelopes never modify
 * the stored pitch: they are summed into a per-tick offset, so nothing drifts.
 */
#ifndef IT_PLAYER_H
#define IT_PLAYER_H

#include "it_parser.h"
#include "../fastmix.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IT_PL_LOOP     0x01      /* restart at end of song (default for players) */
#define IT_CFG_NEAREST (1u << 0)

#define IT_VS_FREE     0
#define IT_VS_MASTER   1         /* voice currently owned by a channel           */
#define IT_VS_BG       2         /* NNA background voice                         */
#define IT_VS_DYING    3         /* ramping to zero, freed when ramp ends        */

typedef struct {
    uint32_t mixrate;            /* 48000 */
    uint32_t flags;
    int32_t  out_gain;           /* Q8 trim applied to every voice, 256 = 1.0     */
} it_player_cfg_t;

typedef struct {
    uint8_t  state, host, note, orig_note;
    uint8_t  inst, smp, nna;
    uint8_t  keyoff, fadeflag;
    uint8_t  env_off;            /* bit0 vol, bit1 pan, bit2 pitch disabled        */
    uint8_t  surround, cvol, fresh;
    uint16_t env_pos[3];
    uint16_t avib_pos, avib_depth;
    uint32_t fade;               /* 65536 .. 0                                     */
    int16_t  vol256, pan256;     /* snapshot from channel (tremolo/panbrello in)   */
    int32_t  pitch, mod_lf;      /* snapshot from channel                          */
    /* last computed output, for trace/tests */
    uint32_t freq;
    int32_t  out_vol;
    int32_t  out_pan;
    const it_instrument_t *ins;
    const it_sample_t     *smp_p;
} it_voice_t;

typedef struct {
    /* current row */
    uint8_t  cmd, param, vcol, has_vcol;
    uint8_t  nd_tick, nd_pending;        /* SDx note delay */
    it_cell_t nd_cell;
    uint8_t  cut_tick;                   /* SCx, 0 = none */
    /* note state */
    uint8_t  inst, last_note, vol, cvol, pan, surround, muted;
    int32_t  pitch, target;
    int32_t  mod_lf;
    /* memories */
    uint8_t  m_vs, m_pu, m_pg, m_vib, m_arp, m_trm, m_off, m_rtg, m_s, m_cvs, m_pans, m_tempo, m_gvs, m_pbr, m_vcvs;
    uint8_t  hi_off;
    /* oscillators */
    uint8_t  vib_speed, vib_depth, vib_pos, vib_wave, vib_fine;
    uint8_t  trm_speed, trm_depth, trm_pos, trm_wave;
    uint8_t  pbr_speed, pbr_depth, pbr_pos, pbr_wave;
    uint8_t  tremor_cnt, rtg_cnt;
    uint16_t loop_row;
    uint8_t  loop_cnt;
    int8_t   vidx;                       /* index into voices, -1 = none */
    int16_t  vol_delta, pan_delta;       /* tremolo / panbrello of this tick */
    uint8_t  tremor_mute;
} it_chn_t;

typedef struct it_player {
    const it_module_t *m;
    uint32_t mixrate, flags;
    int32_t  out_gain;
    uint64_t step_mul;                   /* 2^48 / mixrate */
    /* position */
    uint16_t ord, pat, row, rows;
    int16_t  next_ord, next_row;         /* pending jump (-1 = none) */
    int16_t  jump_ord, break_row, loop_row;
    uint8_t  speed, tempo, gv, mv, sep;
    uint8_t  patdelay, finedelay;
    uint16_t tick, row_ticks;
    uint8_t  finished;
    uint32_t loops;
    uint32_t ticks;                      /* ticks processed since init */
    /* timing */
    uint32_t tick_len_q16, tick_frac, left;
    /* decoding */
    it_patdec_t dec;
    uint16_t    dec_next;                /* row the decoder will return next */
    uint16_t    dec_pat;                 /* pattern the decoder is positioned in */
    it_cell_t   cells[IT_MAX_CHANNELS];
    uint32_t    present[2];
    uint8_t     nch, linear, oldeff, compatg, instmode;
    uint32_t    rng;
    it_chn_t    chn[IT_MAX_CHANNELS];
    it_voice_t  vox[FM_MAX_VOICES];
    fm_voice_t  fm[FM_MAX_VOICES];
    int         nv_hi;
} it_player_t;

void it_player_cfg_default(it_player_cfg_t *c);
void it_player_init(it_player_t *p, const it_module_t *m, const it_player_cfg_t *cfg);

/* Advance exactly one tick (updates voices, not audio). */
void it_player_tick(it_player_t *p);

/* Render stereo int16 frames; ticks are processed as needed. */
void it_player_render(it_player_t *p, int16_t *dst, int frames);

/* Frames of the current tick (Q16 accumulated): for exact timing checks. */
uint32_t it_player_tick_frames(it_player_t *p);

/* Number of voices that are audible right now. */
int it_player_active_voices(const it_player_t *p);

#ifdef __cplusplus
}
#endif
#endif
