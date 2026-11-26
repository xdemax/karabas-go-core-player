// PSID/RSID parser and replayer: runs 6502 code, writes to SID registers via MMIO
#include <string.h>
#include <stdlib.h>
#include "sid_fpga.h"
#include "sid_cpu.h"
#ifdef HOST
#include <stdio.h>
#endif
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

static void sid_io_write(uint16_t addr, uint8_t val) {
  sid_wr(addr - 0xd400, val);
}

int sid_fpga_start(const play_src_t *src, uint32_t size, play_progress_t progress) {
  (void)progress;
  
  if (size < 0x7c) {
    sid_error("file too short");
    return -1;
  }

  const uint8_t *data = src->mem;

  /* The PSID header is read at its fixed offsets: relying on a struct layout broke
   * on the compiler's padding (sizeof became 0x7d and the image loaded shifted). */
  uint16_t version    = rd16be(data + 0x04);
  uint16_t data_off   = rd16be(data + 0x06);
  uint16_t load_field = rd16be(data + 0x08);
  uint16_t init_addr  = rd16be(data + 0x0a);
  uint16_t play_addr  = rd16be(data + 0x0c);
  uint16_t songs      = rd16be(data + 0x0e);
  uint16_t start_song = rd16be(data + 0x10);
  uint32_t speed      = rd32be(data + 0x12);
  uint16_t flags      = (version >= 2 && size >= 0x78) ? rd16be(data + 0x76) : 0;

  if (version < 1 || version > 4) {
    sid_error("unsupported PSID version");
    return -1;
  }

  uint32_t offset = data_off ? data_off : 0x7c;
  if (offset < 0x7c) offset = 0x7c;
  if (offset >= size) {
    sid_error("data offset out of range");
    return -1;
  }

  if (!sid.cpu) {
    sid.cpu = m6502_new();
    if (sid.cpu) sid.cpu->io_write = sid_io_write;
    if (!sid.cpu) {
      sid_error("malloc failed");
      return -1;
    }
  }

  m6502_reset(sid.cpu);
  memset(sid.cpu->mem, 0, 65536);

  int has_load = (load_field == 0);
  uint16_t load_addr = load_field;
  if (has_load) load_addr = (data[offset + 1] << 8) | data[offset];

  const uint8_t *img = data + offset + (has_load ? 2 : 0);
  uint32_t img_len = size - (uint32_t)(img - data);
  if ((uint32_t) load_addr + img_len > 65536) img_len = 65536 - load_addr;

  memcpy(sid.cpu->mem + load_addr, img, img_len);
  memcpy(sid.title, data + 0x16, 31);
  sid.title[31] = 0;

#ifdef HOST
  fprintf(stderr, "[sid] has_load=%d img0=%02x %02x load=%04x\n", has_load, data[offset], data[offset+1], load_addr);
  fprintf(stderr, "[sid] v=%d off=%04x load=%04x init=%04x play=%04x songs=%d start=%d speed=%08x\n",
          version, data_off, load_addr, init_addr, play_addr,
          songs, start_song, (unsigned) speed);
  fprintf(stderr, "[sid] code at init: %02x %02x %02x %02x %02x %02x\n",
          sid.cpu->mem[init_addr], sid.cpu->mem[init_addr+1], sid.cpu->mem[init_addr+2],
          sid.cpu->mem[init_addr+3], sid.cpu->mem[init_addr+4], sid.cpu->mem[init_addr+5]);
#endif
  sid.init_addr = init_addr;
  sid.play_addr = play_addr;
  sid.songs = songs ? songs : 1;
  sid.speed = speed;
  sid.flags = flags;
  sid.data = data;
  sid.size = size;
  

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

  if (init_addr) {
    /* JSR-style call with a return address of $FFFE on the stack */
    sid.cpu->a = (start_song - 1) & 0xff;
    sid.cpu->x = 0;
    sid.cpu->y = 0;
    sid.cpu->mem[0x01fd] = 0xff;
    sid.cpu->mem[0x01fc] = 0xfd;
    sid.cpu->s = 0xfb;
    sid.cpu->pc = init_addr;
    int steps = 0;
    for (; steps < 4000000 && sid.cpu->pc != 0xfffe; steps++) m6502_step(sid.cpu);
#ifdef HOST
    fprintf(stderr, "[sid] init steps=%d pc=%04x writes=%u\n", steps, sid.cpu->pc, (unsigned) sid.sid_regs_written);
#endif
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
      sid.cpu->mem[0x01fd] = 0xff;
      sid.cpu->mem[0x01fc] = 0xfd;
      sid.cpu->s = 0xfb;
      sid.cpu->pc = sid.play_addr;
      sid.cpu->x = 0;
      sid.cpu->y = 0;
      int steps = 0;
      for (; steps < 200000 && sid.cpu->pc != 0xfffe; steps++) m6502_step(sid.cpu);
#ifdef HOST
      if (sid.play_calls < 4) fprintf(stderr, "[sid] play pc=%04x steps=%d end=%04x writes=%u\n",
                                      sid.play_addr, steps, sid.cpu->pc, (unsigned) sid.sid_regs_written);
#endif

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
