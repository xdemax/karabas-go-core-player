#include <string.h>
#include "src.h"

int src_read(play_src_t *s, uint32_t off, void *buf, uint32_t n, UINT *got) {
  *got = 0;
  if (s->mem) {
    memcpy(buf, s->mem + off, n);
    *got = n;
    return 0;
  }
  if (!s->f) return -1;
  if (f_lseek(s->f, off) != FR_OK) return -1;
  return f_read(s->f, buf, n, got) == FR_OK ? 0 : -1;
}
