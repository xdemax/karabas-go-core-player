// S3M (Scream Tracker 3) parser and replayer. Compatible with XM/IT engines.
#include <string.h>
#include "s3m_fpga.h"
#include "fastmix.h"
#include "mmio.h"
#include "audio.h"
#include "mixer.h"
#include "analysis.h"

#define S3M_META_BYTES  (32u * 1024u)
#define S3M_OUT_CHUNK   256
#define S3M_OUT_MAX     512

// S3M format structures
typedef struct {
  uint8_t note, instr, vol, eff, par;
} s3m_cell_t;

typedef struct {
  const uint8_t *data;
  uint32_t len, loop_start, loop_len;
  uint8_t vol, pan;
  uint8_t flags;  // bit 0: loop enabled; bit 1: stereo; bit 2: 16-bit
  uint8_t type;   // 0: PCM, 2: AdLib (not supported)
  uint32_t c2spd;  // base frequency in Hz (C-2)
} s3m_sample_t;

typedef struct {
  uint32_t dosname[3];
  uint8_t memseg;
  uint16_t length, loopbeg, loopend;
  uint8_t vol, dsk, pack, attr;
  uint8_t type;
} s3m_samphdr_t;

typedef struct {
  uint8_t type, button, vol, dsk;
  uint16_t memseg, length;
} s3m_insthdr_t;

typedef struct {
  const uint8_t *data;
  uint32_t len, pos, end;
} s3m_pattern_t;

typedef struct {
  uint32_t note, pos, step;
  uint32_t per, per_target, per_base;   /* MikMod Amiga period state */
  uint32_t c2spd;
  int smp, vol, pan, active, keyoff;
  uint8_t eff, par;
  int porta_speed, retrig_tick;
  int vol_slide, tone_slide;        /* per-channel parameter memory (0 = repeat last) */
  int32_t mix_vol, lgain, rgain;
  uint32_t vib_pos;
  int vib_speed, vib_depth;  // vibrato speed and depth
  int vol_cur;               // smoothed volume, Q8 of 0..64
  int end_ramp;              // 64..0 while a one-shot sample fades out
} s3m_voice_t;

typedef struct {
  const uint8_t *mod;
  uint32_t size;
  char title[29];
  int chn, npat, nins, nsmp;
  uint32_t orders[256];
  uint32_t ins_off[99];    /* instrument parapointers (were written past orders[] and
                              clobbered smp[]: only some samples ever loaded) */
  uint32_t pat_off[99];    /* pattern parapointers */
  s3m_sample_t smp[99];
  s3m_pattern_t pat[99];
  s3m_voice_t vox[32];
  int order, row, tick, speed, bpm, tleft, spt;
  int pat_break, break_row, pos_jump;
  int gvol, panning[32];
  int active, ended, paused;
  char err[48];
  int vol;
  int32_t last_l, last_r, peak_l, peak_r;
  uint8_t vu[FMT_CHANNELS];
  uint32_t sample_acc;
  uint32_t samples_per_tick;
} s3m_module_t;

static uint8_t s3m_meta[S3M_META_BYTES];
static s3m_module_t s3m_mod;
static uint8_t *s3m_file;
static uint32_t s3m_file_size;
static int s3m_active, s3m_ended, s3m_paused;
static int s3m_volume = 64;
static char s3m_err[48];
static int16_t s3m_out[S3M_OUT_CHUNK * 2];
static int32_t s3m_last_l, s3m_last_r, s3m_peak_l, s3m_peak_r;
static uint8_t s3m_vu[FMT_CHANNELS];
static char s3m_dbg[32];

static void s3m_error(const char *s) {
  int i = 0;
  for (; s && s[i] && i < (int) sizeof(s3m_err) - 1; i++) s3m_err[i] = s[i];
  s3m_err[i] = 0;
}

static void s3m_release(void) {
  s3m_active = s3m_ended = s3m_paused = 0;
  s3m_file = 0;
  s3m_file_size = 0;
  memset(s3m_vu, 0, sizeof(s3m_vu));
  s3m_last_l = s3m_last_r = s3m_peak_l = s3m_peak_r = 0;
  s3m_dbg[0] = 0;
}

static uint8_t read8(const uint8_t *p, uint32_t off, uint32_t max) {
  return off < max ? p[off] : 0;
}

static uint16_t read16le(const uint8_t *p, uint32_t off, uint32_t max) {
  if (off + 1 >= max) return 0;
  return p[off] | ((uint32_t)p[off+1] << 8);
}

static uint32_t read32le(const uint8_t *p, uint32_t off, uint32_t max) {
  if (off + 3 >= max) return 0;
  return p[off] | ((uint32_t)p[off+1] << 8) | ((uint32_t)p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
}

static int s3m_load_patterns(const uint8_t *buf, uint32_t size, s3m_module_t *m) {
  int max_chn = 0;
  
  for (int p = 0; p < m->npat; p++) {
    uint32_t off = m->pat_off[p];
    if (off == 0 || off * 16 >= size) {
      m->pat[p].data = NULL;
      m->pat[p].len = 0;
      continue;
    }
    off *= 16;
    uint16_t plen = read16le(buf, off, size);
    if (off + 2 + plen > size) {
      s3m_error("S3M pattern truncated");
      return -1;
    }
    m->pat[p].data = buf + off + 2;
    m->pat[p].len = plen;
    m->pat[p].pos = 0;
    m->pat[p].end = 0;
    
    // Scan to find max channel
    uint32_t pos = 0;
    while (pos < plen) {
      uint8_t what = buf[off + 2 + pos++];
      if (what == 0) break;
      
      uint8_t ch = what & 0x1f;
      if (ch + 1 > max_chn) max_chn = ch + 1;
      
      if (what & 0x20) pos += 2;   // note + instr
      if (what & 0x40) pos++;       // vol
      if (what & 0x80) pos += 2;    // cmd + info
    }
  }
  
  m->chn = (max_chn > 32) ? 32 : max_chn;
  if (m->chn < 1) m->chn = 1;
  
  return 0;
}

static int s3m_load_samples(const uint8_t *buf, uint32_t size, s3m_module_t *m, uint8_t *pcm_base, uint32_t pcm_size) {
  uint32_t pcm_off = 0;
  
  for (int s = 0; s < m->nsmp; s++) {
    uint32_t hoff = m->ins_off[s];
    if (hoff == 0) {
      m->smp[s].data = NULL;
      m->smp[s].len = 0;
      continue;
    }
    hoff *= 16;
    if (hoff + 32 > size) {
      m->smp[s].data = NULL;
      m->smp[s].len = 0;
      continue;
    }
    
    uint8_t type = read8(buf, hoff, size);
    
    if (type == 2) {
      s3m_error("S3M AdLib not supported");
      return -1;
    }
    if (type != 1) {
      m->smp[s].data = NULL;
      m->smp[s].len = 0;
      continue;
    }
    
    // sample data pointer is 3 bytes: a high byte at +13 and the low word at +14
    uint32_t memseg = ((uint32_t) read8(buf, hoff + 13, size) << 16)
                    | read16le(buf, hoff + 14, size);
    uint32_t doff = memseg * 16;
    
    // length at +16 u32
    uint32_t length = read32le(buf, hoff + 16, size);
    // loop start/end are 32-bit (+0x14, +0x18)
    uint32_t loopbeg = read32le(buf, hoff + 20, size);
    uint32_t loopend = read32le(buf, hoff + 24, size);
    // volume at +28
    uint8_t vol = read8(buf, hoff + 28, size);
    // flags at +31
    uint8_t flags = read8(buf, hoff + 31, size);
    // pan at +29
    uint8_t pan = read8(buf, hoff + 29, size);
    
    m->smp[s].len = length;
    m->smp[s].vol = vol;
    m->smp[s].flags = (flags & 1) ? 1 : 0;  // loop
    m->smp[s].c2spd = read32le(buf, hoff + 32, size);
    if (m->smp[s].c2spd < 100) m->smp[s].c2spd = 8363;
    
    if (doff + m->smp[s].len > size) {
      m->smp[s].data = NULL;
      m->smp[s].len = 0;
      continue;
    }
    
    if (pcm_off + m->smp[s].len > pcm_size) {
      s3m_error("S3M out of sample memory");
      return -1;
    }
    
    memcpy(pcm_base + pcm_off, buf + doff, m->smp[s].len);
    m->smp[s].data = pcm_base + pcm_off;
    
    if (flags & 1) {
      m->smp[s].loop_start = loopbeg;
      m->smp[s].loop_len = loopend > loopbeg ? loopend - loopbeg : 0;
      if (m->smp[s].loop_start > m->smp[s].len) m->smp[s].loop_start = m->smp[s].len;
      if (m->smp[s].loop_start + m->smp[s].loop_len > m->smp[s].len)
        m->smp[s].loop_len = m->smp[s].len - m->smp[s].loop_start;
    }
    
    if (pan == 32) {
      m->smp[s].pan = 128;
    } else if (pan & 0x80) {
      m->smp[s].pan = (pan & 0x0f) * 17;
    } else {
      m->smp[s].pan = 128;
    }
    
    pcm_off += m->smp[s].len;
  }
  return 0;
}

int s3m_fpga_start(const play_src_t *src, uint32_t size, play_progress_t progress) {
  s3m_release();
  if (!src || !size) { s3m_error("empty S3M"); return -1; }
  if (size > SDRAM_FILE_MAX) { s3m_error("S3M over 32 MB"); return -1; }
  
  uint8_t *buf;
  if (src->f) {
    uint32_t t0 = R_US;
    while (!(R_SDRAM & 1)) {
      if ((uint32_t)(R_US - t0) > 200000u) { s3m_error("SDRAM not ready"); return -1; }
    }
    buf = (uint8_t *) SDRAM_BASE;
    uint32_t off = 0;
    while (off < size) {
      uint32_t n = size - off;
      if (n > 4096) n = 4096;
      UINT got = 0;
      if (src_read((play_src_t *) src, off, buf + off, n, &got) != 0 || got == 0) {
        s3m_error("read error"); return -1;
      }
      off += got;
      if (progress && ((off >> 15) != ((off - got) >> 15) || off == size)) progress(off, size);
    }
  } else {
    buf = (uint8_t *) src->mem;
  }
  s3m_file = buf;
  s3m_file_size = size;
  
  if (size < 0x60) { s3m_error("S3M too small"); return -1; }
  if (memcmp(buf + 0x2c, "SCRM", 4)) { s3m_error("S3M invalid header"); return -1; }
  
  memcpy(s3m_mod.title, buf, 28);
  s3m_mod.title[28] = 0;
  
  uint16_t ordnum = read16le(buf, 0x20, size);
  uint16_t insnum = read16le(buf, 0x22, size);
  uint16_t patnum = read16le(buf, 0x24, size);
  
  if (ordnum > 256 || insnum > 99 || patnum > 99) {
    s3m_error("S3M limits exceeded");
    return -1;
  }
  
  s3m_mod.nins = insnum;
  s3m_mod.npat = patnum;
  s3m_mod.nsmp = insnum;
  
  for (int i = 0; i < ordnum; i++) s3m_mod.orders[i] = read8(buf, 0x60 + i, size);
  
  uint32_t ioff = 0x60 + ordnum;
  for (int i = 0; i < insnum; i++) {
    s3m_mod.ins_off[i] = read16le(buf, ioff + i*2, size);
  }
  
  uint32_t poff = ioff + insnum * 2;
  for (int i = 0; i < patnum; i++) {
    s3m_mod.pat_off[i] = read16le(buf, poff + i*2, size);
  }
  
  s3m_mod.chn = 0;
  
  uint8_t *pcm_base = s3m_meta;
  uint32_t pcm_size = sizeof(s3m_meta);
  
  if (s3m_load_samples(buf, size, &s3m_mod, pcm_base, pcm_size) < 0) return -1;
  if (s3m_load_patterns(buf, size, &s3m_mod) < 0) return -1;
  
  s3m_mod.gvol = read8(buf, 0x30, size) & 0x7f;
  s3m_mod.speed = read8(buf, 0x31, size);
  if (s3m_mod.speed < 1) s3m_mod.speed = 6;
  s3m_mod.bpm = read8(buf, 0x32, size);
  if (s3m_mod.bpm < 32) s3m_mod.bpm = 125;
  
  for (int i = 0; i < 32; i++) {
    /* MikMod's rule: the ST3.2 pan table applies only when bit 0x20 is set in the
     * channel byte; otherwise the old S3M default splits channels <8 left, >=8 right. */
    uint8_t chpan = read8(buf, 64 + i, size);
    if (chpan == 0xFF) {
      s3m_mod.panning[i] = 0x80;
    } else if (chpan & 0x20) {
      s3m_mod.panning[i] = (chpan & 0x0f) << 4;
    } else {
      s3m_mod.panning[i] = (chpan < 8) ? 0x30 : 0xC0;
    }
  }
  
  s3m_mod.pos_jump = -1;
  s3m_mod.pat_break = 0;
  s3m_mod.break_row = 0;
  s3m_mod.order = 0;
  s3m_mod.row = 0;
  s3m_mod.tick = s3m_mod.speed > 0 ? s3m_mod.speed - 1 : 0;
  s3m_mod.tleft = s3m_mod.spt = 0;
  
  s3m_mod.sample_acc = 0;
  s3m_mod.samples_per_tick = 120000 / s3m_mod.bpm;
  
  for (int i = 0; i < 32; i++) {
    s3m_mod.vox[i].active = 0;
    s3m_mod.vox[i].smp = -1;
    s3m_mod.vox[i].vol = 64;
    s3m_mod.vox[i].pan = 128;
  }
  
  s3m_mod.active = 1;
  s3m_mod.ended = 0;
  s3m_mod.paused = 0;
  s3m_error("");
  
  s3m_active = 1;
  s3m_ended = 0;
  
  fm_reset();
  fm_set_max_mixed(32);
  
  return 0;
}

/* MikMod's tables (the engine that plays this file correctly): Amiga periods,
 * vibrato table, rate = 8363*1712/period. Effects work on the period, not on a
 * frequency ratio. */
static const uint16_t s3m_amiga[24] = {
  0x6b00,0x6800,0x6500,0x6220,0x5f50,0x5c80,0x5a00,0x5740,
  0x54d0,0x5260,0x5010,0x4dc0,0x4b90,0x4960,0x4750,0x4540,
  0x4350,0x4160,0x3f90,0x3dc0,0x3c10,0x3a40,0x38b0,0x3700
};
static const uint8_t s3m_vibtab[128] = {
    0,  6, 13, 19, 25, 31, 37, 44, 50, 56, 62, 68, 74, 80, 86, 92,
   98,103,109,115,120,126,131,136,142,147,152,157,162,167,171,176,
  180,185,189,193,197,201,205,208,212,215,219,222,225,228,231,233,
  236,238,240,242,244,246,247,249,250,251,252,253,254,254,255,255,
  255,255,255,254,254,253,252,251,250,249,247,246,244,242,240,238,
  236,233,231,228,225,222,219,215,212,208,205,201,197,193,189,185,
  180,176,171,167,162,157,152,147,142,136,131,126,120,115,109,103,
   98, 92, 86, 80, 74, 68, 62, 56, 50, 44, 37, 31, 25, 19, 13,  6
};

static uint32_t s3m_step_from_period(uint32_t per) {
  if (!per) per = 1;
  uint32_t rate = (uint32_t) ((8363ull * 1712ull) / per);
  uint64_t step = ((uint64_t) rate << 16) / 48000u;
  return step > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t) step;
}

static uint32_t s3m_period_from_n12(int n12, uint32_t c2spd) {
  if (n12 < 0) n12 = 0;
  int n2 = n12 * 2;
  int o = n2 / 24;
  uint32_t p = s3m_amiga[n2 % 24];
  uint32_t per = (uint32_t) (((uint64_t) 8363u * p) >> (o > 15 ? 15 : o));
  per /= (c2spd ? c2spd : 8363u);
  return per ? per : 1;
}

static uint32_t s3m_period_for(int note, uint32_t c2spd) {
  int n12 = ((note >> 4) & 0x0F) * 12 + (note & 0x0F);
  return s3m_period_from_n12(n12, c2spd);
}

static void s3m_apply_period(s3m_voice_t *v) { v->step = s3m_step_from_period(v->per); }

/* Per-tick effects (G tone porta, H vibrato, J arpeggio, D volume slide). */
static void s3m_note_on(s3m_module_t *m, int ch, int note, int instr, int vol) {
  if (ch < 0 || ch >= 32) return;
  s3m_voice_t *v = &m->vox[ch];
  if (instr > 0 && instr <= m->nsmp) {
    v->smp = instr - 1;
    v->vol = m->smp[instr - 1].vol;
  }
  if (vol > 0 && vol <= 64) v->vol = vol;
  v->pan = 128;   /* openmpt plays this file centred */
  if (note > 0 && note < 254) {
    uint32_t c2spd = (v->smp >= 0 && v->smp < 99) ? m->smp[v->smp].c2spd : 8363;
    v->note = note;
    v->c2spd = c2spd;
    v->pos = 0;
    v->per = v->per_target = v->per_base = s3m_period_for(note, c2spd);
    s3m_apply_period(v);
    v->vib_pos = 0;
    v->end_ramp = 0;
    v->vol_cur = 0;
    v->active = 1;
    v->keyoff = 0;
  }
}

static void s3m_note_off(s3m_voice_t *v) {
  v->keyoff = 1;
  v->active = 0;
}

static void s3m_tick_effects(s3m_module_t *m) {
  int tick = m->tick;
  for (int ch = 0; ch < m->chn; ch++) {
    s3m_voice_t *v = &m->vox[ch];
    if (!v->active) continue;
    int eff = v->eff;
    int par = v->par;
    if (!eff) continue;

    if (eff == 4 || eff == 'D') {                 /* volume slide, parameter memory */
      int inf = par ? par : v->vol_slide;
      int hi = inf >> 4, lo = inf & 15;
      if (!lo) { if (tick) v->vol += hi; }
      else if (!hi) { if (tick) v->vol -= lo; }
      else if (lo == 0x0F) { if (!tick) v->vol += hi ? hi : 15; }
      else if (hi == 0x0F) { if (!tick) v->vol -= lo ? lo : 15; }
      if (v->vol < 0) v->vol = 0; else if (v->vol > 64) v->vol = 64;
    } else if (eff == 5 || eff == 'E' || eff == 6 || eff == 'F') {
      int inf = par ? par : v->tone_slide;
      int hi = inf >> 4, lo = inf & 15;
      int32_t d = 0;
      if (hi == 0x0F) { if (!tick) d = lo << 2; }
      else if (hi == 0x0E) { if (!tick) d = lo; }
      else if (tick) d = inf << 2;
      if (d) {
        int down = (eff == 5 || eff == 'E');      /* E slides the period up (pitch down) */
        if (down) v->per += (uint32_t) d;
        else v->per = v->per > (uint32_t) d ? v->per - (uint32_t) d : 1;
        v->per_base = v->per;
        s3m_apply_period(v);
      }
    } else if (eff == 7 || eff == 'G') {          /* tone portamento to the target note */
      if (tick) {
        uint32_t speed = (uint32_t) (v->porta_speed ? v->porta_speed : 1) * 4u;
        if (v->per > v->per_target) v->per = (v->per - v->per_target > speed) ? v->per - speed : v->per_target;
        else if (v->per < v->per_target) v->per = (v->per_target - v->per > speed) ? v->per + speed : v->per_target;
        v->per_base = v->per;                     /* vibrato modulates the slid pitch */
        s3m_apply_period(v);
      }
    } else if (eff == 8 || eff == 'H') {          /* vibrato on the period */
      if (tick) {
        if (par >> 4) v->vib_speed = (par & 0xF0) >> 2;
        if (par & 15) v->vib_depth = par & 15;
        int amp = s3m_vibtab[v->vib_pos & 0x7F];
        if ((int8_t) (v->vib_pos & 0xFF) < 0) amp = -amp;
        int32_t d = ((int32_t) amp * v->vib_depth) >> 7;
        d <<= 2;
        int32_t np = (int32_t) v->per_base + d;
        v->per = np > 0 ? (uint32_t) np : 1;
        s3m_apply_period(v);
        v->vib_pos += (uint32_t) (v->vib_speed ? v->vib_speed : 1);
      }
    } else if (eff == 10 || eff == 'J') {         /* arpeggio x/y semitones */
      int x = par >> 4, y = par & 15;
      int n12 = ((v->note >> 4) & 0x0F) * 12 + (v->note & 0x0F);
      int t = tick % 3;
      v->per = s3m_period_from_n12(n12 + (t == 1 ? x : t == 2 ? y : 0), v->c2spd);
      s3m_apply_period(v);
    }
  }
}

void s3m_fpga_service(void) {
  if (!s3m_active || s3m_paused) return;
  
  int free = audio_free();
  if (free <= 0) return;
  int n = free > S3M_OUT_CHUNK ? S3M_OUT_CHUNK : free;
  
  int out_count = 0;
  
  for (int sample = 0; sample < n; sample++) {
    s3m_module_t *m = &s3m_mod;
    
    // Tick management with accumulator
    m->sample_acc++;
    if (m->sample_acc >= m->samples_per_tick) {
      m->sample_acc = 0;
      m->tick++;
      if (m->tick >= m->speed) m->tick = 0;
      
      // Process next row
      if (m->tick == 0 && m->row < 64) {
        /* A command applies to its own row only; without this the last vibrato
         * or portamento kept running until another command replaced it. */
        for (int i = 0; i < m->chn; i++) m->vox[i].eff = 0;
        uint8_t ord_idx = m->orders[m->order];
        if (ord_idx < 0xFF && ord_idx < m->npat && m->pat[ord_idx].data) {
          s3m_pattern_t *pat = &m->pat[ord_idx];
          
          // Reset pattern pos for new row
          uint32_t row_pos = 0;
          int row_count = 0;
          uint32_t pos = 0;
          
          // Scan to current row
          while (pos < pat->len && row_count <= m->row) {
            uint32_t row_start = pos;
            int channels_in_row = 0;
            
            while (pos < pat->len) {
              uint8_t what = pat->data[pos++];
              if (what == 0) break;
              
              uint8_t ch = what & 0x1f;
              if (what & 0x20) pos += 2;
              if (what & 0x40) pos++;
              if (what & 0x80) pos += 2;
              
              channels_in_row++;
            }
            
            if (row_count == m->row) {
              row_pos = row_start;
              break;
            }
            row_count++;
          }
          
          // Decode cells at current row
          pos = row_pos;
          while (pos < pat->len) {
            uint8_t what = pat->data[pos++];
            if (what == 0) break;
            
            uint8_t ch = what & 0x1f;
            
            uint8_t note = 0, instr = 0, vol = 0, eff = 0, par = 0;
            int has_vol = 0;
            
            if (what & 0x20) {
              if (pos < pat->len) note = pat->data[pos++];
              if (pos < pat->len) instr = pat->data[pos++];
            }
            if (what & 0x40) {
              if (pos < pat->len) vol = pat->data[pos++];
              has_vol = 1;
            }
            if (what & 0x80) {
              if (pos < pat->len) eff = pat->data[pos++];
              if (pos < pat->len) par = pat->data[pos++];
            }
            
            if (ch < m->chn) {
              s3m_voice_t *v = &m->vox[ch];
              if (note && (eff == 7 || eff == 'G') && v->active) {
                /* tone portamento: slide to the new note instead of retriggering */
                uint32_t c2 = (v->smp >= 0 && v->smp < 99) ? m->smp[v->smp].c2spd : 8363;
                v->per_target = s3m_period_for(note, c2);
                if (par) v->porta_speed = par;
              } else if (note) {
                s3m_note_on(m, ch, note, instr, vol);
              }
              if (has_vol && vol <= 64) v->vol = vol;          /* 0 is a real volume */
              if (note == 254) s3m_note_off(v);                 /* ST3 key off */
              
              if (eff) {
                v->eff = eff;
                v->par = par;
                switch (eff) {
                  case 'A': case 1: if (par) m->speed = par; break;
                  case 'B': case 2: m->pos_jump = par; break;
                  case 'C': case 3:            /* old-style S3M: the row is BCD */
                    m->pat_break = 1;
                    m->break_row = ((par >> 4) * 10) + (par & 0x0F);
                    break;
                  case 'D': case 4: if (par) v->vol_slide = par; break;
                  case 'E': case 5: case 'F': case 6: if (par) v->tone_slide = par; break;
                  case 'G': case 7: if (par) v->porta_speed = par; break;
                  case 'H': case 8: break;
                  case 'J': case 10: break;
                  case 'V': m->gvol = par & 0x7f; break;
                }
              }
            }
          }
        }
      }
      
      s3m_tick_effects(m);

      if (m->tick == 0) {
        if (m->pos_jump >= 0) {            /* Bxx: jump after this row */
          m->order = m->pos_jump;
          m->row = 0;
          m->pos_jump = -1;
        } else if (m->pat_break) {         /* Cxx: next order, row from the command */
          m->order++;
          m->row = m->break_row;
          m->pat_break = 0;
        } else {
          m->row++;
          if (m->row >= 64) { m->row = 0; m->order++; }
        }
        /* notes keep ringing across a pattern boundary (ST3 does not cut them) */
        if (m->order >= 256 || m->orders[m->order] >= 254) {
          m->ended = 1;
          s3m_active = 0;
          return;
        }
      }
    }
    
    int32_t mix_l = 0, mix_r = 0;
    for (int ch = 0; ch < m->chn; ch++) {
      s3m_voice_t *v = &m->vox[ch];
      if (!v->active || v->smp < 0 || v->smp >= m->nsmp) continue;
      
      const s3m_sample_t *sm = &m->smp[v->smp];
      int looping = (sm->flags & 1) && sm->loop_len > 0;
      uint32_t loop_lo = sm->loop_start;
      uint32_t stop = looping ? loop_lo + sm->loop_len : sm->len;
      if (looping && stop > sm->len) stop = sm->len;

      uint32_t pos_int = v->pos >> 16;
      if (pos_int >= stop) {
        if (looping) {
          v->pos = loop_lo << 16;
          pos_int = loop_lo;
        } else if (v->end_ramp == 0) {
          v->end_ramp = 64;               /* fade instead of cutting mid-wave */
          pos_int = stop ? stop - 1 : 0;
        } else if (v->end_ramp <= 1) {
          v->active = 0;
          continue;
        } else {
          pos_int = stop ? stop - 1 : 0;
        }
      }

      /* Linear interpolation: nearest sampling at 8 bits crackled on high notes. */
      uint32_t next = pos_int + 1;
      if (next >= stop) next = looping ? loop_lo : pos_int;
      int s0 = (int) sm->data[pos_int] - 128;
      int s1 = (int) sm->data[next] - 128;
      int fr = (int) ((v->pos >> 8) & 0xFF);
      int32_t val = (((s0 * (256 - fr)) + (s1 * fr)) >> 8) << 8;
      
      /* The volume used to jump at every tick (50 Hz), which buzzed; slide it per sample. */
      int vc = v->vol_cur, tgt = v->vol << 8;
      if (vc < 0) vc = 0;
      vc += (tgt - vc) >> 4;
      v->vol_cur = vc;
      int vol = (((vc >> 8) * m->gvol) >> 6);
      if (v->end_ramp) { vol = vol * v->end_ramp / 64; v->end_ramp--; }
      val = (val * vol) >> 8;
      
      int pan = v->pan;
      int32_t l = (val * (256 - pan)) >> 8;
      int32_t r = (val * pan) >> 8;
      
      mix_l += l;
      mix_r += r;
      
      v->pos += v->step;
    }
    
    /* gain calibrated against the openmpt render */
    mix_l = (mix_l * 292) >> 8;
    mix_r = (mix_r * 292) >> 8;
    if (mix_l > 32767) mix_l = 32767; else if (mix_l < -32768) mix_l = -32768;
    if (mix_r > 32767) mix_r = 32767; else if (mix_r < -32768) mix_r = -32768;
    s3m_out[2 * out_count] = (int16_t) mix_l;
    s3m_out[2 * out_count + 1] = (int16_t) mix_r;
    out_count++;
  }
  
  for (int i = 0; i < out_count; i++) {
    int32_t l = s3m_out[2 * i];
    int32_t r = s3m_out[2 * i + 1];
    int32_t v = s3m_volume;
    
    l = (l * v) >> 6;
    r = (r * v) >> 6;
    
    int32_t al = l < 0 ? -l : l;
    int32_t ar = r < 0 ? -r : r;
    if (al > s3m_peak_l) s3m_peak_l = al;
    if (ar > s3m_peak_r) s3m_peak_r = ar;
    s3m_last_l = l;
    s3m_last_r = r;
    
    mixer_write((int16_t)l, (int16_t)r);   /* also feed the spectrum analyzer */
  }
  
  memset(s3m_vu, 0, sizeof(s3m_vu));
  for (int ch = 0; ch < s3m_mod.chn; ch++) {
    if (s3m_mod.vox[ch].active && ch < FMT_CHANNELS) {
      int val = s3m_mod.vox[ch].vol;
      if (val > 255) val = 255;
      if (val > s3m_vu[ch]) s3m_vu[ch] = (uint8_t)val;
    }
  }
}

int s3m_fpga_active(void) { return s3m_active; }
int s3m_fpga_finished(void) { return s3m_ended; }

void s3m_fpga_stop(void) {
  s3m_release();
}

const char *s3m_fpga_error(void) { return s3m_err; }
const char *s3m_fpga_title(void) { return s3m_mod.title; }

int s3m_fpga_position(uint32_t *cur, uint32_t *total) {
  if (!s3m_active && !s3m_ended) return 0;
  if (cur) *cur = s3m_mod.order * 64 + s3m_mod.row;
  if (total) *total = 256 * 64;
  return 1;
}

void s3m_fpga_info(fmt_info_t *o) {
  o->channels = s3m_mod.chn;
  o->samples = s3m_mod.nsmp;
  o->order = s3m_mod.order;
  o->pattern = s3m_mod.orders[s3m_mod.order];
  o->row = s3m_mod.row;
  o->song_len = 256;
  o->speed = s3m_mod.speed;
  o->bpm = s3m_mod.bpm;
  o->size = s3m_file_size;
}

void s3m_fpga_audio(fmt_audio_t *o) {
  o->level_l = s3m_last_l;
  o->level_r = s3m_last_r;
  o->peak_l = s3m_peak_l;
  o->peak_r = s3m_peak_r;
  o->volume = s3m_volume;
  o->paused = s3m_paused;
  for (int i = 0; i < FMT_CHANNELS; i++) o->vu[i] = s3m_vu[i];
  analysis_bands(o->bands, FMT_BANDS);
}

int s3m_fpga_get_volume(void) { return s3m_volume; }

int s3m_fpga_set_volume(int v) {
  if (v < 0) v = 0;
  if (v > 128) v = 128;
  s3m_volume = v;
  return v;
}

int s3m_fpga_pause(int on) {
  if (on < 0) s3m_paused = !s3m_paused;
  else s3m_paused = on;
  return s3m_paused;
}
