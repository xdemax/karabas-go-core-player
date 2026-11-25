#include "plog.h"
#include "ff.h"
#include <string.h>

#define PLOG_BUF 2048

static FIL  pf;
static char pbuf[PLOG_BUF];
static int  pn;
static int  popen;

int plog_open(const char *path) {
  plog_close();
  pn = 0;
  if (f_open(&pf, path, FA_WRITE | FA_OPEN_ALWAYS) != FR_OK) return -1;
  f_lseek(&pf, f_size(&pf));          /* append: one log per boot, all tracks */
  popen = 1;
  return 0;
}

int plog_is_open(void) { return popen; }

/* The card used to be read-only (disk_write was a stub). Before writing a session log,
 * prove that writing actually works: write a small file, read it back, compare. */
int plog_selftest(void) {
  FIL f;
  UINT w = 0, r = 0;
  char b[64], rb[64];
  int i, ok = 0;

  for (i = 0; i < 64; i++) b[i] = (char) ('A' + i % 26);
  memset(rb, 0, sizeof(rb));

  if (f_open(&f, "/PLRTEST.TXT", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return 0;
  if (f_write(&f, b, sizeof(b), &w) == FR_OK && w == sizeof(b)) {
    f_sync(&f);
    f_close(&f);
    if (f_open(&f, "/PLRTEST.TXT", FA_READ) == FR_OK) {
      if (f_read(&f, rb, sizeof(rb), &r) == FR_OK && r == sizeof(rb) && !memcmp(b, rb, sizeof(b)))
        ok = 1;
      f_close(&f);
    }
  } else {
    f_close(&f);
  }
  f_unlink("/PLRTEST.TXT");
  return ok;
}

void plog_str(const char *s) {
  if (!popen) return;
  while (*s && pn < PLOG_BUF - 1) pbuf[pn++] = *s++;
  if (pn >= PLOG_BUF - 64) plog_flush();
}

void plog_num(uint32_t v) {
  char t[10];
  int k = 0;
  if (!v) { plog_str("0"); return; }
  while (v) { t[k++] = (char) ('0' + (v % 10u)); v /= 10u; }
  while (k) { char c[2]; c[0] = t[--k]; c[1] = 0; plog_str(c); }
}

void plog_snum(int32_t v) {
  if (v < 0) { plog_str("-"); plog_num((uint32_t) (-v)); }
  else plog_num((uint32_t) v);
}

void plog_flush(void) {
  UINT w = 0;
  if (!popen || pn == 0) return;
  if (f_write(&pf, pbuf, (UINT) pn, &w) == FR_OK) {
    pn = 0;
    f_sync(&pf);
  } else {
    pn = 0;
  }
}

void plog_close(void) {
  if (!popen) return;
  plog_flush();
  f_close(&pf);
  popen = 0;
  pn = 0;
}
