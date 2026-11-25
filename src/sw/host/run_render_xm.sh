#!/bin/sh
# Render an XM to a 48 kHz stereo WAV with the real xm_fpga.c mixer (host).
#   ./host/run_render_xm.sh song.xm out.wav [seconds]
set -e
cd "$(dirname "$0")/.."
cc -O2 -DHOST -I. -Ifatfs -o host/render_xm host/render_xm.c xm_fpga.c fastmix.c resample.c analysis.c mixer.c
[ $# -ge 2 ] && ./host/render_xm "$1" "$2" "${3:-10}" || echo "usage: $0 song.xm out.wav [seconds]"
