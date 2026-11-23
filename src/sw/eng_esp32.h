// The ESP32-S3 tracker engine of the VDAC32 shield: the SPI protocol and the chunk stream; the
// format modules (formats/*.c) call esp32_play() with their format.
#pragma once
#include <stdint.h>
#include "fmt.h"

enum { ESP32_FMT_MOD = 0, ESP32_FMT_XM, ESP32_FMT_S3M };

int esp32_init(void);                 // 0 when the module answers GET_VER
int esp32_present(void);
const char *esp32_ver(void);
int esp32_play(int fmt, const play_src_t *src, uint32_t size, play_progress_t progress);
int esp32_stop(void);
int esp32_playing(void);
const char *esp32_error(void);

extern const play_engine_t esp32_engine;
