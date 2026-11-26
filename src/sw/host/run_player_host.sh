#!/bin/sh
# The host build of the player screen (a fake list, no cards, no ESP): host/out/frame.png, for a
# look at the layout. Needs gcc and Python PIL (as the menu core's host build).
set -e
cd "$(dirname "$0")/.."
mkdir -p host/out
gcc -O1 -g -DHOST -Wall -Wno-unused-function -I. -Ifatfs -o host/player_host \
  host/player_host.c player.c fmt.c browser.c ui_skin.c skin_graphic.c ui.c gfx.c fonts.c sound.c
./host/player_host
cd host/out && python3 -c "from PIL import Image; Image.open('frame.ppm').save('frame.png')" && rm frame.ppm
