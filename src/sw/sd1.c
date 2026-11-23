#include "sd1.h"
#include "mmio.h"

int sd1_ready(void) { return (int) (R_LDF & 3); }
const uint8_t *sd1_data(void) { return (const uint8_t *) LD_FILE_BASE; }
uint32_t sd1_size(void) { return R_LDLEN; }
uint32_t sd1_ext(void) { return R_LDEXT; }
void sd1_ack(void) { R_LDACK = 1; }
