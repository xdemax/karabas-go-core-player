// The S3M/IT player: MikMod as a complete tracker engine, its PCM goes to the common mixer.
#pragma once
#include <stdint.h>
#include "fmt.h"

enum { MIKMOD_S3M = 0, MIKMOD_IT };

int mikmod_start(int kind, const play_src_t *src, uint32_t size, play_progress_t progress);
void mikmod_service(void);
int mikmod_active(void);
int mikmod_finished(void);
void mikmod_stop(void);
const char *mikmod_error(void);
const char *mikmod_title(void);
int mikmod_position(uint32_t *cur, uint32_t *total);
void mikmod_info(fmt_info_t *o);
void mikmod_audio(fmt_audio_t *o);
int mikmod_get_volume(void);
int mikmod_set_volume(int v);
int mikmod_pause(int on);
