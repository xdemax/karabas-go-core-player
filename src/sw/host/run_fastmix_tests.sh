#!/bin/sh
# Host tests + bench build. Usage: ./run_tests.sh [bench]
set -e
cd "$(dirname "$0")"
CFLAGS="-std=c99 -O2 -Wall -Wextra -fsanitize=address,undefined -g"
gcc $CFLAGS -D_GNU_SOURCE -o /tmp/test_fastmix test_fastmix.c ../fastmix.c -lm
/tmp/test_fastmix
if [ "$1" = bench ]; then
  gcc -std=c99 -O3 -D_POSIX_C_SOURCE=199309L -o /tmp/bench_fastmix bench_fastmix.c ../fastmix.c
  /tmp/bench_fastmix
fi
# target build check (if toolchain present):
# riscv32-unknown-elf-gcc -std=c99 -march=rv32im -mabi=ilp32 -O3 -c ../fastmix.c -o /tmp/fastmix.o && riscv32-unknown-elf-size /tmp/fastmix.o
