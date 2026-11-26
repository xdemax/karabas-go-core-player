// The registry of the format modules and the playback engines. Adding a format: a file in
// formats/, its entry in fmt_modules[] here, the file in the Makefile, and the extension in the
// FL item of core.json (the firmware's SD1 file browser).
#include <string.h>
#include "fmt.h"
#include "analysis.h"
#include "mixer.h"
#include "fpga.h"
#include "playlist.h"

extern const fmt_module_t fmt_pt1, fmt_asc, fmt_psc, fmt_sqt, fmt_stp, fmt_vtx, fmt_psg, fmt_mp3;
static const fmt_module_t *const fmt_modules[] = { &fmt_mod, &fmt_xm, &fmt_s3m, &fmt_stm, &fmt_it, &fmt_pt3, &fmt_pt2, &fmt_stc, &fmt_pt1, &fmt_asc, &fmt_psc, &fmt_sqt, &fmt_stp, &fmt_vtx, &fmt_psg, &fmt_mp3 };
#define NFMT (sizeof(fmt_modules) / sizeof(fmt_modules[0]))

static const play_engine_t *const engines[] = { &fpga_engine };
#define NENG (sizeof(engines) / sizeof(engines[0]))

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ext_eq(const char *a, const char *b) {
  while (*a && *b) {
    if (lower(*a) != lower(*b)) return 0;
    a++; b++;
  }
  return !*a && !*b;
}

const fmt_module_t *fmt_by_ext(const char *name) {
  if (!name) return NULL;
  const char *dot = strrchr(name, '.');
  const char *e = dot ? dot + 1 : name;
  for (unsigned i = 0; i < NFMT; i++)
    if (ext_eq(e, fmt_modules[i]->ext)) return fmt_modules[i];
  return NULL;
}

const fmt_module_t *fmt_by_head(const uint8_t *head, uint32_t len) {
  for (unsigned i = 0; i < NFMT; i++)
    if (fmt_modules[i]->detect && fmt_modules[i]->detect(head, len)) return fmt_modules[i];
  return NULL;
}

int fmt_supported(const char *name) { return fmt_by_ext(name) != NULL; }

// The formats advertised on the title screen. MP3 stays playable if the user selects it,
// but is not advertised while its quality is still being worked on.
static int title_hidden(const char *e) {
  return strcmp(e, "mp3") == 0;
}

// "mod, xm, s3m": the extensions of the registry, in the registration order. A new format
// module automatically appears in the startup panel of the skin unless it is hidden above.
// The title screen groups the formats by the chip / tracker they come from.
static const struct { const char *label, *exts; } fmt_groups[] = {
  { "AY8912",           "pt2 pt3 stc pt1 asc psc sqt stp vtx psg" },
  { "SID8580",          "sid" },
  { "Scream Tracker",   "stm s3m" },
  { "ProTracker",       "mod" },
  { "FastTracker",      "xm" },
  { "Impulse Tracker",  "it" },
  { "Playlists",        "m3u ayl" },
};
#define NFMT_GROUPS (sizeof(fmt_groups) / sizeof(fmt_groups[0]))

int fmt_grouped_list(char *out, int max) {
  int n = 0;
  if (max <= 0) return 0;
  out[0] = 0;
  for (unsigned g = 0; g < NFMT_GROUPS; g++) {
    const char *lab = fmt_groups[g].label, *e = fmt_groups[g].exts;
    int need = (int) strlen(lab) + 2;
    for (const char *q = e; *q; q++) need++;
    if (n + need + 2 > max) break;
    if (n) out[n++] = '\n';
    while (*lab) out[n++] = *lab++;
    out[n++] = ':'; out[n++] = ' ';
    while (*e) out[n++] = *e++;
    out[n] = 0;
  }
  return n;
}

int fmt_supported_list(char *out, int max) {
  int n = 0;
  if (max <= 0) return 0;
  out[0] = 0;
  for (unsigned i = 0; i < NFMT; i++) {
    const char *e = fmt_modules[i]->ext;
    if (title_hidden(e)) continue;
    int len = 0;
    while (e[len]) len++;
    if (n + len + (n ? 2 : 0) + 1 > max) break;
    if (n) { out[n++] = ','; out[n++] = ' '; }
    for (int j = 0; j < len; j++) out[n++] = e[j];
    out[n] = 0;
  }
  {
    const char *pl = playlist_extensions();
    int len = 0;
    while (pl[len]) len++;
    if (n + 2 + len + 1 <= max) {
      out[n++] = ','; out[n++] = ' ';
      for (int j = 0; j < len; j++) out[n++] = pl[j];
      out[n] = 0;
    }
  }
  return n;
}

void fmt_init(void) {
  for (unsigned i = 0; i < NENG; i++)
    if (engines[i]->init) engines[i]->init();
}

void fmt_service(void) {
  for (unsigned i = 0; i < NENG; i++)
    if (engines[i]->service) engines[i]->service();
}

int fmt_finished(void) { return fpga_finished(); }

int fmt_active(void) {
  for (unsigned i = 0; i < NENG; i++)
    if (engines[i]->active && engines[i]->active()) return 1;
  return 0;
}

void fmt_stop(void) {
  mixer_reset();
  for (unsigned i = 0; i < NENG; i++)
    if (engines[i]->stop) engines[i]->stop();
}

const char *fmt_title(void) {
  for (unsigned i = 0; i < NENG; i++) {
    if (engines[i]->title) {
      const char *s = engines[i]->title();
      if (s && s[0]) return s;
    }
  }
  return "";
}

int fmt_position(uint32_t *cur, uint32_t *total) {
  for (unsigned i = 0; i < NENG; i++)
    if (engines[i]->position && engines[i]->position(cur, total)) return 1;
  return 0;
}

void fmt_info(fmt_info_t *o) {
  memset(o, 0, sizeof(*o));
  for (unsigned i = 0; i < NENG; i++)
    if (engines[i]->info) { engines[i]->info(o); return; }
}

void fmt_audio(fmt_audio_t *o) {
  memset(o, 0, sizeof(*o));
  o->volume = 64;
  for (unsigned i = 0; i < NENG; i++)
    if (engines[i]->audio) { engines[i]->audio(o); return; }
}

void fmt_set_analysis(int on) { analysis_set_enabled(on); }

int fmt_volume(void) {
  for (unsigned i = 0; i < NENG; i++)
    if (engines[i]->get_volume) return engines[i]->get_volume();
  return 64;
}

int fmt_set_volume(int v) {
  for (unsigned i = 0; i < NENG; i++)
    if (engines[i]->set_volume) return engines[i]->set_volume(v);
  return v;
}

int fmt_pause(int on) {
  for (unsigned i = 0; i < NENG; i++)
    if (engines[i]->pause) return engines[i]->pause(on);
  return 0;
}

const char *fmt_error(void) {
  for (unsigned i = 0; i < NENG; i++)
    if (engines[i]->error) return engines[i]->error();
  return "";
}

const char *fmt_engine_name(void) { return NENG ? engines[0]->name : ""; }
const char *fmt_engine_version(void) { return NENG && engines[0]->version ? engines[0]->version() : ""; }
