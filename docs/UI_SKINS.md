# UI skins

The player **core** (`src/sw/player.c`) owns the state (the file list, the keys, the playback, the
now-playing data) and knows neither a format nor a skin. It renders a `player_view_t`
(`src/sw/ui_skin.h`) through a `ui_skin_t`:

```c
typedef struct {
  const char *name;
  void (*init)(void);
  void (*draw)(const player_view_t *v);      // a full redraw
  void (*update)(const player_view_t *v);    // the now-playing panel + the status
  void (*progress)(const player_view_t *v);  // the loading/stream progress
} ui_skin_t;
```

The compiled skins are in `src/sw/ui_skin.c`; **Tab** switches them. Now there is only
`skin_graphic.c` (the current look). The next skin is the text/OCP one, and after it a **JSON skin**.

## The view model

`player_view_t` (see `ui_skin.h`): `path`, the `fs_entry_t` list (`nent/sel/top`), `playing`,
`now`, `status`, `format`, `title`, `skin`, `elapsed`, `total`, `cur/total_pos`, `has_pos`.
A skin draws only from this; it does not call the filesystem or the player.

## Add a C skin

1. `skin_foo.c`: implement the four functions and `const ui_skin_t skin_foo = {...}`.
2. `ui_skin.h`: `extern const ui_skin_t skin_foo;`
3. `ui_skin.c`: add `&skin_foo` to `skins[]`.
4. `src/sw/Makefile` (and `host/run_player_host.sh` for the host picture): add the file.
5. The core does not change; **Tab** reaches the new skin.

## JSON skin

The JSON skin is a **module** (`skin_json.c`) that interprets a declarative layout/theme, so a
skin can be changed without rebuilding the core. It is not a full browser: a fixed widget set with
bindings to the view.

```json
{
  "name": "ocp-text",
  "font": "mono8x16",
  "colors": { "bg": "#000080", "panel": "#0000AA", "text": "#AAAAAA", "accent": "#55FFFF" },
  "screen": [
    { "type": "panel", "x": 0, "y": 2, "w": 50, "h": 31, "title": "SD2  FILES" },
    { "type": "files", "x": 2, "y": 4, "w": 46, "h": 26 },
    { "type": "text", "x": 52, "y": 3, "text": "{format}  {title}" },
    { "type": "progress", "x": 52, "y": 6, "w": 30, "value": "position" },
    { "type": "spectrum", "x": 52, "y": 10, "w": 30, "h": 12, "channels": 2 },
    { "type": "level", "x": 88, "y": 10, "h": 12, "channel": "L" },
    { "type": "level", "x": 92, "y": 10, "h": 12, "channel": "R" },
    { "type": "text", "x": 2, "y": 34, "text": "Up/Dn Enter Esc BkSp Menu+F5 Tab" },
    { "type": "text", "x": 70, "y": 34, "text": "{status}" }
  ]
}
```

Widgets: `panel`, `text`, `files`, `bar`, `progress`, `spectrum`, `level`, `separator`, `keys`.
Bindings: `{path}`, `{status}`, `{format}`, `{title}`, `{elapsed}`, `{total}`, `{pos}`, and the
widget data `files` / `spectrum` / `levels` from the view. Colors are a theme; the font is
`mono8x16` (text) or the graphic fonts.

The JSON files go to `/skins/*.json` on SD2 (or the ROM default); the core loads the selected one
and falls back to `skin_graphic` on a parse error. The parser (a small JSON subset, no malloc) and
the widget renderer live in `skin_json.c`; the core and the formats do not change.


## The full view model (in the core)

`player_view_t` now carries everything a graphical skin needs, without calls into the player:

- files: `path`, `ent/nent/sel/top`;
- state: `playing`, `paused`, `loading`, `now`, `status`, `error`;
- engine: `engine`, `version`, `aud_rate`, `skin`, `format`, `title`;
- track: `channels`, `samples`, `order`, `pattern`, `row`, `song_len`, `speed`, `bpm`, `size`;
- time/position: `elapsed`, `total`, `cur/total_pos`, `has_pos`;
- audio: `level_l/r`, `peak_l/r`, `bands[32]`, `vu[32]`, `volume`, `paused`;
- screen: `frame`, `fb`.

The engines fill `fmt_info_t`/`fmt_audio_t` through `info`/`audio`/`get_volume`/`set_volume`/`pause`;
the MOD fills the levels, the per-channel VU and the volume now. `bands[]` is reserved for the
frequency analyzer (the graphical/text spectrum); the hook is already in the core.

## Skin lifecycle and events

```c
const char *name;
int  (*init)(void);
void (*draw)(const player_view_t *v);
void (*update)(const player_view_t *v);
void (*progress)(const player_view_t *v);
void (*event)(const skin_event_t *e);   // SKIN_EV_KEY / SKIN_EV_MOUSE
void (*unload)(void);
```

The core passes every key to `event` before its own handling; a graphical skin can use it for its
buttons. The core keys: arrows/Enter/Esc/Backspace/Menu+F5, **Tab** (skin), **Space** (pause),
**+/-** (volume), **F12** (soft reset).

## Assets and SD1

`skin_asset_load(name, dst, cap, &len)` reads `/skins/name` from SD2, or the SD1 FL buffer (the
stock `SD1 SKIN` item). `skin_image_t` + `skin_image_bmp()` are the image contract; the decoder and
the first graphical skin are separate (skins last). The SD1 buffer `0x080000` is shared with
MOD/XM, so loading a skin stops the playback first.
