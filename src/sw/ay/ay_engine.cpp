// Whole replayers from libayfly: PT1/PT2/PT3, STC, ASC, PSC, SQT, STP, VTX, PSG.
// They only produce YM2149 register writes, which go to our two RTL chips.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "ay_engine.h"
#ifndef abs
#define abs(x) ((x) < 0 ? -(x) : (x))
#endif

struct AYSongInfo {
  unsigned char *module, *module1, *file_data;
  unsigned long module_len, file_len;
  unsigned long Length, Loop;
  void *data, *data1;
  bool is_ts;
  bool ts_single;       // one PT3 module holds both chips (mirrored patterns)
  unsigned int ts_mode; // mirrored-pattern base from the PT3 3.7 Mode byte
  unsigned char chip_type;
  const char *Name, *Author, *TrackName, *CompName, *PrgName;
};


// newlib-nano without libstdc++: the replayers only use new/delete for their state.
extern "C" void *malloc(size_t); extern "C" void free(void *);
void *operator new(size_t n) { return malloc(n); }
void *operator new[](size_t n) { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

static AYSongInfo ayi;
static int ay_ticks, ay_total;
static void (*ay_play_fn)(AYSongInfo&);
static void (*ay_cleanup_fn)(AYSongInfo&);

unsigned short ay_sys_getword(unsigned char *p) {
  return (unsigned short) (p[0] | (p[1] << 8));
}
unsigned long ay_sys_getdword(unsigned char *p) {
  return (unsigned long) (p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned long) p[3] << 24));
}
void ay_sys_writeword(unsigned char *p, unsigned short v) {
  p[0] = (unsigned char) v;
  p[1] = (unsigned char) (v >> 8);
}
static char ay_strbuf[4][64];
static int ay_strn;
const char *ay_sys_getstr(const unsigned char *p, unsigned long n) {
  char *d = ay_strbuf[ay_strn++ & 3];
  if (n > 63) n = 63;
  memcpy(d, p, n);
  d[n] = 0;
  for (int i = (int) n - 1; i >= 0 && d[i] == ' '; i--) d[i] = 0;
  return d;
}
void ay_setchiptype(AYSongInfo *, int) {}
void ay_setayfreq(AYSongInfo *, int) {}
int ay_sys_decodelha(AYSongInfo &, unsigned long) { return 0; }

void ay_resetay(AYSongInfo *, int chip = 0) {
  for (int r = 0; r < 16; r++) ayp_write_reg(chip, r, r == 7 ? 0xff : 0);
}
void ay_writeay(AYSongInfo *, int reg, int val, int chip = 0) {
  if (reg >= 0 && reg < 16) ayp_write_reg(chip, reg, val);
}
int ay_readay(AYSongInfo *, int reg) {
  return (reg >= 0 && reg < 16) ? ayp_read_reg(0, reg) : 0;
}

#define AY_CHNL_A_FINE 0
#define AY_CHNL_A_COARSE 1
#define AY_CHNL_B_FINE 2
#define AY_CHNL_B_COARSE 3
#define AY_CHNL_C_FINE 4
#define AY_CHNL_C_COARSE 5
#define AY_NOISE_PERIOD 6
#define AY_MIXER 7
#define AY_CHNL_A_VOL 8
#define AY_CHNL_B_VOL 9
#define AY_CHNL_C_VOL 10
#define AY_ENV_FINE 11
#define AY_ENV_COARSE 12
#define AY_ENV_SHAPE 13

#include "players/PT1Play.h"
#include "players/PT2Play.h"
#include "players/PT3Play.h"
#include "players/STCPlay.h"
#include "players/ASCPlay.h"
#include "players/PSCPlay.h"
#include "players/SQTPlay.h"
#include "players/STPPlay.h"
#include "players/VTXPlay.h"
#include "players/PSGPlay.h"

static void init_one(void (*fn)(AYSongInfo &)) {
  if (fn) fn(ayi);
}

extern "C" int ay_engine_start(int fmt, const uint8_t *data, uint32_t size) {
  ay_engine_stop();
  if (!data || size < 16) return -1;
  memset(&ayi, 0, sizeof(ayi));
  ayi.module = (unsigned char *) data;
  ayi.file_data = (unsigned char *) data;
  ayi.module_len = ayi.file_len = size;
  ay_ticks = 0;
  ay_total = 0;
  ay_play_fn = 0; ay_cleanup_fn = 0;
  switch (fmt) {
    case AYF_PT1: init_one(PT1_Init); ay_play_fn=PT1_Play; ay_cleanup_fn=PT1_Cleanup; break;
    case AYF_PT2: init_one(PT2_Init); ay_play_fn=PT2_Play; ay_cleanup_fn=PT2_Cleanup; break;
    case AYF_PT3: init_one(PT3_Init); ay_play_fn=PT3_Play; ay_cleanup_fn=PT3_Cleanup; break;
    case AYF_STC: init_one(STC_Init); ay_play_fn=STC_Play; ay_cleanup_fn=STC_Cleanup; break;
    case AYF_ASC: init_one(ASC_Init); ay_play_fn=ASC_Play; ay_cleanup_fn=ASC_Cleanup; break;
    case AYF_PSC: init_one(PSC_Init); ay_play_fn=PSC_Play; ay_cleanup_fn=PSC_Cleanup; break;
    case AYF_SQT: init_one(SQT_Init); ay_play_fn=SQT_Play; ay_cleanup_fn=SQT_Cleanup; break;
    case AYF_STP: init_one(STP_Init); ay_play_fn=STP_Play; ay_cleanup_fn=STP_Cleanup; break;
    case AYF_VTX: init_one(VTX_Init); ay_play_fn=VTX_Play; ay_cleanup_fn=VTX_Cleanup; break;
    case AYF_PSG: init_one(PSG_Init); ay_play_fn=PSG_Play; ay_cleanup_fn=PSG_Cleanup; break;
    default: return -1;
  }
  if (!ay_play_fn) return -1;
  switch (fmt) {
    case AYF_PT1: PT1_GetInfo(ayi); break;
    case AYF_PT2: PT2_GetInfo(ayi); break;
    case AYF_PT3: PT3_GetInfo(ayi); break;
    case AYF_STC: STC_GetInfo(ayi); break;
    case AYF_ASC: ASC_GetInfo(ayi); break;
    case AYF_PSC: PSC_GetInfo(ayi); break;
    case AYF_SQT: SQT_GetInfo(ayi); break;
    case AYF_STP: STP_GetInfo(ayi); break;
    case AYF_VTX: VTX_GetInfo(ayi); break;
    case AYF_PSG: PSG_GetInfo(ayi); break;
  }
  ay_total = (int) ayi.Length;
  return 0;
}
extern "C" void ay_engine_tick(void) {
  if (!ay_play_fn) return;
  ay_play_fn(ayi);
  ay_ticks++;
}
extern "C" void ay_engine_stop(void) {
  if (ay_cleanup_fn) ay_cleanup_fn(ayi);
  ay_play_fn = 0; ay_cleanup_fn = 0;
  memset(&ayi, 0, sizeof(ayi));
}
extern "C" int ay_engine_active(void) { return ay_play_fn != 0; }
extern "C" int ay_engine_channels(void) { return ayi.is_ts ? 6 : 3; }
extern "C" int ay_engine_position(void) { return ay_ticks; }
extern "C" int ay_engine_length(void) { return ay_total; }
extern "C" const char *ay_engine_title(void) { return ayi.Name ? ayi.Name : ""; }
