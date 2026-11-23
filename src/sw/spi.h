// The SPI master of the core (rtl/spi/spi_master.v): the SD2 card and the ESP32-S3 of the
// VDAC32 shield. Mode 0, MSB first; CS is a register held across a whole command / block.
#pragma once
#include <stdint.h>

#define SPI_CS_SD2 0
#define SPI_CS_FT  1
#define SPI_CS_ESP 3

void spi_div(uint16_t half_period);   // SCK = 75 MHz / (2 * (half_period + 1))
void spi_cs(uint8_t mask);            // the four CS lines, active low; 1 = deasserted
uint8_t spi_xfer(uint8_t b);          // one byte, the answer
void spi_send(const uint8_t *p, int n);
void spi_recv(uint8_t *p, int n);
void spi_fill(uint8_t b, int n);
int spi_busy(void);
