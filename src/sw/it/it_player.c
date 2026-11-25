/*
 * it_player.c - Impulse Tracker replayer. Integer only; no malloc/printf/float.
 * See docs/PLAYER_NOTES.md for what is verified, approximated or missing.
 */
#include "it_player.h"
#include "it_tables.h"
#include "it_mix.h"
#include <string.h>      /* init only (memset) - not used in the tick path */

#define LF_MAX   9000
#define LF_MIN  (-9000)
#define PER_MIN  32
#define PER_MAX  0x7FFFFF
#define DECLICK_FRAMES 96
#define START_RAMP     64
#define MAX_VOICES     32  /* hard limit to match IT voice stealing */

static const it_pattern_t g_empty_pat = { NULL, 0, 64, 64 };
static const uint16_t s3m_tab[12] = { 1712,1616,1524,1440,1356,1280,1208,1140,1076,1016,960,907 };
static const uint8_t  vcol_g_tab[10] = { 0, 1, 4, 8, 16, 32, 64, 96, 128, 255 };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int absi(int v) { return v < 0 ? -v : v; }
static uint32_t rnd(it_player_t *p) { p->rng = p->rng * 1664525u + 1013904223u; return p->rng >> 8; }

/* ======================================================================= */
/* pitch                                                                   */
/* ======================================================================= */
static int32_t note_pitch(const it_player_t *p, unsigned note, uint32_t c5)
{
    if (p->linear) return ((int32_t)note - 60) * 64;
    {
        uint32_t per = ((8363u * ((uint32_t)s3m_tab[note % 12] << 5)) >> (note / 12)) / (c5 ? c5 : 8363u);
        return (int32_t)clampi((int)(per > PER_MAX ? PER_MAX : per), PER_MIN, PER_MAX);
    }
}

static uint32_t scale_freq(uint32_t f, int32_t lf)
{
    int32_t o = lf / 768, r = lf % 768;
    uint64_t v;
    if (r < 0) { r += 768; o--; }
    v = ((uint64_t)f * it_exp2_tab[r]) >> 16;
    if (o > 0)      v = (o >= 16) ? 0x3FFFFFFu : (v << o);
    else if (o < 0) v = (-o >= 40) ? 0 : (v >> (-o));
    return v > 0x3FFFFFFu ? 0x3FFFFFFu : (uint32_t)v;
}

static uint32_t calc_freq(const it_player_t *p, const it_sample_t *s, int32_t pitch, int32_t extra)
{
    if (p->linear) return scale_freq(s->c5speed, pitch + extra);
    {
        uint32_t per = pitch < PER_MIN ? PER_MIN : (uint32_t)pitch;
        return scale_freq((uint32_t)(((uint64_t)8363u * 1712u) / per), extra);
    }
}

static void pitch_slide(it_chn_t *ch, const it_player_t *p, int up, int amt)
{
    if (p->linear) ch->pitch = clampi(ch->pitch + (up ? amt : -amt), LF_MIN, LF_MAX);
    else           ch->pitch = clampi(ch->pitch + (up ? -amt : amt), PER_MIN, PER_MAX);
}

static void tone_porta(it_chn_t *ch, const it_player_t *p, int amt)
{
    int up_dir = p->linear ? (ch->target > ch->pitch) : (ch->target < ch->pitch);
    int dist   = absi(ch->target - ch->pitch);
    if (dist == 0) return;
    if (amt >= dist) { ch->pitch = ch->target; return; }
    pitch_slide(ch, p, up_dir, amt);
}

/* ======================================================================= */
/* waveforms / envelopes                                                   */
/* ======================================================================= */
static int wave_val(it_player_t *p, unsigned type, unsigned pos)
{
    pos &= 255u;
    switch (type & 3u) {
    case 0:  return it_sine_tab[pos];
    case 1:  return 64 - (int)((pos + 1u) >> 1);
    case 2:  return pos < 128u ? 64 : -64;
    default: return (int)(rnd(p) & 127u) - 64;
    }
}

/* Q8 result */
static int env_eval(const it_env_t *e, unsigned pos)
{
    unsigned n = e->num, i;
    int v0, v1;
    unsigned t0, t1;
    if (n == 0) return 0;
    if (pos >= e->node[n - 1].tick) return e->node[n - 1].value * 256;
    for (i = 0; i + 1 < n; i++) if (pos < e->node[i + 1].tick) break;
    t0 = e->node[i].tick; v0 = e->node[i].value;
    if (pos <= t0) return v0 * 256;
    t1 = e->node[i + 1].tick; v1 = e->node[i + 1].value;
    if (t1 <= t0) return v1 * 256;
    return v0 * 256 + (v1 - v0) * 256 * (int)(pos - t0) / (int)(t1 - t0);
}

static void env_advance(const it_env_t *e, uint16_t *pos, int keyoff)
{
    unsigned np, last;
    if (e->num == 0) return;
    last = e->node[e->num - 1].tick;
    np = (unsigned)*pos + 1u;
    if (!keyoff && (e->flags & IT_ENV_SUSLOOP) && np > e->node[e->sle].tick) np = e->node[e->slb].tick;
    else if ((e->flags & IT_ENV_LOOP) && np > e->node[e->lpe].tick)           np = e->node[e->lpb].tick;
    else if (np > last)                                                        np = last;
    *pos = (uint16_t)np;
}

static int env_ended_at_zero(const it_env_t *e, unsigned pos)
{
    if (e->num == 0 || (e->flags & (IT_ENV_LOOP)) ) return 0;
    return pos >= e->node[e->num - 1].tick && e->node[e->num - 1].value == 0;
}

/* ======================================================================= */
/* voice pool                                                              */
/* ======================================================================= */
static void vox_free(it_player_t *p, int i)
{
    it_voice_t *v = &p->vox[i];
    if (v->state == IT_VS_MASTER && p->chn[v->host].vidx == i) p->chn[v->host].vidx = -1;
    v->state = IT_VS_FREE;
    p->fm[i].flags = 0;
}

static void vox_kill(it_player_t *p, int i)
{
    it_voice_t *v = &p->vox[i];
    fm_voice_t *f = &p->fm[i];
    if (v->state == IT_VS_FREE || v->state == IT_VS_DYING) return;
    if (!(f->flags & FM_ACTIVE) || (f->cur_l == 0 && f->cur_r == 0 && v->out_vol == 0)) { vox_free(p, i); return; }
    if (v->state == IT_VS_MASTER && p->chn[v->host].vidx == i) p->chn[v->host].vidx = -1;
    v->state = IT_VS_DYING;
    fm_voice_set_gain(f, 0, f->pan, DECLICK_FRAMES);
}

static int vox_alloc(it_player_t *p)
{
    int i, best = 0, cls_best = 99;
    int32_t bv = 0x7FFFFFFF;
    /* First pass: find a free slot, but only if under MAX_VOICES limit */
    for (i = 0; i < MAX_VOICES; i++) {
        if (p->vox[i].state == IT_VS_FREE) { if (i + 1 > p->nv_hi) p->nv_hi = i + 1; return i; }
    }
    /* At capacity: steal the quietest voice by priority: DYING > BG > MASTER, then by volume */
    for (i = 0; i < MAX_VOICES; i++) {
        const it_voice_t *v = &p->vox[i];
        int cls = v->state == IT_VS_DYING ? 0 : (v->state == IT_VS_BG ? 1 : 2);
        if (cls < cls_best || (cls == cls_best && v->out_vol < bv)) { cls_best = cls; bv = v->out_vol; best = i; }
    }
    vox_free(p, best);
    return best;
}

static void voice_loop(const it_voice_t *v, fm_voice_t *f)
{
    const it_sample_t *s = v->smp_p;
    f->flags &= (uint8_t)~(FM_LOOP | FM_PINGPONG);
    if (s->sus_mode && !v->keyoff) {
        f->loop_start = s->sus_begin; f->loop_end = s->sus_end;
        f->flags |= (uint8_t)(FM_LOOP | (s->sus_mode == IT_LOOP_PINGPONG ? FM_PINGPONG : 0));
    } else if (s->loop_mode) {
        f->loop_start = s->loop_begin; f->loop_end = s->loop_end;
        f->flags |= (uint8_t)(FM_LOOP | (s->loop_mode == IT_LOOP_PINGPONG ? FM_PINGPONG : 0));
    }
}

static void voice_keyoff(it_player_t *p, int i)
{
    it_voice_t *v = &p->vox[i];
    if (v->state == IT_VS_FREE || v->state == IT_VS_DYING || v->keyoff) return;
    v->keyoff = 1;
    if (!v->ins) { vox_kill(p, i); return; }      /* sample mode: note off = cut */
    voice_loop(v, &p->fm[i]);
}

static int voice_start(it_player_t *p, unsigned ci, const it_sample_t *s, unsigned smpno,
                       const it_instrument_t *ins, unsigned instno,
                       unsigned note_raw, unsigned note_tr, uint32_t offset)
{
    int i = vox_alloc(p);
    it_voice_t *v = &p->vox[i];
    fm_voice_t *f = &p->fm[i];
    it_chn_t *ch = &p->chn[ci];
    unsigned k;
    for (k = 0; k < sizeof *v; k++) ((uint8_t *)v)[k] = 0;
    fm_voice_init(f);
    f->data = s->pcm; f->length = s->length;
    f->flags = (uint8_t)(FM_ACTIVE | (s->bits == 8 ? FM_8BIT : 0) | ((p->flags & IT_CFG_NEAREST) ? FM_NEAREST : 0));
    f->pos = offset;
    v->state = IT_VS_MASTER; v->host = (uint8_t)ci;
    v->note = (uint8_t)note_tr; v->orig_note = (uint8_t)note_raw;
    v->inst = (uint8_t)instno; v->smp = (uint8_t)smpno;
    v->nna = ins ? ins->nna : IT_NNA_CUT;
    v->fade = 65536u; v->fresh = 1;
    v->ins = ins; v->smp_p = s;
    voice_loop(v, f);
    ch->vidx = (int8_t)i;
    return i;
}

/* new-note-action + duplicate check against everything the channel owns */
static void do_nna(it_player_t *p, unsigned ci, const it_instrument_t *nins, unsigned nsmp, unsigned note_tr)
{
    it_chn_t *ch = &p->chn[ci];
    int i;
    if (nins && nins->dct) {
        for (i = 0; i < p->nv_hi; i++) {
            it_voice_t *v = &p->vox[i];
            int dup = 0;
            if ((v->state != IT_VS_MASTER && v->state != IT_VS_BG) || v->host != ci) continue;
            switch (nins->dct) {
            case IT_DCT_NOTE:   dup = (v->note == note_tr && v->ins == nins); break;
            case IT_DCT_SAMPLE: dup = (v->smp == nsmp && v->ins == nins);     break;
            case IT_DCT_INST:   dup = (v->ins == nins);                       break;
            default: break;
            }
            if (!dup) continue;
            if (nins->dca == IT_DCA_CUT)      vox_kill(p, i);
            else if (nins->dca == IT_DCA_OFF) voice_keyoff(p, i);
            else                              v->fadeflag = 1;
        }
    }
    i = ch->vidx;
    if (i >= 0) {
        it_voice_t *v = &p->vox[i];
        if (v->nna == IT_NNA_CUT) { vox_kill(p, i); }
        else {
            v->state = IT_VS_BG; ch->vidx = -1;
            if (v->nna == IT_NNA_OFF) voice_keyoff(p, i);
            else if (v->nna == IT_NNA_FADE) v->fadeflag = 1;
        }
    }
}

/* ======================================================================= */
/* per-voice tick: envelopes, fades, final volume/pan/frequency -> fastmix  */
/* ======================================================================= */
static void voice_tick(it_player_t *p, int i)
{
    it_voice_t *v = &p->vox[i];
    fm_voice_t *f = &p->fm[i];
    const it_instrument_t *ins = v->ins;
    const it_sample_t *s = v->smp_p;
    int ev = 64 * 256, ep = 0, ef = 0, end0 = 0;
    uint32_t b;
    int vol, pan;
    int32_t extra = v->mod_lf;
    uint32_t freq, step;
    uint16_t ramp;

    if (v->state == IT_VS_FREE) return;
    if (v->state == IT_VS_DYING) {
        if (f->ramp == 0 || !(f->flags & FM_ACTIVE)) vox_free(p, i);
        return;
    }
    /* For non-looped samples or BG voices with their sample ended, free when sample is done.
       For looped samples, the voice lives until fadeout/envelope kills it. */
    if (!(f->flags & FM_ACTIVE)) { vox_free(p, i); return; }
    if (!(f->flags & FM_LOOP) && f->pos >= f->length) { vox_free(p, i); return; }
    /* BG voice with sample end should die unless it has active fadeout/envelope sustaining it */
    if (v->state == IT_VS_BG && (f->flags & FM_LOOP) && f->pos >= f->length) {
        if (!ins || !ins->fadeout) { vox_free(p, i); return; }
    }

    if (ins) {
        if ((ins->vol.flags & IT_ENV_ON) && !(v->env_off & 1)) {
            ev = env_eval(&ins->vol, v->env_pos[0]);
            end0 = env_ended_at_zero(&ins->vol, v->env_pos[0]);
        }
        if ((ins->pan.flags & IT_ENV_ON) && !(v->env_off & 2))
            ep = env_eval(&ins->pan, v->env_pos[1]);
        if ((ins->pitch.flags & IT_ENV_ON) && !(ins->pitch.flags & IT_ENV_FILTER) && !(v->env_off & 4))
            ef = env_eval(&ins->pitch, v->env_pos[2]);
        if ((v->keyoff || v->fadeflag) && ins->fadeout) {
            uint32_t dec = (uint32_t)ins->fadeout * (ins->old_format ? 64u : 32u);
            v->fade = v->fade > dec ? v->fade - dec : 0;
        }
    }
    if (v->fade == 0 || end0) { vox_kill(p, i); return; }

    /* volume: everything normalised so that 16384 == unity */
    b = (uint32_t)v->vol256 * v->cvol;                   /* 256*64            */
    b = b * s->gvl >> 6;
    if (ins) b = b * ins->gbv >> 7;
    b = b * p->gv >> 7;
    b = (b * (uint32_t)ev) >> 14;
    b = (b * (v->fade >> 4)) >> 12;
    b = b * p->mv >> 7;
    vol = (int)(b >> 6);
    vol = clampi((int)(((int64_t)vol * p->out_gain) >> 8), 0, 256);

    /* pan */
    pan = v->pan256;
    if (ep) pan += ep * (128 - absi(pan - 128)) / (32 * 256);
    if (ins && ins->pps) pan += ((int)v->note - (int)ins->ppc) * ins->pps / 8;
    pan = clampi(pan, 0, 256);
    if (v->surround || !(p->m->flags & IT_F_STEREO)) pan = 128;
    else pan = 128 + (pan - 128) * (int)p->sep / 128;
    pan = clampi(pan - 128, -128, 127);

    /* auto-vibrato */
    if (s->vid) {
        int w;
        if (s->vir == 0) v->avib_depth = (uint16_t)(s->vid << 8);
        else { uint32_t d = (uint32_t)v->avib_depth + ((uint32_t)s->vir << 3); uint32_t mx = (uint32_t)s->vid << 8; v->avib_depth = (uint16_t)(d > mx ? mx : d); }
        w = wave_val(p, s->vit, v->avib_pos);
        extra += w * (int)v->avib_depth / 16384;
        v->avib_pos = (uint16_t)((v->avib_pos + s->vis) & 255u);
    }
    extra += ef * 32 / 256;

    freq = calc_freq(p, s, v->pitch, extra);
    step = (uint32_t)(((uint64_t)freq * p->step_mul) >> 32);
    if (step == 0) step = 1;
    f->step = step;

    ramp = v->fresh ? (uint16_t)START_RAMP : (uint16_t)clampi((int)(p->tick_len_q16 >> 16), 1, 65535);
    fm_voice_set_gain(f, vol, pan, ramp);
    v->fresh = 0;
    v->freq = freq; v->out_vol = vol; v->out_pan = pan;

    if (ins) {
        if ((ins->vol.flags & IT_ENV_ON) && !(v->env_off & 1))   env_advance(&ins->vol,   &v->env_pos[0], v->keyoff);
        if ((ins->pan.flags & IT_ENV_ON) && !(v->env_off & 2))   env_advance(&ins->pan,   &v->env_pos[1], v->keyoff);
        if ((ins->pitch.flags & IT_ENV_ON) && !(v->env_off & 4)) env_advance(&ins->pitch, &v->env_pos[2], v->keyoff);
    }
    /* BG voice dies if computed volume reaches zero (from envelope, fadeout, or channel volume).
       Don't kill while ramping to avoid clicks. This covers:
       - Envelope ended at zero (ev -> 0 -> vol -> 0)
       - Fadeout completed (fade -> 0 -> vol -> 0)
       - Channel/sample volume zero
       Main bug fix: envelope-disabled voices were stuck because ev stayed at default. */
    if (v->state == IT_VS_BG && vol == 0 && f->ramp == 0) vox_kill(p, i);
}

/* ======================================================================= */
/* channel: notes                                                          */
/* ======================================================================= */
static void apply_defaults(it_player_t *p, it_chn_t *ch, const it_sample_t *s, const it_instrument_t *ins)
{
    int vol = s->vol, pan = -1;
    if (ins && ins->rv) vol += ((int)(rnd(p) % (2u * ins->rv + 1u)) - (int)ins->rv) * vol / 100;
    ch->vol = (uint8_t)clampi(vol, 0, 64);
    if (s->dfp & 0x80) pan = s->dfp & 0x7F;
    if (ins && !(ins->dfp & 0x80)) pan = ins->dfp & 0x7F;
    if (pan >= 0) { ch->pan = (uint8_t)clampi(pan, 0, 64); ch->surround = 0; }
    if (ins && ins->rp)
        ch->pan = (uint8_t)clampi((int)ch->pan + (int)(rnd(p) % (2u * ins->rp + 1u)) - (int)ins->rp, 0, 64);
}

static int tp_requested(const it_chn_t *ch)
{
    return ch->cmd == 7 || ch->cmd == 12 || (ch->has_vcol && ch->vcol >= 193 && ch->vcol <= 202);
}

static int resolve_sample(const it_player_t *p, const it_chn_t *ch, unsigned n,
                          const it_instrument_t **pins, unsigned *psmp, unsigned *pntr)
{
    const it_module_t *m = p->m;
    *pins = NULL; *pntr = n;
    if (!ch->inst) return 0;
    if (p->instmode) {
        const it_instrument_t *ins = &m->ins[ch->inst - 1];
        if (!ins->present) return 0;
        *pins = ins; *psmp = ins->kb_smp[n]; *pntr = ins->kb_note[n];
        if (!*psmp || *psmp > m->smpnum) return 0;
    } else {
        *psmp = ch->inst;
    }
    return 1;
}

static void note_on(it_player_t *p, unsigned ci, unsigned n, int set_def)
{
    it_chn_t *ch = &p->chn[ci];
    const it_instrument_t *ins; const it_sample_t *s;
    unsigned smpno = 0, ntr = n;
    uint32_t off = 0;

    if (!resolve_sample(p, ch, n, &ins, &smpno, &ntr)) return;
    s = &p->m->smp[smpno - 1];
    if (!s->pcm || !s->length) return;

    if (tp_requested(ch) && ch->vidx >= 0 && p->vox[ch->vidx].state == IT_VS_MASTER) {
        ch->target = note_pitch(p, ntr, s->c5speed);
        ch->last_note = (uint8_t)n;
        if (set_def) apply_defaults(p, ch, s, ins);
        return;
    }

    if (p->instmode) do_nna(p, ci, ins, smpno, ntr);
    else if (ch->vidx >= 0) vox_kill(p, ch->vidx);

    if (set_def) apply_defaults(p, ch, s, ins);
    ch->pitch = ch->target = note_pitch(p, ntr, s->c5speed);
    ch->last_note = (uint8_t)n;

    if (ch->cmd == 15) {
        unsigned prm = ch->param;
        if (prm) ch->m_off = (uint8_t)prm; else prm = ch->m_off;
        off = ((uint32_t)prm << 8) + ((uint32_t)ch->hi_off << 16);
        if (off >= s->length) off = s->loop_mode ? s->loop_begin : s->length;   /* see PLAYER_NOTES */
    }
    voice_start(p, ci, s, smpno, ins, ch->inst, n, ntr, off);

    if (!(ch->vib_wave & 4)) ch->vib_pos = 0;
    if (!(ch->trm_wave & 4)) ch->trm_pos = 0;
    if (!(ch->pbr_wave & 4)) ch->pbr_pos = 0;
    ch->tremor_cnt = 0; ch->rtg_cnt = 0;
}

static void chn_note(it_player_t *p, unsigned ci, const it_cell_t *c)
{
    it_chn_t *ch = &p->chn[ci];
    int set_def = 0;
    unsigned n;

    if (c->flags & IT_CF_INST) {
        unsigned lim = p->instmode ? p->m->insnum : p->m->smpnum;
        if (c->inst >= 1 && c->inst <= lim) { ch->inst = c->inst; set_def = 1; }
    }
    if (!(c->flags & IT_CF_NOTE)) {
        if (set_def && ch->vidx >= 0) {            /* instrument without note: defaults only */
            const it_instrument_t *ins; unsigned smpno, ntr;
            if (resolve_sample(p, ch, ch->last_note < 120 ? ch->last_note : 0, &ins, &smpno, &ntr))
                apply_defaults(p, ch, &p->m->smp[smpno - 1], ins);
        }
        return;
    }
    n = c->note;
    if (n < 120)        note_on(p, ci, n, set_def);
    else if (n == 255)  { if (ch->vidx >= 0) voice_keyoff(p, ch->vidx); }
    else if (n == 254)  { if (ch->vidx >= 0) vox_kill(p, ch->vidx); }
    else                { if (ch->vidx >= 0) p->vox[ch->vidx].fadeflag = 1; }
}

/* ======================================================================= */
/* channel: effects                                                        */
/* ======================================================================= */
static int slide_d(int val, int max, unsigned prm, int t0)
{
    unsigned hi = prm >> 4, lo = prm & 15u;
    if (lo == 0)          { if (!t0) val += (int)hi; }
    else if (hi == 0)     { if (!t0) val -= (int)lo; }
    else if (lo == 0xF)   { if (t0)  val += (int)hi; }
    else if (hi == 0xF)   { if (t0)  val -= (int)lo; }
    return clampi(val, 0, max);
}

static void set_tempo(it_player_t *p, int t)
{
    p->tempo = (uint8_t)clampi(t, 32, 255);
    p->tick_len_q16 = (uint32_t)(((uint64_t)p->mixrate * 5u << 16) / (2u * p->tempo));
}

static void porta_exec(it_player_t *p, it_chn_t *ch, int up, unsigned prm, int t0)
{
    if (prm >= 0xF0)      { if (t0) pitch_slide(ch, p, up, (int)(prm & 15u) * 4); }
    else if (prm >= 0xE0) { if (t0) pitch_slide(ch, p, up, (int)(prm & 15u)); }
    else if (!t0)         pitch_slide(ch, p, up, (int)prm * 4);
}

static unsigned tp_param(it_player_t *p, it_chn_t *ch, unsigned prm)
{
    if (prm) { if (p->compatg) ch->m_pu = (uint8_t)prm; else ch->m_pg = (uint8_t)prm; return prm; }
    return p->compatg ? ch->m_pu : ch->m_pg;
}

static void vib_set(it_chn_t *ch, unsigned prm)
{
    if (prm & 0xF0u) ch->vib_speed = (uint8_t)(prm >> 4);
    if (prm & 0x0Fu) ch->vib_depth = (uint8_t)(prm & 15u);
}

static void vib_run(it_player_t *p, it_chn_t *ch, int t0, int fine)
{
    int w, d;
    if (p->oldeff && t0) return;
    w = wave_val(p, ch->vib_wave, ch->vib_pos);
    d = w * ((int)ch->vib_depth * 4) / (p->oldeff ? 64 : 128);
    if (fine) d /= 4;
    ch->mod_lf += d;
    ch->vib_pos = (uint8_t)(ch->vib_pos + ch->vib_speed * 4u);
}

static void retrig_vol(it_chn_t *ch, unsigned x)
{
    int v = ch->vol;
    switch (x) {
    case 1: v -= 1; break;  case 2: v -= 2; break;  case 3: v -= 4; break;
    case 4: v -= 8; break;  case 5: v -= 16; break; case 6: v = v * 2 / 3; break;
    case 7: v /= 2; break;  case 9: v += 1; break;  case 10: v += 2; break;
    case 11: v += 4; break; case 12: v += 8; break; case 13: v += 16; break;
    case 14: v = v * 3 / 2; break; case 15: v *= 2; break;
    default: break;
    }
    ch->vol = (uint8_t)clampi(v, 0, 64);
}

static void s_cmd(it_player_t *p, unsigned ci, unsigned prm, int t0)
{
    it_chn_t *ch = &p->chn[ci];
    unsigned x = prm & 15u;
    int i;
    if (!t0) return;                        /* all handled at tick 0 (SCx/SDx scheduled at row start) */
    switch (prm >> 4) {
    case 0x3: ch->vib_wave = (uint8_t)x; break;
    case 0x4: ch->trm_wave = (uint8_t)x; break;
    case 0x5: ch->pbr_wave = (uint8_t)x; break;
    case 0x6: p->finedelay = (uint8_t)(p->finedelay + x); break;
    case 0x7:
        if (x <= 2) {
            for (i = 0; i < p->nv_hi; i++) {
                it_voice_t *v = &p->vox[i];
                if (v->state != IT_VS_BG || v->host != ci) continue;
                if (x == 0) vox_kill(p, i); else if (x == 1) voice_keyoff(p, i); else v->fadeflag = 1;
            }
        } else if (ch->vidx >= 0) {
            it_voice_t *v = &p->vox[ch->vidx];
            switch (x) {
            case 3: v->nna = IT_NNA_CUT;  break;  case 4: v->nna = IT_NNA_CONT; break;
            case 5: v->nna = IT_NNA_OFF;  break;  case 6: v->nna = IT_NNA_FADE; break;
            case 7: v->env_off |= 1;  break;      case 8: v->env_off &= (uint8_t)~1; break;
            case 9: v->env_off |= 2;  break;      case 10: v->env_off &= (uint8_t)~2; break;
            case 11: v->env_off |= 4; break;      case 12: v->env_off &= (uint8_t)~4; break;
            default: break;
            }
        }
        break;
    case 0x8: ch->pan = (uint8_t)clampi((int)(x * 17u + 2u) >> 2, 0, 64); ch->surround = 0; break;
    case 0x9: if (x == 1) ch->surround = 1; break;
    case 0xA: ch->hi_off = (uint8_t)x; break;
    case 0xB:
        if (x == 0) ch->loop_row = p->row;
        else {
            if (ch->loop_cnt == 0) ch->loop_cnt = (uint8_t)x; else ch->loop_cnt--;
            if (ch->loop_cnt) p->loop_row = (int16_t)ch->loop_row;
        }
        break;
    case 0xE: p->patdelay = (uint8_t)x; break;
    default: break;                         /* S0x/S1x/S2x/SFx: not implemented */
    }
}

static void volcol(it_player_t *p, unsigned ci, int t0)
{
    it_chn_t *ch = &p->chn[ci];
    unsigned v = ch->vcol, prm;
    if (!ch->has_vcol) return;
    if (v <= 64) { if (t0) ch->vol = (uint8_t)v; return; }
    if (v >= 128 && v <= 192) { if (t0) { ch->pan = (uint8_t)(v - 128); ch->surround = 0; } return; }
    if (v <= 124) {
        if (v <= 114 && v >= 65) {                                  /* a..d, e */
            prm = (v - 65) % 10;
            if (v <= 104) {
                if (prm) ch->m_vcvs = (uint8_t)prm; else prm = ch->m_vcvs;
                switch ((v - 65) / 10) {
                case 0: if (t0)  ch->vol = (uint8_t)clampi(ch->vol + (int)prm, 0, 64); break;
                case 1: if (t0)  ch->vol = (uint8_t)clampi(ch->vol - (int)prm, 0, 64); break;
                case 2: if (!t0) ch->vol = (uint8_t)clampi(ch->vol + (int)prm, 0, 64); break;
                default: if (!t0) ch->vol = (uint8_t)clampi(ch->vol - (int)prm, 0, 64); break;
                }
            } else {                                                  /* e: porta down */
                if (prm) ch->m_pu = (uint8_t)prm; else prm = ch->m_pu;
                if (!t0) pitch_slide(ch, p, 0, (int)prm * 4);
            }
        } else if (v >= 115) {                                        /* f: porta up */
            prm = v - 115;
            if (prm) ch->m_pu = (uint8_t)prm; else prm = ch->m_pu;
            if (!t0) pitch_slide(ch, p, 1, (int)prm * 4);
        }
        return;
    }
    if (v >= 193 && v <= 202) {                                       /* g */
        prm = vcol_g_tab[v - 193];
        prm = tp_param(p, ch, prm);
        if (!t0) tone_porta(ch, p, (int)prm * 4);
    } else if (v >= 203 && v <= 212) {                                /* h */
        prm = v - 203;
        if (prm) ch->vib_depth = (uint8_t)prm;
        vib_run(p, ch, t0, 0);
    }
}

static void chn_fx(it_player_t *p, unsigned ci, unsigned tick)
{
    it_chn_t *ch = &p->chn[ci];
    unsigned prm = ch->param, hi = prm >> 4, lo = prm & 15u;
    int t0 = (tick == 0);

    switch (ch->cmd) {
    case 1:  if (t0 && prm) p->speed = (uint8_t)prm; break;                       /* A */
    case 2:  if (t0) p->jump_ord = (int16_t)prm; break;                           /* B */
    case 3:  if (t0) p->break_row = (int16_t)prm; break;                          /* C */
    case 4:  if (prm) ch->m_vs = (uint8_t)prm; else prm = ch->m_vs;               /* D */
             ch->vol = (uint8_t)slide_d(ch->vol, 64, prm, t0); break;
    case 5:  if (prm) ch->m_pu = (uint8_t)prm; else prm = ch->m_pu;               /* E */
             porta_exec(p, ch, 0, prm, t0); break;
    case 6:  if (prm) ch->m_pu = (uint8_t)prm; else prm = ch->m_pu;               /* F */
             porta_exec(p, ch, 1, prm, t0); break;
    case 7:  prm = tp_param(p, ch, prm);                                          /* G */
             if (!t0) tone_porta(ch, p, (int)prm * 4);
             break;
    case 8:  vib_set(ch, prm); ch->vib_fine = 0; vib_run(p, ch, t0, 0); break;    /* H */
    case 9: {                                                                     /* I */
        unsigned on, off, tot;
        if (prm) ch->m_trm = (uint8_t)prm; else prm = ch->m_trm;
        on = prm >> 4; off = prm & 15u;
        if (!p->oldeff) { on++; off++; }
        tot = on + off;
        if (tot) ch->tremor_mute = (uint8_t)((ch->tremor_cnt % tot) >= on);
        ch->tremor_cnt++;
        break; }
    case 10: {                                                                    /* J */
        unsigned ph = tick % 3u;
        if (prm) ch->m_arp = (uint8_t)prm; else prm = ch->m_arp;
        if (ph == 1) ch->mod_lf += (int)(prm >> 4) * 64;
        else if (ph == 2) ch->mod_lf += (int)(prm & 15u) * 64;
        break; }
    case 11: if (prm) ch->m_vs = (uint8_t)prm; else prm = ch->m_vs;               /* K */
             vib_run(p, ch, t0, 0);
             ch->vol = (uint8_t)slide_d(ch->vol, 64, prm, t0); break;
    case 12: if (prm) ch->m_vs = (uint8_t)prm; else prm = ch->m_vs;               /* L */
             if (!t0) tone_porta(ch, p, (int)tp_param(p, ch, 0) * 4);
             ch->vol = (uint8_t)slide_d(ch->vol, 64, prm, t0); break;
    case 13: if (t0) ch->cvol = (uint8_t)(prm > 64 ? 64 : prm); break;            /* M */
    case 14: if (prm) ch->m_cvs = (uint8_t)prm; else prm = ch->m_cvs;             /* N */
             ch->cvol = (uint8_t)slide_d(ch->cvol, 64, prm, t0); break;
    case 15: break;                                                               /* O: at note start */
    case 16: {                                                                    /* P */
        int pan = ch->pan;
        if (prm) ch->m_pans = (uint8_t)prm; else prm = ch->m_pans;
        hi = prm >> 4; lo = prm & 15u;
        if (lo == 0)            { if (!t0) pan -= (int)hi; }
        else if (hi == 0)       { if (!t0) pan += (int)lo; }
        else if (lo == 0xF)     { if (t0)  pan -= (int)hi; }
        else if (hi == 0xF)     { if (t0)  pan += (int)lo; }
        ch->pan = (uint8_t)clampi(pan, 0, 64); ch->surround = 0;
        break; }
    case 17: {                                                                    /* Q */
        if (prm) ch->m_rtg = (uint8_t)prm; else prm = ch->m_rtg;
        lo = prm & 15u; hi = prm >> 4;
        if (!t0 && lo) {
            ch->rtg_cnt++;
            if (ch->rtg_cnt >= lo) {
                ch->rtg_cnt = 0;
                retrig_vol(ch, hi);
                if (ch->vidx >= 0) { fm_voice_t *f = &p->fm[ch->vidx]; f->pos = 0; f->frac = 0; f->dir = 0; }
            }
        }
        break; }
    case 18: {                                                                    /* R */
        int w;
        if (prm & 0xF0u) ch->trm_speed = (uint8_t)(prm >> 4);
        if (prm & 0x0Fu) ch->trm_depth = (uint8_t)(prm & 15u);
        if (p->oldeff && t0) break;
        w = wave_val(p, ch->trm_wave, ch->trm_pos);
        ch->vol_delta = (int16_t)(w * ((int)ch->trm_depth * 4) / 32);
        ch->trm_pos = (uint8_t)(ch->trm_pos + ch->trm_speed * 4u);
        break; }
    case 19: s_cmd(p, ci, prm, t0); break;                                        /* S */
    case 20:                                                                      /* T */
        if (prm) ch->m_tempo = (uint8_t)prm; else prm = ch->m_tempo;
        if (prm >= 0x20) { if (t0) set_tempo(p, (int)prm); }
        else if (!t0) {
            if (prm >> 4 == 0) set_tempo(p, (int)p->tempo - (int)(prm & 15u));
            else if (prm >> 4 == 1) set_tempo(p, (int)p->tempo + (int)(prm & 15u));
        }
        break;
    case 21: vib_set(ch, prm); vib_run(p, ch, t0, 1); break;                      /* U */
    case 22: if (t0 && prm <= 128) p->gv = (uint8_t)prm; break;                   /* V */
    case 23: if (prm) ch->m_gvs = (uint8_t)prm; else prm = ch->m_gvs;             /* W */
             p->gv = (uint8_t)slide_d(p->gv, 128, prm, t0); break;
    case 24: if (t0) { ch->pan = (uint8_t)clampi((int)(prm + 2u) >> 2, 0, 64); ch->surround = 0; } break; /* X */
    case 25: {                                                                    /* Y */
        int w;
        if (prm & 0xF0u) ch->pbr_speed = (uint8_t)(prm >> 4);
        if (prm & 0x0Fu) ch->pbr_depth = (uint8_t)(prm & 15u);
        w = wave_val(p, ch->pbr_wave, ch->pbr_pos);
        ch->pan_delta = (int16_t)((w * (int)ch->pbr_depth) / 8);
        ch->pbr_pos = (uint8_t)(ch->pbr_pos + ch->pbr_speed * 4u);
        break; }
    default: break;                                                               /* Z: MIDI macros not implemented */
    }
}

static void chn_update_voice(it_player_t *p, unsigned ci)
{
    it_chn_t *ch = &p->chn[ci];
    it_voice_t *v;
    int vol;
    if (ch->vidx < 0) return;
    v = &p->vox[ch->vidx];
    vol = ch->vol * 4 + ch->vol_delta;
    if (ch->tremor_mute || ch->muted) vol = 0;
    v->vol256 = (int16_t)clampi(vol, 0, 256);
    v->cvol = ch->cvol;
    v->pan256 = (int16_t)clampi(ch->pan * 4 + ch->pan_delta, 0, 256);
    v->surround = ch->surround;
    v->pitch = ch->pitch;
    v->mod_lf = ch->mod_lf;
}

/* ======================================================================= */
/* song position                                                           */
/* ======================================================================= */
static const it_pattern_t *cur_pat(const it_player_t *p)
{
    return p->pat < p->m->patnum ? &p->m->pat[p->pat] : &g_empty_pat;
}

static void resolve_order(it_player_t *p, int ord, int row)
{
    const it_module_t *m = p->m;
    int guard = 0;
    for (;;) {
        unsigned o;
        if (++guard > 2048) { p->finished = 1; p->pat = 0xFFFF; p->ord = 0; p->row = 0; p->rows = 64; return; }
        if (ord >= (int)m->song_len || ord >= (int)m->ordnum) {
            p->loops++;
            if (!(p->flags & IT_PL_LOOP)) p->finished = 1;
            ord = 0; row = 0;
            if (m->song_len == 0) { p->finished = 1; p->pat = 0xFFFF; p->ord = 0; p->row = 0; p->rows = 64; return; }
            continue;
        }
        o = m->orders[ord];
        if (o == 255) { ord = m->song_len; continue; }
        if (o == 254 || o >= m->patnum) { ord++; continue; }
        p->ord = (uint16_t)ord; p->pat = (uint16_t)o;
        p->rows = m->pat[o].rows;
        p->row = (uint16_t)(row >= p->rows ? 0 : row);
        return;
    }
}

static void advance_row(it_player_t *p)
{
    int ord = p->ord, row = p->row + 1, newpat = 0;
    if (p->jump_ord >= 0 || p->break_row >= 0) {
        ord = p->jump_ord >= 0 ? p->jump_ord : ord + 1;
        row = p->break_row >= 0 ? p->break_row : 0;
        newpat = 1;
    } else if (p->loop_row >= 0) {
        row = p->loop_row;
    } else if (row >= p->rows) {
        ord++; row = 0; newpat = 1;
    }
    p->jump_ord = p->break_row = p->loop_row = -1;
    if (newpat) resolve_order(p, ord, row);
    else p->row = (uint16_t)row;
}

/* ======================================================================= */
/* row / tick                                                              */
/* ======================================================================= */
static void row_start(it_player_t *p)
{
    const it_pattern_t *pt = cur_pat(p);
    unsigned ci;
    if (p->dec_pat != p->pat || p->dec_next != p->row) {
        it_patdec_seek(&p->dec, pt, p->row);
        p->dec_pat = p->pat;
    }
    it_patdec_row(&p->dec, p->cells, p->present);
    p->dec_next = (uint16_t)(p->row + 1u);
    p->patdelay = 0; p->finedelay = 0;

    for (ci = 0; ci < p->nch; ci++) {
        it_chn_t *ch = &p->chn[ci];
        int has = (p->present[ci >> 5] >> (ci & 31u)) & 1u;
        const it_cell_t *c = &p->cells[ci];
        ch->cmd = 0; ch->param = 0; ch->has_vcol = 0; ch->cut_tick = 0; ch->nd_pending = 0;
        if (!has) continue;
        if (c->flags & IT_CF_CMD) { ch->cmd = c->cmd; ch->param = c->param; }
        if (c->flags & IT_CF_VOL) { ch->vcol = c->vol; ch->has_vcol = 1; }
        if (ch->cmd == 19) {
            if (ch->param) ch->m_s = ch->param; else ch->param = ch->m_s;
            if ((ch->param >> 4) == 0xC) ch->cut_tick = (uint8_t)((ch->param & 15u) ? (ch->param & 15u) : 1u);
            if ((ch->param >> 4) == 0xD) {
                unsigned d = (ch->param & 15u) ? (ch->param & 15u) : 1u;
                if (d < p->speed && (c->flags & (IT_CF_NOTE | IT_CF_INST))) {
                    ch->nd_pending = 1; ch->nd_tick = (uint8_t)d; ch->nd_cell = *c;
                }
            }
        }
    }
}

static void tick_channels(it_player_t *p, unsigned tick)
{
    unsigned ci;
    for (ci = 0; ci < p->nch; ci++) {
        it_chn_t *ch = &p->chn[ci];
        ch->mod_lf = 0; ch->vol_delta = 0; ch->pan_delta = 0; ch->tremor_mute = 0;
        if (tick == 0) {
            if ((p->present[ci >> 5] >> (ci & 31u)) & 1u) {
                if (!ch->nd_pending) { chn_note(p, ci, &p->cells[ci]); volcol(p, ci, 1); }
            }
        } else {
            if (ch->nd_pending && tick == ch->nd_tick) {
                chn_note(p, ci, &ch->nd_cell); volcol(p, ci, 1); ch->nd_pending = 0;
            } else if (ch->has_vcol) volcol(p, ci, 0);
            if (ch->cut_tick && tick == ch->cut_tick && ch->vidx >= 0) vox_kill(p, ch->vidx);
        }
        if (ch->cmd) chn_fx(p, ci, tick);
    }
}

void it_player_tick(it_player_t *p)
{
    unsigned ci;
    int i;
    if (p->finished) {
        for (i = 0; i < p->nv_hi; i++) if (p->vox[i].state != IT_VS_FREE) vox_free(p, i);
        return;
    }
    p->ticks++;
    if (p->tick == 0) row_start(p);
    tick_channels(p, p->tick);
    if (p->tick == 0)
        p->row_ticks = (uint16_t)(p->speed * (1u + p->patdelay) + p->finedelay);

    for (ci = 0; ci < p->nch; ci++) chn_update_voice(p, ci);
    for (i = 0; i < p->nv_hi; i++) voice_tick(p, i);

    if (++p->tick >= p->row_ticks) { p->tick = 0; advance_row(p); }
}

/* ======================================================================= */
/* init / render                                                           */
/* ======================================================================= */
void it_player_cfg_default(it_player_cfg_t *c)
{
    c->mixrate = 48000; c->flags = IT_PL_LOOP; c->out_gain = 256;
}

void it_player_init(it_player_t *p, const it_module_t *m, const it_player_cfg_t *cfg)
{
    it_player_cfg_t def;
    unsigned ci;
    if (!cfg) { it_player_cfg_default(&def); cfg = &def; }
    memset(p, 0, sizeof *p);
    p->m = m;
    p->mixrate = cfg->mixrate ? cfg->mixrate : 48000u;
    p->flags = cfg->flags;
    p->out_gain = cfg->out_gain ? cfg->out_gain : 256;
    p->step_mul = (1ull << 48) / p->mixrate;
    p->linear = (m->flags & IT_F_LINEAR) != 0;
    p->oldeff = (m->flags & IT_F_OLDEFF) != 0;
    p->compatg = (m->flags & IT_F_COMPATGXX) != 0;
    p->instmode = (m->flags & IT_F_INSTR) != 0;
    p->nch = m->used_channels;
    p->speed = m->speed ? m->speed : 6; p->gv = m->gv; p->mv = m->mv; p->sep = m->sep;
    p->rng = 0x12345678u;
    p->jump_ord = p->break_row = p->loop_row = -1;
    p->dec_pat = 0xFFFF;
    set_tempo(p, m->tempo);
    for (ci = 0; ci < IT_MAX_CHANNELS; ci++) {
        it_chn_t *ch = &p->chn[ci];
        unsigned pn = m->chn_pan[ci];
        ch->vidx = -1;
        ch->cvol = m->chn_vol[ci];
        ch->vol = 64;
        if (pn == 100) { ch->pan = 32; ch->surround = 1; }
        else if (pn >= 128) { ch->pan = 32; ch->muted = 1; }
        else ch->pan = (uint8_t)(pn > 64 ? 64 : pn);
    }
    for (ci = 0; ci < FM_MAX_VOICES; ci++) fm_voice_init(&p->fm[ci]);
    resolve_order(p, 0, 0);
}

uint32_t it_player_tick_frames(it_player_t *p) { return p->tick_len_q16 >> 16; }

int it_player_active_voices(const it_player_t *p)
{
    int i, n = 0;
    for (i = 0; i < p->nv_hi; i++)
        if (p->vox[i].state != IT_VS_FREE && p->vox[i].state != IT_VS_DYING && p->vox[i].out_vol > 0) n++;
    return n;
}

void it_player_render(it_player_t *p, int16_t *dst, int frames)
{
    while (frames > 0) {
        int n;
        if (p->left == 0) {
            it_player_tick(p);
            p->tick_frac += p->tick_len_q16;
            p->left = p->tick_frac >> 16;
            p->tick_frac &= 0xFFFFu;
            if (p->left == 0) continue;
        }
        n = frames < (int)p->left ? frames : (int)p->left;
        it_mix(dst, n, p->fm, p->nv_hi);
        dst += 2 * n; frames -= n; p->left -= (uint32_t)n;
    }
}
