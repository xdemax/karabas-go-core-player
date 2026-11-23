// libxmp tracker player (S3M/IT/MOD/XM): one complete C engine, output into the common mixer.
#pragma once
#include <stdint.h>
#include "fmt.h"

int xmp_start(const play_src_t *src, uint32_t size, play_progress_t progress);
void xmp_service(void);
int xmp_active(void);
int xmp_finished(void);
void xmp_stop(void);
const char *xmp_error(void);
const char *xmp_title(void);
int xmp_position(uint32_t *cur, uint32_t *total);
void xmp_info(fmt_info_t *o);
void xmp_audio(fmt_audio_t *o);
int xmp_get_volume(void);
int xmp_set_volume(int v);
int xmp_pause(int on);
