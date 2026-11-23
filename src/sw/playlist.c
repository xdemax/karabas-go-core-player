#include <string.h>
#include <stdlib.h>
#include "playlist.h"
#include "fmt.h"

#define PLAYLIST_MAX  (64u * 1024u)
#define PLAYLIST_PATH (FS_PATH_MAX * 2)

typedef int (*line_parser_t)(const char *line, const char *base, char *out, int cap);

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ext_eq(const char *name, const char *ext) {
  const char *dot = strrchr(name, '.');
  if (!dot) return 0;
  dot++;
  while (*dot && *ext) {
    if (lower(*dot) != lower(*ext)) return 0;
    dot++; ext++;
  }
  return !*dot && !*ext;
}

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r') s++;
  int n = (int) strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
  if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s[n - 1] = 0; s++; }
  return s;
}

static int is_meta(const char *s) {
  static const char *const key[] = { "Author=", "Name=", "Length=", "Time=", "Loop=",
                                     "Title=", "Comment=", "Genre=" };
  for (unsigned i = 0; i < sizeof(key) / sizeof(key[0]); i++) {
    int n = (int) strlen(key[i]);
    if (!strncmp(s, key[i], n)) return 1;
  }
  return 0;
}

// Resolve a line against the playlist directory and normalize "." / "..".
static int resolve(const char *base, const char *line, char *out, int cap) {
  char tmp[PLAYLIST_PATH];
  int n = 0;
  if (line[0] == '/' || line[0] == '\\') {
    tmp[n++] = '/';
    if (line[0] == '/' || line[0] == '\\') line++;
  } else {
    for (const char *p = base; *p && n < (int) sizeof(tmp) - 2; p++) tmp[n++] = *p;
    if (n == 0 || tmp[n - 1] != '/') {
      if (n >= (int) sizeof(tmp) - 2) return -1;
      tmp[n++] = '/';
    }
  }
  for (const char *p = line; *p && n < (int) sizeof(tmp) - 1; p++) tmp[n++] = *p;
  if (*line && n >= (int) sizeof(tmp) - 1) return -1;
  tmp[n] = 0;

  int w = 0;
  if (cap <= 1) return -1;
  out[w++] = '/';
  char *p = tmp;
  while (*p) {
    while (*p == '/' || *p == '\\') p++;
    char *seg = p;
    while (*p && *p != '/' && *p != '\\') p++;
    int len = (int) (p - seg);
    if (len == 0 || (len == 1 && seg[0] == '.')) continue;
    if (len == 2 && seg[0] == '.' && seg[1] == '.') {
      while (w > 1 && out[w - 1] != '/') w--;
      if (w > 1) w--;
      continue;
    }
    int need = len + (w > 1 ? 1 : 0);
    if (w + need >= cap) return -1;
    if (w > 1) out[w++] = '/';
    memcpy(out + w, seg, len);
    w += len;
  }
  out[w] = 0;
  return 0;
}

static int ayl_line(const char *raw, const char *base, char *out, int cap) {
  char buf[PLAYLIST_PATH];
  int n = 0;
  for (const char *p = raw; *p && n < (int) sizeof(buf) - 1; p++) buf[n++] = *p;
  buf[n] = 0;
  char *line = trim(buf);
  if (!line[0] || line[0] == '<' || line[0] == '>' || line[0] == '#' || line[0] == ';')
    return 1;
  if (is_meta(line)) return 1;
  if (!fmt_supported(line)) return 1;
  return resolve(base, line, out, cap) == 0 ? 0 : -2;
}

static const struct {
  const char *ext;
  line_parser_t parse;
} parsers[] = {
  { "ayl", ayl_line },
};

static line_parser_t parser_for(const char *name) {
  for (unsigned i = 0; i < sizeof(parsers) / sizeof(parsers[0]); i++)
    if (ext_eq(name, parsers[i].ext)) return parsers[i].parse;
  return NULL;
}

int playlist_supported(const char *name) { return name && parser_for(name) != NULL; }

static char *copy_str(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

static void dir_of(const char *path, char *out, int cap) {
  int n = 0;
  while (path[n] && n < cap - 1) { out[n] = path[n]; n++; }
  out[n] = 0;
  char *slash = strrchr(out, '/');
  if (!slash) { out[0] = '/'; out[1] = 0; }
  else if (slash == out) out[1] = 0;
  else *slash = 0;
}

int playlist_load(const char *path, fs_entry_t **out) {
  *out = NULL;
  line_parser_t parse = parser_for(path);
  if (!parse) return -2;
  FIL f;
  if (fs_open(path, &f) != 0) return -2;
  FSIZE_t sz = f_size(&f);
  if (sz == 0 || sz > PLAYLIST_MAX) { f_close(&f); return -4; }
  char *buf = malloc((size_t) sz + 1);
  if (!buf) { f_close(&f); return -3; }
  UINT got = 0;
  if (f_read(&f, buf, (UINT) sz, &got) != FR_OK || got != sz) {
    free(buf); f_close(&f); return -2;
  }
  f_close(&f);
  buf[sz] = 0;

  char base[FS_PATH_MAX];
  dir_of(path, base, sizeof(base));

  fs_entry_t *arr = NULL;
  int n = 0, cap = 0;
  char *line = buf;
  while (line && *line) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    char full[FS_PATH_MAX];
    int r = parse(line, base, full, sizeof(full));
    if (r == 0) {
      const char *name = strrchr(full, '/');
      name = name ? name + 1 : full;
      if (n == cap) {
        int nc = cap ? cap * 2 : 32;
        fs_entry_t *na = realloc(arr, (size_t) nc * sizeof(fs_entry_t));
        if (!na) { fs_free_list(arr, n); free(buf); return -3; }
        arr = na; cap = nc;
      }
      arr[n].name = copy_str(name);
      arr[n].path = copy_str(full);
      arr[n].size = 0;
      arr[n].dir = 0;
      if (!arr[n].name || !arr[n].path) {
        free(arr[n].name); free(arr[n].path);
        fs_free_list(arr, n); free(buf); return -3;
      }
      n++;
    } else if (r < 0) {
      fs_free_list(arr, n); free(buf); return -3;
    }
    line = nl ? nl + 1 : NULL;
  }
  free(buf);
  *out = arr;
  return n;
}
