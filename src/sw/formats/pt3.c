// ZX Spectrum ProTracker 3 / Vortex Tracker II modules: the YM2149 register stream on the FPGA.
#include <string.h>
#include "../fmt.h"
#include "../fpga.h"

static int detect(const uint8_t *p, uint32_t n) {
  return p && n >= 13 && (!memcmp(p, "ProTracker 3.", 13) || !memcmp(p, "Vortex Tracke", 13));
}

static int play(const play_src_t *src, uint32_t size, play_progress_t progress) {
  return pt3_start(src, size, progress);
}

static const char *error(void) { return pt3_error(); }

const fmt_module_t fmt_pt3 = { "pt3", "pt3", detect, play, error };
