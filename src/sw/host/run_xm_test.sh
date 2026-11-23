#!/bin/sh
# the host test of the FPGA XM player (xm_fpga.c): parser, delta samples, loop, mixer, pause, volume
set -e
cd "$(dirname "$0")/.."
cc -O1 -g -DHOST -Wall -Wno-unused-function -I. -Ifatfs -o host/xm_test host/xm_test.c xm_fpga.c analysis.c
./host/xm_test
