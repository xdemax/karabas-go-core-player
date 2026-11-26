// NMOS 6502 emulator, 151 official opcodes
#pragma once
#include <stdint.h>

typedef struct {
  uint8_t a, x, y, s;
  uint16_t pc;
  uint8_t p;  // flags: NV-BDIZC
  uint8_t mem[65536];
  void (*io_write)(uint16_t addr, uint8_t val);   /* SID registers $D400..$D41F */
} m6502_t;

m6502_t *m6502_new(void);
void m6502_free(m6502_t *cpu);
void m6502_reset(m6502_t *cpu);
int m6502_step(m6502_t *cpu);
uint16_t m6502_pc(m6502_t *cpu);
int m6502_halted(m6502_t *cpu);
