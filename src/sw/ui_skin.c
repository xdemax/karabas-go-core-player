#include "ui_skin.h"

// The compiled skins. Add a skin here (and the Makefile) and it is reachable with Tab.
static const ui_skin_t *const skins[] = {
  &skin_graphic,
};

static unsigned idx = 0;

const ui_skin_t *skin_current(void) {
  return skins[idx];
}

const char *skin_name(void) {
  return skins[idx]->name;
}

void skin_next(void) {
  idx = (idx + 1) % (sizeof(skins) / sizeof(skins[0]));
  if (skins[idx]->init) skins[idx]->init();
}
