# Testing on the board

The core is built for **Karabas Go** and **Mini G** (rev.G / G2). **MOD, XM, PT3, PT2 and STC**
play on the FPGA itself: on the Go the sound comes from the PCM5102 and the picture from VGA; on the
Mini G from the PCM5102 / HDMI. **S3M and IT play through MikMod in the FPGA core.** No firmware
change is needed.

## What to put on the cards

- **SD1 music** is temporarily disabled. The core reads SD2 only; `musicplayer.kg` can still live on SD1.
- **SD2** (the FPGA slot): the `.mod` / `.xm` / `.s3m` / `.it` / `.pt3` / `.pt2` / `.stc` files, from `/` down. The card must be
  FAT12 / FAT16 / FAT32 / exFAT with 512-byte sectors (FatFs R0.15 reads all of them). The core
  owns this slot while it runs (`msc2` is off). Music files are copied directly into the board's
  32 MB SDRAM; the load progress shows in the right panel.


## Steps

1. Put the cards in, power on, pick **Music Player** in the boot menu.
2. The list of the root of SD2. Up / Down, Left / Right (a screen up / down, also PgUp / PgDn),
   Home / End, Enter on a directory.
3. Enter on a `.mod` / `.xm` / `.s3m` / `.it` / `.pt3` / `.pt2` / `.stc`: the FPGA engine starts,
   the footer shows `playing …`, the sound is from the board. AY modules use the two RTL YM2149s.
4. S3M and IT use MikMod; their module and sample memory lives in the 32 MB SDRAM.
5. Esc stops; Backspace goes up a directory.
6. The optional LED matrix (the add-on of the Mini G) scrolls `KARABAS GO MUSIC PLAYER` once at
   startup, then shows the L/R audio meters of the player (`audio_peaks` -> `kg_mcu2` AUX). It is
   configured by the `matrix` key of `core.json`; the firmware's `[matrix]` settings apply too.
7. **SD1 music is disabled for now**; the player uses the SD2 list only.

## What can go wrong

| Screen | Meaning |
|---|---|
| `no mod / xm / s3m / it / pt3 / pt2 / stc files here` | no matching files in this directory |
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
src/timing_ascii.sh go         # the timing report of the Go bitstream (from /tmp)
src/timing_ascii.sh minig      # the timing report of the Mini G bitstream (from /tmp)
```
