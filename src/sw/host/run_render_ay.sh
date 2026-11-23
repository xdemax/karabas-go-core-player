#!/bin/sh
# Render PT3 / PT2 / STC with the FPGA AY engine (host software model) to a 48 kHz stereo WAV.
set -e
cd "$(dirname "$0")/.."
tmp="${TMPDIR:-/tmp}/karabas-ay-render.$$"
trap 'rm -f "$tmp"-*.o' EXIT HUP INT TERM
cc -O2 -DHOST -I. -Ifatfs -c ay_fpga.c -o "$tmp-ay.o"
cc -O2 -DHOST -I. -Ifatfs -c analysis.c -o "$tmp-analysis.o"
cc -O2 -DHOST -I. -Ifatfs -c host/render_ay.c -o "$tmp-main.o"
c++ -O2 -DHOST -I. -Ifatfs -fno-exceptions -fno-rtti -c ay_players.cpp -o "$tmp-players.o"
c++ -O2 -DHOST -I. -Ifatfs -fno-exceptions -fno-rtti -c ay/ay_engine.cpp -o "$tmp-engine.o"
c++ "$tmp-main.o" "$tmp-ay.o" "$tmp-analysis.o" "$tmp-players.o" "$tmp-engine.o" -o host/render_ay
[ $# -ge 2 ] && ./host/render_ay "$1" "$2" "${3:-10}" || echo "usage: $0 song.pt3|pt2|stc out.wav [seconds]"
