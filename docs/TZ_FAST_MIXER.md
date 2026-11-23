# ТЗ: быстрый микшер трекерных голосов и профилирование плеера

Это задание для отдельного исполнителя (Claude/другого coding agent). Нужен **только
микшер и оптимизация существующего воспроизведения трекерных модулей**. Новый
трекерный движок, сторонние декодеры и замена форматов в эту задачу не входят.

## 1. Зачем

На Karabas Go стоит VexRiscv `rv32imf` на 75 МГц с I-кэшем 16 КБ, D-кэшем 8 КБ
и single-precision FPU; софт собран под `rv32imf`/`ilp32f`.

**Важный результат:** переход с 4 КБ кэшей и soft-float на 16/8 КБ кэши и FPU
(локальная сборка `26112353`) **не изменил скорость трекерного воспроизведения**
на слух и по поведению `MIX`/`U`. Значит, дальнейшее улучшение надо искать не
в CPU/FPU/разгоне, а в микшере и профиле горячих функций.

Текущая сборка `26112353` играет:

- MOD/XM — собственными FPGA-плеерами (`mod_fpga.c`, `xm_fpga.c`);
- S3M/IT — MikMod (`mikmod_fpga.c`, `mikmod/playercode/virtch.c`);
- общий выход — `mixer.c` + FIFO 48 кГц.

Сейчас уже есть компромиссы:

- XM с числом каналов > 16 идёт по nearest-пути, потому что линейная интерполяция
  на 32 каналах не успевает;
- MikMod использует свой 32-битный микшер;
- наш MOD/XM-микшер и MikMod дублируют похожие внутренние циклы;
- неясно, сколько CPU тратится на сам микс, а сколько на разбор эффектов,
  конвертацию сэмплов, анализ спектра и UI.

Нужно сделать **один компактный fixed-point микшер** для трекерных голосов и
профилировать текущее воспроизведение, чтобы убрать лишнюю работу без потери
качества.

## 2. Железо и текущая система

- FPGA: Xilinx Spartan-6 `xc6slx25`, 75 МГц.
- CPU: VexRiscv, `rv32imf`, 75 МГц, I$ 16 КБ, D$ 8 КБ, FPU есть; прошивка собрана под `rv32imf`/`ilp32f`. SRAM общая с видео.
- Память: 3 МБ SRAM под код/кучу/стек; 32 МБ SDRAM, окно `0x10000000`.
- Аудио: стерео FIFO 8192 сэмпла, 48 кГц, регистр свободного места `0x74`.
- SRAM: `0x000000–0x300000`; фреймбуферы `0x300000/0x380000`.
- SDRAM: файлы модулей и сэмплы.
- Текущая сборка: `26112353` (локальная, FPU); Go/MiniG проверены, Mini добавлена в 1.0b.
- libxmp **не входит** в текущую сборку и в эту задачу не входит.
- MP3/другие декодеры **не входят** в эту задачу.

## 3. Границы задачи

Входит:

- профилирование текущих движков на target: cycles на блок/сэмпл/голос;
- переносимый C-модуль `src/sw/fastmix.c/.h`;
- fixed-point сведение 8/16-bit mono сэмплов;
- linear interpolation (обязательно), опционально 4-point cubic;
- forward и ping-pong loops;
- volume/pan/ramp без щелчков;
- 64 голоса; при перегрузе — пропуск/ограничение тихих голосов;
- блоковый API `fm_mix(dst, frames, voices, count)`;
- интеграция сначала в native MOD/XM, затем — в MikMod, если это не ломает
  replayer/эффекты;
- host-тесты, target-бенчмарк и отчёт по горячим точкам.

Не входит:

- загрузка модулей, парсинг эффектов, envelopes, NNA, IT-фильтры;
- замена replayer’а MikMod/libxmp;
- сторонние декодеры и новые форматы;
- UI, файловый браузер, SD-драйвер;
- изменения FPGA/CPU/частоты;
- ESP32 и внешние декодеры.

## 4. Что профилировать

До переписывания микшера нужен честный профиль текущей сборки `26112353`:

1. `mod_fpga.c`: `mix()`, парсинг эффектов, загрузка сэмплов.
2. `xm_fpga.c`: `mix()`, `sample_decode()`, envelopes, limiter >28 голосов.
3. `mikmod_fpga.c` + `virtch.c`: `VC_WriteBytes()`, `Mix32Stereo*`, `do_anticlick`,
   ramping.
4. `mixer.c`: low-pass + анализатор + FIFO на каждый выходной сэмпл.
5. `analysis.c` / UI: сколько реально стоит обновление панели и спектра.

Методика:

- `R_CYCLES` вокруг подозрительных участков;
- отдельные тесты: `Keijsig.it` (4 канала), `GRD_TanDan.it`, `an32xm` (32 канала),
  обычный MOD/XM;
- считать cycles/сэмпл на голос и cycles/блок 256 сэмплов;
- вывод: % CPU при 75 МГц и запас до underrun.

Отчёт профиля — **обязательная часть сдачи**. Без него оптимизация не принимается.

## 5. Формат голоса

Предлагаемый API (имена можно менять, смысл — нет):

```c
#define FM_MAX_VOICES 64

enum {
  FM_8BIT       = 1u << 0,
  FM_LOOP       = 1u << 1,
  FM_PINGPONG   = 1u << 2,
  FM_ACTIVE     = 1u << 3
};

typedef struct {
  const void *data;       // signed 8-bit or signed 16-bit, mono
  uint32_t length;        // frames
  uint32_t loop_start;
  uint32_t loop_end;      // exclusive

  uint32_t pos;           // 16.16
  uint32_t step;          // 16.16 source frames per output frame
  int32_t  volume;        // 0..256 Q8
  int32_t  pan;           // -128..127
  uint16_t ramp;          // frames left
  uint16_t ramp_len;
  uint8_t  flags;
  uint8_t  reserved[3];
} fm_voice_t;

void fm_reset(void);
void fm_mix(int16_t *dst, int frames, const fm_voice_t *voices, int nvoices);
```

Требования:

- `pos`/`step` 16.16; поддерживать downsampling и высокие ноты;
- `loop_end` исключающий; ping-pong без чтения за границами;
- volume/pan ramp при смене ноты/громкости;
- выход interleaved stereo int16, внутренняя сумма int32;
- saturate только на выходе.

## 6. Алгоритм и производительность

Обязательно:

- только целочисленная арифметика; `int64_t` — только вне inner loop;
- никаких `float`, `double`, `malloc`, `printf`, `memcpy` в audio-path;
- **channel-major** обход: локальные `pos/step/gain` в регистрах;
- границы loop/end вне inner loop; без `%` и ветвления на каждый сэмпл;
- linear interpolation:
  `s = s0 + ((s1 - s0) * frac) >> 16`;
- без указателя на функцию на каждый сэмпл;
- выбор 8/16-bit и linear делать один раз на блок/голос;
- не сканировать 64 неактивных голоса на каждый сэмпл;
- при >32 активных голосах — отбрасывать самые тихие, как в XM;
- не менять `mixer_write()`/low-pass/анализатор без отдельного замера.

Ориентиры только для микшера (75 МГц):

- 4 голоса, 48 кГц, linear, 16-bit: **< 10 млн циклов/с**;
- 16 голосов: **< 30 млн циклов/с**;
- 32 голоса: **< 55 млн циклов/с** с отбрасыванием тихих;
- полный `Keijsig.it`: `MIX ≈ 48 000`, `U` не растёт;
- полный `an32xm`: без underrun, `MIX` стабильно около 48 000;
- код микшера < 24 КБ, горячий цикл желательно < 2–3 КБ.

## 7. Интеграция

### 7.1 Native MOD/XM (первый этап)

- `mod_fpga.c` и `xm_fpga.c` остаются парсерами/реплеерами.
- Их `mix()` заменяется вызовом `fm_mix()`.
- Сохранить текущие `volume/pan`, `mix_skip`, envelopes, эффекты и порядок тиков.
- Для XM:
  - 32-канальный модуль должен идти **без underrun**;
  - linear interpolation для ≤16 каналов должна остаться;
  - для >16 каналов разрешён fallback, но он должен быть явно измерен;
  - если 16-bit samples для XM дают лучшее качество при приемлемой памяти/CPU —
    предложить это отдельным пунктом с цифрами.
- Для MOD: linear interpolation сохранить.

### 7.2 MikMod S3M/IT (второй этап)

- MikMod остаётся полным replayer’ом эффектов/загрузчиком.
- Допустимо:
  - заменить только inner-loop `virtch.c` на те же приёмы;
  - либо сделать adapter к `fm_mix()` без изменения эффектов.
- Нельзя переписывать replayer/эффекты и нельзя менять `md_mode` на HQ без замеров.
- Критерий: `Keijsig.it` без тормозов и деградации тона/арпеджио.

### 7.3 Общий выход

`mixer_write()` + low-pass + анализатор остаются общими. Если профиль покажет,
что они дорогие, предложить отдельный патч, но не смешивать с микшером.

## 8. Тесты и приёмка

Host-тесты:

1. синус 1 кГц, 8/16-bit;
2. linear interpolation против эталонного float-микшера **только в тесте**;
3. forward loop: бесшовно, без выхода за `loop_end`;
4. ping-pong loop: корректный разворот;
5. volume/pan ramp: без клика;
6. 64 голоса: нет переполнения/некорректного saturate;
7. блоки 1, 255, 256, 512 кадров;
8. A/B на `Keijsig.it`, `GRD_TanDan.it`, `an32xm`, MOD/XM.

Target-тесты:

- `R_CYCLES` вокруг `fm_mix()`;
- cycles/block, cycles/сэмпл, % CPU;
- `MIX`/`U` из UI;
- прослушивание: тон, арпеджио, щелчки, loop.

Критерии готовности:

- `Keijsig.it` стабильно, без плавающего тона;
- `an32xm` без underrun;
- 4/16 голосов укладываются в бюджет;
- A/B с `26112353` не хуже по качеству;
- нет регрессий MOD/XM/S3M/IT;
- FPU не требуется в audio-path; тяжёлых аллокаций там нет.

## 9. Что сдать

- `src/sw/fastmix.c`, `src/sw/fastmix.h`;
- `src/sw/host/test_fastmix.c` + скрипт;
- `src/sw/host/bench_fastmix.c`;
- патч интеграции в `mod_fpga.c`/`xm_fpga.c`;
- отдельный патч/инструкция для MikMod;
- отчёт профиля: горячие функции, циклы, % CPU, размер кода;
- список ограничений и того, что осталось за рамками;
- C99, без C++, без внешних зависимостей, сборка `-march=rv32im -O3`.

## 10. Стартовый prompt для исполнителя

> Read `docs/TZ_FAST_MIXER.md`, `src/sw/mod_fpga.c`, `src/sw/xm_fpga.c`,
> `src/sw/mikmod_fpga.c`, `src/sw/mikmod/playercode/virtch.c`, `src/sw/mixer.c`,
> `src/sw/analysis.c`, `src/sw/Makefile`, `src/sw/link.ld`.
> First measure the current build `26112353` on target as described in section 4.
> Then implement `fastmix` within the scope above and integrate it into the native
> MOD/XM players. Do not touch loaders, effects or replayers. After that, propose the
> MikMod adapter/inner-loop change with measurements. Do not add new formats or
> change the CPU.
