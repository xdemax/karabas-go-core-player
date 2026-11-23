#include <string.h>
#include "browser.h"
#include "playlist.h"

static void clamp(browser_t *b) {
  if (b->sel >= b->nent) b->sel = b->nent ? b->nent - 1 : 0;
  if (b->sel < 0) b->sel = 0;
  if (b->sel < b->top) b->top = b->sel;
}

static int load(browser_t *b, const char *path, fs_keep_t keep, fs_idle_t idle) {
  fs_entry_t *ent = NULL;
  int n = fs_list_alloc(path, &ent, keep, idle);
  if (n < 0) return n;
  fs_free_list(b->ent, b->nent);
  b->ent = ent;
  b->nent = n;
  strncpy(b->path, path, sizeof(b->path) - 1);
  b->path[sizeof(b->path) - 1] = 0;
  clamp(b);
  return n;
}

void browser_init(browser_t *b) {
  memset(b, 0, sizeof(*b));
  b->path[0] = '/';
  b->path[1] = 0;
}

void browser_free(browser_t *b) {
  if (b->playlist) fs_free_list(b->back_ent, b->back_nent);
  fs_free_list(b->ent, b->nent);
  b->ent = NULL;
  b->back_ent = NULL;
  b->nent = b->sel = b->top = 0;
  b->playlist = 0;
}

int browser_reload(browser_t *b, fs_keep_t keep, fs_idle_t idle) {
  return load(b, b->path, keep, idle);
}

int browser_selected_path(const browser_t *b, char *out, int size) {
  const fs_entry_t *e = browser_selected(b);
  if (!e || size <= 0) { if (size > 0) out[0] = 0; return -1; }
  if (e->path) {
    int i = 0;
    for (const char *p = e->path; *p && i < size - 1; p++) out[i++] = *p;
    out[i] = 0;
    return (i < size - 1) ? 0 : -2;
  }
  int i = 0;
  if (strcmp(b->path, "/") != 0) {
    for (const char *p = b->path; *p && i < size - 1; p++) out[i++] = *p;
  }
  if (i == 0 || out[i - 1] != '/') {
    if (i >= size - 1) { out[0] = 0; return -2; }
    out[i++] = '/';
  }
  for (const char *p = e->name; *p && i < size - 1; p++) out[i++] = *p;
  out[i] = 0;
  return (i < size - 1) ? 0 : -2;
}

int browser_enter(browser_t *b, fs_keep_t keep, fs_idle_t idle) {
  const fs_entry_t *e = browser_selected(b);
  if (!e) return BROWSER_ENTER_FILE;
  char full[FS_PATH_MAX + FS_NAME_MAX + 2];
  if (browser_selected_path(b, full, sizeof(full)) != 0) return -2;

  if (e->dir) {
    int n = load(b, full, keep, idle);
    if (n < 0) return n;
    b->sel = 0;
    b->top = 0;
    return BROWSER_ENTER_DIR;
  }
  if (playlist_supported(e->name)) {
    fs_entry_t *pl = NULL;
    int n = playlist_load(full, &pl);
    if (n < 0) return n;
    if (b->playlist) {
      fs_free_list(b->ent, b->nent);
    } else {
      b->back_ent = b->ent;
      b->back_nent = b->nent;
      b->back_sel = b->sel;
      b->back_top = b->top;
      strncpy(b->back_path, b->path, sizeof(b->back_path) - 1);
      b->back_path[sizeof(b->back_path) - 1] = 0;
    }
    b->ent = pl;
    b->nent = n;
    strncpy(b->path, full, sizeof(b->path) - 1);
    b->path[sizeof(b->path) - 1] = 0;
    b->playlist = 1;
    b->sel = 0;
    b->top = 0;
    return BROWSER_ENTER_PLAYLIST;
  }
  return BROWSER_ENTER_FILE;
}

int browser_up(browser_t *b, fs_keep_t keep, fs_idle_t idle) {
  if (b->playlist) {
    fs_free_list(b->ent, b->nent);
    b->ent = b->back_ent;
    b->nent = b->back_nent;
    b->sel = b->back_sel;
    b->top = b->back_top;
    strncpy(b->path, b->back_path, sizeof(b->path) - 1);
    b->path[sizeof(b->path) - 1] = 0;
    b->back_ent = NULL;
    b->back_nent = 0;
    b->playlist = 0;
    return 1;
  }
  if (strcmp(b->path, "/") == 0) return 0;
  char full[FS_PATH_MAX];
  strncpy(full, b->path, sizeof(full) - 1);
  full[sizeof(full) - 1] = 0;
  fs_path_up(full);
  int n = load(b, full, keep, idle);
  if (n < 0) return n;
  b->sel = 0;
  b->top = 0;
  return 1;
}
