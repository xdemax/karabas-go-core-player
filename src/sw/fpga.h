// The FPGA playback engine: the MOD (ProTracker) and XM (FastTracker II) players on the VexRiscv.
// The format modules call fpga_mod_start() / fpga_xm_start(); the unified fpga_* dispatches to
// the active player, so the player core and the skins see one engine.
#pragma once
#include <stdint.h>
#include "fmt.h"

// MOD
int fpga_mod_start(const play_src_t *src, uint32_t size, play_progress_t progress);
void fpga_mod_service(void);
int fpga_mod_active(void);
int fpga_mod_finished(void);
void fpga_mod_stop(void);
const char *fpga_mod_error(void);
const char *fpga_mod_title(void);
int fpga_mod_position(uint32_t *cur, uint32_t *total);
void fpga_mod_info(fmt_info_t *o);
void fpga_mod_audio(fmt_audio_t *o);
int fpga_mod_get_volume(void);
int fpga_mod_set_volume(int v);
int fpga_mod_pause(int on);

// XM
int fpga_xm_start(const play_src_t *src, uint32_t size, play_progress_t progress);
void fpga_xm_service(void);
int fpga_xm_active(void);
int fpga_xm_finished(void);
void fpga_xm_stop(void);
const char *fpga_xm_error(void);
const char *fpga_xm_title(void);
int fpga_xm_position(uint32_t *cur, uint32_t *total);
void fpga_xm_info(fmt_info_t *o);
void fpga_xm_audio(fmt_audio_t *o);
int fpga_xm_get_volume(void);
int fpga_xm_set_volume(int v);
int fpga_xm_pause(int on);

// PT3
int pt3_start(const play_src_t *src, uint32_t size, play_progress_t progress);
int pt2_start(const play_src_t *src, uint32_t size, play_progress_t progress);
int stc_start(const play_src_t *src, uint32_t size, play_progress_t progress);
void pt3_service(void);
int pt3_active(void);
void pt3_stop(void);
const char *pt3_error(void);
const char *pt3_title(void);
int pt3_position(uint32_t *cur, uint32_t *total);
void pt3_info(fmt_info_t *o);
void pt3_audio(fmt_audio_t *o);
int pt3_get_volume(void);
int pt3_set_volume(int v);
int pt3_pause(int on);
int pt3_finished(void);

// the unified engine
void fpga_service(void);
int fpga_active(void);
int fpga_finished(void);
void fpga_stop(void);
const char *fpga_error(void);
const char *fpga_title(void);
int fpga_position(uint32_t *cur, uint32_t *total);
void fpga_info(fmt_info_t *o);
void fpga_audio(fmt_audio_t *o);
int fpga_get_volume(void);
int fpga_set_volume(int v);
int fpga_pause(int on);

extern const play_engine_t fpga_engine;
