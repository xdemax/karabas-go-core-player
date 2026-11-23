// The file browser of the player: the current directory, its entries, the cursor and the
// navigation. The player core only asks for the current state and for the selected file.
#pragma once
#include "fs.h"

typedef struct {
  char path[FS_PATH_MAX];
  fs_entry_t *ent;
  int nent, sel, top;

  int playlist;                         // the current list is an open playlist
  char back_path[FS_PATH_MAX];          // the directory it was opened from
  fs_entry_t *back_ent;
  int back_nent, back_sel, back_top;
} browser_t;

enum { BROWSER_ENTER_FILE = 0, BROWSER_ENTER_DIR = 1, BROWSER_ENTER_PLAYLIST = 2 };

void browser_init(browser_t *b);
void browser_free(browser_t *b);

// Reload the current directory. On an error the old path and list stay in place.
// Returns the entry count or -1 mount, -2 open, -3 no memory.
int browser_reload(browser_t *b, fs_keep_t keep, fs_idle_t idle);

// Enter the selected directory or playlist. Returns BROWSER_ENTER_DIR / BROWSER_ENTER_PLAYLIST,
// BROWSER_ENTER_FILE for a file, <0 on an error (the old directory stays).
int browser_enter(browser_t *b, fs_keep_t keep, fs_idle_t idle);

// Go to the parent directory. Returns 1 when the path changed, 0 at the root or an error.
int browser_up(browser_t *b, fs_keep_t keep, fs_idle_t idle);

// The absolute path of the selected entry, "" when there is no selection.
int browser_selected_path(const browser_t *b, char *out, int size);

static inline const fs_entry_t *browser_selected(const browser_t *b) {
  return (b->sel >= 0 && b->sel < b->nent) ? &b->ent[b->sel] : 0;
}
