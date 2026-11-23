# Testing on the board

The core is built for the **Mini G** (rev.G / G2) with the **VDAC32 shield** (ESP32-S3). **MOD, XM,
PT3, PT2 and STC** play on the FPGA itself: the sound comes from the board's PCM5102 DAC / HDMI, no
ESP32. **S3M** still uses the ESP32 (its I2S goes to the shield / an external DAC). No firmware change
is needed.

## What to put on the cards

- **SD1** (the card of the RP2040): `musicplayer.kg` (the root or `cores/`) and the modules in
  `/mods` (`.mod`, `.xm`, `.pt3`, `.pt2`, `.stc`, `.s3m`). The core does not read this card itself; the stock file loader of the firmware (the
  `SD1 MUSIC` OSD item, Menu+F5) sends the selected file to the core.
- **SD2** (the FPGA slot): the `.mod` / `.xm` / `.pt3` / `.pt2` / `.stc` / `.s3m` files, from `/` down. The card must be
  FAT12 / FAT16 / FAT32 / exFAT with 512-byte sectors (FatFs R0.15 reads all of them). The core
  owns this slot while it runs (`msc2` is off). Files up to 2.5 MB load into the fast SRAM; larger
  ones (e.g. `Radix - I Realize.xm`, 2.8 MB) are copied into the board's 32 MB SDRAM first. The
  load progress shows in the right panel; a 2.8 MB file takes a few seconds.
- The ESP32-S3 module: its tracker firmware (`player/esp32`, API 1, feature 10).

## Steps

1. Put the cards in, power on, pick **Music Player** in the boot menu.
2. The list of the root of SD2. Up / Down, Left / Right (a screen up / down, also PgUp / PgDn),
   Home / End, Enter on a directory.
3. Enter on a `.mod` / `.xm` / `.pt3` / `.pt2` / `.stc`: the FPGA engine starts, the footer shows
   `playing …`, the sound is from the board. AY modules use the two RTL YM2149s.
4. Enter on a `.s3m`: the file goes to the ESP32 (`sending...`, a bar, then `playing s3m`).
5. Esc stops; Backspace goes up a directory.
6. The optional LED matrix (the add-on of the Mini G) scrolls `KARABAS GO MUSIC PLAYER` once at
   startup, then shows the L/R audio meters of the player (`audio_peaks` -> `kg_mcu2` AUX). It is
   configured by the `matrix` key of `core.json`; the firmware's `[matrix]` settings apply too.
7. **SD1**: Menu+F5 Space/pause +/- vol (or Menu+Esc, then `SD1 MUSIC`) opens the firmware's file browser at `/mods`;
   Enter sends the file to the core, which plays it. The browser shows the name; the core shows
   `playing SD1`. Esc closes the firmware OSD; the core's SD2 list is back.

## What can go wrong

| Screen | Meaning |
|---|---|
| `no ESP32` | the module does not answer `GET_VER`: the shield, its CS (`K5` / `UBUS_26`), the SPI bus, or its firmware |
| `no chunk request` / `module rejected the chunk` | the module's tracker refused the stream; check its firmware version and the free PSRAM |
| `no mod / xm / pt3 / pt2 / stc / s3m files here` | no matching files in this directory |
| `SD1: not mod/xm/...` / `SD1: file too big` | the FL file has no known signature / is over 2.5 MB |
| `SD2: no card or no FAT` | the card is missing, not FAT / exFAT, or the SD init failed (check the card and the slot) |

The core has no serial log on the board; the messages are on the screen (`src/sw/player.c`).

## Host tests

```
src/sw/host/run_fs_test.sh     # FatFs on a FAT16 image: the list, the filter, a file read
src/sw/host/run_mod_test.sh    # the FPGA MOD player: a tiny module, the parser, the mixer
src/sw/host/run_xm_test.sh     # the FPGA XM player: a synthetic module, delta samples, loop, pause
src/sw/host/run_render_xm.sh s.xm out.wav 10
                               # render XM with the real engine (compare with libopenmpt)
src/sw/host/run_render_ay.sh s.pt3 out.wav 10
                               # render PT3/PT2/STC through the AY engine (host model)
src/sw/host/run_sd_test.sh     # sd.c against an SDHC card model
src/sim/spi/run_spi_sim.sh     # the SPI master against a slave model (Verilator)
src/timing_ascii.sh minig      # the timing report of the bitstream (from /tmp)
```
