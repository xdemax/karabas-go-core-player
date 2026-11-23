// The registers of menu_soc (rtl/soc/menu_soc.v): word registers at 0x80000000.
#pragma once
#include <stdint.h>

#ifdef HOST
// the host build of the drawing (host/): the registers and the frame buffers are arrays there
volatile uint32_t *host_reg(uint32_t off);
extern uint8_t host_fb[2][0x80000];
#define MMIO(off) (*host_reg(off))
#else
#define MMIO(off) (*(volatile uint32_t *) (0x80000000u + (off)))
#endif
#define R_DBG    MMIO(0x00) // W: a byte of the debug console
#define R_FB     MMIO(0x04) // W: bit 0 the buffer to show from the next frame; R: the one shown
#define R_FRAMES MMIO(0x08) // R: frames begun
#define R_CYCLES MMIO(0x0c) // R: clocks since the start
#define R_SHOW   MMIO(0x10) // W: bit 0 the frame buffer on the screen
#define R_GRAD_TOP MMIO(0x18) // W: the top colour of the background gradient
#define R_GRAD_BOT MMIO(0x20) // W: its bottom colour (3/4 of the height and below)
#define R_MIDI   MMIO(0xa0) // W: a MIDI byte to the SAM2695; R: bit 0 its fifo is full
#define R_ESP    MMIO(0xa4) // W: a byte to the ESP8266; R: bit 8 a byte is there, 7..0 the byte (taken)
#define R_ESP_OWN MMIO(0xa8) // W/R: bit 0 the CPU has the uart of the ESP (else the MCU, PORT 1)
#define R_LAUNCH MMIO(0x14) // W: the SRAM is cleared, then the RPC call written is made (CORES LAUNCH)
#define R_MS     MMIO(0x1c) // R: milliseconds since the start
#define R_SDRAM  MMIO(0xbc) // R: bit 0 = the 32 MB SDRAM controller finished init
#define R_KBD0   MMIO(0x28) // R: { key 3, key 2, key 1, modifiers }
#define R_KBD1   MMIO(0x2c) // R: { 0, key 6, key 5, key 4 }
#define R_KBDN   MMIO(0x30) // R: keyboard reports seen
#define R_MOUSE  MMIO(0x34) // R: buttons 31..24, dy 23..12, dx 11..0 (reading clears)
#define R_WHEEL  MMIO(0x3c) // R: the wheel sum 11..0, + away from the user (reading clears)
#define R_JOY    MMIO(0x38) // R: { joy_r, joy_l }
#define R_US     MMIO(0x40) // R: microseconds since the start
#define R_SW     MMIO(0xb0) // R: the bytes of the OSD switches S0..S3 (one byte per S item)
#define R_AY     MMIO(0xc0) // W: { reset, 3'b0, chip, reg[3:0], data[7:0] } to the TS YM2149 pair
#define R_AY_CTL MMIO(0xc4) // W: bit 0 active, bits 15..8 volume 0..64
#define R_LDF    MMIO(0x60) // R: LOAD (file loader / FL item): bit 0 ready, 1 too big, 3..2 kind
#define R_LDLEN  MMIO(0x64) // R: the size of the loaded file
#define R_LDACK  MMIO(0x68) // W: acknowledge (clear ready / too big)
#define R_LDEXT  MMIO(0x6c) // R: the extension of an FL item ("mod", "xm", "s3m"; 0 for the fileloader)
#define LD_FILE_BASE 0x080000u   // the file of LOAD kind 1 / 2 in the SRAM
#define LD_FILE_MAX  0x00280000u // the file buffer: 0x080000 .. 0x2fffff (2.5 MB)
#define SDRAM_BASE     0x10000000u // the 32 MB SDRAM (the SD2 file buffer; see menu_soc)
#define SDRAM_FILE_MAX 0x02000000u // the whole SDRAM window
#define R_PAL(b, i) MMIO(0x400 + 0x400 * (b) + 4 * (i)) // W: entry i of the palette of frame buffer b:
                                                     // 0xARRGGBB, alpha 0..15 (0 transparent, 15 opaque)

// the mailbox of kg_l2_rpc: byte k at 0x80002000 + 4 k (low byte of a word)
#define RPC(k)   MMIO(0x2000 + 4 * (k))
#define RPC_REQ   0x000 // the payload of the request
#define RPC_RESP  0x200 // the payload of the response
#define RPC_EVT   0x400 // the payload of an event
#define RPC_ID    0x600
#define RPC_SVC   0x601
#define RPC_FN    0x602
#define RPC_LEN   0x603 // 2 bytes
#define RPC_CALL  0x605
#define RPC_FLAGS 0x608 // bit 0 busy, 1 response in, 2 event in, 3 enabled
#define RPC_RST   0x609
#define RPC_RLEN  0x60A // 2 bytes
#define RPC_EDONE 0x614
#define RPC_FWVER 0x618

#define FB_W 800
#define FB_H 600
#ifdef HOST
#define FB0 ((volatile uint8_t *) host_fb[0])
#define FB1 ((volatile uint8_t *) host_fb[1])
#else
#define FB0 ((volatile uint8_t *) 0x300000)
#define FB1 ((volatile uint8_t *) 0x380000)
#endif
