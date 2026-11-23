// A host test of the SD SPI driver (src/sw/sd.c) against a model of an SDHC card in SPI mode: the
// command frames, the R1 / R3 answers, the data tokens, CMD17 / CMD18 / CMD12. The blocks come from
// a card image (host/test.img of mkimg.py).   host/run_sd_test.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../sd.h"
#include "../spi.h"

static FILE *img;
static int failures;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } while (0)

// ---- the card model ----

static int cs_low;
static uint8_t cmd[6];
static int cmd_n;
static uint8_t resp[4096];
static int resp_pos, resp_len;
static int acmd41_polls;
static int multi_plan;            // the blocks of the next CMD18 (the test tells the model)

static void append_block(uint32_t l) {
  resp[resp_len++] = 0xFE;
  if (fseek(img, (long) l * 512, SEEK_SET) != 0 || fread(resp + resp_len, 512, 1, img) != 1) { FAIL("image lba %u", l); memset(resp + resp_len, 0, 512); }
  resp_len += 512;
  resp[resp_len++] = 0;
  resp[resp_len++] = 0;
}

static void execute(void) {
  uint8_t c = cmd[0] & 0x3F;
  uint32_t arg = ((uint32_t) cmd[1] << 24) | ((uint32_t) cmd[2] << 16) | ((uint32_t) cmd[3] << 8) | cmd[4];
  resp_pos = 0;
  resp_len = 0;
  switch (c) {
    case 0: resp[resp_len++] = 0x01; break;                        // GO_IDLE_STATE
    case 8:                                                        // SEND_IF_COND
      resp[resp_len++] = 0x01; resp[resp_len++] = 0; resp[resp_len++] = 0;
      resp[resp_len++] = 1; resp[resp_len++] = 0xAA;
      break;
    case 55: resp[resp_len++] = 0x00; break;     // APP_CMD
    case 41:                                                       // ACMD41
      acmd41_polls++;
      resp[resp_len++] = acmd41_polls < 3 ? 0x01 : 0x00;
      break;
    case 58:                                                       // READ_OCR: CCS = 1
      resp[resp_len++] = 0x00; resp[resp_len++] = 0xC0; resp[resp_len++] = 0xFF;
      resp[resp_len++] = 0x80; resp[resp_len++] = 0x00;
      break;
    case 16: resp[resp_len++] = 0x00; break;                       // SET_BLOCKLEN
    case 17:                                                       // READ_SINGLE_BLOCK
      resp[resp_len++] = 0x00;
      append_block(arg);
      break;
    case 18:                                                       // READ_MULTIPLE_BLOCK
      resp[resp_len++] = 0x00;
      for (int i = 0; i < multi_plan; i++) append_block(arg + i);
      break;
    case 12: resp[resp_len++] = 0x00; resp[resp_len++] = 0x00; break;  // STOP_TRANSMISSION
    default: resp[resp_len++] = 0x04; break;                       // illegal command
  }
}

void spi_div(uint16_t d) { (void) d; }
void spi_cs(uint8_t mask) {
  int low = (mask & 1) == 0;
  if (low && !cs_low) { cmd_n = 0; resp_pos = resp_len = 0; }
  cs_low = low;
}

uint8_t spi_xfer(uint8_t b) {
  if (!cs_low) return 0xFF;
  if (resp_pos < resp_len) return resp[resp_pos++];
  if (cmd_n < 6) {
    cmd[cmd_n++] = b;
    if (cmd_n == 6) { cmd_n = 0; execute(); return 0xFF; }
    return 0xFF;
  }
  return 0xFF;
}

void spi_send(const uint8_t *p, int n) { while (n-- > 0) spi_xfer(*p++); }
void spi_recv(uint8_t *p, int n) { while (n-- > 0) *p++ = spi_xfer(0xFF); }
void spi_fill(uint8_t b, int n) { while (n-- > 0) spi_xfer(b); }
int spi_busy(void) { return 0; }

// ---- the test ----

static uint8_t want[512];

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "test.img";
  img = fopen(path, "rb");
  if (!img) { printf("no %s\n", path); return 2; }

  int rc = sd_init();
  if (rc) FAIL("sd_init: %d", rc);
  if (!sd_is_hc()) FAIL("the card is not seen as SDHC");

  uint32_t lbas[] = { 0, 1, 37, 100, 1000, 20000 };
  for (int i = 0; i < 6; i++) {
    fseek(img, (long) lbas[i] * 512, SEEK_SET);
    fread(want, 512, 1, img);
    uint8_t got[512];
    rc = sd_read(lbas[i], got, 1);
    if (rc) FAIL("read %u: %d", lbas[i], rc);
    else if (memcmp(want, got, 512)) FAIL("block %u differs", lbas[i]);
  }

  // three blocks in one CMD18
  uint8_t got[1536];
  multi_plan = 3;
  rc = sd_read(100, got, 3);
  if (rc) FAIL("multi read: %d", rc);
  else {
    fseek(img, 100L * 512, SEEK_SET);
    fread(want, 512, 1, img);
    if (memcmp(want, got, 512)) FAIL("multi: the first block differs");
    fseek(img, 102L * 512, SEEK_SET);
    fread(want, 512, 1, img);
    if (memcmp(want, got + 1024, 512)) FAIL("multi: the third block differs");
  }

  fclose(img);
  if (failures) { printf("sd_test: %d errors\n", failures); return 1; }
  printf("sd_test: 0 errors\n");
  return 0;
}
