# HANDOFF — состояние на 25.11 (трекерный плеер Karabas Go)

## Что работает (стабильная сборка)
- `src/musicplayer.kg`, md5 `f13346326f9e8f1be921c1fb0a6fe9cf` — ALU выключен, 48 кГц на выходе.
- Коммит стабильной линии: `61946ec` (shared resampler + FIFO budget + nearest 8-bit fix).
- IT ≤16 каналов → 48k linear; IT >16 → 24k + nearest; XM ≤16 → 48k; XM >16 → 32k + nearest.
- Host-проверки: fastmix 3265/3265; En-trance и 16-канальный XM побитово совпадают с прежним 48k-рендером.

## Что в работе (ALU, ветка main, НЕ залито)
- `21aba55` — LERP разбит: `LERP1A` (funct7=11, setup), `LERP1B` (funct7=10, умножение), `LERP2` (8).
- ALU-тест на Verilator-модели CPU: **PASS** (SETF/GETF/LERP1A/LERP1B/LERP2/LERP8_1/LERP8_2/MAC/ADV/ADVB).
- Прошивка с `-DFM_CUSTOM_CPU=1` собирается; синтез split-ALU проходит по площади (2505 slices, 22 DSP), timing — проверяется.
- Предыдущий замер timing: путь `RS2 → h_p` 14.9 нс при 13.33 → разбит на `LERP1A/B`.

## Заморожено
- **S3M**: нативный реплеер молчит (неверный масштаб шага `s3m_note_to_step`, сырые паттерны). Не включать в релиз; вариант отката — MikMod.
- **FM_OPT_SAMPLE_CACHE** выключен.

## Следующие шаги
1. Синтез `LERP1A/B` → timing score. Если 0 — собрать `kg` с ALU.
2. A/B на железе: `MIX`/`U` на En-trance, TanDan, an32.xm против `f1334632`.
3. Если выигрыш <15% — ALU закрыть.
4. Кандидаты: `LOADPAIR`, D-cache 8K→16K, профиль `IT_PROFILE` (арифметика vs память).
5. S3M — только после оживления реплеера (потом на общий `resample.c`).

## Инструменты
- Заливка: `/home/demo/Загрузки/c64/karabas-go-core-c64/tools/karabas.py` (`msc`, `version`, `list`, `key:`), том `/dev/sdb1` → корень SD.
- CPU-генератор: `/home/demo/Загрузки/player/VexRiscv-src` (`runMain vexriscv.demo.GenKarabas`).
  **Важно:** `CustomMixAluPlugin.scala` правится в двух местах — в репозитории и в `VexRiscv-src`; без синхронизации генерация даёт старый CPU.
