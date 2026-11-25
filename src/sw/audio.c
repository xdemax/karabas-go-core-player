#include "audio.h"
#include "mmio.h"

static char *dec(char *q, uint32_t v) {
  char t[10];
  int n = 0;
  if (!v) { *q++ = '0'; return q; }
  while (v) { t[n++] = (char) ('0' + (v % 10u)); v /= 10u; }
  while (n) *q++ = t[--n];
  return q;
}

int audio_free(void) { return (int) (MMIO(0x74) & 0xFFFF); }
uint16_t audio_drop(void) { return (uint16_t) (MMIO(0x78) & 0xFFFF); }
uint16_t audio_under(void) { return (uint16_t) (MMIO(0x7c) & 0xFFFF); }
void audio_reset_stats(void) { MMIO(0x78) = 1; }

static uint32_t nwritten;
static int aud_hz = 48000;
void audio_note_written(int n) { if (n > 0) nwritten += (uint32_t) n; }
uint32_t audio_written(void) { return nwritten; }
int audio_rate(void) { return aud_hz; }

// Returns the rate the hardware actually runs at. The mixer MUST use this value:
// if the FPGA register does not take the new rate, falling back to 48 kHz keeps the
// mixer and the FIFO in step (otherwise the pitch and the tempo drift).
int audio_set_rate(int hz) {
  MMIO(0x44) = (uint32_t) hz;
  uint32_t rb = MMIO(0x44) & 0x1FFFFFFu;
  if (rb != (uint32_t) hz) {
    hz = 48000;
    MMIO(0x44) = 48000u;
  }
  aud_hz = hz;
  return hz;
}

// Diagnostic: what the software asked for and what the FPGA register actually holds.
static char rate_dbg[32];
const char *audio_rate_debug(void) {
  uint32_t hw = MMIO(0x44) & 0x7FFFFFFu;
  char *q = rate_dbg;
  *q++ = 'R';
  q = dec(q, (uint32_t) aud_hz);
  *q++ = '/';
  q = dec(q, hw);
  *q = 0;
  return rate_dbg;
}

void audio_write(int16_t l, int16_t r) {
  MMIO(0x70) = ((uint32_t) (uint16_t) l << 16) | (uint32_t) (uint16_t) r;
  nwritten++;
}

// Fill the FIFO with silence and measure how long the audio side takes to pop it. The result is
// the real hardware sample rate (48000 when the clock/tick are right); -1 if it does not drain.
int audio_rate_test(void) {
  if (aud_hz != 48000) return -1;  // already configured to a non-default rate
  for (int i = 0; i < AUDIO_CAP - 1; i++) audio_write(0, 0);
  int f1 = audio_free();                 // the index takes ~ms: the FIFO already drained some
  uint32_t t1 = R_US;
  while (audio_free() < AUDIO_CAP - 1) {
    if ((uint32_t) (R_US - t1) > 500000u) return -1;   // 500 ms (the 8192 FIFO takes ~171 ms to drain)
  }
  uint32_t t2 = R_US;
  uint32_t n = (uint32_t) (AUDIO_CAP - 1 - f1);
  if (!n || t2 <= t1) return -1;
  int rate = (int) (((uint64_t) n * 1000000u) / (t2 - t1));
  if (rate >= 32000 && rate <= 96000) aud_hz = rate;
  return rate;
}
