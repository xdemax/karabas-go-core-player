// The SD2 card on the SPI master (mode 0): init, single and multi block reads. Read only.
#pragma once
#include <stdint.h>

int sd_init(void);                                  // 0 ok, else an error code (1 no card, ...)
int sd_read(uint32_t lba, uint8_t *buf, uint32_t count);   // 0 ok
int sd_write(uint32_t lba, const uint8_t *buf, uint32_t count); // 0 ok
int sd_is_hc(void);
uint32_t sd_sectors(void);                          // 0 if unknown
