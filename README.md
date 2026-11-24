# Music Player 2 — experimental hardware mixer

This is the separate local working repository for the next generation of the
Karabas music player: a **universal FPGA wavetable mixer** for sample-based
tracker formats (MOD, XM, IT, S3M).

The released player source stays in the neighbouring repository:

    ../karabas-go-core-player

That repository is kept at the last release (`v1.0b`) and must not be used for
the hardware-mixer experiments.

## Status

- Baseline imported from the experimental IT/fastmix state (`76e2cf8`) plus the
  local debug/fallback changes that used to be uncommitted.
- No release is made from this repository yet.
- The first task is the design review and an 8–16 voice TDM prototype.
- The target is 64 voices on Go and 32 voices on Mini/Mini G first.

## Documents

- [`docs/HW_MIXER_TZ.md`](docs/HW_MIXER_TZ.md) — main technical specification.
- [`docs/PERFORMANCE_FINDINGS.md`](docs/PERFORMANCE_FINDINGS.md) — profiling of the
  current software mixer and the FPU audit.
- [`docs/S3M_INTEGRATION.md`](docs/S3M_INTEGRATION.md) — the MikMod/S3M problem and
  possible integration paths.

## Constraints

- No external decoder chips.
- Preserve the existing MOD/XM/S3M/IT paths as fallback until hardware is tested.
- Preserve F12 reset, pause, volume, and the existing audio FIFO.
- Do not advertise MP3 in release notes.
- Do not make a release from this repository before hardware verification.
