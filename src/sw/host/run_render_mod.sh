#!/bin/sh
# Render a MOD to a 48 kHz stereo WAV with the real mod_fpga.c mixer (host).
#   ./host/run_render_mod.sh song.mod out.wav [seconds]
set -e
cd "$(dirname "$0")/.."
cc -O2 -DHOST -I. -Ifatfs -o host/render_mod host/render_mod.c mod_fpga.c analysis.c
[ $# -ge 2 ] && ./host/render_mod "$1" "$2" "${3:-10}" || echo "usage: $0 song.mod out.wav [seconds]"
