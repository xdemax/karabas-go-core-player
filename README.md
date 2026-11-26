# Music Player для Karabas Go / Mini / Mini G

Плеер трекерных, чиптюн- и SID-модулей для семейства **Karabas Go, Mini rev A–F
и Mini G/G2**. Ядро — форк menu-ядра: RISC-V VexRiscv на 75 МГц (I-кэш 16 КБ, D-кэш 8 КБ, FPU),
фреймбуфер 800×600×8 в SRAM, блиттер/шрифты, клавиатура и джойстики через
MCU link v2 (`kg_mcu2`). Звук — через PCM5102 или HDMI.

**Форматы:**

| Группа | Форматы | Движок |
|---|---|---|
| SID | `sid` | SID 8580 в FPGA + 6502 в прошивке (PSID/RSID) |
| Scream Tracker | `stm`, `s3m` | нативный FPGA-плеер |
| ProTracker | `mod` | нативный FPGA-плеер |
| FastTracker | `xm` | нативный FPGA-плеер |
| Impulse Tracker | `it` | нативный FPGA-плеер |
| AY8912 | `pt2`, `pt3`, `stc`, `pt1`, `asc`, `psc`, `sqt`, `stp`, `vtx`, `psg` | FPGA-реплеи, два YM2149 (Turbo Sound) |
| Прочее | `mp3` | экспериментальный Helix, скрыт из титульного списка |

**SD2** (слот FPGA) ядро читает само через SPI; **SD1** (карта RP2040) — штатным
file loader прошивки (`FL`-пункт, как в C64/NES/Sega). Прошивка остаётся штатной.

Ядро собирается для **Go**, **Mini** и **Mini G**; проверка идёт на **Mini G**,
**Go** — удобная тестовая плата. Битстрим Mini включён в 1.1b, но на железе ещё не проверен.

![Экран (host preview)](docs/screenshot.png)

## Статус (release 1.1b)

- **SID** — SID 8580 (ядро MiSTer) в FPGA, CE 1 МГц, фильтр и гейн в RTL.
  6502 (PSID v1–v4) работает в прошивке; RSID — частично (свой IRQ-вектор `$0314`).
  Частота CIA берётся из latch таймера, поддерживаются PAL/NTSC. Для несовместимых
  файлов в панели плеера выводится предупреждение (RSID, 6581, digi через `$D418`).
  Петля трека определяется по потоку регистров; иначе используется оценка 3 минуты.
- **MOD/XM/STM/S3M/IT** — нативные FPGA-плееры, 48 кГц stereo; сэмплы в SDRAM.
- **AY** — PT3/PT2/STC и остальные AY-форматы через libayfly, два YM2149.
  Звук AY идёт из RTL в общий выход, громкость общая.
- **SID/CHANNELS** — индикаторы каналов SID считают модель ADSR по регистрам.
- **Анализатор** — три вида по клавише `A`: LED-сегменты, сплошные столбики,
  зеркальный; общий стиль — зелёный/жёлтый/красный по уровню.
- **UI** — группы форматов на стартовом экране, `Backspace` возвращается в каталог,
  из которого вышли, пауза и громкость управляют FPGA-гейном.
- **SD2** — SPI master в FPGA, SD SPI mode 0, FatFs R0.15; музыка грузится
  прямо в 32 МБ SDRAM.
- **Платы** — в 1.1b включены **Go**, **Mini** и **Mini G**; проверены Go и Mini G,
  Mini ждёт проверки на железе.
- **MP3** — экспериментальный программный Helix; на 75 МГц возможны пропуски,
  формат скрыт из титульного списка, но выбирается в браузере.
- Ограничения: RSID и digi через `$D418` воспроизводятся не полностью;
  MUS не поддерживается.

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
- SDRAM — 32 МБ, окно `0x10000000`; модули и сэмплы всех форматов.
- SD2 — карта FPGA, SPI mode 0; pass-through `msc2` на время работы ядра
  выключен.
- SD1 — карта RP2040; доступ через file loader прошивки, без своего MCU.

## Файлы

| Файл | Роль |
|---|---|
| `src/rtl/karabas_go_top.v` | верхняя обёртка Go: VGA, SPI/SD2, SDRAM, PCM5102 |
| `src/rtl/karabas_minig_top.v` | верхняя обёртка Mini G: HDMI, SPI, SD2, обвязка |
| `src/rtl/soc/menu_soc.v` | CPU, SRAM, видео, загрузчик, MMIO, SID/AY |
| `src/rtl/sound/sid_mister8580/` | SID 8580 (Verilog-порт ядра MiSTer) |
| `src/rtl/sound/ym2149.v` | два YM2149 (tsconf), Turbo Sound |
| `src/rtl/spi/spi_master.v` | байтовый SPI master (mode 0, 4 CS) |
| `src/rtl/soc/player_sdram.v` | контроллер SDRAM |
| `src/sw/sid_fpga.c`, `sid_cpu.c` | SID-плеер: PSID/RSID, 6502, темп, метрики |
| `src/sw/mod_fpga.c`, `xm_fpga.c`, `stm_fpga.c`, `s3m_fpga.c`, `it_fpga.c` | нативные FPGA-плееры |
| `src/sw/sid_cpu.c` | эмулятор NMOS 6502 (включая нелегальные опкоды и BCD) |
| `src/sw/ay_fpga.c`, `ay_players.cpp` | AY-форматы, YM2149 |
| `src/sw/mp3_fpga.c`, `src/sw/helix` | Helix: экспериментальный MP3 |
| `src/sw/fpga.c`, `fmt.c` | общий движок и реестр форматов |
| `src/sw/audio.c`, `mixer.c` | FIFO 48 кГц stereo и общий микшер |
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
| `Space` | пауза |
| `A` | вид анализатора (LED / solid / mirror) |
| `Shift+=` / `Shift+-` | размер шрифта списка файлов (8 шагов) |
| `-` / `=` | громкость |
| `Backspace` | на уровень выше (с выделением каталога, из которого вышли) |
| `Menu+F5` / `Menu+Esc` | SD1 временно отключён |

Громкость — также пункт `Volume` в OSD (Menu+Esc).
