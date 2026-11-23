// The saved state of the player, kept by the firmware in /config/musicplayer.ini:
// [player] dir=... file=... volume=...  (kgo2 core settings, SD1).
#pragma once

int settings_load(char *dir, int dir_cap, char *file, int file_cap, int *volume);
int settings_save(const char *dir, const char *file, int volume);
