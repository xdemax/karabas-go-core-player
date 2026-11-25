// Scream Tracker 3 modules: "SCRM" at offset 0x2c. Native S3M engine.
#include <string.h>
#include "../fmt.h"
#include "../s3m_fpga.h"

static int detect(const uint8_t *p, uint32_t n) {
  return p && n >= 0x30 && !memcmp(p + 0x2C, "SCRM", 4);
}

static int play(const play_src_t *src, uint32_t size, play_progress_t progress) {
  return s3m_fpga_start(src, size, progress);
}

static const char *error(void) { return s3m_fpga_error(); }

const fmt_module_t fmt_s3m = { "s3m", "s3m", detect, play, error };
