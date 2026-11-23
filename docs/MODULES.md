# Modules: the player and the formats

## The audio path: one output mixer, one analyzer

Every format is a **source of stereo samples** only. The sources are:

- `mod_fpga.c` / `xm_fpga.c` — their own sample mixers;
- `ay_fpga.c` + `ay_engine` — a libayfly replayer followed by the software YM2149
  synthesizer (PT1/PT2/PT3, STC, ASC, PSC, SQT, STP, VTX, PSG).

They all feed `mixer_write()` (`sw/mixer.c`). The output mixer owns:

- the output low-pass filter (two-pole, 12 kHz at 48 kHz, to remove the square-wave
  "sand" of AY modules);
- the one and only `analysis_push()` feed for the spectrum analyzer;
- the write to the 48 kHz audio FIFO (`audio_write()`).

Format engines must not call `audio_write()` or `analysis_push()` themselves and must
not contain their own level-meter/spectrum code. The UI reads the common `fmt_audio_t`
and draws the common analyzer; it does not know which format is playing. The AY
software synthesizer is driven by the FIFO free space, so its tempo cannot depend on
GUI redraw timing.

The core is split into a player, format modules and playback engines:

```
player.c        the GUI, the file list (SD2 / SD1), the keys, the progress, the status
  |
  +-- fmt.c     the registry: by extension / by the first bytes -> a fmt_module_t
  |     |
  |     +-- formats/mod.c  -> mod_fpga.c    the ProTracker player on the VexRiscv
  |     +-- formats/xm.c   -> xm_fpga.c     the FastTracker II player on the VexRiscv
  |     +-- formats/pt3.c  -> ay_fpga.c     the YM2149 register stream (PT3)
  |     +-- formats/pt2.c  -> ay_fpga.c     the YM2149 register stream (PT2)
  |     +-- formats/stc.c  -> ay_fpga.c     the YM2149 register stream (STC)
  |     +-- formats/ay_more.c -> ay_fpga.c  PT1/ASC/PSC/SQT/STP/VTX/PSG
  |     |                     fpga.c        the shared FPGA engine dispatcher
  |     +-- formats/s3m.c  -> mikmod_fpga.c
  |     +-- formats/it.c   -> mikmod_fpga.c
  |
  +-- src.c     a play_src_t: a FatFs file on SD2, or the SRAM buffer from SD1
  +-- sd1.c     the SD1 file of the stock file loader (menu_soc 0x60 / 0x64 / 0x6c / 0x68)
  +-- mixer.c   the one output mixer: filter + analyzer + FIFO
  +-- audio.c   the 48 kHz stereo FIFO (menu_soc 0x70 / 0x74)
```

The player does not know a format or an engine: it asks `fmt_by_ext()` (the file name) or
`fmt_by_head()` (the first 1 KB), calls `module->play(&src, size, progress)`, and shows
`module->error()` on a failure. The list is filtered by `fmt_supported()`. The main loop calls
`fmt_service()` (the FPGA mixer fills the audio FIFO; the ESP engine does nothing) and
`fmt_active()` (the song is still playing).

## The interfaces (`fmt.h`, `src.h`, `audio.h`)

```c
typedef struct {          // src.h: a FIL on SD2 or the SRAM bytes from SD1
  FIL *f;
  const uint8_t *mem;
} play_src_t;             // src_read(): n bytes at an offset

typedef struct {
  const char *id;       // "mod"
  const char *ext;      // "mod" (lowercase, no dot)
  int (*detect)(const uint8_t *head, uint32_t len);
  int (*play)(const play_src_t *src, uint32_t size, play_progress_t progress);
  const char *(*error)(void);
} fmt_module_t;

typedef struct {
  const char *name;                 // "FPGA"
  int (*init)(void);
  int (*stop)(void);
  const char *(*version)(void);
  const char *(*error)(void);
  void (*service)(void);            // the main loop: the FIFO / the stream
  int (*active)(void);              // still playing
} play_engine_t;
```

`play` starts the playback: the FPGA engine returns at once and mixes in `service`. The output of
`play` is 0 or an error code; the module's `error()` has the text.

## Add a format

1. Write the decoder/mixer in the SRAM (the module bytes are at `0x080000` for an SD1 file, or copied
   there from SD2 in `play`), like `mod_fpga.c`, or use a complete third-party engine like MikMod.
2. `formats/foo.c` exposes a `fmt_module_t` with `detect()`, `play()` and `error()`.
3. `fmt.h`: `extern const fmt_module_t fmt_foo;`
4. `fmt.c`: add `&fmt_foo` to `fmt_modules[]` and to the engine's dispatch.
5. `src/sw/Makefile`: add the format and its engine sources to `SRCS` / `MIKMOD_SRCS`.
6. `src/core.json`: add `foo` to the `FL` item's `extensions` (the firmware's SD1 file browser).
7. `make -C src/sw`, then `cd src && make go-core minig-core` (the bitstream is unchanged).

## Add a format on the FPGA

1. Write the decoder/mixer in the SRAM (the module bytes are at `0x080000` for an SD1 file, or copied
   there from SD2 in `play`), like `mod_fpga.c`.
2. The format module's `play` starts it; `fpga_engine.service` mixes into the audio FIFO.
3. Add the format to the registry and the Makefile as above. No ESP32 and no firmware change.

## A different engine

Add a `play_engine_t` and the format modules of that engine. Add the engine to `engines[]` in
`fmt.c`; the player initializes all and shows the first one's name / version. The FPGA engine is
the only one in the current build; S3M/IT use MikMod inside it.

## The audio FIFO (`menu_soc` 0x70 / 0x74)

- `0xc0` W: one AY write `{ reset, 3'b0, chip, reg[3:0], data[7:0] }` to the two YM2149s;
  `0xc4` W: bit 0 on (the RTL pair drives the output), bits 15..8 volume 0..64.
- `0x70` W: one stereo sample `{ L[15:0], R[15:0] }`; `0x74` R: the free entries of the FIFO (8191
  usable of the 8192-deep BRAM: the 13-bit pointers cannot tell 8192 from 0, so one slot stays
  empty). The FIFO pops at 48 kHz (`aud_l/r`, `aud_t`); the top syncs them to `clk_sys` and adds
  them to the ADC and the menu sounds for the PCM5102 DAC and HDMI. The AY pair bypasses the FIFO
  and is sampled directly into the same 48 kHz output.
- The mixer (`fpga_service`) fills the FIFO in chunks (256 samples) from the main loop; the GUI and
  the keys keep running between the chunks. The ~8191 samples (171 ms) hide an SD read or a redraw.
  **Never** report the FIFO as free when it is full: the mixer would race ahead of the audio and the
  song would play at fast-forward (the first bug of the player).

## What is not dynamic

The modules are compiled into `player.bin` (the ROM of the `.kg`), not loaded from the SD card. A
dynamic loader would need a position-independent ABI and a jump table; the small player does not
need it. A new format is a new `formats/*.c` + a rebuild of the program (the bitstream stays).

## The current formats

| Module | Extension | Engine | Detect |
|---|---|---|---|
| `mod` | `.mod` | **FPGA** (`mod_fpga.c`) | `M.K.` / `M!K!` / `4CHN` ... at 1080 |
| `xm` | `.xm` | **FPGA** (`xm_fpga.c`) | `Extended Module: ` at 0 |
| `pt3` | `.pt3` | **FPGA** (`ay_fpga.c`, two YM2149) | `ProTracker 3.` / `Vortex Tracke` at 0 |
| `pt2` | `.pt2` | **FPGA** (`ay_fpga.c`, two YM2149) | ProTracker 2 header |
| `stc` | `.stc` | **FPGA** (`ay_fpga.c`, libayfly) | Sound Tracker header |
| `pt1` | `.pt1` | **FPGA** (`ay_fpga.c`, libayfly) | extension |
| `asc` | `.asc` | **FPGA** (`ay_fpga.c`, libayfly) | extension |
| `psc` | `.psc` | **FPGA** (`ay_fpga.c`, libayfly) | extension |
| `sqt` | `.sqt` | **FPGA** (`ay_fpga.c`, libayfly) | extension |
| `stp` | `.stp` | **FPGA** (`ay_fpga.c`, libayfly) | extension |
| `vtx` | `.vtx` | **FPGA** (`ay_fpga.c`, libayfly) | extension |
| `psg` | `.psg` | **FPGA** (`ay_fpga.c`, libayfly) | extension |
| `s3m` | `.s3m` | FPGA (MikMod) | `SCRM` at 0x2c |
| `it` | `.it` | FPGA (MikMod) | `IMPM` at 0 |
