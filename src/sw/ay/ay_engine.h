// One AY/YM register-stream engine. The replayers come from libayfly
// (AY-3-8910/12 Emulator, S.V. Bulba; GPLv2, see ay/COPYING).
#pragma once
#include <stdint.h>

enum {
  AYF_PT1 = 0,
  AYF_PT2,
  AYF_PT3,
  AYF_STC,
  AYF_ASC,
  AYF_PSC,
  AYF_SQT,
  AYF_STP,
  AYF_VTX,
  AYF_PSG
};

#ifdef __cplusplus
extern "C" {
#endif
int ay_engine_start(int fmt, const uint8_t *data, uint32_t size);
void ay_engine_tick(void);
void ay_engine_stop(void);
int ay_engine_active(void);
int ay_engine_channels(void);
int ay_engine_position(void);
int ay_engine_length(void);
const char *ay_engine_title(void);
#ifdef __cplusplus
}
#endif

// implemented in ay_players.cpp: the shared two-chip register file
#ifdef __cplusplus
extern "C" {
#endif
void ayp_write_reg(int chip, int reg, int val);
int ayp_read_reg(int chip, int reg);
#ifdef __cplusplus
}
#endif
