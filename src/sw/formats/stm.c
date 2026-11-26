// Scream Tracker 1/2 modules: file type 2 at 0x1d, version 2.x at 0x1e.
#include <string.h>
#include "../fmt.h"
#include "../stm_fpga.h"

static int detect(const uint8_t *p, uint32_t n) {
  return p && n >= 0x30 && p[0x1C] == 0x1A && p[0x1D] == 2 && p[0x1E] == 2;
}

static int play(const play_src_t *src, uint32_t size, play_progress_t progress) {
  return stm_fpga_start(src, size, progress);
}

static const char *error(void) { return stm_fpga_error(); }

const fmt_module_t fmt_stm = { "stm", "stm", detect, play, error };
