// The player's own state in the kgo2 file /config/musicplayer.ini (SD1, written by the firmware):
// [player] dir=..., file=..., volume=...
#include <string.h>
#include <stdlib.h>
#include "settings.h"
#include "rpc.h"

#define SECT "player"

static char *put(char *p, const char *s) {
  while (s && *s) *p++ = *s++;
  *p++ = 0;
  return p;
}

static void copy(char *d, int cap, const char *s) {
  int i = 0;
  for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
  if (cap > 0) d[i] = 0;
}

static int get_value(const uint8_t **q, const uint8_t *end, char *out, int cap) {
  if (*q >= end) return 0;
  int found = *(*q)++;
  const char *s = (const char *) *q;
  while (*q < end && **q) (*q)++;
  if (*q < end) (*q)++;
  if (found) copy(out, cap, s);
  return found;
}

int settings_load(char *dir, int dir_cap, char *file, int file_cap, int *volume) {
  if (dir_cap > 0) copy(dir, dir_cap, "/");
  if (file_cap > 0) file[0] = 0;
  *volume = -1;
  if (!rpc_enabled()) return -1;

  char req[96], *p = req;
  p = put(p, "1");
  p = put(p, SECT); p = put(p, "dir");
  p = put(p, SECT); p = put(p, "file");
  p = put(p, SECT); p = put(p, "volume");
  uint8_t resp[192];
  uint16_t rn = 0;
  int rc = rpc_call(RPC_CFG, 0, req, (uint16_t) (p - req), resp, sizeof(resp), &rn, 1500);
  if (rc != RPC_OK) return rc;

  char v[96];
  const uint8_t *q = resp, *end = resp + rn;
  v[0] = 0;
  if (get_value(&q, end, v, sizeof(v)) && v[0] == '/') copy(dir, dir_cap, v);
  v[0] = 0;
  if (get_value(&q, end, v, sizeof(v))) copy(file, file_cap, v);
  v[0] = 0;
  if (get_value(&q, end, v, sizeof(v)) && v[0]) {
    int n = atoi(v);
    if (n < 0) n = 0;
    if (n > 64) n = 64;
    *volume = n;
  }
  return 0;
}

int settings_save(const char *dir, const char *file, int volume) {
  if (!rpc_enabled()) return -1;
  char num[8], *q = num;
  if (volume < 0) volume = 0;
  if (volume > 64) volume = 64;
  if (volume >= 10) *q++ = (char) ('0' + volume / 10);
  *q++ = (char) ('0' + volume % 10);
  *q = 0;

  char req[384], *p = req;
  p = put(p, "1");
  p = put(p, SECT); p = put(p, "dir");  p = put(p, dir ? dir : "/");
  p = put(p, SECT); p = put(p, "file"); p = put(p, file ? file : "");
  p = put(p, SECT); p = put(p, "volume"); p = put(p, num);
  uint8_t resp[16];
  uint16_t rn = 0;
  int rc = rpc_call(RPC_CFG, 1, req, (uint16_t) (p - req), resp, sizeof(resp), &rn, 1500);
  if (rc != RPC_OK) return rc;
  // file 1 is written at once by the firmware; SAVE is kept for a staged implementation
  rpc_call(RPC_CFG, 2, "1", 2, resp, sizeof(resp), &rn, 1500);
  return 0;
}
