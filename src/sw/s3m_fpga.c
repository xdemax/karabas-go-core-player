// S3M (Scream Tracker 3) parser and replayer. Compatible with XM/IT engines.
#include <string.h>
#include <stdio.h>
#include "s3m_fpga.h"
#include "fastmix.h"
#include "mmio.h"
#include "audio.h"
#include "mixer.h"

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
  uint32_t note, pos, step, target_step;
  int smp, vol, pan, active, keyoff;
  uint8_t eff, par;
  int porta_speed, retrig_tick;
  int32_t mix_vol, lgain, rgain;
} s3m_voice_t;

typedef struct {
  const uint8_t *mod;
  uint32_t size;
  char title[29];
  int chn, npat, nins, nsmp;
  uint32_t orders[256];
  s3m_sample_t smp[99];
  s3m_pattern_t pat[99];
  s3m_voice_t vox[32];
  int order, row, tick, speed, bpm, tleft, spt;
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
    uint32_t off = m->orders[256 + 99 + p];
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
    uint32_t hoff = m->orders[256 + s];
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
    
    // memseg at +14 u16: points directly to sample data
    uint32_t memseg = read16le(buf, hoff + 14, size);
    uint32_t doff = memseg * 16;
    
    // length at +16 u32
    uint32_t length = read32le(buf, hoff + 16, size);
    // loopbeg at +20 u16
    uint16_t loopbeg = read16le(buf, hoff + 20, size);
    // loopend at +24 u16
    uint16_t loopend = read16le(buf, hoff + 24, size);
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
      m->smp[s].loop_len = loopend - loopbeg;
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
      if (progress && (off >> 15) != ((off - got) >> 15)) progress(off, size);
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
    s3m_mod.orders[256 + i] = read16le(buf, ioff + i*2, size);
  }
  
  uint32_t poff = ioff + insnum * 2;
  for (int i = 0; i < patnum; i++) {
    s3m_mod.orders[256 + 99 + i] = read16le(buf, poff + i*2, size);
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
    uint8_t chpan = read8(buf, 64 + i, size);
    if (chpan & 0x80) {
      s3m_mod.panning[i] = (chpan & 0x0f) * 17;
    } else {
      s3m_mod.panning[i] = 128;
    }
  }
  
  s3m_mod.order = 0;
  s3m_mod.row = 0;
  s3m_mod.tick = 0;
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

static const uint32_t s3m_semitone_mul[12] = {
  65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715
};

static uint32_t s3m_note_to_step(int note, uint32_t c2spd) {
  int octave = (note >> 4) & 0x0F;
  int semitone = (note & 0x0F);
  
  if (semitone == 0) return 0;
  semitone--;
  
  uint32_t step = c2spd;
  if (octave > 5) {
    for (int i = 0; i < octave - 5; i++) step *= 2;
  } else if (octave < 5) {
    for (int i = 0; i < 5 - octave; i++) step /= 2;
  }
  
  step = (step * s3m_semitone_mul[semitone]) >> 16;
  step = (step * 65536) / 48000;
  return step;
}

static void s3m_note_on(s3m_module_t *m, int ch, int note, int instr, int vol) {
  if (ch < 0 || ch >= 32) return;
  s3m_voice_t *v = &m->vox[ch];
  
  if (instr > 0 && instr <= m->nsmp) {
    v->smp = instr - 1;
    v->vol = m->smp[instr - 1].vol;
    v->pan = m->smp[instr - 1].pan;
  }
  
  if (vol > 0 && vol <= 64) v->vol = vol;
  
  if (note > 0 && note < 254) {
    uint32_t c2spd = (v->smp >= 0 && v->smp < 99) ? m->smp[v->smp].c2spd : 8363;
    v->pos = 0;
    v->step = s3m_note_to_step(note, c2spd);
    v->active = 1;
    v->keyoff = 0;
  }
}

static void s3m_note_off(s3m_voice_t *v) {
  v->keyoff = 1;
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
      
      // Process next row
      if (m->row < 64) {
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
            
            if (what & 0x20) {
              if (pos < pat->len) note = pat->data[pos++];
              if (pos < pat->len) instr = pat->data[pos++];
            }
            if (what & 0x40) {
              if (pos < pat->len) vol = pat->data[pos++];
            }
            if (what & 0x80) {
              if (pos < pat->len) eff = pat->data[pos++];
              if (pos < pat->len) par = pat->data[pos++];
            }
            
            if (ch < m->chn) {
              if (note) s3m_note_on(m, ch, note, instr, vol);
              if (vol == 255) s3m_note_off(&m->vox[ch]);
              
              if (eff) {
                switch (eff) {
                  case 'A': case 1: m->speed = par; break;
                  case 'T': case 2: m->bpm = par; if (m->bpm < 32) m->bpm = 32; m->samples_per_tick = 120000 / m->bpm; break;
                  case 'V': m->gvol = par & 0x7f; break;
                }
              }
            }
          }
        }
      }
      
      m->row++;
      if (m->row >= 64) {
        m->row = 0;
        m->order++;
        for (int i = 0; i < 32; i++) m->vox[i].active = 0;
        if (m->order >= 256 || m->orders[m->order] >= 254) {
          m->ended = 1;
          s3m_active = 0;
          return;
        }
      }
    }
    
    // Mix voices
    int32_t mix_l = 0, mix_r = 0;
    for (int ch = 0; ch < m->chn; ch++) {
      s3m_voice_t *v = &m->vox[ch];
      if (!v->active || v->smp < 0 || v->smp >= m->nsmp) continue;
      
      uint32_t pos_int = v->pos >> 16;
      if (pos_int >= m->smp[v->smp].len) {
        if ((m->smp[v->smp].flags & 1) && m->smp[v->smp].loop_len > 0) {
          v->pos = (m->smp[v->smp].loop_start << 16);
          pos_int = m->smp[v->smp].loop_start;
        } else {
          v->active = 0;
          continue;
        }
      }
      
      uint8_t smp_byte = *(m->smp[v->smp].data + pos_int);
      int32_t val = ((int32_t)smp_byte - 128) << 8;
      
      int vol = (v->vol * m->gvol) >> 6;
      val = (val * vol) >> 8;
      
      int pan = v->pan;
      int32_t l = (val * (256 - pan)) >> 8;
      int32_t r = (val * pan) >> 8;
      
      mix_l += l;
      mix_r += r;
      
      v->pos += v->step;
    }
    
    s3m_out[2 * out_count] = (int16_t)(mix_l >> 2);
    s3m_out[2 * out_count + 1] = (int16_t)(mix_r >> 2);
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
    
    mixer_write_noana((int16_t)l, (int16_t)r);
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
