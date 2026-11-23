// The files of SD2 (the FPGA card) through FatFs: the player lists a directory and reads a file
// for the ESP32. The card is the one the core owns: see rtl/spi/spi_master.v and sw/sd.c.
#pragma once
#include <stdint.h>
#include "ff.h"

#define FS_NAME_MAX 64

typedef struct {
  char name[FS_NAME_MAX];   // UTF-8, as FatFs gives it
  uint32_t size;
  uint8_t dir;
} fs_entry_t;

int fs_mount(void);                                       // 0 ok
// the entries, <0 on an error; keep (may be NULL) gets every non-hidden entry: 1 to list it
typedef int (*fs_keep_t)(const char *name, int dir);
int fs_list(const char *path, fs_entry_t *out, int max, fs_keep_t keep);
// the whole directory, dynamically grown (NULL when empty). -1 mount, -2 open, -3 no memory.
typedef void (*fs_idle_t)(void);
int fs_list_alloc(const char *path, fs_entry_t **out, fs_keep_t keep, fs_idle_t idle);
int fs_open(const char *path, FIL *f);                     // FA_READ
int fs_dir_exists(const char *path);                       // 1: the directory opens
void fs_path_up(char *path);                               // "/a/b" -> "/a"
