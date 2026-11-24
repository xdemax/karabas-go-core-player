// New complete IT player (parser + replayer + fastmix), alternative to MikMod.
#pragma once
#include <stdint.h>
#include "fmt.h"

int it_fpga_start(const play_src_t *src, uint32_t size, play_progress_t progress);
void it_fpga_service(void);
int it_fpga_active(void);
int it_fpga_finished(void);
void it_fpga_stop(void);
const char *it_fpga_error(void);
const char *it_fpga_title(void);
int it_fpga_position(uint32_t *cur, uint32_t *total);
void it_fpga_info(fmt_info_t *o);
void it_fpga_audio(fmt_audio_t *o);
int it_fpga_get_volume(void);
int it_fpga_set_volume(int v);
int it_fpga_pause(int on);
