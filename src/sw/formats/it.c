// Impulse Tracker modules: "IMPM" at offset 0. MikMod is the complete IT engine.
#include <string.h>
#include "../fmt.h"
#include "../mikmod_fpga.h"

static int detect(const uint8_t *p, uint32_t n) {
  return p && n >= 4 && !memcmp(p, "IMPM", 4);
}

static int play(const play_src_t *src, uint32_t size, play_progress_t progress) {
  return mikmod_start(MIKMOD_IT, src, size, progress);
}

static const char *error(void) { return mikmod_error(); }

const fmt_module_t fmt_it = { "it", "it", detect, play, error };
