# HANDOFF — состояние на 25.11 (трекерный плеер Karabas Go)

## Что работает (стабильная сборка)
- `src/musicplayer.kg`, md5 `f13346326f9e8f1be921c1fb0a6fe9cf` — ALU выключен, 48 кГц на выходе.
- Коммит стабильной линии: `61946ec` (shared resampler + FIFO budget + nearest 8-bit fix).
- IT ≤16 каналов → 48k linear; IT >16 → 24k + nearest; XM ≤16 → 48k; XM >16 → 32k + nearest.
- Host-проверки: fastmix 3265/3265; En-trance и 16-канальный XM побитово совпадают с прежним 48k-рендером.

## Что в работе (ALU, ветка main, НЕ залито)
- `21aba55` — LERP разбит: `LERP1A` (funct7=11, setup), `LERP1B` (funct7=10, умножение), `LERP2` (8).
- ALU-тест на Verilator-модели CPU: **PASS** (SETF/GETF/LERP1A/LERP1B/LERP2/LERP8_1/LERP8_2/MAC/ADV/ADVB).
- **Timing закрыт: score 0.** Последним был путь `h_p8` (-0.603 нс): умножение собиралось каскадом из двух DSP48.
  Фикс без новой инструкции — 18-битные операнды `(ad.resize(18) * h_frac.resize(18)).resize(32)` → один DSP48A1.
- Площадь: 2510 slices (66%), 20 DSP (52%). Побитовость: En-trance IT и 16-канальный XM совпадают с эталоном.
- `kg` с ALU собран: `5912e3210e0d956e658c4a5905984bf0` (НЕ залит, плата отключена).

## Заморожено
- **S3M**: нативный реплеер молчит (неверный масштаб шага `s3m_note_to_step`, сырые паттерны). Не включать в релиз; вариант отката — MikMod.
- **FM_OPT_SAMPLE_CACHE** выключен.

## Следующие шаги
1. Залить `5912e321` и сделать A/B на железе: `MIX`/`U` на En-trance, TanDan, an32.xm против `f1334632`
   (критерий: >=15% и без хрипа, иначе ALU закрываем).
2. Если хрип вернётся — проверить, что все 8-битные линейные голоса идут через LERP8_1/2, и повторить ALU-тест CPU.
3. Если выигрыш <15% — ALU закрыть.
4. Кандидаты: `LOADPAIR`, D-cache 8K→16K, профиль `IT_PROFILE` (арифметика vs память).
5. S3M — только после оживления реплеера (потом на общий `resample.c`).

## Инструменты
- Заливка: `/home/demo/Загрузки/c64/karabas-go-core-c64/tools/karabas.py` (`msc`, `version`, `list`, `key:`), том `/dev/sdb1` → корень SD.
- CPU-генератор: `/home/demo/Загрузки/player/VexRiscv-src` (`runMain vexriscv.demo.GenKarabas`).
  **Важно:** `CustomMixAluPlugin.scala` правится в двух местах — в репозитории и в `VexRiscv-src`; без синхронизации генерация даёт старый CPU.
