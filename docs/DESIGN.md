# Design notes

## The board and the pins (Mini G, rev.G / G2)

The core is built for the Mini G only for now (`core.json` has the `minig` board). It is the menu
core's Mini G top with the SPI bus of the VDAC32 shield taken from the MCU pass-through:

| Signal | FPGA pin | In the core |
|---|---|---|
| `ESP32_SPI_CS_N` | K5 (`UBUS_26`) | CS 3 of the SPI master |
| `FT_SPI_SCK` / `FT_SPI_MOSI` / `FT_SPI_MISO` | E11 / B15 / E12 | the shared bus of the FT812 and the ESP32 (the SPI master; the FT812 stays deselected) |
| `FT_SPI_CS_N` | F12 | CS 1 of the SPI master (always high while the player runs; the firmware may take the bus while `FT_SPI_ON`) |
| `SD_CS_N` / `SD_DI` / `SD_DO` / `SD_CLK` | L4 / L7 / M7 / L8 | the SD2 slot: CS 0 and the same SPI master |
| `ESP32_PCM_*` | R2 / R1 / K6 | not used: the module's I2S goes outside the FPGA |

The MCU's SD2 pass-through is disabled (`ENABLE_SD2(0)`): the card's clock, MOSI and CS come from the
SPI master, not from `MCU_SCK`. The MCU keeps its own SPI link (`MCU_*`), the HID/OSD/loader and the
RPC mailbox. This is the TS-Conf way: the core owns the FPGA card; the firmware is not involved.

SD1 (the card of the RP2040) is not wired to the FPGA. The core reaches it through the **stock file
loader** of the firmware (the same `FL` item the C64 / NES / Sega cores use): no firmware change,
no service of our own.

## SPI master (`rtl/spi/spi_master.v`)

Mode 0, MSB first, one byte a write, CS held by a register so a command or a block has no gaps that
matter. SCK = 75 MHz / (2 x (DIV + 1)): 12.5 MHz (`DIV` 2) for the data, 399 kHz (`DIV` 93) for the
SD init. The byte received with the last written one is in `rdat`; `busy` is low when it is valid.

MMIO (`menu_soc`, uncached at 0x80000000):

| Offset | Register |
|---|---|
| 0x50 | W: the byte to send (starts a transfer); R: bit 8 busy, 7..0 the answer |
| 0x54 | W/R: CS, active low: bit 0 SD2, 1 FT812, 2 spare, 3 ESP32 |
| 0x58 | W/R: the half period of SCK in clocks |

## SD2

SD SPI mode 0: CMD0, CMD8 (v2), ACMD41 with HCS, CMD58 (CCS), CMD16 for the byte-addressed cards;
CMD17 / CMD18 for reads (CMD12 ends a multi-block read). FatFs R0.15 (`src/sw/fatfs`, LFN UTF-8,
one volume) with `src/sw/diskio.c`: drive 0 is SD2. The player is read only. `GET_SECTOR_COUNT` is
not implemented (the card's capacity is not read; nothing needs it).

## SD1: the stock file loader (`FL` item)

`src/core.json` has one OSD item:

```json
{"type": "FL", "name": "SD1 MUSIC", "slot": 1, "extensions": ".mod,.xm,.s3m",
 "dir": "/mods", "hotkey": "Menu+F5"}
```

Menu+F5 (or the OSD, Menu+Esc, then the item) opens the **firmware's** file browser on SD1. The
selected file is sent to the core by the stock file loader: `LINK_LOAD_IOCTL` (LOAD **kind 2**) with
the real size and the extension in the START, then the bytes with `ld_addr` = the offset from 0, then
END. This is the C64 core's `file_slot` / `ld_kind == 2` path.

The SoC (`menu_soc`):

- kind 0 (the ROMs of the core) holds the CPU and writes at the ROM address, as before;
- kind 1 / 2 (the file loader / FL item) writes at `0x080000 + offset` (the buffer up to 0x280000,
  2.5 MB) **while the CPU keeps running**: the SRAM arbiter gives the loader its bytes and the CPU
  what is left; the CPU is not reset (a reset would restart the program — the C64 core does the
  same, it writes the file into the running machine);
- `0x60` R: bit 0 ready, bit 1 too big, bits 3..2 the kind; `0x64` R: the size; `0x6c` R: the
  extension (`"mod"`, `"xm"`, `"s3m"` as ASCII); `0x68` W: acknowledge.

The program (`player.c`, `check_sd1`) polls 0x60, takes the format from the extension (or the
signature if there is none), and streams the file from `0x080000` to the ESP32 (`esp_play_mem`).
The file name is not sent by the loader: the firmware's browser shows it, the core shows the size.

## ESP32-S3 protocol

The module is an SPI slave (ESP-IDF `spi_slave_hd`, mode 0, 8-bit command / address / dummy), the
shared registers at 0..63 and the DMA channels. The commands used:

| Command | Code | Use |
|---|---|---|
| `WRBUF` | 0x01 | write registers from an address |
| `RDBUF` | 0x02 | read registers |
| `WRDMA` | 0x03 | a chunk to the module (the header is `03 00 FF`) |
| `WR_DONE` | 0x07 | end of the chunk (one byte) |
| `GET_VER` | 0x02 | API and version |
| `MOD_STREAM_LOAD` / `XM` / `S3M` | 0xAC / 0xAB / 0xAD | a stream chunk: `DATA_SIZE` (3..6) and `DATA_OFFSET` (7..10) |
| `TRACK_PLAY` | 0xA1 | play the handle (0x0B) |
| `TRACK_STOP` | 0xA2 | stop |
| `DELETE_OBJECT` | 0xE3 | free the module's PSRAM |

A command is executed when CS is released; the registers from 0 are written in one WRBUF:
`cmd, 0 (STATUS reset), params...`.

Stream load of a 4 KB chunk (the firmware's `tracker_stream_*`):

1. First: `DATA_SIZE` = min(4096, (size + 3) & ~3), `DATA_OFFSET` = the file size. Every next one:
   `DATA_OFFSET` = 0 and `DATA_SIZE` = the same chunk size (or the rounded remainder for the last).
2. The module sets `STATUS = DATA_M2S`; the core reads the requested offset / size from the
   registers (3..10), reads that many bytes of the file (padding the tail to 4 bytes), sends them by
   `WRDMA` and ends with `WR_DONE`.
3. `STATUS = READY` means the next chunk; after the last one `OBJ_HANDLE` has the tracker context.
4. `TRACK_PLAY` with the handle starts it; `STATUS = READY` when the player is running.

The module has no volume / pause / position commands in this firmware (`+++` in its `cmd` list), so
the core has only load / play / stop / delete. The previous module is stopped and deleted before a
new one is streamed (the PSRAM is limited).

## Limits

- Mini G only (the Go and Mini tops are still in the tree, not built, and the ESP32 pins are the Mini
  G shield's).
- SD2 is read by the core; SD1 only through the stock `FL` file loader (the firmware's browser),
  one file at a time, up to 2.5 MB (the SRAM file buffer), no file name in the core.
- Read only, `.mod` / `.xm` / `.s3m` only, no subdirectory shortcuts or settings.
- The module streams at ~12.5 Mbit/s: a 1 MB module takes about a second plus the card read and the
  module's parse (the last chunk can take a few seconds on a big module).
- The progress bar is drawn between chunks; Esc is not read during the transfer (a future BREAK).
