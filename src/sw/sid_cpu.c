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
  if (addr >= 0xd400 && addr <= 0xd41f && cpu->io_read) return cpu->io_read(addr);
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

static inline uint16_t rd16zp(m6502_t *cpu, uint8_t zp) {
  return rd(cpu, zp) | (rd(cpu, (uint8_t) (zp + 1)) << 8);
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


/* ADC/SBC with the decimal mode of the NMOS 6502 (some players keep counters in BCD). */
static void adc_op(m6502_t *cpu, uint8_t val) {
  unsigned a = cpu->a, c = (cpu->p & FLAG_C) ? 1u : 0u;
  unsigned bin = a + val + c;
  if (cpu->p & FLAG_D) {
    unsigned lo = (a & 0xf) + (val & 0xf) + c;
    unsigned hi = (a >> 4) + (val >> 4);
    if (lo > 9) { lo += 6; hi++; }
    unsigned v = (hi << 4) | (lo & 0xf);
    if (hi > 9) v += 0x60;
    cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
             (bin > 0xff ? FLAG_C : 0) |
             ((~(a ^ val) & (a ^ bin) & 0x80) ? FLAG_V : 0);
    cpu->a = (uint8_t) v;
    set_nz(cpu, cpu->a);
    return;
  }
  cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
           (bin > 0xff ? FLAG_C : 0) |
           ((~(a ^ val) & (a ^ bin) & 0x80) ? FLAG_V : 0);
  cpu->a = (uint8_t) bin;
  set_nz(cpu, cpu->a);
}

static void sbc_op(m6502_t *cpu, uint8_t val) {
  unsigned a = cpu->a, c = (cpu->p & FLAG_C) ? 1u : 0u;
  int res = (int) a - (int) val - (int) (1 - c);
  if (cpu->p & FLAG_D) {
    int lo = (int) (a & 0xf) - (int) (val & 0xf) - (int) (1 - c);
    int hi = (int) (a >> 4) - (int) (val >> 4);
    if (lo < 0) { lo -= 6; hi--; }
    if (hi < 0) hi -= 6;
    cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
             (res >= 0 ? FLAG_C : 0) |
             (((a ^ val) & (a ^ (unsigned) res) & 0x80) ? FLAG_V : 0);
    cpu->a = (uint8_t) (((hi << 4) | (lo & 0xf)) & 0xff);
    set_nz(cpu, cpu->a);
    return;
  }
  cpu->p = (cpu->p & ~(FLAG_N | FLAG_V | FLAG_Z | FLAG_C)) |
           (res >= 0 ? FLAG_C : 0) |
           (((a ^ val) & (a ^ (unsigned) res) & 0x80) ? FLAG_V : 0);
  cpu->a = (uint8_t) res;
  set_nz(cpu, cpu->a);
}


/* The NMOS 6502 illegal operations the SID players use. Multi-byte ones must consume their
 * operand, otherwise the CPU desynchronises and the tune runs into the weeds. Values:
 * kind << 4 | mode. */
enum { IM_IMM = 0, IM_ZP, IM_ZPX, IM_ZPY, IM_ABS, IM_ABSX, IM_ABSY, IM_IZX, IM_IZY };
enum { IK_NOP = 1, IK_SLO, IK_RLA, IK_SRE, IK_RRA, IK_SAX, IK_LAX, IK_DCP, IK_ISC,
       IK_ANC, IK_ALR, IK_ARR, IK_SBX, IK_LAS };

static const uint8_t ill_tbl[256] = {
  [0x03] = (IK_SLO << 4) | IM_IZX, [0x07] = (IK_SLO << 4) | IM_ZP,  [0x0f] = (IK_SLO << 4) | IM_ABS,
  [0x13] = (IK_SLO << 4) | IM_IZY, [0x17] = (IK_SLO << 4) | IM_ZPX, [0x1b] = (IK_SLO << 4) | IM_ABSY,
  [0x1f] = (IK_SLO << 4) | IM_ABSX,
  [0x23] = (IK_RLA << 4) | IM_IZX, [0x27] = (IK_RLA << 4) | IM_ZP,  [0x2f] = (IK_RLA << 4) | IM_ABS,
  [0x33] = (IK_RLA << 4) | IM_IZY, [0x37] = (IK_RLA << 4) | IM_ZPX, [0x3b] = (IK_RLA << 4) | IM_ABSY,
  [0x3f] = (IK_RLA << 4) | IM_ABSX,
  [0x43] = (IK_SRE << 4) | IM_IZX, [0x47] = (IK_SRE << 4) | IM_ZP,  [0x4f] = (IK_SRE << 4) | IM_ABS,
  [0x53] = (IK_SRE << 4) | IM_IZY, [0x57] = (IK_SRE << 4) | IM_ZPX, [0x5b] = (IK_SRE << 4) | IM_ABSY,
  [0x5f] = (IK_SRE << 4) | IM_ABSX,
  [0x63] = (IK_RRA << 4) | IM_IZX, [0x67] = (IK_RRA << 4) | IM_ZP,  [0x6f] = (IK_RRA << 4) | IM_ABS,
  [0x73] = (IK_RRA << 4) | IM_IZY, [0x77] = (IK_RRA << 4) | IM_ZPX, [0x7b] = (IK_RRA << 4) | IM_ABSY,
  [0x7f] = (IK_RRA << 4) | IM_ABSX,
  [0x83] = (IK_SAX << 4) | IM_IZX, [0x87] = (IK_SAX << 4) | IM_ZP,  [0x8f] = (IK_SAX << 4) | IM_ABS,
  [0x97] = (IK_SAX << 4) | IM_ZPY,
  [0xa3] = (IK_LAX << 4) | IM_IZX, [0xa7] = (IK_LAX << 4) | IM_ZP,  [0xaf] = (IK_LAX << 4) | IM_ABS,
  [0xb3] = (IK_LAX << 4) | IM_IZY, [0xb7] = (IK_LAX << 4) | IM_ZPY, [0xbf] = (IK_LAX << 4) | IM_ABSY,
  [0xab] = (IK_LAX << 4) | IM_IMM,
  [0xc3] = (IK_DCP << 4) | IM_IZX, [0xc7] = (IK_DCP << 4) | IM_ZP,  [0xcf] = (IK_DCP << 4) | IM_ABS,
  [0xd3] = (IK_DCP << 4) | IM_IZY, [0xd7] = (IK_DCP << 4) | IM_ZPX, [0xdb] = (IK_DCP << 4) | IM_ABSY,
  [0xdf] = (IK_DCP << 4) | IM_ABSX,
  [0xe3] = (IK_ISC << 4) | IM_IZX, [0xe7] = (IK_ISC << 4) | IM_ZP,  [0xef] = (IK_ISC << 4) | IM_ABS,
  [0xf3] = (IK_ISC << 4) | IM_IZY, [0xf7] = (IK_ISC << 4) | IM_ZPX, [0xfb] = (IK_ISC << 4) | IM_ABSY,
  [0xff] = (IK_ISC << 4) | IM_ABSX,
  [0x0b] = (IK_ANC << 4) | IM_IMM, [0x2b] = (IK_ANC << 4) | IM_IMM,
  [0x4b] = (IK_ALR << 4) | IM_IMM, [0x6b] = (IK_ARR << 4) | IM_IMM,
  [0xcb] = (IK_SBX << 4) | IM_IMM, [0xbb] = (IK_LAS << 4) | IM_ABSY,
  /* multi-byte NOPs */
  [0x80] = (IK_NOP << 4) | IM_IMM, [0x82] = (IK_NOP << 4) | IM_IMM, [0x89] = (IK_NOP << 4) | IM_IMM,
  [0xc2] = (IK_NOP << 4) | IM_IMM, [0xe2] = (IK_NOP << 4) | IM_IMM,
  [0x04] = (IK_NOP << 4) | IM_ZP,  [0x44] = (IK_NOP << 4) | IM_ZP,  [0x64] = (IK_NOP << 4) | IM_ZP,
  [0x14] = (IK_NOP << 4) | IM_ZPX, [0x34] = (IK_NOP << 4) | IM_ZPX, [0x54] = (IK_NOP << 4) | IM_ZPX,
  [0x74] = (IK_NOP << 4) | IM_ZPX, [0xd4] = (IK_NOP << 4) | IM_ZPX, [0xf4] = (IK_NOP << 4) | IM_ZPX,
  [0x0c] = (IK_NOP << 4) | IM_ABS,
  [0x1c] = (IK_NOP << 4) | IM_ABSX, [0x3c] = (IK_NOP << 4) | IM_ABSX, [0x5c] = (IK_NOP << 4) | IM_ABSX,
  [0x7c] = (IK_NOP << 4) | IM_ABSX, [0xdc] = (IK_NOP << 4) | IM_ABSX, [0xfc] = (IK_NOP << 4) | IM_ABSX,
  [0x93] = (IK_NOP << 4) | IM_IZY,  [0x9f] = (IK_NOP << 4) | IM_ABSY,
  [0x9e] = (IK_NOP << 4) | IM_ABSY, [0x9c] = (IK_NOP << 4) | IM_ABSX,
  [0x9b] = (IK_NOP << 4) | IM_ABSY, [0x8b] = (IK_NOP << 4) | IM_IMM,
  [0x1a] = (IK_NOP << 4) | IM_IMM, [0x3a] = (IK_NOP << 4) | IM_IMM, [0x5a] = (IK_NOP << 4) | IM_IMM,
  [0x7a] = (IK_NOP << 4) | IM_IMM, [0xda] = (IK_NOP << 4) | IM_IMM, [0xfa] = (IK_NOP << 4) | IM_IMM,
};

static void do_illegal(m6502_t *cpu, uint8_t op) {
  uint8_t kind = ill_tbl[op] >> 4, mode = ill_tbl[op] & 0xf;
  uint16_t addr = 0;
  uint8_t val = 0;

  if (mode == IM_IMM) NEXT_B(val);
  else if (mode == IM_ZP) { uint8_t zp; NEXT_B(zp); addr = zp; }
  else if (mode == IM_ZPX) { uint8_t zp; NEXT_B(zp); addr = (uint8_t) (zp + cpu->x); }
  else if (mode == IM_ZPY) { uint8_t zp; NEXT_B(zp); addr = (uint8_t) (zp + cpu->y); }
  else if (mode == IM_ABS) NEXT_W_VAL(addr);
  else if (mode == IM_ABSX) { NEXT_W_VAL(addr); addr += cpu->x; }
  else if (mode == IM_ABSY) { NEXT_W_VAL(addr); addr += cpu->y; }
  else if (mode == IM_IZX) { uint8_t zp; NEXT_B(zp); addr = rd16zp(cpu, (uint8_t) (zp + cpu->x)); }
  else { uint8_t zp; NEXT_B(zp); addr = rd16zp(cpu, zp) + cpu->y; }

  if (kind == IK_NOP) return;

  if (kind == IK_LAX) {
    cpu->a = cpu->x = rd(cpu, addr);
    set_nz(cpu, cpu->a);
  } else if (kind == IK_SAX) {
    wr(cpu, addr, cpu->a & cpu->x);
  } else if (kind == IK_LAS) {
    cpu->a = cpu->x = (uint8_t) (rd(cpu, addr) & cpu->s);
    set_nz(cpu, cpu->a);
  } else if (kind == IK_SLO) {
    val = rd(cpu, addr);
    cpu->p = (cpu->p & ~FLAG_C) | ((val >> 7) & 1);
    val <<= 1;
    wr(cpu, addr, val);
    cpu->a |= val;
    set_nz(cpu, cpu->a);
  } else if (kind == IK_RLA) {
    val = rd(cpu, addr);
    uint8_t c = (cpu->p & FLAG_C) ? 1 : 0;
    cpu->p = (cpu->p & ~FLAG_C) | ((val >> 7) & 1);
    val = (uint8_t) ((val << 1) | c);
    wr(cpu, addr, val);
    cpu->a &= val;
    set_nz(cpu, cpu->a);
  } else if (kind == IK_SRE) {
    val = rd(cpu, addr);
    cpu->p = (cpu->p & ~FLAG_C) | (val & 1);
    val >>= 1;
    wr(cpu, addr, val);
    cpu->a ^= val;
    set_nz(cpu, cpu->a);
  } else if (kind == IK_RRA) {
    val = rd(cpu, addr);
    uint8_t c = (cpu->p & FLAG_C) ? 1 : 0;
    cpu->p = (cpu->p & ~FLAG_C) | (val & 1);
    val = (uint8_t) ((val >> 1) | (c << 7));
    wr(cpu, addr, val);
    adc_op(cpu, val);
  } else if (kind == IK_DCP) {
    val = (uint8_t) (rd(cpu, addr) - 1);
    wr(cpu, addr, val);
    cpu->p = (cpu->p & ~FLAG_C) | (cpu->a >= val ? FLAG_C : 0);
    set_nz(cpu, (uint8_t) (cpu->a - val));
  } else if (kind == IK_ISC) {
    val = (uint8_t) (rd(cpu, addr) + 1);
    wr(cpu, addr, val);
    sbc_op(cpu, val);
  } else if (kind == IK_ANC) {
    cpu->a &= val;
    set_nz(cpu, cpu->a);
    cpu->p = (cpu->p & ~FLAG_C) | ((cpu->a >> 7) & 1);
  } else if (kind == IK_ALR) {
    cpu->a &= val;
    cpu->p = (cpu->p & ~FLAG_C) | (cpu->a & 1);
    cpu->a >>= 1;
    set_nz(cpu, cpu->a);
  } else if (kind == IK_ARR) {
    cpu->a &= val;
    cpu->a = (uint8_t) ((cpu->a >> 1) | ((cpu->p & FLAG_C) ? 0x80 : 0));
    set_nz(cpu, cpu->a);
    cpu->p = (cpu->p & ~(FLAG_C | FLAG_V)) |
             ((cpu->a & 0x40) ? FLAG_C : 0) |
             ((((cpu->a >> 6) ^ (cpu->a >> 5)) & 1) ? FLAG_V : 0);
  } else if (kind == IK_SBX) {
    int t = (int) (cpu->a & cpu->x) - val;
    cpu->p = (cpu->p & ~FLAG_C) | (t >= 0 ? FLAG_C : 0);
    cpu->x = (uint8_t) t;
    set_nz(cpu, cpu->x);
  }
}

int m6502_step(m6502_t *cpu) {
  if (cpu->pc >= 0xfffe) return 1;   /* put the sentinel at the top of memory */
  
  uint8_t op = rd(cpu, cpu->pc);
  NEXT();
  if (ill_tbl[op]) { do_illegal(cpu, op); return 0; }
  
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
      addr = rd16zp(cpu, (uint8_t) (lo + cpu->x));
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
      addr = rd16zp(cpu, lo) + cpu->y;
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
      push(cpu, ((cpu->pc - 1) >> 8) & 0xff);
      push(cpu, (cpu->pc - 1) & 0xff);
      cpu->pc = addr;
      break;

    // AND (Indirect,X)
    case 0x21:
      NEXT_B(lo);
      addr = rd16zp(cpu, (uint8_t) (lo + cpu->x));
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
      addr = rd16zp(cpu, lo) + cpu->y;
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
      { uint8_t lo = pop(cpu), hi = pop(cpu); cpu->pc = (uint16_t) (lo | (hi << 8)); }
      break;

    // EOR (Indirect,X)
    case 0x41:
      NEXT_B(lo);
      addr = rd16zp(cpu, (uint8_t) (lo + cpu->x));
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
      addr = rd16zp(cpu, lo) + cpu->y;
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
      { uint8_t lo = pop(cpu), hi = pop(cpu); cpu->pc = (uint16_t) ((lo | (hi << 8)) + 1); }
      break;

    // ADC (Indirect,X)
    case 0x61: {
      NEXT_B(lo);
      addr = rd16zp(cpu, (uint8_t) (lo + cpu->x));
      val = rd(cpu, addr);
      adc_op(cpu, val);
      break;
    }

    // ADC Zero Page
    case 0x65: {
      NEXT_B(lo);
      val = rd(cpu, lo);
      adc_op(cpu, val);
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
      adc_op(cpu, val);
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
      adc_op(cpu, val);
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
      addr = rd16zp(cpu, lo) + cpu->y;
      val = rd(cpu, addr);
      adc_op(cpu, val);
      break;
    }

    // ADC Zero Page,X
    case 0x75: {
      NEXT_B(lo);
      val = rd(cpu, (lo + cpu->x) & 0xff);
      adc_op(cpu, val);
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
      adc_op(cpu, val);
      break;
    }

    // ADC Absolute,X
    case 0x7d: {
      NEXT_W_VAL(addr);
      addr += cpu->x;
      val = rd(cpu, addr);
      adc_op(cpu, val);
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
      addr = rd16zp(cpu, (uint8_t) (lo + cpu->x));
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
      addr = rd16zp(cpu, lo) + cpu->y;
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
      addr = rd16zp(cpu, (uint8_t) (lo + cpu->x));
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
      addr = rd16zp(cpu, lo) + cpu->y;
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
      addr = rd16zp(cpu, (uint8_t) (lo + cpu->x));
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
      addr = rd16zp(cpu, lo) + cpu->y;
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
      addr = rd16zp(cpu, (uint8_t) (lo + cpu->x));
      val = rd(cpu, addr);
      sbc_op(cpu, val);
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
      sbc_op(cpu, val);
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
      sbc_op(cpu, val);
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
      sbc_op(cpu, val);
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
      addr = rd16zp(cpu, lo) + cpu->y;
      val = rd(cpu, addr);
      sbc_op(cpu, val);
      break;
    }

    // SBC Zero Page,X
    case 0xf5: {
      NEXT_B(lo);
      val = rd(cpu, (lo + cpu->x) & 0xff);
      sbc_op(cpu, val);
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
      sbc_op(cpu, val);
      break;
    }

    // SBC Absolute,X
    case 0xfd: {
      NEXT_W_VAL(addr);
      addr += cpu->x;
      val = rd(cpu, addr);
      sbc_op(cpu, val);
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
