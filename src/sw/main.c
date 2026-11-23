// The MOD player core of the Karabas Go: the file manager of SD2 / SD1 and the streaming of a
// module to the ESP32-S3 of the VDAC32 shield. The screen is player.c.
#include <stdint.h>
#include "player.h"

int main(void) {
  player_run();
  return 0;
}
