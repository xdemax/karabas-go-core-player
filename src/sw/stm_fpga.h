// Scream Tracker 1/2 modules: native parser and replayer.
#pragma once
#include <stdint.h>
#include "fmt.h"

int stm_fpga_start(const play_src_t *src, uint32_t size, play_progress_t progress);
void stm_fpga_service(void);
int stm_fpga_active(void);
int stm_fpga_finished(void);
void stm_fpga_stop(void);
const char *stm_fpga_error(void);
const char *stm_fpga_title(void);
int stm_fpga_position(uint32_t *cur, uint32_t *total);
void stm_fpga_info(fmt_info_t *o);
void stm_fpga_audio(fmt_audio_t *o);
int stm_fpga_get_volume(void);
int stm_fpga_set_volume(int v);
int stm_fpga_pause(int on);
