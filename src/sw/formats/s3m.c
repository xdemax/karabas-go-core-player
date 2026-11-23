// Scream Tracker 3 modules: "SCRM" at offset 0x2c. The ESP32-S3 tracker engine plays them.
#include <string.h>
#include "../fmt.h"
#include "../eng_esp32.h"

static int detect(const uint8_t *p, uint32_t n) {
  return p && n >= 0x30 && !memcmp(p + 0x2C, "SCRM", 4);
}

static int play(const play_src_t *src, uint32_t size, play_progress_t progress) {
  return esp32_play(ESP32_FMT_S3M, src, size, progress);
}

static const char *error(void) { return esp32_error(); }

const fmt_module_t fmt_s3m = { "s3m", "s3m", detect, play, error };
