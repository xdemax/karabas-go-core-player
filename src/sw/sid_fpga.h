// The hardware SID 8580 (MiSTer model) inside menu_soc.v:
//   0xc8 W: addr << 8 | data   (one register write)
//   0xcc W: bit 0 on, bits 15..8 volume
#pragma once
#include "fmt.h"

int sid_hw_present(void);
void sid_test_tone(int on);
int sid_tap_peak(int ms);

int sid_fpga_start(const play_src_t *src, uint32_t size, play_progress_t progress);
void sid_fpga_service(void);
int sid_fpga_active(void);
int sid_fpga_finished(void);
void sid_fpga_stop(void);
const char *sid_fpga_error(void);
const char *sid_fpga_warning(void);
const char *sid_fpga_title(void);
int sid_fpga_position(uint32_t *cur, uint32_t *total);
void sid_fpga_info(fmt_info_t *o);
void sid_fpga_audio(fmt_audio_t *o);
int sid_fpga_get_volume(void);
int sid_fpga_set_volume(int v);
int sid_fpga_pause(int on);
