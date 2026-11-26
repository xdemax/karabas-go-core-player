// PSID/RSID parser and replayer: runs 6502 code, writes to SID registers via MMIO
#include <string.h>
#include <stdlib.h>
#include "sid_fpga.h"
#include "sid_cpu.h"
#include "mmio.h"
#include "audio.h"
#include "mixer.h"
#include "analysis.h"
#include "fmt.h"

typedef struct {
  uint8_t magic[4];
  uint16_t version;
  uint16_t data_offset;
  uint16_t load_addr;
  uint16_t init_addr;
  uint16_t play_addr;
  uint16_t songs;
  uint16_t start_song;
  uint32_t speed;
  char title[32];
  char author[32];
  char released[32];
  uint16_t flags;
  uint8_t startpage;
  uint8_t pagesize;
  uint8_t reserved[2];
} psid_hdr_t;

#define SID_OUT_CHUNK 256
#define SID_OUT_MAX   512

typedef struct {
  const uint8_t *data;
  uint32_t size;
  m6502_t *cpu;
  uint16_t init_addr;
  uint16_t play_addr;
  uint16_t songs;
  uint16_t speed;
  uint16_t flags;
  
  int active, finished, paused;
  char err[48];
  int vol;
  int32_t last_l, last_r, peak_l, peak_r;
  uint8_t vu[FMT_CHANNELS];
  uint32_t sample_acc;
  uint32_t samples_per_tick;
  uint32_t sid_regs_written;
  uint32_t play_calls;
  char title[32];
} sid_state_t;

static sid_state_t sid;
static int16_t sid_out[SID_OUT_CHUNK * 2];
static int sid_service_active;

static void sid_error(const char *s) {
  int i = 0;
  for (; s && s[i] && i < (int)sizeof(sid.err) - 1; i++) sid.err[i] = s[i];
  sid.err[i] = 0;
}

static uint16_t rd16be(const uint8_t *p) {
  return (p[0] << 8) | p[1];
}

static uint32_t rd32be(const uint8_t *p) {
  return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static void sid_wr(uint32_t reg, uint32_t val) {
  MMIO(0xc8) = ((reg & 0x1f) << 8) | (val & 0xff);
  sid.sid_regs_written++;
}

int sid_fpga_start(const play_src_t *src, uint32_t size, play_progress_t progress) {
  (void)progress;
  
  if (size < sizeof(psid_hdr_t)) {
    sid_error("file too short");
    return -1;
  }

  const uint8_t *data = src->mem;
  psid_hdr_t hdr;
  
  memcpy(&hdr, data, sizeof(hdr));
  hdr.version = rd16be((uint8_t *)&hdr.version);
  hdr.data_offset = rd16be((uint8_t *)&hdr.data_offset);
  hdr.load_addr = rd16be((uint8_t *)&hdr.load_addr);
  hdr.init_addr = rd16be((uint8_t *)&hdr.init_addr);
  hdr.play_addr = rd16be((uint8_t *)&hdr.play_addr);
  hdr.songs = rd16be((uint8_t *)&hdr.songs);
  hdr.start_song = rd16be((uint8_t *)&hdr.start_song);
  hdr.speed = rd32be((uint8_t *)&hdr.speed);
  hdr.flags = rd16be((uint8_t *)&hdr.flags);
  
  if (hdr.version < 2 || hdr.version > 4) {
    sid_error("unsupported PSID version");
    return -1;
  }

  uint16_t offset = hdr.data_offset;
  if (offset < sizeof(psid_hdr_t)) offset = sizeof(psid_hdr_t);
  if (offset >= size) {
    sid_error("data offset out of range");
    return -1;
  }

  if (!sid.cpu) {
    sid.cpu = m6502_new();
    if (!sid.cpu) {
      sid_error("malloc failed");
      return -1;
    }
  }

  m6502_reset(sid.cpu);
  memset(sid.cpu->mem, 0, 65536);

  uint16_t load_addr = hdr.load_addr;
  if (load_addr == 0) load_addr = (data[offset + 1] << 8) | data[offset];
  
  const uint8_t *img = data + offset + (load_addr == 0 ? 2 : 0);
  uint32_t img_len = size - (uint32_t)(img - data);
  if (load_addr + img_len > 65536) img_len = 65536 - load_addr;
  
  memcpy(sid.cpu->mem + load_addr, img, img_len);

  sid.init_addr = hdr.init_addr;
  sid.play_addr = hdr.play_addr;
  sid.songs = hdr.songs ? hdr.songs : 1;
  sid.speed = hdr.speed;
  sid.flags = hdr.flags;
  sid.data = data;
  sid.size = size;
  
  memcpy(sid.title, hdr.title, 31);
  sid.title[31] = 0;

  sid.vol = 64;
  sid.last_l = sid.last_r = 0;
  sid.peak_l = sid.peak_r = 0;
  memset(sid.vu, 0, sizeof(sid.vu));
  sid.samples_per_tick = 48000 / 50;
  sid.sample_acc = 0;
  sid.sid_regs_written = 0;
  sid.play_calls = 0;
  sid.active = 1;
  sid.finished = 0;
  sid.paused = 0;

  if (hdr.init_addr) {
    sid.cpu->pc = hdr.init_addr;
    sid.cpu->a = (hdr.start_song - 1) & 0xff;
    sid.cpu->x = 0;
    sid.cpu->y = 0;
    sid.cpu->s = 0xfd;

    int steps = 0;
    while (steps < 100000) {
      if (m6502_step(sid.cpu)) break;
      steps++;
    }
  }

  MMIO(0xcc) = 1 | (64 << 8);

  return 0;
}

void sid_fpga_service(void) {
  if (!sid.active || sid_service_active) return;
  
  sid_service_active = 1;

  if (sid.play_addr) {
    sid.sample_acc += SID_OUT_CHUNK;
    uint32_t play_ticks = sid.sample_acc / sid.samples_per_tick;
    while (play_ticks > 0) {
      sid.cpu->pc = sid.play_addr;
      sid.cpu->x = 0;
      sid.cpu->y = 0;
      sid.cpu->s = 0xfd;
      
      int steps = 0;
      while (steps < 10000) {
        if (m6502_step(sid.cpu)) break;
        steps++;
      }

      sid.play_calls++;
      play_ticks--;
      sid.sample_acc -= sid.samples_per_tick;
    }
  }

  for (int i = 0; i < SID_OUT_CHUNK; i++) {
    uint32_t sample = 0;
    while (MMIO(0xdc)) {
      uint32_t v = MMIO(0xd8);
      int16_t l = (int16_t)(v >> 16), r = (int16_t)v;
      sample = (uint32_t)l | ((uint32_t)r << 16);
      
      if (l < 0) l = -l;
      if (r < 0) r = -r;
      if (l > sid.peak_l) sid.peak_l = l;
      if (r > sid.peak_r) sid.peak_r = r;
    }

    int16_t l = (int16_t)(sample >> 16);
    int16_t r = (int16_t)sample;
    
    l = (int32_t)l * sid.vol / 64;
    r = (int32_t)r * sid.vol / 64;

    sid_out[i * 2] = l;
    sid_out[i * 2 + 1] = r;
    
    sid.last_l = l;
    sid.last_r = r;
  }

  for (int i = 0; i < SID_OUT_CHUNK; i++) {
    mixer_write(sid_out[i * 2], sid_out[i * 2 + 1]);
  }

  sid_service_active = 0;
}

int sid_fpga_active(void) {
  return sid.active;
}

int sid_fpga_finished(void) {
  return sid.finished;
}

void sid_fpga_stop(void) {
  sid.active = 0;
  sid.finished = 1;
  MMIO(0xcc) = 0;
  
  if (sid.cpu) {
    m6502_free(sid.cpu);
    sid.cpu = NULL;
  }
}

const char *sid_fpga_error(void) {
  return sid.err;
}

const char *sid_fpga_title(void) {
  return sid.title;
}

int sid_fpga_position(uint32_t *cur, uint32_t *total) {
  if (cur) *cur = sid.play_calls;
  if (total) *total = sid.songs;
  return 0;
}

void sid_fpga_info(fmt_info_t *o) {
  if (!o) return;
  o->channels = 1;
  o->samples = 0;
  o->order = 0;
  o->pattern = 0;
  o->row = 0;
  o->song_len = sid.songs;
  o->speed = 50;
  o->bpm = 0;
  o->size = sid.size;
}

void sid_fpga_audio(fmt_audio_t *o) {
  if (!o) return;
  o->level_l = sid.last_l;
  o->level_r = sid.last_r;
  o->peak_l = sid.peak_l;
  o->peak_r = sid.peak_r;
  memset(o->bands, 0, sizeof(o->bands));
  memcpy(o->vu, sid.vu, sizeof(o->vu));
  o->volume = sid.vol;
  o->paused = sid.paused;
}

int sid_fpga_get_volume(void) {
  return sid.vol;
}

int sid_fpga_set_volume(int v) {
  if (v < 0) v = 0;
  if (v > 64) v = 64;
  sid.vol = v;
  return v;
}

int sid_fpga_pause(int on) {
  if (on < 0) sid.paused ^= 1;
  else sid.paused = on;
  return sid.paused;
}
