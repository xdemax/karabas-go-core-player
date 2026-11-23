// The player and the format modules. The player (player.c) knows nothing about a format: it asks
// the registry by the file name / the first bytes, and calls the module's play(). A module uses an
// engine (the FPGA mixer or the ESP32-S3).
#pragma once
#include <stdint.h>
#include "src.h"

typedef void (*play_progress_t)(uint32_t done, uint32_t total);

#define FMT_BANDS 32
#define FMT_CHANNELS 32

typedef struct {
  int channels;
  int samples;
  int order;
  int pattern;
  int row;
  int song_len;
  int speed;
  int bpm;
  uint32_t size;
} fmt_info_t;

typedef struct {
  int32_t level_l, level_r;
  int32_t peak_l, peak_r;
  uint8_t bands[FMT_BANDS];
  uint8_t vu[FMT_CHANNELS];
  int volume;
  int paused;
} fmt_audio_t;

typedef struct {
  const char *id;       // "mod"
  const char *ext;      // "mod" (lowercase, no dot)
  int (*detect)(const uint8_t *head, uint32_t len);
  int (*play)(const play_src_t *src, uint32_t size, play_progress_t progress);
  const char *(*error)(void);
} fmt_module_t;

typedef struct {
  const char *name;                 // "FPGA", "ESP32"
  int (*init)(void);
  int (*stop)(void);
  const char *(*version)(void);
  const char *(*error)(void);
  void (*service)(void);
  int (*active)(void);
  const char *(*title)(void);
  int (*position)(uint32_t *cur, uint32_t *total);
  void (*info)(fmt_info_t *o);
  void (*audio)(fmt_audio_t *o);
  int (*get_volume)(void);
  int (*set_volume)(int v);
  int (*pause)(int on);              // -1 toggle; returns the paused state
} play_engine_t;

extern const fmt_module_t fmt_mod;
extern const fmt_module_t fmt_xm;
extern const fmt_module_t fmt_pt3;
extern const fmt_module_t fmt_pt2;
extern const fmt_module_t fmt_stc;
extern const fmt_module_t fmt_s3m;
extern const fmt_module_t fmt_it;

const fmt_module_t *fmt_by_ext(const char *name);
const fmt_module_t *fmt_by_head(const uint8_t *head, uint32_t len);
int fmt_supported(const char *name);
int fmt_supported_list(char *out, int max);

void fmt_init(void);
void fmt_service(void);
int fmt_active(void);
int fmt_finished(void);
void fmt_stop(void);
const char *fmt_error(void);
const char *fmt_engine_name(void);
const char *fmt_engine_version(void);
const char *fmt_title(void);
int fmt_position(uint32_t *cur, uint32_t *total);
void fmt_info(fmt_info_t *o);
void fmt_audio(fmt_audio_t *o);
void fmt_set_analysis(int on);
int fmt_volume(void);
int fmt_set_volume(int v);
int fmt_pause(int on);
