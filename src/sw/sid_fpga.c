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
  int rsid;
  uint32_t irq_acc;
  char err[48];
  int vol;
  int32_t last_l, last_r, peak_l, peak_r;
  uint32_t last_tap;
  uint8_t vu[FMT_CHANNELS];
  uint8_t gate[3];
  uint32_t sample_acc;
  uint32_t samples_per_tick;
  int hz;
  uint32_t last_us;
  uint64_t time_acc;
  uint32_t sid_regs_written;
  uint32_t play_calls;
  char title[32];
} sid_state_t;

static sid_state_t sid;
static void sid_hw_on(void);
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

#ifdef HOST
void sid_host_reg_write(uint32_t reg, uint32_t val);
#endif

static void sid_wr(uint32_t reg, uint32_t val) {
#ifdef HOST
  sid_host_reg_write(reg, val);
#endif
  MMIO(0xc8) = ((reg & 0x1f) << 8) | (val & 0xff);
  sid.sid_regs_written++;
}

static void sid_io_write(uint16_t addr, uint8_t val) {
  uint32_t r = addr - 0xd400;
  if (r == 0x04 || r == 0x0b || r == 0x12) sid.gate[(r == 0x04) ? 0 : (r == 0x0b) ? 1 : 2] = val & 1;
  sid_wr(r, val);
}

static uint8_t sid_io_read(uint16_t addr) {
  uint32_t r = addr - 0xd400;
  if (r == 0x1b) {                 /* OSC3: a random byte, as the noise source */
    static uint32_t lfsr = 0x1d872b41u;
    uint32_t bit = ((lfsr >> 22) ^ (lfsr >> 17) ^ lfsr) & 1u;
    lfsr = (lfsr << 1) | bit;
    return (uint8_t) (lfsr >> 8);
  }
  if (r == 0x1c)                   /* ENV3: crude gate-derived envelope */
    return sid.gate[2] ? 0xff : 0x00;
  return 0;
}

int sid_fpga_start(const play_src_t *src, uint32_t size, play_progress_t progress) {
  (void)progress;
  
  if (size < 0x76) {
    sid_error("file too short");
    return -1;
  }

  const uint8_t *data;
  if (src->f) {
    uint8_t *buf = (uint8_t *) SDRAM_BASE;
    uint32_t off = 0;
    while (off < size) {
      uint32_t n = size - off;
      if (n > 4096) n = 4096;
      UINT got = 0;
      if (src_read((play_src_t *) src, off, buf + off, n, &got) != 0 || got == 0) {
        sid_error("read error");
        return -1;
      }
      off += got;
      if (progress && ((off >> 15) != ((off - got) >> 15) || off == size)) progress(off, size);
    }
    data = buf;
  } else {
    data = src->mem;
  }

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

  if (start_song < 1) start_song = 1;
  if (version < 1 || version > 4) {
    sid_error("unsupported PSID version");
    return -1;
  }

  uint32_t offset = data_off ? data_off : (version == 1 ? 0x76 : 0x7c);
  if (offset < 0x76) offset = 0x76;
  if (offset >= size) {
    sid_error("data offset out of range");
    return -1;
  }

  if (!sid.cpu) {
    sid.cpu = m6502_new();
    if (sid.cpu) { sid.cpu->io_write = sid_io_write; sid.cpu->io_read = sid_io_read; }
    if (!sid.cpu) {
      sid_error("malloc failed");
      return -1;
    }
  }

  m6502_reset(sid.cpu);
  memset(sid.cpu->mem, 0, 65536);

  int is_rsid = (data[0] == 'R' && data[1] == 'S' && data[2] == 'I' && data[3] == 'D');
  int has_load = (load_field == 0);
  uint16_t load_addr = load_field;
  if (has_load) {
    if (offset + 2 > size) {
      sid_error("truncated load address");
      return -1;
    }
    load_addr = (data[offset + 1] << 8) | data[offset];
  }

  const uint8_t *img = data + offset + (has_load ? 2 : 0);
  if ((uint32_t) (img - data) > size) {
    sid_error("truncated image");
    return -1;
  }
  uint32_t img_len = size - (uint32_t)(img - data);
  if ((uint64_t) load_addr + img_len > 65536u) img_len = 65536u - load_addr;

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
  sid.rsid = is_rsid;
  sid.irq_acc = 0;
  sid.data = data;
  sid.size = size;
  

  sid.vol = 64;
  sid.last_l = sid.last_r = 0;
  sid.peak_l = sid.peak_r = 0;
  memset(sid.vu, 0, sizeof(sid.vu));
  /* PSID flags bits 2-3: 1 PAL, 2 NTSC. A CIA-timed song is approximated at 60 Hz:
   * the firmware has no CIA timer, but 50 Hz made those songs 20% slow. */
  int hz = (((flags >> 2) & 3) == 2) ? 60 : 50;
  if (start_song >= 1 && start_song <= 32 && (speed & (1u << (start_song - 1)))) hz = 60;
  sid.samples_per_tick = (uint32_t) ((48000 + hz / 2) / hz);
  sid.hz = hz;
  sid.last_us = R_US;
  sid.time_acc = 0;
  sid.sample_acc = 0;
  sid.sid_regs_written = 0;
  sid.play_calls = 0;
  sid.last_tap = 0;
  memset(sid.gate, 0, sizeof(sid.gate));
  sid.active = 1;
  sid.finished = 0;
  sid.paused = 0;

  if (init_addr && is_rsid) {
    /* RSID: the tune installs its own IRQ vector ($0314) and runs by itself. Start it
     * and warm up briefly; the service keeps the CPU going in real time. */
    sid.cpu->a = 0;
    sid.cpu->x = 0;
    sid.cpu->y = 0;
    sid.cpu->s = 0xff;
    sid.cpu->p = 0x04;
    sid.cpu->pc = init_addr;
    for (int i = 0; i < 50000; i++) if (m6502_step(sid.cpu)) break;
  } else if (init_addr) {
    /* JSR-style call with a return address of $FFFE on the stack */
    sid.cpu->a = (start_song - 1) & 0xff;
    sid.cpu->x = 0;
    sid.cpu->y = 0;
    sid.cpu->mem[0x01fd] = 0xff;
    sid.cpu->mem[0x01fc] = 0xfd;
    sid.cpu->s = 0xfb;
    sid.cpu->pc = init_addr;
    int steps = 0;
    for (; steps < 2000000; steps++) if (m6502_step(sid.cpu)) break;
#ifdef HOST
    fprintf(stderr, "[sid] init steps=%d pc=%04x writes=%u\n", steps, sid.cpu->pc, (unsigned) sid.sid_regs_written);
#endif
  }

  sid.vol = 64;
  sid_hw_on();

  return 0;
}

static void sid_hw_on(void) {
  MMIO(0xcc) = 1u | ((uint32_t) (sid.vol * 3) << 8);   /* 0..64 -> gain 0..192 */
}

void sid_fpga_service(void) {
  if (!sid.active || sid.paused || sid_service_active) return;
  sid_service_active = 1;

  /* The sound is written to the output FIFO by the FPGA on its own tick. The CPU only
   * reads the tap: a late redraw may cost a few meter samples, never the audio. */
  uint32_t cnt = MMIO(0xdc) & 0x3ff;
  if (cnt > 512) { while (cnt-- > 1) (void) MMIO(0xd8); }

  int n = 0;
  while (n < SID_OUT_CHUNK && (MMIO(0xdc) & 0x3ff)) {
    uint32_t sample = MMIO(0xd8);
    sid.last_tap = sample;
    int16_t l = (int16_t)(sample >> 16);
    int16_t r = (int16_t)(sample & 0xffff);
    int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
    if (al > sid.peak_l) sid.peak_l = al;
    if (ar > sid.peak_r) sid.peak_r = ar;
    sid.last_l = l;
    sid.last_r = r;
#ifdef HOST
    sid_out[n * 2] = (int16_t)((int32_t) l * sid.vol / 64);
    sid_out[n * 2 + 1] = (int16_t)((int32_t) r * sid.vol / 64);
#else
    analysis_push(l, r);
    audio_note_written(1);              /* the MIX indicator counts the played samples */
#endif
    n++;
  }

  uint32_t advance, rsid_steps;
#ifdef HOST
  if (n == 0) { sid_service_active = 0; return; }
  advance = (uint32_t) n;
  rsid_steps = (uint32_t) n * 7;
#else
  uint32_t now = R_US;
  uint32_t dt = (uint32_t) (now - sid.last_us);
  sid.last_us = now;
  /* Keep the sub-tick remainder: without it every 1-2 ms pass truncated to zero
   * ticks and the tune stood still. */
  sid.time_acc += (uint64_t) dt * (uint32_t) sid.hz;
  advance = (uint32_t) (sid.time_acc / 1000000u);
  sid.time_acc -= (uint64_t) advance * 1000000u;
  rsid_steps = dt / 3;                 /* ~330k instructions/s, as a 1 MHz 6502 */
  if (rsid_steps > 20000) rsid_steps = 20000;
  if (advance > 4) advance = 4;
#endif

  if (sid.rsid) {
    for (uint32_t i = 0; i < rsid_steps; i++) {
      if (sid.cpu->pc >= 0xfffe) sid.cpu->pc = sid.init_addr;
      if (m6502_step(sid.cpu)) break;
    }

    sid.irq_acc += advance;
    while (sid.irq_acc >= sid.samples_per_tick) {
      sid.irq_acc -= sid.samples_per_tick;
      if (!(sid.cpu->p & 0x04)) {             /* I flag clear */
        uint16_t vec = (uint16_t) (sid.cpu->mem[0x0314] | (sid.cpu->mem[0x0315] << 8));
        if (!vec || vec == 0xffff)
          vec = (uint16_t) (sid.cpu->mem[0xfffe] | (sid.cpu->mem[0xffff] << 8));
        if (vec && vec != 0xffff) {
          uint16_t ret = sid.cpu->pc;
          sid.cpu->mem[0x100 + sid.cpu->s] = (uint8_t) (ret >> 8); sid.cpu->s--;
          sid.cpu->mem[0x100 + sid.cpu->s] = (uint8_t) ret; sid.cpu->s--;
          sid.cpu->mem[0x100 + sid.cpu->s] = (uint8_t) (sid.cpu->p & ~0x10); sid.cpu->s--;
          sid.cpu->p |= 0x04;
          sid.cpu->pc = vec;
          for (int i = 0; i < 50000; i++) if (m6502_step(sid.cpu)) break;
        }
      }
    }
  } else if (sid.play_addr) {
    sid.sample_acc += advance;
    uint32_t ticks = sid.sample_acc / sid.samples_per_tick;
    sid.sample_acc -= ticks * sid.samples_per_tick;
    if (ticks > 4) ticks = 4;
    while (ticks > 0) {
      sid.cpu->mem[0x01fd] = 0xff;
      sid.cpu->mem[0x01fc] = 0xfd;
      sid.cpu->s = 0xfb;
      sid.cpu->pc = sid.play_addr;
      sid.cpu->x = 0;
      sid.cpu->y = 0;
      for (int steps = 0; steps < 50000; steps++) if (m6502_step(sid.cpu)) break;
      sid.play_calls++;
      ticks--;
    }
  }

#ifdef HOST
  for (int i = 0; i < n; i++) mixer_write(sid_out[i * 2], sid_out[i * 2 + 1]);
#endif

  memset(sid.vu, 0, sizeof sid.vu);
  for (int vch = 0; vch < 3; vch++)
    if (sid.gate[vch]) sid.vu[vch] = (uint8_t) (sid.vol * 4 > 255 ? 255 : sid.vol * 4);

  sid_service_active = 0;
}

int sid_fpga_active(void) { return sid.active; }
int sid_fpga_finished(void) { return sid.finished; }

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
  (void) cur; (void) total;
  return 0;                        /* no length: the engine cannot estimate the end */
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
  sid.peak_l = sid.peak_r = 0;
  memcpy(o->vu, sid.vu, sizeof(o->vu));
  analysis_bands(o->bands, FMT_BANDS);
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
