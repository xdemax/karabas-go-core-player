// Where the bytes of a module come from: a FatFs file on SD2, or the SRAM buffer the stock file
// loader filled from SD1. The format modules only see this.
#pragma once
#include <stdint.h>
#include "ff.h"

typedef struct {
  FIL *f;               // a file on SD2, or
  const uint8_t *mem;   // the bytes in the SRAM (SD1)
} play_src_t;

// n bytes at off into buf; *got the count done; 0 ok
int src_read(play_src_t *s, uint32_t off, void *buf, uint32_t n, UINT *got);
