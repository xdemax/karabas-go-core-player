#include "fpga.h"
#include "mikmod_fpga.h"

// The active player. Only one of MOD/XM is started at a time (the player stops the previous one).
int fpga_active(void) {
  return fpga_mod_active() || fpga_xm_active() || pt3_active() || mikmod_active();
}

int fpga_finished(void) {
  return fpga_xm_finished() || fpga_mod_finished() || pt3_finished() || mikmod_finished();
}

void fpga_service(void) {
  if (fpga_mod_active()) fpga_mod_service();
  else if (fpga_xm_active()) fpga_xm_service();
  else if (pt3_active()) pt3_service();
  else if (mikmod_active()) mikmod_service();
}

void fpga_stop(void) {
  fpga_mod_stop();
  fpga_xm_stop();
  pt3_stop();
  mikmod_stop();
}

const char *fpga_error(void) {
  if (fpga_xm_active()) return fpga_xm_error();
  if (fpga_mod_active()) return fpga_mod_error();
  if (pt3_active()) return pt3_error();
  if (mikmod_active()) return mikmod_error();
  return "";
}

const char *fpga_title(void) {
  if (fpga_xm_active()) return fpga_xm_title();
  if (fpga_mod_active()) return fpga_mod_title();
  if (pt3_active()) return pt3_title();
  if (mikmod_active()) return mikmod_title();
  return "";
}

int fpga_position(uint32_t *cur, uint32_t *total) {
  if (fpga_xm_active()) return fpga_xm_position(cur, total);
  if (fpga_mod_active()) return fpga_mod_position(cur, total);
  if (pt3_active()) return pt3_position(cur, total);
  if (mikmod_active()) return mikmod_position(cur, total);
  return 0;
}

void fpga_info(fmt_info_t *o) {
  if (fpga_xm_active()) fpga_xm_info(o);
  else if (fpga_mod_active()) fpga_mod_info(o);
  else if (pt3_active()) pt3_info(o);
  else if (mikmod_active()) mikmod_info(o);
}

void fpga_audio(fmt_audio_t *o) {
  if (fpga_xm_active()) fpga_xm_audio(o);
  else if (fpga_mod_active()) fpga_mod_audio(o);
  else if (pt3_active()) pt3_audio(o);
  else if (mikmod_active()) mikmod_audio(o);
}

int fpga_get_volume(void) {
  if (fpga_xm_active()) return fpga_xm_get_volume();
  if (fpga_mod_active()) return fpga_mod_get_volume();
  if (pt3_active()) return pt3_get_volume();
  if (mikmod_active()) return mikmod_get_volume();
  return 64;
}

int fpga_set_volume(int v) {
  if (fpga_xm_active()) return fpga_xm_set_volume(v);
  if (fpga_mod_active()) return fpga_mod_set_volume(v);
  if (pt3_active()) return pt3_set_volume(v);
  if (mikmod_active()) return mikmod_set_volume(v);
  return v;
}

int fpga_pause(int on) {
  if (fpga_xm_active()) return fpga_xm_pause(on);
  if (fpga_mod_active()) return fpga_mod_pause(on);
  if (pt3_active()) return pt3_pause(on);
  if (mikmod_active()) return mikmod_pause(on);
  return 0;
}

static int e_init(void) { return 0; }
static int e_stop(void) { fpga_stop(); return 0; }
static const char *e_ver(void) { return "1.0"; }
static const char *e_err(void) { return fpga_error(); }

const play_engine_t fpga_engine = { "FPGA", e_init, e_stop, e_ver, e_err, fpga_service, fpga_active,
                                    fpga_title, fpga_position, fpga_info, fpga_audio,
                                    fpga_get_volume, fpga_set_volume, fpga_pause };
