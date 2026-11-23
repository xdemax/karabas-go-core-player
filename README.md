# Music Player для Karabas Mini G (VDAC32)

Плеер трекерных и чиптюн-модулей для **Karabas Mini G rev G/G2** с шилдом
**VDAC32** (ESP32-S3). Ядро — форк menu-ядра: RISC-V VexRiscv на 75 МГц,
фреймбуфер 800×600×8 в SRAM, блиттер/шрифты, клавиатура и джойстики через
MCU link v2 (`kg_mcu2`).

**MOD, XM, PT3, PT2 и STC играет сам FPGA**: микшеры MOD/XM на VexRiscv,
трекерные реплеи и два YM2149 в RTL для PT3/PT2/STC; звук — через PCM5102
или HDMI, без ESP32. **S3M** пока уходит на ESP32-S3. MP3 на FPGA нет.
**SD2** (слот FPGA) ядро читает само через SPI; **SD1** (карта RP2040) —
штатным file loader прошивки (`FL`-пункт, как в C64/NES/Sega). Прошивка
остаётся штатной.

![Экран (host preview)](docs/screenshot.png)

## Статус (2026-11-23)

- **MOD/XM** — FPGA-микшеры, 48 кГц stereo, до 32 каналов XM.
- **PT3/PT2/STC** — FPGA-реплеи, два YM2149 (Turbo Sound), 3/6 каналов,
  ФНЧ на выходе AY.
- **S3M** — через ESP32-S3 (VDAC32), не FPGA.
- **SD2** — SPI master в FPGA, SD SPI mode 0, FatFs R0.15; файлы до
  2.5 МБ играются из SRAM, большие — из SDRAM.
- **SD1** — штатный file loader прошивки (`FL`, слот 1), `Menu+F5`.
- **SDRAM** — 32 МБ MT48LC16M16A2, окно `0x10000000`.
- **LED-матрицы** — L/R-индикаторы через `audio_peaks` → `kg_mcu2` AUX.
- **Прошивка** — без изменений; только штатные сервисы.
- Ограничения: сборка только под Mini G; MP3 не поддерживается; S3M
  зависит от ESP32-прошивки.

## Документация

- `docs/DESIGN.md` — устройство, пины, протокол ESP32, ограничения.
- `docs/MODULES.md` — интерфейсы плеера/формата/движка, как добавить формат.
- `docs/TESTING.md` — bring-up на плате и host-тесты.
- `docs/UI_SKINS.md` — скины интерфейса.
- `docs/XM_FORMAT.md` — разбор XM.
- `docs/screenshot.png` — экран плеера.

## Сборка

Требования (как у menu-ядра):

```sh
git submodule update --init --recursive
```

- ISE 14.7 (`/opt/Xilinx/14.7/ISE_DS/settings64.sh`, `ISE_SETTINGS=`).
- xPack `riscv-none-elf-gcc` 15.2.0-1 в
  `~/opt/xpack-riscv-none-elf-gcc-15.2.0-1` (`XPACK=`).
- `karabas-go-modules` в `modules/` (сабмодуль); `src/make_core.py` уже в
  репозитории, отдельный `karabas-go-tools` не нужен.

```sh
make -C src/sw              # src/roms/player.bin — программа CPU (ROM .kg)
cd src && make minig        # битстрим + musicplayer.kg
make minig-report           # LUT/RAMB/timing
make minig-coremode         # второй timing-прогон Mini G
src/timing_ascii.sh minig   # trce из ASCII-каталога (кириллический путь ломает XML)
```

`musicplayer.kg` положить на SD1 (корень или `cores/`) и выбрать
**Music Player** в boot menu. Прошивка загрузит `roms/player.bin` в SRAM.

## Память и носители

- SRAM — быстрый буфер программы, фреймбуфера и модулей до 2.5 МБ.
- SDRAM — 32 МБ, окно `0x10000000`; большие MOD/XM (3–15 МБ) стримятся
  с SD2 и играются оттуда.
- SD2 — карта FPGA, SPI mode 0; pass-through `msc2` на время работы ядра
  выключен.
- SD1 — карта RP2040; доступ через file loader прошивки, без своего MCU.

## Файлы

| Файл | Роль |
|---|---|
| `src/rtl/karabas_minig_top.v` | верхняя обёртка Mini G: SPI, ESP32 CS, SD2 |
| `src/rtl/soc/menu_soc.v` | CPU, SRAM, видео, загрузчик, MMIO |
| `src/rtl/spi/spi_master.v` | байтовый SPI master (mode 0, 4 CS) |
| `src/rtl/soc/player_sdram.v` | контроллер SDRAM |
| `src/rtl/sound/ym2149.v` | два YM2149 (tsconf), Turbo Sound |
| `src/sw/main.c`, `ui.c` | GUI, файлы, меню |
| `src/sw/sd.c`, `diskio.c`, `fs.c` | SD2, FatFs |
| `src/sw/sd1.c` | файл SD1 из штатного loader |
| `src/sw/mod_fpga.c`, `xm_fpga.c` | MOD/XM парсеры и микшеры |
| `src/sw/ay_fpga.c`, `ay_players.cpp` | PT3/PT2/STC, YM2149 |
| `src/sw/fpga.c`, `fmt.c` | общий движок и реестр форматов |
| `src/sw/eng_esp32.c` | движок S3M через ESP32-S3 |
| `src/sw/audio.c` | FIFO 48 кГц stereo |
| `src/core.json` | `Volume`, `FL "SD1 MUSIC"` |
| `src/make_core.py` | сборка `.kg` |

## Управление

| Клавиша | Действие |
|---|---|
| `Up` / `Down` | выбор файла/строки |
| `Left` / `Right` | страница (как `PgUp` / `PgDn`) |
| `Home` / `End` | начало/конец списка |
| `Enter` | играть файл / войти в каталог |
| `Esc` | остановить воспроизведение |
| `Backspace` | на уровень выше |
| `Menu+F5` / `Menu+Esc` | file browser SD1 (`SD1 MUSIC`) |

Громкость — пункт `Volume` в OSD (Menu+Esc).
