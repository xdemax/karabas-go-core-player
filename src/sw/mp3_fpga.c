// MP3 through the Helix fixed-point decoder (the same decoder karabas-megabuzz uses), without
// an external codec chip. The file is staged in SDRAM, decoded a frame at a time and resampled
// to the player's fixed 48 kHz output.
#include <string.h>
#include "mp3_fpga.h"
#include "helix/mp3dec.h"
#include "mmio.h"
#include "audio.h"
#include "mixer.h"
#include "src.h"

#define MP3_RATE       48000
#define MP3_IN_FRAMES  1152
#define MP3_RING_FRAMES 4096u
#define MP3_RING_MASK  (MP3_RING_FRAMES - 1u)

static HMP3Decoder dec;
static const uint8_t *file_mem;
static uint32_t file_size, file_pos;
static int active, ended, paused;
static char err[48], title[64];
static uint8_t *module_mem;

static int16_t pcm[MP3_IN_FRAMES * 2];
static int16_t ring[MP3_RING_FRAMES * 2];
static uint32_t ring_rd, ring_wr, ring_count;   // frame indices / frames
static uint32_t src_frac;                       // 16.16 within ring_rd
static uint32_t src_step = 0x10000;             // source frames per output frame
static uint32_t src_rate, src_chans;

static int32_t last_l, last_r, peak_l, peak_r;
static uint8_t vu[FMT_CHANNELS];
static int vol = 64;
static MP3FrameInfo fi;

static void set_error(const char *s) {
  int i = 0;
  for (; s && s[i] && i < (int) sizeof(err) - 1; i++) err[i] = s[i];
  err[i] = 0;
}

static int header_ok(const uint8_t *h) {
  uint32_t version = (h[1] >> 3) & 3;
  uint32_t layer = (h[1] >> 1) & 3;
  uint32_t bitrate = h[2] >> 4;
  uint32_t samprate = (h[2] >> 2) & 3;
  return version != 1 && layer == 1 && bitrate != 0 && bitrate != 15 && samprate != 3;
}

static void parse_title(void) {
  title[0] = 0;
  if (file_size >= 128 && !memcmp(file_mem + file_size - 128, "TAG", 3)) {
    const uint8_t *t = file_mem + file_size - 128 + 3;
    int n = 0;
    for (; n < 30 && t[n]; n++) title[n] = (char) t[n];
    while (n > 0 && title[n - 1] == ' ') n--;
    title[n] = 0;
  }
  if (!title[0]) strcpy(title, "MP3");
}

static void reset_ring(void) {
  ring_rd = ring_wr = ring_count = 0;
  src_frac = 0;
}

static void release(void) {
  if (dec) {
    MP3FreeDecoder(dec);
    dec = 0;
  }
  active = ended = paused = 0;
  reset_ring();
  module_mem = 0;
}

int mp3_start(const play_src_t *src, uint32_t size, play_progress_t progress) {
  release();
  if (!src || !size) { set_error("empty MP3"); return -1; }
  if (size > SDRAM_FILE_MAX) { set_error("MP3 over 32 MB"); return -1; }

  uint8_t *buf;
  if (src->f) {
    uint32_t t0 = R_US;
    while (!(R_SDRAM & 1)) {
      if ((uint32_t) (R_US - t0) > 200000u) { set_error("SDRAM not ready"); return -1; }
    }
    buf = (uint8_t *) SDRAM_BASE;
    uint32_t off = 0;
    while (off < size) {
      uint32_t n = size - off;
      if (n > 4096) n = 4096;
      UINT got = 0;
      if (src_read((play_src_t *) src, off, buf + off, n, &got) != 0 || got == 0) {
        set_error("read error"); return -1;
      }
      off += got;
      if (progress && ((off >> 15) != ((off - got) >> 15) || off == size)) progress(off, size);
    }
  } else {
    buf = (uint8_t *) src->mem;
  }

  module_mem = buf;
  file_mem = buf;
  file_size = size;
  file_pos = 0;
  reset_ring();

  // Skip an ID3v2 tag. A false sync word inside cover art must never reach Helix.
  if (size >= 10 && !memcmp(buf, "ID3", 3)) {
    uint32_t tag = 10u + (((uint32_t) buf[6] << 21) | ((uint32_t) buf[7] << 14) |
                          ((uint32_t) buf[8] << 7) | (uint32_t) buf[9]);
    if (tag > size) tag = size;
    file_pos = tag;
  }

  dec = MP3InitDecoder();
  if (!dec) { set_error("Helix init"); release(); return -1; }
  set_error("");
  src_rate = 0;
  src_chans = 0;
  last_l = last_r = peak_l = peak_r = 0;
  memset(vu, 0, sizeof(vu));
  parse_title();
  active = 1;
  ended = paused = 0;
  return 0;
}

// Decode one MP3 frame into the PCM ring. Returns 1 on success, 0 at end, -1 on a hard error.
static int decode_one(void) {
  if (!dec || ended) return 0;
  if (ring_count + MP3_IN_FRAMES > MP3_RING_FRAMES) return 1;

  for (;;) {
    if (file_pos + 4 >= file_size) { ended = 1; return 0; }
    int off = MP3FindSyncWord((unsigned char *) file_mem + file_pos,
                              (int) (file_size - file_pos));
    if (off < 0) { ended = 1; return 0; }
    file_pos += (uint32_t) off;
    if (file_pos + 4 >= file_size) { ended = 1; return 0; }
    if (header_ok(file_mem + file_pos)) break;
    file_pos++;
  }

  unsigned char *ptr = (unsigned char *) (file_mem + file_pos);
  int left = (int) (file_size - file_pos);
  int rc = MP3Decode(dec, &ptr, &left, pcm, 0);
  if (rc != 0) {
    // Bad/truncated frame: move one byte and let the sync search find the next frame.
    file_pos++;
    if (rc == ERR_MP3_INDATA_UNDERFLOW && (file_size - file_pos) < 2048) ended = 1;
    return rc == ERR_MP3_INDATA_UNDERFLOW ? 0 : 1;
  }
  file_pos = (uint32_t) (ptr - file_mem);
  MP3GetLastFrameInfo(dec, &fi);
  if (fi.outputSamps <= 0 || fi.nChans < 1 || fi.nChans > 2) { file_pos++; return 1; }
  src_chans = (uint32_t) fi.nChans;
  unsigned frames = (unsigned) fi.outputSamps / src_chans;
  if (frames > MP3_IN_FRAMES) frames = MP3_IN_FRAMES;
  if (!frames) return 1;

  // The stream's sample rate determines the resampling step. Most files are 44.1 kHz.
  src_rate = (uint32_t) fi.samprate;
  src_step = (src_rate << 16) / MP3_RATE;
  if (!src_step) src_step = 0x10000;

  for (unsigned i = 0; i < frames; i++) {
    uint32_t w = (ring_wr + i) & MP3_RING_MASK;
    if (src_chans == 2) {
      ring[w * 2] = pcm[i * 2];
      ring[w * 2 + 1] = pcm[i * 2 + 1];
    } else {
      ring[w * 2] = ring[w * 2 + 1] = pcm[i];
    }
  }
  ring_wr = (ring_wr + frames) & MP3_RING_MASK;
  ring_count += frames;
  return 1;
}

static void emit_samples(unsigned n) {
  int v = vol;
  while (n--) {
    uint32_t p = src_frac >> 16;
    uint32_t f = src_frac & 0xFFFFu;
    uint32_t i0 = (ring_rd + p) & MP3_RING_MASK;
    uint32_t i1 = (i0 + 1) & MP3_RING_MASK;
    int32_t l0 = ring[i0 * 2], r0 = ring[i0 * 2 + 1];
    int32_t l1 = ring[i1 * 2], r1 = ring[i1 * 2 + 1];
    int32_t l = l0 + (((l1 - l0) * (int32_t) f) >> 16);
    int32_t r = r0 + (((r1 - r0) * (int32_t) f) >> 16);
    l = (l * v) / 64;
    r = (r * v) / 64;
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
    if (al > peak_l) peak_l = al;
    if (ar > peak_r) peak_r = ar;
    last_l = l; last_r = r;
    mixer_write((int16_t) l, (int16_t) r);

    src_frac += src_step;
    while (src_frac >= 0x10000u) {
      src_frac -= 0x10000u;
      ring_rd = (ring_rd + 1) & MP3_RING_MASK;
      if (ring_count) ring_count--;
    }
  }
  vu[0] = peak_l > 255 ? 255 : (uint8_t) peak_l;
  vu[1] = peak_r > 255 ? 255 : (uint8_t) peak_r;
}

void mp3_service(void) {
  if (!active || paused) return;

  // Decode at most one frame and emit at most one short chunk per call. The old loop could
  // fill the whole 8K FIFO in one call and swallow short key presses such as F12.
  int free = audio_free();
  if (free > 0) {
    // Keep at least two source frames so the resampler can interpolate across a frame edge.
    while (ring_count < 2 && !ended) {
      int d = decode_one();
      if (d <= 0) break;
    }
    if (ring_count < 2) {
      if (ended) { ring_count = 0; active = 0; }
      return;
    }

    unsigned n = (unsigned) free;
    if (n > 256) n = 256;
    if (n > ring_count - 1) n = ring_count - 1;
    if (n) emit_samples(n);
  }
}

int mp3_active(void) { return active; }
int mp3_finished(void) { return ended; }
void mp3_stop(void) { release(); }
const char *mp3_error(void) { return err; }
const char *mp3_title(void) { return title; }

int mp3_position(uint32_t *cur, uint32_t *total) {
  if (!active) return 0;
  *cur = file_pos;
  *total = file_size ? file_size : 1;
  return 1;
}

void mp3_info(fmt_info_t *o) {
  if (!active) return;
  o->channels = (int) (src_chans ? src_chans : 2);
  o->samples = 0;
  o->order = 0;
  o->pattern = 0;
  o->row = 0;
  o->song_len = 1;
  o->speed = 0;
  o->bpm = 0;
  o->size = file_size;
}

void mp3_audio(fmt_audio_t *o) {
  o->level_l = last_l;
  o->level_r = last_r;
  o->peak_l = peak_l;
  o->peak_r = peak_r;
  memcpy(o->vu, vu, sizeof(o->vu));
  o->volume = vol;
  o->paused = paused;
  peak_l = peak_r = 0;
}

int mp3_get_volume(void) { return vol; }
int mp3_set_volume(int v) {
  if (v < 0) v = 0;
  if (v > 64) v = 64;
  vol = v;
  return vol;
}
int mp3_pause(int on) {
  if (on < 0) paused = !paused; else paused = on ? 1 : 0;
  return paused;
}
