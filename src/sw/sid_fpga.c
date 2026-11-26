// The hardware SID 8580 (MiSTer model) inside menu_soc.v.
//   0xc8 W: addr << 8 | data   (one register write)
//   0xcc W: bit 0 on, bits 15..8 volume
#include "mmio.h"
#include "sid_fpga.h"

static int tone_on;

static void sid_wr(uint32_t reg, uint32_t val) {
  MMIO(0xc8) = ((reg & 0x1f) << 8) | (val & 0xff);
}

void sid_test_tone(int on) {
  if (on) {
    /* voice 1: ~440 Hz sawtooth, full sustain; the SID clock is 985248 Hz */
    sid_wr(0x00, 440u * 16777216u / 985248u & 0xff);
    sid_wr(0x01, (440u * 16777216u / 985248u >> 8) & 0xff);
    sid_wr(0x05, 0x00);            /* attack 0, decay 0 */
    sid_wr(0x06, 0xf0);            /* sustain 15, release 0 */
    sid_wr(0x04, 0x21);            /* sawtooth, gate on */
    sid_wr(0x18, 0x0f);            /* master volume 15 */
    MMIO(0xcc) = 1u | (64u << 8);
    tone_on = 1;
  } else {
    sid_wr(0x04, 0x20);            /* gate off */
    MMIO(0xcc) = 0;
    tone_on = 0;
  }
}

/* Drain the RTL tap for a few milliseconds and return the peak |sample|. */
int sid_tap_peak(int ms) {
  int peak = 0;
  uint32_t t0 = R_US;
  while ((uint32_t) (R_US - t0) < (uint32_t) ms * 1000u) {
    while (MMIO(0xdc)) {
      uint32_t v = MMIO(0xd8);
      int l = (int16_t) (v >> 16), r = (int16_t) v;
      if (l < 0) l = -l;
      if (r < 0) r = -r;
      if (l > peak) peak = l;
      if (r > peak) peak = r;
    }
  }
  return peak;
}

int sid_hw_present(void) { return 1; }
