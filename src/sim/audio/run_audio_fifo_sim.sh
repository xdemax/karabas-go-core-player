#!/bin/sh
# Unit test of the player's audio FIFO count/full/free (the old 12-bit bug against the 11-bit fix).
set -e
cd "$(dirname "$0")"
verilator --binary --timing -Wno-fatal -Wno-WIDTH -Wno-UNUSED -Wno-DECLFILENAME \
  --top-module tb_audio_fifo -Mdir obj_fifo tb_audio_fifo.v > build_fifo.log 2>&1 || { tail -20 build_fifo.log; exit 1; }
./obj_fifo/Vtb_audio_fifo
