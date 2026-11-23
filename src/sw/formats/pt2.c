// ZX Spectrum ProTracker 2 modules. The shared AY engine does the work.
#include <string.h>
#include "../fmt.h"
#include "../fpga.h"

static int detect(const uint8_t *p, uint32_t n) {
  (void) p; (void) n;
  return 0;                 // no magic: the extension selects it, playback checks the structure
}
static int play(const play_src_t *src, uint32_t size, play_progress_t progress) {
  return pt2_start(src, size, progress);
}
static const char *error(void) { return pt3_error(); }

const fmt_module_t fmt_pt2 = { "pt2", "pt2", detect, play, error };
