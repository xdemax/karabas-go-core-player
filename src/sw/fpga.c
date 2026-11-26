#include "fpga.h"
#include "mikmod_fpga.h"
#include "mp3_fpga.h"
#include "it_fpga.h"
#include "s3m_fpga.h"
#include "stm_fpga.h"
#include "sid_fpga.h"

// The active player. Only one of MOD/XM is started at a time (the player stops the previous one).
int fpga_active(void) {
  return fpga_mod_active() || fpga_xm_active() || pt3_active() || mikmod_active() || it_fpga_active() || s3m_fpga_active() || stm_fpga_active() || sid_fpga_active() || mp3_active();
}

int fpga_finished(void) {
  return fpga_xm_finished() || fpga_mod_finished() || pt3_finished() || mikmod_finished() || it_fpga_finished() || s3m_fpga_finished() || stm_fpga_finished() || sid_fpga_finished() || mp3_finished();
}

void fpga_service(void) {
  if (fpga_mod_active()) fpga_mod_service();
  else if (fpga_xm_active()) fpga_xm_service();
  else if (pt3_active()) pt3_service();
  else if (mikmod_active()) mikmod_service();
  else if (it_fpga_active()) it_fpga_service();
  else if (s3m_fpga_active()) s3m_fpga_service();
  else if (stm_fpga_active()) stm_fpga_service();
  else if (sid_fpga_active()) sid_fpga_service();
  else if (mp3_active()) mp3_service();
}

void fpga_stop(void) {
  fpga_mod_stop();
  fpga_xm_stop();
  pt3_stop();
  mikmod_stop();
  it_fpga_stop();
  s3m_fpga_stop();
  stm_fpga_stop();
  sid_fpga_stop();
  mp3_stop();
}

const char *fpga_error(void) {
  if (fpga_xm_active()) return fpga_xm_error();
  if (fpga_mod_active()) return fpga_mod_error();
  if (pt3_active()) return pt3_error();
  if (mikmod_active()) return mikmod_error();
  if (it_fpga_active()) return it_fpga_error();
  if (s3m_fpga_active()) return s3m_fpga_error();
  if (stm_fpga_active()) return stm_fpga_error();
  if (sid_fpga_active()) return sid_fpga_error();
  if (mp3_active()) return mp3_error();
  return "";
}

const char *fpga_title(void) {
  if (fpga_xm_active()) return fpga_xm_title();
  if (fpga_mod_active()) return fpga_mod_title();
  if (pt3_active()) return pt3_title();
  if (mikmod_active()) return mikmod_title();
  if (it_fpga_active()) return it_fpga_title();
  if (s3m_fpga_active()) return s3m_fpga_title();
  if (stm_fpga_active()) return stm_fpga_title();
  if (sid_fpga_active()) return sid_fpga_title();
  if (mp3_active()) return mp3_title();
  return "";
}

int fpga_position(uint32_t *cur, uint32_t *total) {
  if (fpga_xm_active()) return fpga_xm_position(cur, total);
  if (fpga_mod_active()) return fpga_mod_position(cur, total);
  if (pt3_active()) return pt3_position(cur, total);
  if (mikmod_active()) return mikmod_position(cur, total);
  if (it_fpga_active()) return it_fpga_position(cur, total);
  if (s3m_fpga_active()) return s3m_fpga_position(cur, total);
  if (stm_fpga_active()) return stm_fpga_position(cur, total);
  if (sid_fpga_active()) return sid_fpga_position(cur, total);
  if (mp3_active()) return mp3_position(cur, total);
  return 0;
}

void fpga_info(fmt_info_t *o) {
  if (fpga_xm_active()) fpga_xm_info(o);
  else if (fpga_mod_active()) fpga_mod_info(o);
  else if (pt3_active()) pt3_info(o);
  else if (mikmod_active()) mikmod_info(o);
  else if (it_fpga_active()) it_fpga_info(o);
  else if (s3m_fpga_active()) s3m_fpga_info(o);
  else if (stm_fpga_active()) stm_fpga_info(o);
  else if (sid_fpga_active()) sid_fpga_info(o);
  else if (mp3_active()) mp3_info(o);
}

void fpga_audio(fmt_audio_t *o) {
  if (fpga_xm_active()) fpga_xm_audio(o);
  else if (fpga_mod_active()) fpga_mod_audio(o);
  else if (pt3_active()) pt3_audio(o);
  else if (mikmod_active()) mikmod_audio(o);
  else if (it_fpga_active()) it_fpga_audio(o);
  else if (s3m_fpga_active()) s3m_fpga_audio(o);
  else if (stm_fpga_active()) stm_fpga_audio(o);
  else if (sid_fpga_active()) sid_fpga_audio(o);
  else if (mp3_active()) mp3_audio(o);
}

int fpga_get_volume(void) {
  if (fpga_xm_active()) return fpga_xm_get_volume();
  if (fpga_mod_active()) return fpga_mod_get_volume();
  if (pt3_active()) return pt3_get_volume();
  if (mikmod_active()) return mikmod_get_volume();
  if (it_fpga_active()) return it_fpga_get_volume();
  if (s3m_fpga_active()) return s3m_fpga_get_volume();
  if (stm_fpga_active()) return stm_fpga_get_volume();
  if (sid_fpga_active()) return sid_fpga_get_volume();
  if (mp3_active()) return mp3_get_volume();
  return 64;
}

int fpga_set_volume(int v) {
  if (fpga_xm_active()) return fpga_xm_set_volume(v);
  if (fpga_mod_active()) return fpga_mod_set_volume(v);
  if (pt3_active()) return pt3_set_volume(v);
  if (mikmod_active()) return mikmod_set_volume(v);
  if (it_fpga_active()) return it_fpga_set_volume(v);
  if (s3m_fpga_active()) return s3m_fpga_set_volume(v);
  if (stm_fpga_active()) return stm_fpga_set_volume(v);
  if (sid_fpga_active()) return sid_fpga_set_volume(v);
  if (mp3_active()) return mp3_set_volume(v);
  return v;
}

int fpga_pause(int on) {
  if (fpga_xm_active()) return fpga_xm_pause(on);
  if (fpga_mod_active()) return fpga_mod_pause(on);
  if (pt3_active()) return pt3_pause(on);
  if (mikmod_active()) return mikmod_pause(on);
  if (it_fpga_active()) return it_fpga_pause(on);
  if (s3m_fpga_active()) return s3m_fpga_pause(on);
  if (stm_fpga_active()) return stm_fpga_pause(on);
  if (sid_fpga_active()) return sid_fpga_pause(on);
  if (mp3_active()) return mp3_pause(on);
  return 0;
}

static int e_init(void) { return 0; }
static int e_stop(void) { fpga_stop(); return 0; }
static const char *e_ver(void) { return "1.0"; }
static const char *e_err(void) { return fpga_error(); }

const play_engine_t fpga_engine = { "FPGA", e_init, e_stop, e_ver, e_err, fpga_service, fpga_active,
                                    fpga_title, fpga_position, fpga_info, fpga_audio,
                                    fpga_get_volume, fpga_set_volume, fpga_pause };
