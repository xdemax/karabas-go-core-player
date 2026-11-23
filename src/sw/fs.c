#include <string.h>
#include <stdlib.h>
#include "fs.h"

static FATFS fs;
static int mounted = 0;

static void full(const char *path, char *out, int size) {
  int i = 0;
  out[i++] = '0';
  out[i++] = ':';
  if (*path != '/') out[i++] = '/';
  while (*path && i < size - 1) out[i++] = *path++;
  out[i] = 0;
}

int fs_mount(void) {
  if (mounted) return 0;
  if (f_mount(&fs, "0:", 1) != FR_OK) return -1;
  mounted = 1;
  return 0;
}

static int hidden(const FILINFO *fi) {
  if (fi->fname[0] == '.') return 1;
  if (fi->fattrib & (AM_HID | AM_SYS)) return 1;
  return 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int cmp(const fs_entry_t *a, const fs_entry_t *b) {
  if (a->dir != b->dir) return a->dir ? -1 : 1;
  const char *p = a->name, *q = b->name;
  while (*p && *q) {
    int d = lower(*p) - lower(*q);
    if (d) return d;
    p++; q++;
  }
  return lower(*p) - lower(*q);
}

int fs_list(const char *path, fs_entry_t *out, int max, fs_keep_t keep) {
  static DIR d;
  static FILINFO fi;
  char p[280];
  int n = 0;
  if (fs_mount() != 0) return -1;
  full(path, p, sizeof(p));
  if (f_opendir(&d, p) != FR_OK) return -2;
  while (n < max && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
    if (hidden(&fi)) continue;
    int dir = (fi.fattrib & AM_DIR) != 0;
    if (keep && !keep(fi.fname, dir)) continue;
    fs_entry_t *e = &out[n];
    int i = 0;
    for (; fi.fname[i] && i < FS_NAME_MAX - 1; i++) e->name[i] = fi.fname[i];
    e->name[i] = 0;
    e->size = (uint32_t) fi.fsize;
    e->dir = (uint8_t) dir;
    n++;
  }
  f_closedir(&d);
  // directories first, then the names without the case (the lists stay short)
  for (int i = 1; i < n; i++) {
    fs_entry_t t = out[i];
    int j = i - 1;
    while (j >= 0 && cmp(&out[j], &t) > 0) { out[j + 1] = out[j]; j--; }
    out[j + 1] = t;
  }
  return n;
}

static int cmp_q(const void *a, const void *b) { return cmp((const fs_entry_t *) a, (const fs_entry_t *) b); }

// The same as fs_list, but without the caller's fixed array: the list grows as the directory is
// read. The caller frees *out. This is the list of the file panel, so there is no 400-entry limit.
int fs_list_alloc(const char *path, fs_entry_t **out, fs_keep_t keep, fs_idle_t idle) {
  static DIR d;
  static FILINFO fi;
  char p[280];
  fs_entry_t *arr = NULL;
  int n = 0, cap = 0;
  *out = NULL;
  if (fs_mount() != 0) return -1;
  full(path, p, sizeof(p));
  if (f_opendir(&d, p) != FR_OK) return -2;
  while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
    if (hidden(&fi)) continue;
    int dir = (fi.fattrib & AM_DIR) != 0;
    if (keep && !keep(fi.fname, dir)) continue;
    if (n == cap) {
      int nc = cap ? cap * 2 : 64;
      fs_entry_t *na = realloc(arr, (size_t) nc * sizeof(fs_entry_t));
      if (!na) { free(arr); f_closedir(&d); return -3; }
      arr = na;
      cap = nc;
    }
    fs_entry_t *e = &arr[n];
    int i = 0;
    for (; fi.fname[i] && i < FS_NAME_MAX - 1; i++) e->name[i] = fi.fname[i];
    e->name[i] = 0;
    e->size = (uint32_t) fi.fsize;
    e->dir = (uint8_t) dir;
    n++;
    if (idle && (n & 31) == 0) idle();   // keep the audio FIFO fed while a big directory is read
  }
  f_closedir(&d);
  if (idle) idle();
  if (n > 1) qsort(arr, (size_t) n, sizeof(fs_entry_t), cmp_q);
  *out = arr;
  return n;
}

int fs_open(const char *path, FIL *f) {
  char p[280];
  if (fs_mount() != 0) return -1;
  full(path, p, sizeof(p));
  return f_open(f, p, FA_READ) == FR_OK ? 0 : -2;
}

int fs_dir_exists(const char *path) {
  static DIR d;
  char p[280];
  if (fs_mount() != 0) return 0;
  full(path, p, sizeof(p));
  if (f_opendir(&d, p) != FR_OK) return 0;
  f_closedir(&d);
  return 1;
}

void fs_path_up(char *path) {
  int n = (int) strlen(path);
  while (n > 1 && path[n - 1] != '/') n--;
  if (n > 1) n--;
  path[n] = 0;
  if (!path[0]) { path[0] = '/'; path[1] = 0; }
}
