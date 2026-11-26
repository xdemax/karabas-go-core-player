// User playlists (AYL now): the list lines are paths of tracks; the file looks like a directory
// of the tracks. A new playlist format only needs its own line parser in playlist.c.
#pragma once
#include "fs.h"

int playlist_supported(const char *name);                 // 1: a playlist file
// Parse a playlist and return its entries (NULL when empty). Every entry has name and path;
// path is the absolute track path, name is its last component.
// Returns the count, -1 mount, -2 open/format, -3 no memory, -4 too large.
int playlist_load(const char *path, fs_entry_t **out);

const char *playlist_extensions(void);   // "ayl, m3u"
