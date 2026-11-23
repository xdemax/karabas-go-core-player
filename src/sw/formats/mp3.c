// MP3: MPEG-1/2 Layer III. Helix is the complete fixed-point decoder; detection is by the
// extension and, when available, the first sync word / ID3 header.
#include <string.h>
#include "../fmt.h"
#include "../mp3_fpga.h"

static int detect(const uint8_t *p, uint32_t n) {
  if (!p || n < 4) return 0;
  if (n >= 3 && !memcmp(p, "ID3", 3)) return 1;
  // MPEG audio sync: 11 bits set; layer III is 01 in bits 2..1 of byte 1.
  return p[0] == 0xFF && (p[1] & 0xE0) == 0xE0 && ((p[1] >> 1) & 3) == 1;
}

static int play(const play_src_t *src, uint32_t size, play_progress_t progress) {
  return mp3_start(src, size, progress);
}

static const char *error(void) { return mp3_error(); }

const fmt_module_t fmt_mp3 = { "mp3", "mp3", detect, play, error };
