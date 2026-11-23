// MP3 playback through Helix (fixed-point), decoded into the common 48 kHz mixer.
#pragma once
#include <stdint.h>
#include "fmt.h"

int mp3_start(const play_src_t *src, uint32_t size, play_progress_t progress);
void mp3_service(void);
int mp3_active(void);
int mp3_finished(void);
void mp3_stop(void);
const char *mp3_error(void);
const char *mp3_title(void);
int mp3_position(uint32_t *cur, uint32_t *total);
void mp3_info(fmt_info_t *o);
void mp3_audio(fmt_audio_t *o);
int mp3_get_volume(void);
int mp3_set_volume(int v);
int mp3_pause(int on);
