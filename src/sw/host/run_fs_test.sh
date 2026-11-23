#!/bin/sh
# the host test of the FatFs layer (fs.c / diskio.c) on a FAT16 image (mkimg.py -> test.img)
set -e
cd "$(dirname "$0")/.."
python3 host/mkimg.py host/test.img
cc -O1 -g -DHOST -Wall -Wno-unused-function -I. -Ifatfs -o host/fs_test \
   host/fs_test.c fs.c diskio.c fatfs/ff.c fatfs/ffunicode.c
cd host && ./fs_test test.img
