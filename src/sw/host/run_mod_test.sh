#!/bin/sh
# the host test of the FPGA MOD player (mod_fpga.c): a tiny module, the parser and the mixer
set -e
cd "$(dirname "$0")/.."
cc -O1 -g -DHOST -Wall -Wno-unused-function -I. -Ifatfs -o host/mod_test \
   host/mod_test.c mod_fpga.c analysis.c src.c
./host/mod_test
