// PSID/RSID file format detector
#include <string.h>
#include "../fmt.h"
#include "../sid_fpga.h"

static int detect_psid(const uint8_t *p, uint32_t n) {
  return p && n >= 4 && p[0] == 'P' && p[1] == 'S' && p[2] == 'I' && p[3] == 'D';
}

static int detect_rsid(const uint8_t *p, uint32_t n) {
  return p && n >= 4 && p[0] == 'R' && p[1] == 'S' && p[2] == 'I' && p[3] == 'D';
}

static int detect(const uint8_t *p, uint32_t n) {
  return detect_psid(p, n) || detect_rsid(p, n);
}

static int play(const play_src_t *src, uint32_t size, play_progress_t progress) {
  return sid_fpga_start(src, size, progress);
}

static const char *error(void) {
  return sid_fpga_error();
}

const fmt_module_t fmt_sid = { "sid", "sid", detect, play, error };
