// The file the stock file loader of the firmware (the FL item, Menu+F5) put into the SRAM from SD1:
// the ready / too big / size / extension registers of menu_soc.
#pragma once
#include <stdint.h>

int sd1_ready(void);              // bit 0 ready, bit 1 too big / short
const uint8_t *sd1_data(void);    // 0x080000, the bytes of the file
uint32_t sd1_size(void);
uint32_t sd1_ext(void);           // "mod", "xm", "s3m" as ASCII (0 for the fileloader)
void sd1_ack(void);               // clear the ready / too big
