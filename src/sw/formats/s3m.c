// Scream Tracker 3 modules: "SCRM" at offset 0x2c. Native S3M engine.
#include <string.h>
#include "../fmt.h"
#include "../s3m_fpga.h"
#include "../mikmod_fpga.h"

static int detect(const uint8_t *p, uint32_t n) {
  return p && n >= 0x30 && !memcmp(p + 0x2C, "SCRM", 4);
}

static int play(const play_src_t *src, uint32_t size, play_progress_t progress) {
  /* A/B: MikMod is the complete S3M engine (the one v1.0b used); the native
   * s3m_fpga.c stays as the fallback if MikMod refuses the file. */
  int rc = mikmod_start(MIKMOD_S3M, src, size, progress);
  if (rc != 0) rc = s3m_fpga_start(src, size, progress);
  return rc;
}

static const char *error(void) { return mikmod_active() ? mikmod_error() : s3m_fpga_error(); }

const fmt_module_t fmt_s3m = { "s3m", "s3m", detect, play, error };
