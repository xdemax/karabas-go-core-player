# Music Player для Karabas Go / Mini / Mini G

Плеер трекерных и чиптюн-модулей для семейства **Karabas Go, Mini rev A–F
и Mini G/G2**. Ядро — форк menu-ядра: RISC-V VexRiscv на 75 МГц,
фреймбуфер 800×600×8 в SRAM, блиттер/шрифты, клавиатура и джойстики через
MCU link v2 (`kg_mcu2`).

**MOD/XM — FPGA-плееры, S3M/IT — MikMod, MP3 — экспериментальный Helix; PT3/PT2/STC — FPGA-реплеи**:
трекерные реплеи и два YM2149 в RTL для PT3/PT2/STC; звук — через PCM5102
или HDMI. **SD2** (слот FPGA) ядро читает само через SPI; **SD1** (карта
RP2040) — штатным file loader прошивки (`FL`-пункт, как в C64/NES/Sega).
Прошивка остаётся штатной.

Ядро собирается для **Go**, **Mini** и **Mini G**; проверка идёт на **Mini G**,
**Go** — удобная тестовая плата. Битстрим Mini включён в 1.0b, но на железе ещё не проверен.

![Экран (host preview)](docs/screenshot.png)

## Статус (2026-11-23, release 1.0b)

- **MOD/XM** — FPGA-микшеры, 48 кГц stereo; 32-канальный XM идёт по быстрому пути.
- **S3M/IT** — MikMod, 48 кГц stereo, сэмплы в SDRAM.
- **PT3/PT2/STC** — FPGA-реплеи, два YM2149 (Turbo Sound), 3/6 каналов,
  ФНЧ на выходе AY.
- **SD2** — SPI master в FPGA, SD SPI mode 0, FatFs R0.15; музыка грузится
  прямо в 32 МБ SDRAM.
- **SD1** — поддержка временно отключена.
- **SDRAM** — 32 МБ MT48LC16M16A2, окно `0x10000000`.
- **LED-матрицы** — L/R-индикаторы через `audio_peaks` → `kg_mcu2` AUX
  (на Mini G; на Go не используются).
- **Прошивка** — без изменений; только штатные сервисы.
- **Платы** — в 1.0b включены **Go**, **Mini** и **Mini G**; проверены Go и Mini G,
  Mini ждёт проверки на железе.
- **MP3** — экспериментальный программный Helix; на 75 МГц возможны пропуски, формат скрыт из титульного списка, но выбирается в браузере.
- **Титульный экран** показывает только стабильные форматы; IT/S3M/MP3 из него убраны и остаются доступны в списке файлов.
- Ограничения: MP3 в 1.0b не гарантирует real-time на всех битрейтах.

## Документация

- `docs/DESIGN.md` — устройство, пины, SD2/SDRAM, ограничения.
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
cd src && make go           # битстрим Go + musicplayer.kg
make go-report              # LUT/RAMB/timing Go
make mini                   # битстрим Mini (добавляет его в musicplayer.kg)
make minig                  # битстрим Mini G (добавляет его в musicplayer.kg)
make minig-report           # LUT/RAMB/timing Mini G
make minig-coremode         # второй timing-прогон Mini G
src/timing_ascii.sh go      # trce из ASCII-каталога (кириллический путь ломает XML)
```

`make all` собирает все три битстрима и кладёт их в один `musicplayer.kg`.
Положить его на SD1 (корень или `cores/`) и выбрать **Music Player** в
boot menu. Прошивка загрузит `roms/player.bin` в SRAM.

## Память и носители

- SRAM — программа, heap/stack и фреймбуферы; музыка — в SDRAM.
- SDRAM — 32 МБ, окно `0x10000000`; модули и сэмплы MOD/XM/S3M/IT/MP3.
- SD2 — карта FPGA, SPI mode 0; pass-through `msc2` на время работы ядра
  выключен.
- SD1 — карта RP2040; доступ через file loader прошивки, без своего MCU.

## Файлы

| Файл | Роль |
|---|---|
| `src/rtl/karabas_go_top.v` | верхняя обёртка Go: VGA, SPI/SD2, SDRAM, PCM5102 |
| `src/rtl/karabas_minig_top.v` | верхняя обёртка Mini G: HDMI, SPI, SD2, обвязка |
| `src/rtl/soc/menu_soc.v` | CPU, SRAM, видео, загрузчик, MMIO |
| `src/rtl/spi/spi_master.v` | байтовый SPI master (mode 0, 4 CS) |
| `src/rtl/soc/player_sdram.v` | контроллер SDRAM |
| `src/rtl/sound/ym2149.v` | два YM2149 (tsconf), Turbo Sound |
| `src/sw/main.c`, `ui.c` | GUI, файлы, меню |
| `src/sw/sd.c`, `diskio.c`, `fs.c` | SD2, FatFs |
| `src/sw/sd1.c` | файл SD1 из штатного loader |
| `src/sw/mod_fpga.c`, `xm_fpga.c` | FPGA-плееры MOD/XM |
| `src/sw/mikmod_fpga.c`, `src/sw/mikmod` | MikMod: S3M/IT |
| `src/sw/mp3_fpga.c`, `src/sw/helix` | Helix: экспериментальный MP3 |
| `src/sw/xmp_fpga.c`, `src/sw/libxmp` | libxmp (не входит в текущую сборку) |
| `src/sw/ay_fpga.c`, `ay_players.cpp` | PT3/PT2/STC, YM2149 |
| `src/sw/fpga.c`, `fmt.c` | общий движок и реестр форматов |
| `src/sw/audio.c` | FIFO 48 кГц stereo |
| `src/core.json` | `Volume` (SD1 item отключён) |
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
| `Menu+F5` / `Menu+Esc` | SD1 временно отключён |

Громкость — пункт `Volume` в OSD (Menu+Esc).
