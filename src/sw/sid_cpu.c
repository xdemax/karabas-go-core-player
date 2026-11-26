// NMOS 6502 emulator, 151 official opcodes
#include "sid_cpu.h"
#include <stdlib.h>
#include <string.h>

#define FLAG_N 0x80
#define FLAG_V 0x40
#define FLAG_B 0x10
#define FLAG_D 0x08
#define FLAG_I 0x04
#define FLAG_Z 0x02
#define FLAG_C 0x01

/* One CPU: the firmware has no reliable heap for a 64 KB block, so it lives in .bss. */
static m6502_t g_cpu;
m6502_t *m6502_new(void) {
  memset(&g_cpu, 0, sizeof(g_cpu));
  return &g_cpu;
}

void m6502_free(m6502_t *cpu) { (void) cpu; }

void m6502_reset(m6502_t *cpu) {
  cpu->pc = 0xc000;
  cpu->s = 0xff;
  cpu->p = FLAG_I;
  cpu->a = cpu->x = cpu->y = 0;
}

static inline uint8_t rd(m6502_t *cpu, uint16_t addr) {
  return cpu->mem[addr];
}

static inline void wr(m6502_t *cpu, uint16_t addr, uint8_t val) {
  if (addr >= 0xd400 && addr <= 0xd41f && cpu->io_write) {
    cpu->io_write(addr, val);
    return;                     /* SID registers are not memory */
  }
  cpu->mem[addr] = val;
}

static inline uint16_t rd16(m6502_t *cpu, uint16_t addr) {
  return rd(cpu, addr) | (rd(cpu, addr + 1) << 8);
}

static inline void push(m6502_t *cpu, uint8_t val) {
  wr(cpu, 0x100 + cpu->s, val);
  cpu->s--;
}

static inline uint8_t pop(m6502_t *cpu) {
  cpu->s++;
  return rd(cpu, 0x100 + cpu->s);
}

static inline void set_nz(m6502_t *cpu, uint8_t val) {
  cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z)) | (val & 0x80) | (val == 0 ? FLAG_Z : 0);
}

#define NEXT() do { cpu->pc++; } while(0)
#define NEXT_W() do { cpu->pc += 2; } while(0)
#define NEXT_B(val) do { (val) = rd(cpu, cpu->pc); NEXT(); } while(0)
#define NEXT_W_VAL(val) do { (val) = rd16(cpu, cpu->pc); NEXT_W(); } while(0)

int m6502_step(m6502_t *cpu) {
  if (cpu->pc >= 0xfffe) return 1;   /* put the sentinel at the top of memory */
  
  uint8_t op = rd(cpu, cpu->pc);
  NEXT();
  
  uint8_t lo, hi, val, tmp;
  uint16_t addr;
  int16_t sval;
  
  switch (op) {
    // BRK
    case 0x00:
      push(cpu, (cpu->pc >> 8) & 0xff);
      push(cpu, cpu->pc & 0xff);
      push(cpu, cpu->p | FLAG_B);
      cpu->p |= FLAG_I;
      cpu->pc = rd16(cpu, 0xfffe);
      break;

    // ORA (Indirect,X)
    case 0x01:
      NEXT_B(lo);
      addr = rd16(cpu, (lo + cpu->x) & 0xff);
      cpu->a |= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // ORA Zero Page
    case 0x05:
      NEXT_B(lo);
      cpu->a |= rd(cpu, lo);
      set_nz(cpu, cpu->a);
      break;

    // ASL Zero Page
    case 0x06:
      NEXT_B(lo);
      val = rd(cpu, lo);
      cpu->p = (cpu->p & ~FLAG_C) | ((val >> 7) & 1);
      val <<= 1;
      set_nz(cpu, val);
      wr(cpu, lo, val);
      break;

    // PHP
    case 0x08:
      push(cpu, cpu->p | FLAG_B);
      break;

    // ORA Immediate
    case 0x09:
      NEXT_B(val);
      cpu->a |= val;
      set_nz(cpu, cpu->a);
      break;

    // ASL Accumulator
    case 0x0a:
      cpu->p = (cpu->p & ~FLAG_C) | ((cpu->a >> 7) & 1);
      cpu->a <<= 1;
      set_nz(cpu, cpu->a);
      break;

    // ORA Absolute
    case 0x0d:
      NEXT_W_VAL(addr);
      cpu->a |= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // ASL Absolute
    case 0x0e:
      NEXT_W_VAL(addr);
      val = rd(cpu, addr);
      cpu->p = (cpu->p & ~FLAG_C) | ((val >> 7) & 1);
      val <<= 1;
      set_nz(cpu, val);
      wr(cpu, addr, val);
      break;

    // BPL
    case 0x10:
      NEXT_B(sval);
      if (!(cpu->p & FLAG_N)) cpu->pc += (int16_t)(int8_t)sval;
      break;

    // ORA (Indirect),Y
    case 0x11:
      NEXT_B(lo);
      addr = rd16(cpu, lo) + cpu->y;
      cpu->a |= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // ORA Zero Page,X
    case 0x15:
      NEXT_B(lo);
      cpu->a |= rd(cpu, (lo + cpu->x) & 0xff);
      set_nz(cpu, cpu->a);
      break;

    // ASL Zero Page,X
    case 0x16:
      NEXT_B(lo);
      lo = (lo + cpu->x) & 0xff;
      val = rd(cpu, lo);
      cpu->p = (cpu->p & ~FLAG_C) | ((val >> 7) & 1);
      val <<= 1;
      set_nz(cpu, val);
      wr(cpu, lo, val);
      break;

    // CLC
    case 0x18:
      cpu->p &= ~FLAG_C;
      break;

    // ORA Absolute,Y
    case 0x19:
      NEXT_W_VAL(addr);
      addr += cpu->y;
      cpu->a |= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // ORA Absolute,X
    case 0x1d:
      NEXT_W_VAL(addr);
      addr += cpu->x;
      cpu->a |= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // ASL Absolute,X
    case 0x1e:
      NEXT_W_VAL(addr);
      addr += cpu->x;
      val = rd(cpu, addr);
      cpu->p = (cpu->p & ~FLAG_C) | ((val >> 7) & 1);
      val <<= 1;
      set_nz(cpu, val);
      wr(cpu, addr, val);
      break;

    // JSR
    case 0x20:
      NEXT_W_VAL(addr);
      push(cpu, (cpu->pc >> 8) & 0xff);
      push(cpu, cpu->pc & 0xff);
      cpu->pc = addr;
      break;

    // AND (Indirect,X)
    case 0x21:
      NEXT_B(lo);
      addr = rd16(cpu, (lo + cpu->x) & 0xff);
      cpu->a &= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // BIT Zero Page
    case 0x24:
      NEXT_B(lo);
      val = rd(cpu, lo);
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z)) | (val & (FLAG_N | FLAG_V)) | ((cpu->a & val) == 0 ? FLAG_Z : 0);
      break;

    // AND Zero Page
    case 0x25:
      NEXT_B(lo);
      cpu->a &= rd(cpu, lo);
      set_nz(cpu, cpu->a);
      break;

    // ROL Zero Page
    case 0x26:
      NEXT_B(lo);
      val = rd(cpu, lo);
      tmp = (val << 1) | (cpu->p & FLAG_C);
      cpu->p = (cpu->p & ~FLAG_C) | ((val >> 7) & 1);
      set_nz(cpu, tmp);
      wr(cpu, lo, tmp);
      break;

    // PLP
    case 0x28:
      cpu->p = pop(cpu);
      break;

    // AND Immediate
    case 0x29:
      NEXT_B(val);
      cpu->a &= val;
      set_nz(cpu, cpu->a);
      break;

    // ROL Accumulator
    case 0x2a:
      tmp = (cpu->a << 1) | (cpu->p & FLAG_C);
      cpu->p = (cpu->p & ~FLAG_C) | ((cpu->a >> 7) & 1);
      set_nz(cpu, tmp);
      cpu->a = tmp;
      break;

    // BIT Absolute
    case 0x2c:
      NEXT_W_VAL(addr);
      val = rd(cpu, addr);
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z)) | (val & (FLAG_N | FLAG_V)) | ((cpu->a & val) == 0 ? FLAG_Z : 0);
      break;

    // AND Absolute
    case 0x2d:
      NEXT_W_VAL(addr);
      cpu->a &= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // ROL Absolute
    case 0x2e:
      NEXT_W_VAL(addr);
      val = rd(cpu, addr);
      tmp = (val << 1) | (cpu->p & FLAG_C);
      cpu->p = (cpu->p & ~FLAG_C) | ((val >> 7) & 1);
      set_nz(cpu, tmp);
      wr(cpu, addr, tmp);
      break;

    // BMI
    case 0x30:
      NEXT_B(sval);
      if (cpu->p & FLAG_N) cpu->pc += (int16_t)(int8_t)sval;
      break;

    // AND (Indirect),Y
    case 0x31:
      NEXT_B(lo);
      addr = rd16(cpu, lo) + cpu->y;
      cpu->a &= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // AND Zero Page,X
    case 0x35:
      NEXT_B(lo);
      cpu->a &= rd(cpu, (lo + cpu->x) & 0xff);
      set_nz(cpu, cpu->a);
      break;

    // ROL Zero Page,X
    case 0x36:
      NEXT_B(lo);
      lo = (lo + cpu->x) & 0xff;
      val = rd(cpu, lo);
      tmp = (val << 1) | (cpu->p & FLAG_C);
      cpu->p = (cpu->p & ~FLAG_C) | ((val >> 7) & 1);
      set_nz(cpu, tmp);
      wr(cpu, lo, tmp);
      break;

    // SEC
    case 0x38:
      cpu->p |= FLAG_C;
      break;

    // AND Absolute,Y
    case 0x39:
      NEXT_W_VAL(addr);
      addr += cpu->y;
      cpu->a &= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // AND Absolute,X
    case 0x3d:
      NEXT_W_VAL(addr);
      addr += cpu->x;
      cpu->a &= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // ROL Absolute,X
    case 0x3e:
      NEXT_W_VAL(addr);
      addr += cpu->x;
      val = rd(cpu, addr);
      tmp = (val << 1) | (cpu->p & FLAG_C);
      cpu->p = (cpu->p & ~FLAG_C) | ((val >> 7) & 1);
      set_nz(cpu, tmp);
      wr(cpu, addr, tmp);
      break;

    // RTI
    case 0x40:
      cpu->p = pop(cpu);
      cpu->pc = pop(cpu) | (pop(cpu) << 8);
      break;

    // EOR (Indirect,X)
    case 0x41:
      NEXT_B(lo);
      addr = rd16(cpu, (lo + cpu->x) & 0xff);
      cpu->a ^= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // EOR Zero Page
    case 0x45:
      NEXT_B(lo);
      cpu->a ^= rd(cpu, lo);
      set_nz(cpu, cpu->a);
      break;

    // LSR Zero Page
    case 0x46:
      NEXT_B(lo);
      val = rd(cpu, lo);
      cpu->p = (cpu->p & ~FLAG_C) | (val & 1);
      val >>= 1;
      set_nz(cpu, val);
      wr(cpu, lo, val);
      break;

    // PHA
    case 0x48:
      push(cpu, cpu->a);
      break;

    // EOR Immediate
    case 0x49:
      NEXT_B(val);
      cpu->a ^= val;
      set_nz(cpu, cpu->a);
      break;

    // LSR Accumulator
    case 0x4a:
      cpu->p = (cpu->p & ~FLAG_C) | (cpu->a & 1);
      cpu->a >>= 1;
      set_nz(cpu, cpu->a);
      break;

    // JMP Absolute
    case 0x4c:
      NEXT_W_VAL(addr);
      cpu->pc = addr;
      break;

    // EOR Absolute
    case 0x4d:
      NEXT_W_VAL(addr);
      cpu->a ^= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // LSR Absolute
    case 0x4e:
      NEXT_W_VAL(addr);
      val = rd(cpu, addr);
      cpu->p = (cpu->p & ~FLAG_C) | (val & 1);
      val >>= 1;
      set_nz(cpu, val);
      wr(cpu, addr, val);
      break;

    // BVC
    case 0x50:
      NEXT_B(sval);
      if (!(cpu->p & FLAG_V)) cpu->pc += (int16_t)(int8_t)sval;
      break;

    // EOR (Indirect),Y
    case 0x51:
      NEXT_B(lo);
      addr = rd16(cpu, lo) + cpu->y;
      cpu->a ^= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // EOR Zero Page,X
    case 0x55:
      NEXT_B(lo);
      cpu->a ^= rd(cpu, (lo + cpu->x) & 0xff);
      set_nz(cpu, cpu->a);
      break;

    // LSR Zero Page,X
    case 0x56:
      NEXT_B(lo);
      lo = (lo + cpu->x) & 0xff;
      val = rd(cpu, lo);
      cpu->p = (cpu->p & ~FLAG_C) | (val & 1);
      val >>= 1;
      set_nz(cpu, val);
      wr(cpu, lo, val);
      break;

    // CLI
    case 0x58:
      cpu->p &= ~FLAG_I;
      break;

    // EOR Absolute,Y
    case 0x59:
      NEXT_W_VAL(addr);
      addr += cpu->y;
      cpu->a ^= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // EOR Absolute,X
    case 0x5d:
      NEXT_W_VAL(addr);
      addr += cpu->x;
      cpu->a ^= rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // LSR Absolute,X
    case 0x5e:
      NEXT_W_VAL(addr);
      addr += cpu->x;
      val = rd(cpu, addr);
      cpu->p = (cpu->p & ~FLAG_C) | (val & 1);
      val >>= 1;
      set_nz(cpu, val);
      wr(cpu, addr, val);
      break;

    // RTS
    case 0x60:
      cpu->pc = pop(cpu) | (pop(cpu) << 8);
      cpu->pc++;
      break;

    // ADC (Indirect,X)
    case 0x61: {
      NEXT_B(lo);
      addr = rd16(cpu, (lo + cpu->x) & 0xff);
      val = rd(cpu, addr);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a + val + carry;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res > 0xff ? FLAG_C : 0) |
               ((~(cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // ADC Zero Page
    case 0x65: {
      NEXT_B(lo);
      val = rd(cpu, lo);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a + val + carry;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res > 0xff ? FLAG_C : 0) |
               ((~(cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // ROR Zero Page
    case 0x66:
      NEXT_B(lo);
      val = rd(cpu, lo);
      tmp = (val >> 1) | ((cpu->p & FLAG_C) << 7);
      cpu->p = (cpu->p & ~FLAG_C) | (val & 1);
      set_nz(cpu, tmp);
      wr(cpu, lo, tmp);
      break;

    // PLA
    case 0x68:
      cpu->a = pop(cpu);
      set_nz(cpu, cpu->a);
      break;

    // ADC Immediate
    case 0x69: {
      NEXT_B(val);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a + val + carry;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res > 0xff ? FLAG_C : 0) |
               ((~(cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // ROR Accumulator
    case 0x6a:
      tmp = (cpu->a >> 1) | ((cpu->p & FLAG_C) << 7);
      cpu->p = (cpu->p & ~FLAG_C) | (cpu->a & 1);
      set_nz(cpu, tmp);
      cpu->a = tmp;
      break;

    // JMP Indirect
    case 0x6c: {
      NEXT_W_VAL(addr);
      uint8_t lsb = rd(cpu, addr);
      uint8_t msb = rd(cpu, (addr & 0xff00) | ((addr + 1) & 0xff));
      cpu->pc = lsb | (msb << 8);
      break;
    }

    // ADC Absolute
    case 0x6d: {
      NEXT_W_VAL(addr);
      val = rd(cpu, addr);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a + val + carry;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res > 0xff ? FLAG_C : 0) |
               ((~(cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // ROR Absolute
    case 0x6e:
      NEXT_W_VAL(addr);
      val = rd(cpu, addr);
      tmp = (val >> 1) | ((cpu->p & FLAG_C) << 7);
      cpu->p = (cpu->p & ~FLAG_C) | (val & 1);
      set_nz(cpu, tmp);
      wr(cpu, addr, tmp);
      break;

    // BVS
    case 0x70:
      NEXT_B(sval);
      if (cpu->p & FLAG_V) cpu->pc += (int16_t)(int8_t)sval;
      break;

    // ADC (Indirect),Y
    case 0x71: {
      NEXT_B(lo);
      addr = rd16(cpu, lo) + cpu->y;
      val = rd(cpu, addr);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a + val + carry;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res > 0xff ? FLAG_C : 0) |
               ((~(cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // ADC Zero Page,X
    case 0x75: {
      NEXT_B(lo);
      val = rd(cpu, (lo + cpu->x) & 0xff);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a + val + carry;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res > 0xff ? FLAG_C : 0) |
               ((~(cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // ROR Zero Page,X
    case 0x76:
      NEXT_B(lo);
      lo = (lo + cpu->x) & 0xff;
      val = rd(cpu, lo);
      tmp = (val >> 1) | ((cpu->p & FLAG_C) << 7);
      cpu->p = (cpu->p & ~FLAG_C) | (val & 1);
      set_nz(cpu, tmp);
      wr(cpu, lo, tmp);
      break;

    // SEI
    case 0x78:
      cpu->p |= FLAG_I;
      break;

    // ADC Absolute,Y
    case 0x79: {
      NEXT_W_VAL(addr);
      addr += cpu->y;
      val = rd(cpu, addr);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a + val + carry;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res > 0xff ? FLAG_C : 0) |
               ((~(cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // ADC Absolute,X
    case 0x7d: {
      NEXT_W_VAL(addr);
      addr += cpu->x;
      val = rd(cpu, addr);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a + val + carry;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res > 0xff ? FLAG_C : 0) |
               ((~(cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // ROR Absolute,X
    case 0x7e:
      NEXT_W_VAL(addr);
      addr += cpu->x;
      val = rd(cpu, addr);
      tmp = (val >> 1) | ((cpu->p & FLAG_C) << 7);
      cpu->p = (cpu->p & ~FLAG_C) | (val & 1);
      set_nz(cpu, tmp);
      wr(cpu, addr, tmp);
      break;

    // STA (Indirect,X)
    case 0x81:
      NEXT_B(lo);
      addr = rd16(cpu, (lo + cpu->x) & 0xff);
      wr(cpu, addr, cpu->a);
      break;

    // STY Zero Page
    case 0x84:
      NEXT_B(lo);
      wr(cpu, lo, cpu->y);
      break;

    // STA Zero Page
    case 0x85:
      NEXT_B(lo);
      wr(cpu, lo, cpu->a);
      break;

    // STX Zero Page
    case 0x86:
      NEXT_B(lo);
      wr(cpu, lo, cpu->x);
      break;

    // DEY
    case 0x88:
      cpu->y--;
      set_nz(cpu, cpu->y);
      break;

    // TXA
    case 0x8a:
      cpu->a = cpu->x;
      set_nz(cpu, cpu->a);
      break;

    // STY Absolute
    case 0x8c:
      NEXT_W_VAL(addr);
      wr(cpu, addr, cpu->y);
      break;

    // STA Absolute
    case 0x8d:
      NEXT_W_VAL(addr);
      wr(cpu, addr, cpu->a);
      break;

    // STX Absolute
    case 0x8e:
      NEXT_W_VAL(addr);
      wr(cpu, addr, cpu->x);
      break;

    // BCC
    case 0x90:
      NEXT_B(sval);
      if (!(cpu->p & FLAG_C)) cpu->pc += (int16_t)(int8_t)sval;
      break;

    // STA (Indirect),Y
    case 0x91:
      NEXT_B(lo);
      addr = rd16(cpu, lo) + cpu->y;
      wr(cpu, addr, cpu->a);
      break;

    // STY Zero Page,X
    case 0x94:
      NEXT_B(lo);
      wr(cpu, (lo + cpu->x) & 0xff, cpu->y);
      break;

    // STA Zero Page,X
    case 0x95:
      NEXT_B(lo);
      wr(cpu, (lo + cpu->x) & 0xff, cpu->a);
      break;

    // STX Zero Page,Y
    case 0x96:
      NEXT_B(lo);
      wr(cpu, (lo + cpu->y) & 0xff, cpu->x);
      break;

    // TYA
    case 0x98:
      cpu->a = cpu->y;
      set_nz(cpu, cpu->a);
      break;

    // STA Absolute,Y
    case 0x99:
      NEXT_W_VAL(addr);
      wr(cpu, addr + cpu->y, cpu->a);
      break;

    // TXS
    case 0x9a:
      cpu->s = cpu->x;
      break;

    // STA Absolute,X
    case 0x9d:
      NEXT_W_VAL(addr);
      wr(cpu, addr + cpu->x, cpu->a);
      break;

    // LDY Immediate
    case 0xa0:
      NEXT_B(cpu->y);
      set_nz(cpu, cpu->y);
      break;

    // LDA (Indirect,X)
    case 0xa1:
      NEXT_B(lo);
      addr = rd16(cpu, (lo + cpu->x) & 0xff);
      cpu->a = rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // LDX Immediate
    case 0xa2:
      NEXT_B(cpu->x);
      set_nz(cpu, cpu->x);
      break;

    // LDY Zero Page
    case 0xa4:
      NEXT_B(lo);
      cpu->y = rd(cpu, lo);
      set_nz(cpu, cpu->y);
      break;

    // LDA Zero Page
    case 0xa5:
      NEXT_B(lo);
      cpu->a = rd(cpu, lo);
      set_nz(cpu, cpu->a);
      break;

    // LDX Zero Page
    case 0xa6:
      NEXT_B(lo);
      cpu->x = rd(cpu, lo);
      set_nz(cpu, cpu->x);
      break;

    // TAY
    case 0xa8:
      cpu->y = cpu->a;
      set_nz(cpu, cpu->y);
      break;

    // LDA Immediate
    case 0xa9:
      NEXT_B(cpu->a);
      set_nz(cpu, cpu->a);
      break;

    // TAX
    case 0xaa:
      cpu->x = cpu->a;
      set_nz(cpu, cpu->x);
      break;

    // LDY Absolute
    case 0xac:
      NEXT_W_VAL(addr);
      cpu->y = rd(cpu, addr);
      set_nz(cpu, cpu->y);
      break;

    // LDA Absolute
    case 0xad:
      NEXT_W_VAL(addr);
      cpu->a = rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // LDX Absolute
    case 0xae:
      NEXT_W_VAL(addr);
      cpu->x = rd(cpu, addr);
      set_nz(cpu, cpu->x);
      break;

    // BCS
    case 0xb0:
      NEXT_B(sval);
      if (cpu->p & FLAG_C) cpu->pc += (int16_t)(int8_t)sval;
      break;

    // LDA (Indirect),Y
    case 0xb1:
      NEXT_B(lo);
      addr = rd16(cpu, lo) + cpu->y;
      cpu->a = rd(cpu, addr);
      set_nz(cpu, cpu->a);
      break;

    // LDY Zero Page,X
    case 0xb4:
      NEXT_B(lo);
      cpu->y = rd(cpu, (lo + cpu->x) & 0xff);
      set_nz(cpu, cpu->y);
      break;

    // LDA Zero Page,X
    case 0xb5:
      NEXT_B(lo);
      cpu->a = rd(cpu, (lo + cpu->x) & 0xff);
      set_nz(cpu, cpu->a);
      break;

    // LDX Zero Page,Y
    case 0xb6:
      NEXT_B(lo);
      cpu->x = rd(cpu, (lo + cpu->y) & 0xff);
      set_nz(cpu, cpu->x);
      break;

    // CLV
    case 0xb8:
      cpu->p &= ~FLAG_V;
      break;

    // LDA Absolute,Y
    case 0xb9:
      NEXT_W_VAL(addr);
      cpu->a = rd(cpu, addr + cpu->y);
      set_nz(cpu, cpu->a);
      break;

    // TSX
    case 0xba:
      cpu->x = cpu->s;
      set_nz(cpu, cpu->x);
      break;

    // LDY Absolute,X
    case 0xbc:
      NEXT_W_VAL(addr);
      cpu->y = rd(cpu, addr + cpu->x);
      set_nz(cpu, cpu->y);
      break;

    // LDA Absolute,X
    case 0xbd:
      NEXT_W_VAL(addr);
      cpu->a = rd(cpu, addr + cpu->x);
      set_nz(cpu, cpu->a);
      break;

    // LDX Absolute,Y
    case 0xbe:
      NEXT_W_VAL(addr);
      cpu->x = rd(cpu, addr + cpu->y);
      set_nz(cpu, cpu->x);
      break;

    // CPY Immediate
    case 0xc0:
      NEXT_B(val);
      tmp = cpu->y - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->y >= val ? FLAG_C : 0);
      break;

    // CMP (Indirect,X)
    case 0xc1:
      NEXT_B(lo);
      addr = rd16(cpu, (lo + cpu->x) & 0xff);
      val = rd(cpu, addr);
      tmp = cpu->a - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->a >= val ? FLAG_C : 0);
      break;

    // CPY Zero Page
    case 0xc4:
      NEXT_B(lo);
      val = rd(cpu, lo);
      tmp = cpu->y - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->y >= val ? FLAG_C : 0);
      break;

    // CMP Zero Page
    case 0xc5:
      NEXT_B(lo);
      val = rd(cpu, lo);
      tmp = cpu->a - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->a >= val ? FLAG_C : 0);
      break;

    // DEC Zero Page
    case 0xc6:
      NEXT_B(lo);
      val = rd(cpu, lo) - 1;
      set_nz(cpu, val);
      wr(cpu, lo, val);
      break;

    // INY
    case 0xc8:
      cpu->y++;
      set_nz(cpu, cpu->y);
      break;

    // CMP Immediate
    case 0xc9:
      NEXT_B(val);
      tmp = cpu->a - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->a >= val ? FLAG_C : 0);
      break;

    // DEX
    case 0xca:
      cpu->x--;
      set_nz(cpu, cpu->x);
      break;

    // CPY Absolute
    case 0xcc:
      NEXT_W_VAL(addr);
      val = rd(cpu, addr);
      tmp = cpu->y - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->y >= val ? FLAG_C : 0);
      break;

    // CMP Absolute
    case 0xcd:
      NEXT_W_VAL(addr);
      val = rd(cpu, addr);
      tmp = cpu->a - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->a >= val ? FLAG_C : 0);
      break;

    // DEC Absolute
    case 0xce:
      NEXT_W_VAL(addr);
      val = rd(cpu, addr) - 1;
      set_nz(cpu, val);
      wr(cpu, addr, val);
      break;

    // BNE
    case 0xd0:
      NEXT_B(sval);
      if (!(cpu->p & FLAG_Z)) cpu->pc += (int16_t)(int8_t)sval;
      break;

    // CMP (Indirect),Y
    case 0xd1:
      NEXT_B(lo);
      addr = rd16(cpu, lo) + cpu->y;
      val = rd(cpu, addr);
      tmp = cpu->a - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->a >= val ? FLAG_C : 0);
      break;

    // CMP Zero Page,X
    case 0xd5:
      NEXT_B(lo);
      val = rd(cpu, (lo + cpu->x) & 0xff);
      tmp = cpu->a - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->a >= val ? FLAG_C : 0);
      break;

    // DEC Zero Page,X
    case 0xd6:
      NEXT_B(lo);
      lo = (lo + cpu->x) & 0xff;
      val = rd(cpu, lo) - 1;
      set_nz(cpu, val);
      wr(cpu, lo, val);
      break;

    // CLD
    case 0xd8:
      cpu->p &= ~FLAG_D;
      break;

    // CMP Absolute,Y
    case 0xd9:
      NEXT_W_VAL(addr);
      addr += cpu->y;
      val = rd(cpu, addr);
      tmp = cpu->a - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->a >= val ? FLAG_C : 0);
      break;

    // CMP Absolute,X
    case 0xdd:
      NEXT_W_VAL(addr);
      addr += cpu->x;
      val = rd(cpu, addr);
      tmp = cpu->a - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->a >= val ? FLAG_C : 0);
      break;

    // DEC Absolute,X
    case 0xde:
      NEXT_W_VAL(addr);
      addr += cpu->x;
      val = rd(cpu, addr) - 1;
      set_nz(cpu, val);
      wr(cpu, addr, val);
      break;

    // CPX Immediate
    case 0xe0:
      NEXT_B(val);
      tmp = cpu->x - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->x >= val ? FLAG_C : 0);
      break;

    // SBC (Indirect,X)
    case 0xe1: {
      NEXT_B(lo);
      addr = rd16(cpu, (lo + cpu->x) & 0xff);
      val = rd(cpu, addr);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a - val - (1 - carry);
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res >= 0 ? FLAG_C : 0) |
               (((cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // CPX Zero Page
    case 0xe4:
      NEXT_B(lo);
      val = rd(cpu, lo);
      tmp = cpu->x - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->x >= val ? FLAG_C : 0);
      break;

    // SBC Zero Page
    case 0xe5: {
      NEXT_B(lo);
      val = rd(cpu, lo);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a - val - (1 - carry);
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res >= 0 ? FLAG_C : 0) |
               (((cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // INC Zero Page
    case 0xe6:
      NEXT_B(lo);
      val = rd(cpu, lo) + 1;
      set_nz(cpu, val);
      wr(cpu, lo, val);
      break;

    // INX
    case 0xe8:
      cpu->x++;
      set_nz(cpu, cpu->x);
      break;

    // SBC Immediate
    case 0xe9: {
      NEXT_B(val);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a - val - (1 - carry);
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res >= 0 ? FLAG_C : 0) |
               (((cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // NOP
    case 0xea:
      break;

    // CPX Absolute
    case 0xec:
      NEXT_W_VAL(addr);
      val = rd(cpu, addr);
      tmp = cpu->x - val;
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_Z | FLAG_C)) | (tmp & 0x80) | (tmp == 0 ? FLAG_Z : 0) | (cpu->x >= val ? FLAG_C : 0);
      break;

    // SBC Absolute
    case 0xed: {
      NEXT_W_VAL(addr);
      val = rd(cpu, addr);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a - val - (1 - carry);
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res >= 0 ? FLAG_C : 0) |
               (((cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // INC Absolute
    case 0xee:
      NEXT_W_VAL(addr);
      val = rd(cpu, addr) + 1;
      set_nz(cpu, val);
      wr(cpu, addr, val);
      break;

    // BEQ
    case 0xf0:
      NEXT_B(sval);
      if (cpu->p & FLAG_Z) cpu->pc += (int16_t)(int8_t)sval;
      break;

    // SBC (Indirect),Y
    case 0xf1: {
      NEXT_B(lo);
      addr = rd16(cpu, lo) + cpu->y;
      val = rd(cpu, addr);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a - val - (1 - carry);
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res >= 0 ? FLAG_C : 0) |
               (((cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // SBC Zero Page,X
    case 0xf5: {
      NEXT_B(lo);
      val = rd(cpu, (lo + cpu->x) & 0xff);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a - val - (1 - carry);
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res >= 0 ? FLAG_C : 0) |
               (((cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // INC Zero Page,X
    case 0xf6:
      NEXT_B(lo);
      lo = (lo + cpu->x) & 0xff;
      val = rd(cpu, lo) + 1;
      set_nz(cpu, val);
      wr(cpu, lo, val);
      break;

    // SED
    case 0xf8:
      cpu->p |= FLAG_D;
      break;

    // SBC Absolute,Y
    case 0xf9: {
      NEXT_W_VAL(addr);
      addr += cpu->y;
      val = rd(cpu, addr);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a - val - (1 - carry);
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res >= 0 ? FLAG_C : 0) |
               (((cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // SBC Absolute,X
    case 0xfd: {
      NEXT_W_VAL(addr);
      addr += cpu->x;
      val = rd(cpu, addr);
      int carry = cpu->p & FLAG_C;
      int res = cpu->a - val - (1 - carry);
      cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
               (res >= 0 ? FLAG_C : 0) |
               (((cpu->a ^ val) & (cpu->a ^ res) & 0x80) ? FLAG_V : 0);
      cpu->a = res & 0xff;
      set_nz(cpu, cpu->a);
      break;
    }

    // INC Absolute,X
    case 0xfe:
      NEXT_W_VAL(addr);
      addr += cpu->x;
      val = rd(cpu, addr) + 1;
      set_nz(cpu, val);
      wr(cpu, addr, val);
      break;

    // All illegal opcodes treated as NOP
    default:
      break;
  }

  return 0;
}

uint16_t m6502_pc(m6502_t *cpu) {
  return cpu->pc;
}

int m6502_halted(m6502_t *cpu) {
  return 0;
}
