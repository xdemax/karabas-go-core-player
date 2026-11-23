// ProTracker / Scream Tracker 2 modules: "M.K." and the other 4-byte IDs at offset 1080.
// The ESP32-S3 tracker engine plays them.
#include <string.h>
#include "../fmt.h"
#include "../fpga.h"

static int detect(const uint8_t *p, uint32_t n) {
  if (!p || n < 1084) return 0;
  static const char *id[] = { "M.K.", "M!K!", "M&K!", "N.T.", "FLT4", "FLT8", "4CHN", "6CHN",
                              "8CHN", "OCTA", "OKTA", "CD81", "16CN", "32CN" };
  for (unsigned i = 0; i < sizeof(id) / sizeof(id[0]); i++)
    if (!memcmp(p + 1080, id[i], 4)) return 1;
  return 0;
}

static int play(const play_src_t *src, uint32_t size, play_progress_t progress) {
  return fpga_mod_start(src, size, progress);      // the FPGA mixer, no ESP32
}

static const char *error(void) { return fpga_mod_error(); }

const fmt_module_t fmt_mod = { "mod", "mod", detect, play, error };
