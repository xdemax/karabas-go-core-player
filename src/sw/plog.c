#include "plog.h"
#include "ff.h"

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
