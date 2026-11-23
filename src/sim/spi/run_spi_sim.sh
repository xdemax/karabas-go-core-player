#!/bin/sh
# tb_spi (Verilator): the SPI master (rtl/spi/spi_master.v) against a slave model: CS, a byte out
# and in at 12.5 and 0.4 MHz. Prints "tb_spi: 0 errors".
set -e
cd "$(dirname "$0")"
verilator --binary --timing -Wno-fatal -Wno-WIDTH -Wno-UNUSED -Wno-CASEINCOMPLETE -Wno-DECLFILENAME \
  --top-module tb_spi -Mdir obj_spi tb_spi.v ../../rtl/spi/spi_master.v > build.log 2>&1 || { tail -20 build.log; exit 1; }
./obj_spi/Vtb_spi
