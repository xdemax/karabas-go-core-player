// The ESP32-S3 tracker module over the SPI master: the protocol of player/esp32/doc/esp32-spi-wifi.md
// and examples/esp_spi_slave_protocol (WRBUF 0x01, RDBUF 0x02, WRDMA 0x03, RDDMA 0x04, WR_DONE 0x07).
//
// The stream load: the module is cut into 4 KB chunks. The first MOD_STREAM_LOAD carries the size of
// the file and the size of the chunk (DATA_OFFSET = total, DATA_SIZE = chunk); every next one has
// DATA_OFFSET = 0. The module asks for a chunk with DATA_M2S, the core writes it (WRDMA) and ends it
// (WR_DONE); READY after the chunk means the next one. At the end the module leaves OBJ_HANDLE of the
// built tracker context, TRACK_PLAY starts it.
#include <stdint.h>
#include <string.h>
#include "spi.h"
#include "src.h"
#include "eng_esp32.h"
#include "mmio.h"

#define ESP_REG_CMD      0x00
#define ESP_REG_STATUS   0x01
#define ESP_REG_ERROR    0x02
#define ESP_REG_SIZE     0x03
#define ESP_REG_OFFSET   0x07
#define ESP_REG_HANDLE   0x0B

#define ESP_ST_READY     0x01
#define ESP_ST_BUSY      0x02
#define ESP_ST_M2S       0x03

#define ESP_CMD_GET_VER  0x02
#define ESP_CMD_MOD_LOAD 0xAC
#define ESP_CMD_XM_LOAD  0xAB
#define ESP_CMD_S3M_LOAD 0xAD
#define ESP_CMD_PLAY     0xA1
#define ESP_CMD_STOP     0xA2
#define ESP_CMD_BREAK    0xEF
#define ESP_CMD_DELETE   0xE3

#define ESP_CHUNK 4096

static int present = 0;
static uint8_t ver_api = 0, ver_major = 0, ver_minor = 0, ver_patch = 0;
static char err[48] = "";
static int playing = 0;
static int handle = -1;
static uint8_t chunk[ESP_CHUNK];

// ---- the transport

static void cs_esp(int on) { spi_cs(on ? (0xF & ~(1 << SPI_CS_ESP)) : 0xF); }

static void wrbuf(uint8_t addr, const uint8_t *p, int n) {
  cs_esp(1);
  spi_xfer(0x01);
  spi_xfer(addr);
  spi_xfer(0xFF);
  spi_send(p, n);
  cs_esp(0);
}

static void rdbuf(uint8_t addr, uint8_t *p, int n) {
  cs_esp(1);
  spi_xfer(0x02);
  spi_xfer(addr);
  spi_xfer(0xFF);
  spi_recv(p, n);
  cs_esp(0);
}

static uint8_t last_status = 0xFF;

static uint8_t status(void) {
  uint8_t a = 0, b = 0;
  for (int i = 0; i < 4; i++) {
    rdbuf(ESP_REG_STATUS, &a, 1);
    rdbuf(ESP_REG_STATUS, &b, 1);
    if (a == b) break;
  }
  last_status = a;
  return a;
}

// the status is written by the module at the end of its work; BUSY is a command running
static int wait_status(uint8_t want, uint32_t ms) {
  uint32_t t0 = R_MS;
  for (;;) {
    uint8_t s = status();
    if (s == want) return 0;
    if (s >= 0x80) { return -(int) s; }
    if (R_MS - t0 > ms) return -1;
  }
}

// a command with its parameters in the registers from 0: cmd, 0 (STATUS reset), then the payload
static void command(const uint8_t *p, int n) {
  wrbuf(0, p, n);
}

// a command whose parameter is OBJ_HANDLE at register 0x0B (TRACK_PLAY, DELETE_OBJECT)
static void command_handle(uint8_t cmd, uint8_t h) {
  uint8_t p[12];
  memset(p, 0, sizeof(p));
  p[0] = cmd;
  p[ESP_REG_HANDLE] = h;
  command(p, sizeof(p));
}

static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }

// ---- the commands

int esp32_present(void) { return present; }
const char *esp32_ver(void) {
  static char s[24];
  s[0] = '0' + ver_major; s[1] = '.';
  s[2] = '0' + ver_minor; s[3] = '.';
  s[4] = '0' + ver_patch; s[5] = 0;
  return s;
}

// one GET_VER attempt: the module answers when its SPI slave is initialised (after the reset of
// the core, the Wi-Fi / SDMMC init of the module comes first, so this can take a few seconds)
static int init_once(void) {
  uint8_t p[2] = { ESP_CMD_GET_VER, 0 };
  command(p, 2);
  if (wait_status(ESP_ST_READY, 100) == 0) {
    uint8_t r[6];
    rdbuf(0x02, r, 6);
    ver_api = r[0]; ver_minor = r[4]; ver_patch = r[5]; ver_major = r[3];
    present = 1;
    return 0;
  }
  return -1;
}

static void delay_ms(uint32_t ms) {
  uint32_t t0 = R_MS;
  while (R_MS - t0 < ms) ;
}

// the short probe of the registry at the core start
int esp32_init(void) {
  present = 0;
  handle = -1;
  playing = 0;
  for (int i = 0; i < 10; i++) {           // 1 s
    if (init_once() == 0) return 0;
    delay_ms(100);
  }
  return -1;
}

// the play waits longer: the firmware resets the module at the core start
static int wait_present(int ms) {
  for (int i = 0; i < ms / 100; i++) {
    if (init_once() == 0) return 0;
    delay_ms(100);
  }
  return -1;
}

static void err_status(void) {
  static const char hx[] = "0123456789abcdef";
  strcpy(err, "no ESP32 (st ");
  int n = (int) strlen(err);
  err[n++] = hx[last_status >> 4];
  err[n++] = hx[last_status & 15];
  err[n++] = ')';
  err[n] = 0;
}

int esp32_stop(void) {
  if (!present) return -1;
  uint8_t p[2] = { ESP_CMD_STOP, 0 };
  command(p, 2);
  playing = 0;
  return wait_status(ESP_ST_READY, 3000);
}

// abort a half-open stream load (harmless when idle): the module forgets the chunk state
static int stream_break(void) {
  uint8_t p[2] = { ESP_CMD_BREAK, 0 };
  command(p, 2);
  return wait_status(ESP_ST_READY, 2000);
}

static int del_object(int h) {
  command_handle(ESP_CMD_DELETE, (uint8_t) h);
  return wait_status(ESP_ST_READY, 3000);
}

int esp32_playing(void) { return playing; }
const char *esp32_error(void) { return err; }

static int prepare(void) {
  if (!present) wait_present(10000);        // the module may still be booting after its reset
  if (!present) { err_status(); return -1; }
  stream_break();          // a stream left open by an earlier error
  if (handle >= 0) {       // the previous module: stop it and give its memory back
    esp32_stop();
    del_object(handle);
    handle = -1;
  }
  playing = 0;
  return 0;
}

static int stream(uint8_t cmd, play_src_t src, uint32_t size, play_progress_t progress) {
  uint32_t chunk_size = (size + 3) & ~3UL;
  if (chunk_size > ESP_CHUNK) chunk_size = ESP_CHUNK;

  uint32_t sent = 0;
  int first = 1;
  int rc = 0;
  while (sent < size) {
    uint32_t want = (size - sent + 3) & ~3UL;
    if (want > chunk_size) want = chunk_size;
    uint8_t p[11];
    p[0] = cmd; p[1] = 0; p[2] = 0;
    le32(p + 3, want);
    le32(p + 7, first ? size : 0);
    command(p, sizeof(p));
    first = 0;

    rc = wait_status(ESP_ST_M2S, 3000);
    if (rc != 0) { strcpy(err, "no chunk request"); rc = -5; break; }

    uint8_t r[8];
    rdbuf(ESP_REG_SIZE, r, 8);          // the size it asks, then the offset it is at
    uint32_t n = rd32(r);
    uint32_t off = rd32(r + 4);
    if (n == 0 || n > chunk_size) { strcpy(err, "bad chunk size"); rc = -6; break; }
    if (off + n > ((size + 3) & ~3UL)) { strcpy(err, "bad chunk offset"); rc = -7; break; }

    UINT got = 0;
    if (src_read(&src, off, chunk, n, &got) != 0) { strcpy(err, "read error"); rc = -8; break; }
    for (uint32_t i = got; i < n; i++) chunk[i] = 0;   // the last chunk is padded to 4 bytes

    cs_esp(1);                          // WRDMA
    spi_xfer(0x03); spi_xfer(0x00); spi_xfer(0xFF);
    spi_send(chunk, (int) n);
    cs_esp(0);
    cs_esp(1);                          // WR_DONE
    spi_xfer(0x07);
    cs_esp(0);

    rc = wait_status(ESP_ST_READY, 30000);
    if (rc != 0) { strcpy(err, "module rejected the chunk"); rc = -9; break; }

    sent = off + n;
    if (sent > size) sent = size;
    if (progress) progress(sent, size);
  }
  if (rc != 0) { stream_break(); return rc; }

  uint8_t h = 0;
  rdbuf(ESP_REG_HANDLE, &h, 1);
  if (!h) { strcpy(err, "no module handle"); return -10; }
  handle = h;

  command_handle(ESP_CMD_PLAY, h);
  if (wait_status(ESP_ST_READY, 5000) != 0) { strcpy(err, "play failed"); handle = -1; return -11; }
  playing = 1;
  return 0;
}

// the format modules call this with their engine format
int esp32_play(int fmt, const play_src_t *src, uint32_t size, play_progress_t progress) {
  uint8_t cmd;
  switch (fmt) {
    case ESP32_FMT_MOD: cmd = ESP_CMD_MOD_LOAD; break;
    case ESP32_FMT_XM: cmd = ESP_CMD_XM_LOAD; break;
    case ESP32_FMT_S3M: cmd = ESP_CMD_S3M_LOAD; break;
    default: strcpy(err, "unknown format"); return -3;
  }
  if (!src || !size) { strcpy(err, "empty file"); return -2; }
  int rc = prepare();
  if (rc != 0) return rc;
  return stream(cmd, *src, size, progress);
}

// ---- the engine of the registry ----

static int e_init(void) { return esp32_init(); }
static int e_stop(void) { return esp32_stop(); }

const play_engine_t esp32_engine = { "ESP32", e_init, e_stop, esp32_ver, esp32_error, NULL, esp32_playing,
                                    NULL, NULL, NULL, NULL, NULL, NULL, NULL };
