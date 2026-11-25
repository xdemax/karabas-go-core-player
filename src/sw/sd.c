// SD (SPI mode) on the SPI master of the core: rtl/spi/spi_master.v, CS bit SPI_CS_SD2.
// The init is at 399 kHz, the transfers at 12.5 MHz. Only the reads the file system needs.
#include <stdint.h>
#include "spi.h"
#include "sd.h"

#define SD_CS spi_cs(0xF & ~(1 << SPI_CS_SD2))
#define SD_RELEASE() do { spi_cs(0xF); spi_xfer(0xFF); } while (0)

static int hc = 0;               // block addressing (SDHC / SDXC)
static uint32_t nsec = 0;

static uint8_t crc7(const uint8_t *d, int n) {
  uint8_t crc = 0;
  for (int i = 0; i < n; i++)
    for (int b = 7; b >= 0; b--) {
      crc <<= 1;
      if (((d[i] >> b) & 1) ^ ((crc >> 7) & 1)) crc ^= 0x09;
    }
  return (uint8_t) ((crc << 1) | 1);
}

// the next response byte: the card holds DO high until it answers
static uint8_t r1(void) {
  for (int i = 0; i < 16; i++) {
    uint8_t v = spi_xfer(0xFF);
    if (!(v & 0x80)) return v;
  }
  return 0xFF;
}

static uint8_t cmd(uint8_t c, uint32_t arg) {
  uint8_t b[6];
  b[0] = 0x40 | c;
  b[1] = arg >> 24; b[2] = arg >> 16; b[3] = arg >> 8; b[4] = arg;
  b[5] = c == 0 ? 0x95 : c == 8 ? 0x87 : crc7(b, 5);
  spi_send(b, 6);
  return r1();
}

// a data token, then 512 bytes and their CRC (not checked: read only, the CRC mode is off)
static int block_read(uint8_t *buf) {
  int tries = 200000;
  while (tries-- > 0) {
    uint8_t v = spi_xfer(0xFF);
    if (v == 0xFE) {
      spi_recv(buf, 512);
      spi_xfer(0xFF); spi_xfer(0xFF);
      return 0;
    }
    if (v != 0xFF) return 2;
  }
  return 3;
}

int sd_is_hc(void) { return hc; }
uint32_t sd_sectors(void) { return nsec; }

int sd_init(void) {
  hc = 0;
  nsec = 0;
  spi_div(93);                    // ~400 kHz
  spi_cs(0xF);
  spi_fill(0xFF, 10);

  uint8_t r = 0xFF;
  for (int i = 0; i < 3 && r != 0x01; i++) {
    SD_CS;
    r = cmd(0, 0);                // GO_IDLE_STATE, CRC 0x95
    SD_RELEASE();
  }
  if (r != 0x01) return 1;

  SD_CS;
  r = cmd(8, 0x1AA);              // SEND_IF_COND: v2 and the voltage
  uint8_t v2 = 0;
  if (r == 0x01) {
    uint8_t e[4];
    spi_recv(e, 4);
    v2 = (e[2] == 0x01 && e[3] == 0xAA);
  }
  SD_RELEASE();
  if (!v2) v2 = 0;                // v1 or a card that does not answer CMD8

  // ACMD41 until the card is out of idle
  for (int i = 0; i < 100; i++) {
    SD_CS;
    r = cmd(55, 0);
    if (r <= 0x01) r = cmd(41, v2 ? 0x40000000UL : 0);
    SD_RELEASE();
    if (r == 0x00) break;
    if (r > 0x01) return 2;
  }
  if (r != 0x00) return 3;

  SD_CS;
  r = cmd(58, 0);                 // READ_OCR: CCS bit 30 = block addressing
  if (r == 0x00) {
    uint8_t o[4];
    spi_recv(o, 4);
    hc = (o[0] >> 6) & 1;
  }
  SD_RELEASE();
  if (r != 0x00) return 4;

  if (!hc) {
    SD_CS;
    r = cmd(16, 512);             // SET_BLOCKLEN for the byte addressed cards
    SD_RELEASE();
    if (r != 0x00) return 5;
  }

  spi_div(2);                     // 12.5 MHz
  spi_cs(0xF);
  spi_fill(0xFF, 2);
  return 0;
}

/* Write one 512-byte block: start token, data, dummy CRC, data response, then wait
 * until the card releases the busy line. Returns 0 on success. */
static int block_write(const uint8_t *buf) {
  int i;
  for (i = 0; i < 20000; i++) if (spi_xfer(0xFF) == 0xFF) break;   /* card ready */
  if (i == 20000) return 20;
  spi_xfer(0xFE);                                                  /* start token */
  spi_send((uint8_t *) buf, 512);
  spi_xfer(0xFF); spi_xfer(0xFF);                                  /* CRC: ignored by SD */
  uint8_t r = spi_xfer(0xFF);
  if ((r & 0x1Fu) != 0x05) return 21;                              /* data not accepted */
  for (i = 0; i < 2000000; i++) if (spi_xfer(0xFF) == 0xFF) return 0;
  return 22;                                                       /* stuck busy */
}

int sd_write(uint32_t lba, const uint8_t *buf, uint32_t count) {
  uint32_t addr = hc ? lba : lba * 512;
  for (uint32_t b = 0; b < count; b++) {
    SD_CS;
    uint8_t r = cmd(24, addr + (hc ? b : b * 512));                /* WRITE_BLOCK */
    if (r != 0x00) { SD_RELEASE(); return 30; }
    int rc = block_write(buf + b * 512);
    SD_RELEASE();
    if (rc) return rc;
  }
  return 0;
}

int sd_read(uint32_t lba, uint8_t *buf, uint32_t count) {
  if (!count) return 0;
  uint32_t addr = hc ? lba : lba * 512;

  SD_CS;
  if (count == 1) {
    uint8_t r = cmd(17, addr);
    if (r != 0x00) { SD_RELEASE(); return 10; }
    int rc = block_read(buf);
    SD_RELEASE();
    return rc;
  }

  uint8_t r = cmd(18, addr);      // READ_MULTIPLE_BLOCK
  if (r != 0x00) { SD_RELEASE(); return 11; }
  for (uint32_t i = 0; i < count; i++) {
    int rc = block_read(buf + i * 512);
    if (rc) {
      cmd(12, 0);                 // STOP_TRANSMISSION, then the busy byte
      spi_xfer(0xFF);
      SD_RELEASE();
      return rc;
    }
  }
  cmd(12, 0);
  for (int i = 0; i < 8; i++) if (!(spi_xfer(0xFF) & 0x80)) break;
  SD_RELEASE();
  return 0;
}
