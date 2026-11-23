# Karabas Go Music Player Core

A small self-drawing core for the **Karabas Mini G** (rev.G / G2) with the **VDAC32 shield** (ESP32-S3):
a file manager for **both SD cards of the board** and a player for tracker and chiptune modules.
**MOD, XM, PT3, PT2 and STC are played by the FPGA itself**: the VexRiscv mixers for MOD/XM, the
tracker replayers plus **two YM2149s in RTL** for PT3/PT2/STC, and the board's PCM5102 DAC / HDMI,
no ESP32. **S3M** still goes to the ESP32-S3 for now. MP3 is not on this FPGA. **SD2** (the slot
wired to the FPGA) is read by the core itself, as the TS-Conf core does; **SD1** (the card of the
RP2040) uses the stock file loader of the firmware (an `FL` item, as the C64 / NES / Sega cores do).
The firmware stays stock.

The core is a fork of the boot menu core ([karabas-go-core-menu](https://github.com/andykarpov/karabas-go-core-menu)):
VexRiscv rv32im at 75 MHz, an 800 x 600 x 8 bpp frame buffer in the SRAM, the menu blitter / fonts, the
keyboard and pads over link v2 (kg_mcu2).

![the screen (the host preview, `src/sw/host/run_player_host.sh`)](docs/screenshot.png)

## What it does

- **SD2** (the card slot wired to the FPGA): the core reads it itself, as the TS-Conf core does —
  a byte SPI master in the FPGA (`rtl/spi/spi_master.v`), SD SPI mode 0 and FatFs R0.15 on the CPU.
  The SD2 pass-through to the MCU (`msc2`) is off in this core: the card belongs to the player while
  it runs.
- **FPGA MOD player**: `src/sw/mod_fpga.c` parses the ProTracker module in the SRAM, mixes 4/6/8
  channels at 48 kHz stereo into the audio FIFO of `menu_soc` (`0x70` / `0x74`), and the top sends
  it to the PCM5102 / HDMI. No ESP32.
- **FPGA XM player**: `src/sw/xm_fpga.c` parses FastTracker II modules (packed patterns, delta
  samples, envelopes) and mixes 32 channels at 48 kHz. `src/sw/fpga.c` is the shared dispatcher;
  the core and the skins see one `FPGA` engine.
- **FPGA AY players**: `src/sw/ay_players.cpp` wraps the PT3 / PT2 / STC replayers, which write
  the YM2149 registers once per 50 Hz frame. The two chips, their stereo mixer and the low-pass are in
  `src/rtl/soc/menu_soc.v` (`ym2149.v` from the tsconf core); no software AY sampler is in the audio
  path. 3-channel and 6-channel (`02TS`) PT3 are supported.
- **AY output filter**: two 1-pole low-pass stages at the 3.5 MHz YM2149 clock enable, before the
  48 kHz sample, remove the worst of the stepped/square edge aliases (the same idea as the OPL4/GS
  filter in tsconf; MegaBuzz uses a longer FIR).
- **LED matrix**: the optional two 8x8 add-on matrices show the player's L/R audio meters
  (`audio_peaks` -> `kg_mcu2` AUX, the same path as the C64/Atari cores).
- **32 MB SDRAM for big files**: the board's MT48LC16M16A2 (the same chip the Doom core uses) is a
  32 MB window at `0x10000000`. SD2 modules up to 2.5 MB still load into the fast SRAM; larger ones
  (3 MB, 15 MB) are streamed from the card into SDRAM and played from there. The SDRAM pins were
  already in the UCF; the controller is `src/rtl/soc/player_sdram.v`.
- **ESP32-S3** (S3M for now) of the VDAC32 shield on the same SPI bus as the FT812 (which stays
  deselected), its own CS on `K5` (`UBUS_26`), the protocol of the module firmware:
  `WRBUF` / `RDBUF` / `WRDMA` / `WR_DONE` and the tracker stream load + `TRACK_PLAY` / `TRACK_STOP` /
  `DELETE_OBJECT`.
- **SD1** (the card of the RP2040, not wired to the FPGA): the stock **file loader** of the firmware,
  an `FL` item in `core.json` (`SD1 MUSIC`, `.mod/.xm/.pt3/.pt2/.stc/.s3m`, `/mods`). **Menu+F5** (or the OSD,
  Menu+Esc) opens the firmware's file browser; the selected file comes to the core with the size
  and extension (LOAD kind 2), the core plays it from the SRAM. No firmware change, no MCU service
  of our own.
- **No firmware change**: the core only uses its own hardware and the stock services of the
  firmware (the file loader), exactly like the other cores.

Controls: **Up / Down**, **Left / Right** (a page, like **PgUp / PgDn**), **Home / End** move;
**Enter** plays a file (or enters a
directory), **Esc** stops the playback, **Backspace** goes up a directory. **Menu+F5** / **Menu+Esc**
opens the firmware's SD1 file browser (the `SD1 MUSIC` item).

## Build

Requirements (as the menu core):

```
git submodule update --init --recursive
```

- ISE 14.7 (`/opt/Xilinx/14.7/ISE_DS/settings64.sh`, `ISE_SETTINGS=` to change), Spartan-6 XC6SLX25.
- xPack `riscv-none-elf-gcc` 15.2.0-1 in `~/opt/xpack-riscv-none-elf-gcc-15.2.0-1` (`XPACK=` to change).
- `karabas-go-modules` in `modules/` (it is a git submodule). The repository includes
  `src/make_core.py`, so no separate `karabas-go-tools` checkout is needed.

```
make -C src/sw            # src/roms/player.bin (the CPU program, the ROM of the .kg)
cd src && make minig      # bitstream + musicplayer.kg (Mini G)
make minig-report         # LUTs / RAMBs / timing
make minig-coremode       # the second timing run of the Mini G
src/timing_ascii.sh minig # the same from an ASCII directory: trce cannot write its report when the
                          # path has non-ASCII characters ("Загрузки"), the analysis is still fine
```

Put `musicplayer.kg` on SD1 (the root or `cores/`) and pick **Music Player** in the boot menu. The
firmware loads `roms/player.bin` into the SRAM and starts the CPU.

## Layout

- `src/rtl/karabas_minig_top.v`: the board (the menu top with the SPI master, the ESP32 CS, the SD2
  pins taken from the MCU pass-through).
- `src/rtl/soc/menu_soc.v`: the CPU, the SRAM, the video, the loader (the program at 0, the SD1 file
  at 0x080000), the MMIO (SPI at 0x50 / 0x54 / 0x58, the loaded file at 0x60 / 0x64 / 0x68 / 0x6c).
- `src/rtl/spi/spi_master.v`: the byte SPI master (mode 0, a programmable clock, four CS lines).
- `src/sw`: `player.c` (the GUI and the files), `fs.c` (FatFs listing), `sd.c` / `diskio.c` (SD2),
  `sd1.c` (the SD1 file of the stock file loader), `src.c` (a file or the SRAM bytes),
  `audio.c` (the 48 kHz stereo FIFO), `mod_fpga.c` / `xm_fpga.c` (the MOD/XM parsers and mixers),
  `ay_fpga.c` / `ay_players.cpp` (the clean PT3/PT2/STC register stream), `fpga.c` (the shared
  engine), `fmt.c` (the module registry), `formats/` (one file a format), `eng_esp32.c` (the
  ESP32-S3 engine), `spi.c`, `fatfs/`.
- `src/rtl/sound/ym2149.v`: the two YM2149 instances used as Turbo Sound (tsconf model, mode 0).
- `docs/MODULES.md`: the player / format module / engine interfaces and how to add a format.
- `src/core.json`: the `osd` item `FL "SD1 MUSIC"` (the stock file browser on SD1).
- `docs/DESIGN.md`: the design notes, the pins, the ESP32 protocol and the limits.
- `docs/TESTING.md`: the board bring-up and the host tests.
- `src/sw/host/run_player_host.sh`: the screen on the host (a fake list) -> `host/out/frame.png`.
