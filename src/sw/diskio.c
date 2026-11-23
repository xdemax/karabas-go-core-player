// FatFs disk I/O of the player: the SD2 card (the FPGA slot) on the core's SPI master (sw/sd.c).
// The card is the second microSD of the board, the one wired to the FPGA — the same one the TS-Conf
// core reads on its own. SD1 belongs to the RP2040; a core cannot see it.
#include <stdint.h>
#include "ff.h"
#include "diskio.h"
#include "sd.h"

static int sd2_ok = 0;

DSTATUS disk_status(BYTE pdrv) {
  if (pdrv != 0) return STA_NOINIT;
  return sd2_ok ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
  if (pdrv != 0) return STA_NOINIT;
  if (!sd2_ok) sd2_ok = sd_init() == 0;
  return sd2_ok ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv != 0) return RES_PARERR;
  return sd2_ok && sd_read((uint32_t) sector, buff, count) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
  (void) buff; (void) sector; (void) count;
  return RES_WRPRT;            // the player only reads
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  (void) pdrv;
  switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *) buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *) buff = 1; return RES_OK;
    case GET_SECTOR_COUNT: {
      uint32_t n = sd_sectors();
      if (!n) return RES_ERROR;
      *(LBA_t *) buff = n;
      return RES_OK;
    }
    default: return RES_PARERR;
  }
}
