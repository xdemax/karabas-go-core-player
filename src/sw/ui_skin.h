// The UI skins: the player core owns the state; a skin renders a player_view_t. The view is the
// only exchange: a skin never calls the player or an engine. Add a skin in ui_skin.c + Makefile;
// Tab switches. The graphical skins can load assets with skin_asset_load().
#pragma once
#include <stdint.h>
#include "fs.h"
#include "fmt.h"

typedef struct {
  const char *path;
  const fs_entry_t *ent;
  int nent, sel, top;

  int playing;
  int paused;
  int loading;
  int now;

  const char *status;
  const char *mix;              // "MIX <samples/s> U<underruns>", shown on its own footer row
  const char *error;
  const char *engine;
  const char *version;
  const char *build;
  const char *debug;
  const char *skin;
  const char *format;
  const char *title;
  const char *formats;          // "mod, xm, s3m": the registry, for the startup panel

  int channels, samples;
  int order, pattern, row, song_len;
  int speed, bpm;
  uint32_t size;

  uint32_t elapsed;
  uint32_t total;
  uint32_t cur, total_pos;
  int has_pos;

  int32_t level_l, level_r;     // the last mixed sample, -32768..32767
  int32_t peak_l, peak_r;       // the peak since the previous audio() call
  uint8_t bands[FMT_BANDS];     // 0..255, low .. high frequency (the analyzer)
  int spectrum;                 // 0: too many channels, the analyzer is off (the skin says so)
  uint8_t vu[FMT_CHANNELS];     // 0..255 per channel
  int volume;                   // 0..64

  uint32_t aud_rate;
  uint32_t frame;               // the update counter (for animations)
  int fb;                       // the shown frame buffer
  void (*idle)(void);           // feed the audio while a long list is being painted
} player_view_t;

enum { SKIN_EV_KEY = 1, SKIN_EV_MOUSE = 2 };
typedef struct {
  int type;
  int key;
  int x, y;
  int buttons;
  int wheel;
} skin_event_t;

typedef struct {
  uint32_t w, h;
  uint8_t bpp;
  uint8_t reserved[3];
  const uint8_t *pixels;
  uint32_t size;
  int stride;
  const uint16_t *palette;
} skin_image_t;

typedef struct {
  const char *name;
  int  (*init)(void);
  void (*draw)(const player_view_t *v);
  void (*update)(const player_view_t *v);
  void (*progress)(const player_view_t *v);
  void (*list)(const player_view_t *v);   // the file list only (a key scroll)
  void (*event)(const skin_event_t *e);
  void (*unload)(void);
} ui_skin_t;

extern const ui_skin_t skin_graphic;

const ui_skin_t *skin_current(void);
const char *skin_name(void);
void skin_next(void);

// the assets of a graphical skin: SD2 /skins/<name> or the SD1 FL buffer; a BMP/raw decoder comes
// with the first graphical skin. The prototypes are the contract.
int skin_asset_load(const char *name, uint8_t *dst, uint32_t cap, uint32_t *len);
int skin_image_bmp(const uint8_t *data, uint32_t len, skin_image_t *img);
