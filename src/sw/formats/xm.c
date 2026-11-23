// FastTracker II XM modules: "Extended Module: " at 0. The FPGA XM engine plays them.
#include <string.h>
#include "../fmt.h"
#include "../fpga.h"

static int detect(const uint8_t *p, uint32_t n) {
  return p && n >= 17 && !memcmp(p, "Extended Module: ", 16);
}

static int play(const play_src_t *src, uint32_t size, play_progress_t progress) {
  return fpga_xm_start(src, size, progress);
}

static const char *error(void) { return fpga_xm_error(); }

const fmt_module_t fmt_xm = { "xm", "xm", detect, play, error };
