// Impulse Tracker modules: "IMPM" at offset 0. New full IT parser/replayer + fastmix.
#include <string.h>
#include "../fmt.h"
#include "../it_fpga.h"
#include "../mikmod_fpga.h"

static int detect(const uint8_t *p, uint32_t n) {
  return p && n >= 4 && !memcmp(p, "IMPM", 4);
}

static int play(const play_src_t *src, uint32_t size, play_progress_t progress) {
  int rc = it_fpga_start(src, size, progress);
  if (rc == -2) return mikmod_start(MIKMOD_IT, src, size, progress);
  return rc;
}

static const char *error(void) { return it_fpga_error(); }

const fmt_module_t fmt_it = { "it", "it", detect, play, error };
