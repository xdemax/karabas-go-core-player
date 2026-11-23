// A host test of the FatFs layer of the player (src/sw/fs.c + diskio.c) on a card image: the mount,
// the directory list (dirs first, .mod/.xm/.s3m only, no hidden), the path prefix and a file read.
//   host/run_fs_test.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../fs.h"
#include "../playlist.h"

static FILE *img;
static int failures;

#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } while (0)

int sd_init(void) { return 0; }
int sd_read(uint32_t lba, uint8_t *buf, uint32_t count) {
  if (fseek(img, (long) lba * 512, SEEK_SET) != 0) return 1;
  return fread(buf, 512, count, img) == count ? 0 : 1;
}
uint32_t sd_sectors(void) { return 0; }
int sd_is_hc(void) { return 1; }

// the filter the player passes: dirs and the modules of the registry (the real one is fmt.c)
static int keep(const char *name, int dir) {
  if (dir) return 1;
  const char *d = strrchr(name, '.');
  if (!d) return 0;
  return !strcasecmp(d, ".mod") || !strcasecmp(d, ".xm") || !strcasecmp(d, ".s3m") ||
         !strcasecmp(d, ".pt3") || !strcasecmp(d, ".ayl");
}
// the playlist parser asks the real registry; this test only needs the extensions
int fmt_supported(const char *name) {
  const char *d = strrchr(name, '.');
  if (!d) return 0;
  return !strcasecmp(d, ".mod") || !strcasecmp(d, ".xm") || !strcasecmp(d, ".s3m") ||
         !strcasecmp(d, ".pt3");
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "test.img";
  img = fopen(path, "rb");
  if (!img) { printf("no %s\n", path); return 2; }

  fs_entry_t e[16];
  int n = fs_list("/", e, 16, keep);
  if (n != 4) { FAIL("root: %d entries (want 4: many, sub, a.mod, b.XM)", n); for (int i = 0; i < n; i++) printf("  %s\n", e[i].name); }
  else {
    if (!e[0].dir || strcmp(e[0].name, "many")) FAIL("the first is not many/");
    if (!e[1].dir || strcmp(e[1].name, "sub")) FAIL("the second is not sub/");
    if (strcmp(e[2].name, "a.mod") || e[2].dir) FAIL("the third is not a.mod");
    if (strcmp(e[3].name, "b.XM") || e[3].dir) FAIL("the fourth is not b.XM");
    if (e[2].size != 5000) FAIL("a.mod is %u bytes", e[2].size);
  }
  fs_free_names(e, n);

  fs_entry_t *many = NULL;
  int nm = fs_list_alloc("/many", &many, keep, NULL);
  if (nm != 600) FAIL("many/: %d entries (want 600)", nm);
  else if (strcmp(many[0].name, "f0000.mod") || strcmp(many[599].name, "f0599.mod")) FAIL("many/ sort: %s .. %s", many[0].name, many[599].name);
  fs_free_list(many, nm);

  n = fs_list("/sub", e, 16, keep);
  if (n != 3 || !e[0].dir || strcmp(e[0].name, "level2 with a long folder name") ||
      strcmp(e[1].name, "c.s3m") || strcmp(e[2].name, "list.ayl"))
    FAIL("sub/: %d entries, first %s", n, n ? e[0].name : "-");
  fs_free_names(e, n < 0 ? 0 : n);

  fs_entry_t *pl = NULL;
  int npl = playlist_load("/sub/list.ayl", &pl);
  if (npl != 2 || strcmp(pl[0].name, "a.mod") || strcmp(pl[0].path, "/a.mod") ||
      strcmp(pl[1].name, "c.s3m") || strcmp(pl[1].path, "/sub/c.s3m"))
    FAIL("playlist: %d entries", npl);
  fs_free_list(pl, npl < 0 ? 0 : npl);

  const char *deepdir = "/sub/level2 with a long folder name/level3 another long folder name/level4";
  fs_entry_t *deep = NULL;
  int nd = fs_list_alloc(deepdir, &deep, keep, NULL);
  if (nd != 1 || strcmp(deep[0].name, "deep-track.mod")) FAIL("deep/: %d entries", nd);
  fs_free_list(deep, nd < 0 ? 0 : nd);
  FIL df;
  if (fs_open("/sub/level2 with a long folder name/level3 another long folder name/level4/deep-track.mod", &df) != 0)
    FAIL("open the deep file");
  else f_close(&df);

  FIL f;
  if (fs_open("/a.mod", &f) != 0) FAIL("open a.mod");
  else {
    uint8_t b[5000];
    UINT got = 0;
    if (f_read(&f, b, sizeof(b), &got) != FR_OK || got != 5000) FAIL("read %u", got);
    else for (int i = 0; i < 5000; i++) if (b[i] != (uint8_t) (i * 7)) { FAIL("byte %d is %02x", i, b[i]); break; }
    f_close(&f);
  }

  char p[32] = "/sub/../sub";
  fs_path_up(p);
  if (strcmp(p, "/sub/..")) FAIL("path_up %s", p);
  strcpy(p, "/a");
  fs_path_up(p);
  if (strcmp(p, "/")) FAIL("path_up %s", p);

  fclose(img);
  if (failures) { printf("fs_test: %d errors\n", failures); return 1; }
  printf("fs_test: 0 errors\n");
  return 0;
}
