#!/bin/sh
# Render an S3M to a 48 kHz stereo WAV with the real s3m_fpga.c mixer (host).
#   ./host/run_render_s3m.sh song.s3m out.wav [seconds]
set -e
cd "$(dirname "$0")/.."
cc -O2 -DHOST -I. -Ifatfs -o host/render_s3m host/render_s3m.c s3m_fpga.c fastmix.c analysis.c mixer.c
[ $# -ge 2 ] && ./host/render_s3m "$1" "$2" "${3:-10}" || echo "usage: $0 song.s3m out.wav [seconds]"
