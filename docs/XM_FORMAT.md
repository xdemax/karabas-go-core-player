# XM (FastTracker II) — рабочие заметки

Источник: https://github.com/lclevy/unmo3/blob/master/spec/xm.txt

## Header

| offset | size | field |
|---|---|---|
| 0 | 17 | `Extended Module: ` |
| 17 | 20 | module name |
| 37 | 1 | 0x1A |
| 38 | 20 | tracker name |
| 58 | 2 | version, LE |
| 60 | 4 | header size (276), LE |
| 64 | 2 | song length |
| 66 | 2 | restart |
| 68 | 2 | channels (2..32) |
| 70 | 2 | patterns (max 256) |
| 72 | 2 | instruments (max 128) |
| 74 | 2 | flags: bit0 0=Amiga, 1=linear |
| 76 | 2 | default tempo (speed, ticks/row) |
| 78 | 2 | default BPM |
| 80 | 256 | order table |

Patterns start at `60 + header_size`.

## Pattern

```
+0 4  pattern header length (9)
+4 1  packing (0)
+5 2  rows (1..256)
+7 2  packed data size
+9 ?  packed rows x channels
```

A row entry is 5 bytes (note, instrument, volume, effect, param). If the first byte has bit 7,
it is a mask (bit0 note, bit1 instrument, bit2 volume, bit3 effect, bit4 param); only the present
bytes follow. Note 1..96 (1=C-0), 97=key off. Packed size 0 = empty pattern (use $80 entries).

## Instrument

```
+0  4   instrument size
+4  22  name
+26 1   type (0)
+27 2   number of samples
+29 4   sample header size (40)
+33 96  keymap (sample number for notes 1..96)
+129 48 volume envelope points (12 x word x,y)
+177 48 panning envelope points
+225 1  number of volume points
+226 1  number of panning points
+227 1  volume sustain / +228 loop start / +229 loop end
+230 1  panning sustain / +231 loop start / +232 loop end
+233 1  volume type (bit0 on, bit1 sustain, bit2 loop)
+234 1  panning type
+235 1  vibrato type / +236 sweep / +237 depth / +238 rate
+239 2  fadeout
```

Sample headers follow the instrument header; sample data follows all sample headers.

## Sample header

```
+0  4 length (bytes)        +4  4 loop start (bytes)   +8  4 loop length (bytes)
+12 1 volume                +13 1 finetune (signed -128..127)
+14 1 type: bits0..1 0 none/1 forward/2 ping-pong, bit4 16-bit
+15 1 panning (0..255)      +16 1 relative note (signed)
+17 1 reserved              +18 22 name
```

Samples are **delta encoded**: `new = old + delta` (also 16-bit, little-endian).

## Pitch

- PatternNote 1..96, RealNote = PatternNote + RelativeTone (0..118, 48=C-4=8363 Hz).
- Linear: `Period = 10*12*16*4 - Note*16*4 - FineTune/2`,
  `Freq = 8363 * 2^((6*12*16*4 - Period)/(12*16*4))`.
- Amiga: interpolate the 96-entry period table (12*8) by FineTune/16; `Freq = 8363*1712/Period`.
- Step (16.16) = Freq * 65536 / 48000.

## Effects (first version)

0 arp, 1/2 porta, 3 tone porta, 4 vibrato, 5/6, 8 pan, 9 offset, A volume slide,
B position jump, C volume, D break, E1/E2/E6/E9/EA/EB/EC/ED/EE, F speed/BPM,
G global volume, H global slide, P panning slide.
Volume column: $10..$50 volume, $60..$9F slides, $A0..$BF vibrato, $C0..$CF pan,
$D0..$EF panning slide, $F0..$FF tone porta.

## Реализация в FPGA (xm_fpga.c)

Разбор: заголовок, порядок, упакованные паттерны, инструменты 241/263 байта, до 256 сэмплов,
delta-сэмплы 8/16 бит, forward loop. Банк сэмплов декодируется на месте прямо в буфере модуля
(SD2 до 2.5 МБ — в SRAM с `0x080000`, большие SD2-файлы — в 32 МБ SDRAM с `0x10000000`, SD1 —
в памяти загрузчика).

Проигрывание: 32 канала, 48 кГц стерео, линейная частота (для Amiga-модулей нота считается по той
же показательной формуле), относительная нота и finetune, громкость/панорама, volume envelope с
sustain/loop и fadeout, эффекты 0 (arpeggio), 1/2/3/5, 4/6, 8, 9, A, B, C, D, F, E6; параметр
портаменто — полный байт (0..255) с памятью скорости; volume column $10..$FF (кроме tone porta
$F0). Громкость канала пересчитывается раз в тик, в микшере — только умножение; для широких
модулей (>8 каналов) есть запас +6 дБ.

Проверка: `src/sw/host/run_render_xm.sh song.xm out.wav` рендерит WAV тем же кодом; на
`tPORt - WavePad 3.0kg.XM` огибающая совпадает с libopenmpt (corr 0.998), пик/RSM в пределах
0.2 дБ. `src/sw/host/run_xm_test.sh` — синтетический модуль (парсер, delta, loop, pause, volume).

Пока нет: pattern loop (E6x), global volume (Gxx/Hxx), pan envelope, ping-pong loop (играется
вперёд), точная Amiga-таблица периодов для эффектов 1/2/3/4/5 (используются показательные
множители), volume column $F0 (tone porta). Они не мешают основным трекам, но это следующие шаги.
