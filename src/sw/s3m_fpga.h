// S3M (Scream Tracker 3) parser and replayer, integer-only, no MikMod.
// Compatible with XM/IT engines: fastmix, 32 channels max, 48000 Hz.
#pragma once
#include <stdint.h>
#include "fmt.h"

int s3m_fpga_start(const play_src_t *src, uint32_t size, play_progress_t progress);
void s3m_fpga_service(void);
int s3m_fpga_active(void);
int s3m_fpga_finished(void);
void s3m_fpga_stop(void);
const char *s3m_fpga_error(void);
const char *s3m_fpga_title(void);
int s3m_fpga_position(uint32_t *cur, uint32_t *total);
void s3m_fpga_info(fmt_info_t *o);
void s3m_fpga_audio(fmt_audio_t *o);
int s3m_fpga_get_volume(void);
int s3m_fpga_set_volume(int v);
int s3m_fpga_pause(int on);
