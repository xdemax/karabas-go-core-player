#!/bin/sh
# the host test of the SD SPI driver (sd.c) against a card model on host/test.img
set -e
cd "$(dirname "$0")/.."
python3 host/mkimg.py host/test.img
cc -O1 -g -DHOST -Wall -Wno-unused-function -I. -o host/sd_test host/sd_test.c sd.c
cd host && ./sd_test test.img
