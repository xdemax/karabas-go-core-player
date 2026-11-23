#include <stdint.h>
#include "mmio.h"
#include "spi.h"

#define R_SPI_DATA MMIO(0x50)   // W: the byte to send (starts); R: bit 8 busy, 7..0 the answer
#define R_SPI_CS   MMIO(0x54)   // W/R: the CS lines, active low
#define R_SPI_DIV  MMIO(0x58)   // W/R: the half period in clocks

void spi_div(uint16_t half_period) { R_SPI_DIV = half_period; }
void spi_cs(uint8_t mask) { R_SPI_CS = mask & 0x0F; }
int spi_busy(void) { return (R_SPI_DATA >> 8) & 1; }

uint8_t spi_xfer(uint8_t b) {
  while (R_SPI_DATA & 0x100) ;
  R_SPI_DATA = b;
  while (R_SPI_DATA & 0x100) ;
  return (uint8_t) R_SPI_DATA;
}

void spi_send(const uint8_t *p, int n) { while (n-- > 0) spi_xfer(*p++); }
void spi_recv(uint8_t *p, int n) { while (n-- > 0) *p++ = spi_xfer(0xFF); }
void spi_fill(uint8_t b, int n) { while (n-- > 0) spi_xfer(b); }
