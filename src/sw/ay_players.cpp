// PT3 / PT2 / STC replayers for the two YM2149s.
//
// This is a clean implementation of the module formats used by the player. The players only
// produce the AY register stream at 50 Hz; the chips themselves are in the FPGA (menu_soc).
// The file layouts and pattern semantics are the public Pro Tracker 2/3 and Sound Tracker
// formats. No third-party replayer source is included here.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { K_PT3 = 0, K_PT2 = 1, K_STC = 2 };

// The period of the C-1..B-7 notes (Pro Tracker / Sound Tracker note table, 96 entries).
static const uint16_t note_tab[96] = {
  0x0ef8, 0x0e10, 0x0d60, 0x0c80, 0x0bd8, 0x0b28, 0x0a88, 0x09f0,
  0x0960, 0x08e0, 0x0858, 0x07e0, 0x077c, 0x0708, 0x06b0, 0x0640,
  0x05ec, 0x0594, 0x0544, 0x04f8, 0x04b0, 0x0470, 0x042c, 0x03fd,
  0x03be, 0x0384, 0x0358, 0x0320, 0x02f6, 0x02ca, 0x02a2, 0x027c,
  0x0258, 0x0238, 0x0216, 0x01f8, 0x01df, 0x01c2, 0x01ac, 0x0190,
  0x017b, 0x0165, 0x0151, 0x013e, 0x012c, 0x011c, 0x010a, 0x00fc,
  0x00ef, 0x00e1, 0x00d6, 0x00c8, 0x00bd, 0x00b2, 0x00a8, 0x009f,
  0x0096, 0x008e, 0x0085, 0x007e, 0x0077, 0x0070, 0x006b, 0x0064,
  0x005e, 0x0059, 0x0054, 0x004f, 0x004b, 0x0047, 0x0042, 0x003f,
  0x003b, 0x0038, 0x0035, 0x0032, 0x002f, 0x002c, 0x002a, 0x0027,
  0x0025, 0x0023, 0x0021, 0x001f, 0x001d, 0x001c, 0x001a, 0x0019,
  0x0017, 0x0016, 0x0015, 0x0013, 0x0012, 0x0011, 0x0010, 0x000f
};

struct Chan {
  uint16_t pat;                 // current byte in the pattern stream
  uint16_t smp;                 // sample data
  uint16_t orn;                 // ornament data
  uint16_t tone;                // period written to the chip
  int16_t tone_acc;             // PT3 sample accumulator (b1 bit 6)
  uint8_t slen, sloop, spos;    // sample length / loop / position
  uint8_t olen, oloop, opos;    // ornament length / loop / position
  uint8_t vol, note, amp, skip, skip_left;
  uint8_t enabled, env;
  int16_t slide, slide_step, slide_left, slide_delay;
  int8_t amp_slide;
  uint8_t on_delay, off_delay, on_left, on_state;
  uint8_t s_tik;                // STC sample tick counter (0xff: stopped)
};

struct Player {
  uint8_t kind;
  const uint8_t *m;
  uint32_t n;
  uint8_t delay, delay_left, pos, loop, pos_count, trans;
  uint16_t pat_tab, smp_tab, orn_tab, pos_tab;
  uint8_t noise_base, noise_add, env_lo, env_hi, env_delay, env_left;
  int16_t env_slide, env_add;
  Chan ch[3];
};

static Player g_pl[2];
static int g_nchips, g_active, g_kind;
static uint32_t g_ticks, g_length;
static char g_title[64], g_error[48];
static uint8_t g_regs[2][16];
static uint16_t g_written[2];

// One pass through the order list: the formats have no length field, but the players use
// 64 rows per pattern and a fixed number of ticks per row. This is the same estimate the
// UI needs for the progress bar; the actual loop can be shorter or longer by a delay effect.
static uint32_t estimate_length(const Player &p) {
  uint32_t positions = p.pos_count ? p.pos_count : 1;
  uint32_t delay = p.delay ? p.delay : 1;
  return positions * 64u * delay;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }
static int clamp_note(int n) { return n < 0 ? 0 : (n > 95 ? 95 : n); }
static uint8_t clamp_amp(int a) { return (uint8_t) (a < 0 ? 0 : (a > 15 ? 15 : a)); }
static int iabs(int x) { return x < 0 ? -x : x; }
static void wr_reg(int chip, int reg, int val) {
  if (reg < 0 || reg > 15) return;
  g_regs[chip & 1][reg] = (uint8_t) val;
  g_written[chip & 1] |= (uint16_t) (1u << reg);
}
static void set_error(const char *s) {
  int i = 0;
  for (; s && s[i] && i < (int) sizeof(g_error) - 1; i++) g_error[i] = s[i];
  g_error[i] = 0;
}
static void set_title(const uint8_t *p, int n, int skip) {
  int i = 0;
  for (; i < n && i + skip < n + skip && i < (int) sizeof(g_title) - 1; i++) {
    uint8_t c = p[skip + i];
    if (c < 32 || c > 126) break;
    g_title[i] = (char) c;
  }
  while (i > 0 && g_title[i - 1] == ' ') i--;
  g_title[i] = 0;
}

static void set_sample_pt3(Chan &c, const uint8_t *m, uint32_t n, int idx) {
  uint16_t p = rd16(m + 0x69 + (idx & 31) * 2);
  if ((uint32_t) p + 1 >= n) return;
  c.smp = p + 2;
  c.sloop = m[p];
  c.slen = m[p + 1];
  if (!c.slen) c.slen = 1;
  if (c.spos >= c.slen) c.spos = c.sloop;
}
static void set_orn_pt3(Chan &c, const uint8_t *m, uint32_t n, int idx) {
  uint16_t p = rd16(m + 0xa9 + (idx & 31) * 2);
  if ((uint32_t) p + 1 >= n) return;
  c.orn = p + 2;
  c.oloop = m[p];
  c.olen = m[p + 1];
  if (!c.olen) c.olen = 1;
  if (c.opos >= c.olen) c.opos = c.oloop;
}
static void set_sample_pt2(Chan &c, const uint8_t *m, uint32_t n, int idx) {
  uint16_t p = rd16(m + 3 + (idx & 31) * 2);
  if ((uint32_t) p + 1 >= n) return;
  c.smp = p + 2;
  c.slen = m[p];
  c.sloop = m[p + 1];
  if (!c.slen) c.slen = 1;
  if (c.spos >= c.slen) c.spos = c.sloop;
}
static void set_orn_pt2(Chan &c, const uint8_t *m, uint32_t n, int idx) {
  uint16_t p = rd16(m + 67 + (idx & 31) * 2);
  if ((uint32_t) p + 1 >= n) return;
  c.orn = p + 2;
  c.olen = m[p];
  c.oloop = m[p + 1];
  if (!c.olen) c.olen = 1;
  if (c.opos >= c.olen) c.opos = c.oloop;
}

static void set_pattern(Player &p, int pos) {
  if (!p.pos_count) p.pos_count = 1;
  if (pos < 0 || pos >= p.pos_count) pos = 0;
  p.pos = (uint8_t) pos;
  if (p.kind == K_PT3) {
    uint8_t v = p.m[0xc9 + pos];
    // The position list stores pattern * 3; two bytes per pattern slot make *6.
    uint16_t base = p.pat_tab + v * 2;
    for (int c = 0; c < 3; c++) p.ch[c].pat = rd16(p.m + base + c * 2);
  } else if (p.kind == K_PT2) {
    uint8_t v = p.m[131 + pos];
    uint16_t base = p.pat_tab + v * 6;
    for (int c = 0; c < 3; c++) p.ch[c].pat = rd16(p.m + base + c * 2);
  }
}

// ---------------------------------------------------------------- PT3

static void pt3_emit(Player &p, Chan &c, int ci, int &mixer) {
  c.amp = 0;
  if (!c.enabled) return;
  uint32_t a = c.smp + (uint32_t) c.spos * 4;
  if (a + 3 >= p.n) return;
  uint8_t b0 = p.m[a], b1 = p.m[a + 1];
  int16_t sample_tone = (int16_t) rd16(p.m + a + 2);
  int16_t tone = (int16_t) (sample_tone + c.tone_acc);
  int j = clamp_note(c.note + (int8_t) p.m[c.orn + c.opos]);
  if (b1 & 0x40) c.tone_acc = tone;
  c.tone = (uint16_t) ((tone + c.slide + note_tab[j]) & 0xfff);

  int amp = b1 & 15;
  if (b0 & 0x80) {
    if (b0 & 0x40) { if (c.amp_slide < 15) c.amp_slide++; }
    else if (c.amp_slide > -15) c.amp_slide--;
  }
  amp = clamp_amp(amp + c.amp_slide);
  c.amp = (uint8_t) ((c.vol * amp + 7) / 15);
  if (!(b0 & 1) && c.env) c.amp |= 16;
  mixer |= (b1 >> 1) & 0x48;   // the PT3 mixer bits; shifted once per channel below
  if (b1 & 0x80) {
    int e = ((b0 >> 1) & 0x0f) + c.amp_slide;
    p.env_add = (int16_t) e;
  } else {
    p.noise_add = (b0 >> 1) & 31;
  }
  c.spos++;
  if (c.spos >= c.slen) c.spos = c.sloop;
  c.opos++;
  if (c.opos >= c.olen) c.opos = c.oloop;
  if (c.slide_left > 0) {
    c.slide_left--;
    if (c.slide_left == 0) {
      c.slide = (int16_t) (c.slide + c.slide_step);
      c.slide_left = c.slide_delay;
    }
  }
}

static void pt3_row(Player &p, Chan &c, int chip) {
  uint8_t flags[8];
  int nf = 0;
  uint32_t a = c.pat;
  int end = 0;
  while (a < p.n) {
    uint8_t v = p.m[a++];
    if (v == 0) { a--; break; }   // #00: end of this track, leave the pointer at it
    if (v >= 0xf0) {
      set_orn_pt3(c, p.m, p.n, v - 0xf0);
      if (a < p.n) set_sample_pt3(c, p.m, p.n, p.m[a++] / 2);
      c.env = 0; c.opos = 0; c.spos = 0;
    } else if (v >= 0xd1 && v <= 0xef) {
      set_sample_pt3(c, p.m, p.n, v - 0xd0);
    } else if (v == 0xd0) {
      end = 1;
    } else if (v >= 0xc1 && v <= 0xcf) {
      c.vol = (uint8_t) (v - 0xc0);
    } else if (v == 0xc0) {
      // No note, but the current sample keeps sounding; only #D0/#00 end the row.
      end = 1;
    } else if (v >= 0xb2 && v <= 0xbf) {
      c.env = 1;
      wr_reg(chip, 13, v - 0xb1);
      if (a + 1 < p.n) { p.env_hi = p.m[a++]; p.env_lo = p.m[a++]; }
      c.opos = 0; p.env_slide = 0;
    } else if (v == 0xb1) {
      if (a < p.n) c.skip = p.m[a++];
    } else if (v == 0xb0) {
      c.env = 0; c.opos = 0;
    } else if (v >= 0x50 && v <= 0xaf) {
      c.note = (uint8_t) (v - 0x50);
      c.spos = c.opos = 0; c.slide = 0; c.amp_slide = 0;
      c.enabled = 1; end = 1;
    } else if (v >= 0x40 && v <= 0x4f) {
      set_orn_pt3(c, p.m, p.n, v - 0x40); c.opos = 0;
    } else if (v >= 0x20 && v <= 0x3f) {
      p.noise_base = (uint8_t) (v - 0x20);
    } else if (v >= 0x10 && v <= 0x1f) {
      if (v == 0x10) c.env = 0;
      else {
        c.env = 1; wr_reg(chip, 13, v - 0x10);
        if (a + 1 < p.n) { p.env_hi = p.m[a++]; p.env_lo = p.m[a++]; }
        p.env_slide = 0;
      }
      if (a < p.n) set_sample_pt3(c, p.m, p.n, p.m[a++] / 2);
      c.opos = 0;
    } else if (v >= 1 && v <= 9 && v != 6 && v != 7) {
      if (nf < 8) flags[nf++] = v;
    }
    if (end) break;
  }
  c.pat = (uint16_t) a;
  // The arguments of the effects follow the note in reverse order of their flags.
  for (int i = nf - 1; i >= 0 && a + 1 < p.n; i--) {
    uint8_t f = flags[i];
    if (f == 1) {
      if (a + 2 >= p.n) break;
      c.slide_delay = p.m[a++];
      c.slide_left = c.slide_delay;
      c.slide_step = (int16_t) rd16(p.m + a); a += 2;
    } else if (f == 2) {
      if (a + 5 >= p.n) break;
      c.slide_delay = p.m[a++];
      a += 3;
      c.slide_step = (int16_t) rd16(p.m + a); a += 2;
      c.slide_left = c.slide_delay;
    } else if (f == 3) {
      c.spos = p.m[a++];
    } else if (f == 4) {
      c.opos = p.m[a++];
    } else if (f == 5) {
      if (a + 1 >= p.n) break;
      c.on_delay = p.m[a++]; c.off_delay = p.m[a++];
      c.on_left = c.on_delay;
    } else if (f == 8) {
      if (a + 2 >= p.n) break;
      p.env_delay = p.m[a++];
      p.env_add = (int16_t) rd16(p.m + a); a += 2;
      p.env_left = p.env_delay;
    } else if (f == 9) {
      p.delay = p.m[a++];
    }
  }
  c.skip_left = c.skip;
}

static void pt3_tick(Player &p, int chip) {
  if (--p.delay_left == 0) {
    p.delay_left = p.delay ? p.delay : 1;
    if (p.pos >= p.pos_count) p.pos = p.loop;
    if (p.ch[0].pat < p.n && p.m[p.ch[0].pat] == 0) {
      p.pos++;
      if (p.pos >= p.pos_count) p.pos = p.loop;
      set_pattern(p, p.pos);
    }
    for (int i = 0; i < 3; i++) {
      if (p.ch[i].skip_left) { p.ch[i].skip_left--; if (p.ch[i].skip_left) continue; }
      pt3_row(p, p.ch[i], chip);
    }
  }
  int mixer = 0;
  p.env_add = 0;
  for (int i = 0; i < 3; i++) { pt3_emit(p, p.ch[i], i, mixer); mixer >>= 1; }
  wr_reg(chip, 7, mixer);
  for (int i = 0; i < 3; i++) {
    wr_reg(chip, i * 2, p.ch[i].tone & 0xff);
    wr_reg(chip, i * 2 + 1, (p.ch[i].tone >> 8) & 15);
    wr_reg(chip, 8 + i, p.ch[i].amp);
  }
  wr_reg(chip, 6, (p.noise_base + p.noise_add) & 31);
  unsigned env = ((unsigned) p.env_hi << 8 | p.env_lo) + p.env_add + p.env_slide;
  wr_reg(chip, 11, env & 0xff);
  wr_reg(chip, 12, (env >> 8) & 0xff);
  if (p.env_left) {
    if (--p.env_left == 0) { p.env_left = p.env_delay; p.env_slide += (int16_t) p.env_add; }
  }
}

static int pt3_start(Player &p, const uint8_t *m, uint32_t n) {
  if (!m || n < 0xcb) return -1;
  if (memcmp(m, "ProTracker 3.", 13) && memcmp(m, "Vortex Tracke", 13)) return -1;
  p.kind = K_PT3; p.m = m; p.n = n;
  p.pat_tab = rd16(m + 0x67);
  // The order list is terminated by #ff; +101 is not the position count in every
  // tracker version.
  p.pos_count = 0;
  while (p.pos_count < 255 && (uint32_t) (0xc9 + p.pos_count) < n &&
         m[0xc9 + p.pos_count] != 0xff)
    p.pos_count++;
  if (!p.pos_count) p.pos_count = 1;
  p.loop = m[0x66];
  if (p.loop >= p.pos_count) p.loop = 0;
  p.delay = m[0x64];
  if (!p.delay) p.delay = 1;
  p.delay_left = 1;
  p.pos = 0;
  p.noise_base = 0; p.env_lo = p.env_hi = 0;
  p.env_slide = p.env_add = 0; p.env_delay = p.env_left = 0;
  for (int i = 0; i < 3; i++) {
    Chan &c = p.ch[i];
    memset(&c, 0, sizeof(c));
    c.vol = 15; c.skip = 0; c.skip_left = 0;
    c.smp = 0; c.orn = 0;
  }
  set_pattern(p, 0);
  if ((uint32_t) p.pat_tab + 6 > n) return -1;
  for (int i = 0; i < 3; i++) {
    Chan &c = p.ch[i];
    uint16_t op = rd16(m + 0xa9);
    uint16_t sp = rd16(m + 0x69 + 2);
    if ((uint32_t) op + 1 < n) { c.oloop = m[op]; c.olen = m[op + 1]; c.orn = op + 2; if (!c.olen) c.olen = 1; }
    if ((uint32_t) sp + 1 < n) { c.sloop = m[sp]; c.slen = m[sp + 1]; c.smp = sp + 2; if (!c.slen) c.slen = 1; }
    c.spos = c.opos = 0;
    c.amp = 0; c.amp_slide = 0;
  }
  set_title(m, 32, 0x1e);
  return 0;
}

// ---------------------------------------------------------------- PT2

static void pt2_emit(Player &p, Chan &c, int ci, int &mixer) {
  c.amp = 0;
  if (!c.enabled) return;
  uint32_t a = c.smp + (uint32_t) c.spos * 3;
  if (a + 2 >= p.n) return;
  uint8_t b0 = p.m[a], b1 = p.m[a + 1], b2 = p.m[a + 2];
  int16_t tone = (int16_t) (b2 | ((b1 & 15) << 8));
  if (!(b0 & 4)) tone = (int16_t) -tone;
  int j = clamp_note(c.note + (int8_t) p.m[c.orn + c.opos]);
  c.tone = (uint16_t) ((tone + c.slide + note_tab[j]) & 0xfff);
  int amp = ((c.vol * 17 + (c.vol > 7)) * (b1 >> 4)) / 256;
  c.amp = clamp_amp(amp);
  if (c.env) c.amp |= 16;
  if (b0 & 2) mixer |= 1 << ci;       // tone off
  if (b0 & 1) mixer |= 8 << ci;       // noise off
  if (!(b0 & 1)) p.noise_add = (b0 >> 3) & 31;
  c.spos++;
  if (c.spos >= c.slen) c.spos = c.sloop;
  c.opos++;
  if (c.opos >= c.olen) c.opos = c.oloop;
  if (c.slide_left > 0) {
    c.slide_left--;
    if (c.slide_left == 0) { c.slide += c.slide_step; c.slide_left = c.slide_delay; }
  }
}

static void pt2_row(Player &p, Chan &c, int chip) {
  uint32_t a = c.pat;
  int end = 0, gliss = 0;
  while (a < p.n) {
    uint8_t v = p.m[a++];
    if (v == 0) { a--; break; }   // #00: end of this track, leave the pointer at it
    if (v >= 0xe1) {
      set_sample_pt2(c, p.m, p.n, v - 0xe0);
    } else if (v == 0xe0) {
      c.enabled = 0; c.slide = 0; end = 1;
    } else if (v >= 0x80 && v <= 0xdf) {
      c.note = (uint8_t) (v - 0x80);
      c.spos = c.opos = 0; c.slide = 0;
      c.enabled = 1; end = 1;
    } else if (v == 0x7f) {
      c.env = 0;
    } else if (v >= 0x71 && v <= 0x7e) {
      c.env = 1; wr_reg(chip, 13, v - 0x70);
      if (a + 1 < p.n) { wr_reg(chip, 11, p.m[a++]); wr_reg(chip, 12, p.m[a++]); }
    } else if (v == 0x70) {
      end = 1;
    } else if (v >= 0x60 && v <= 0x6f) {
      set_orn_pt2(c, p.m, p.n, v - 0x60); c.opos = 0;
    } else if (v >= 0x20 && v <= 0x5f) {
      c.skip = (uint8_t) (v - 0x20);
    } else if (v >= 0x10 && v <= 0x1f) {
      c.vol = (uint8_t) (v - 0x10);
    } else if (v == 0xf) {
      if (a < p.n) p.delay = p.m[a++];
    } else if (v == 0xe) {
      if (a < p.n) { c.slide_step = (int8_t) p.m[a++]; c.slide_delay = 1; c.slide_left = 1; gliss = 1; }
    } else if (v == 0xd) {
      if (a + 2 < p.n) { c.slide_step = iabs((int8_t) p.m[a]); a += 2; c.slide_delay = 1; c.slide_left = 1; gliss = 1; }
    } else if (v == 0xc) {
      c.slide = 0;
    } else if (v < 0x10) {
      if (a < p.n) p.noise_add = p.m[a++] & 31;
    }
    if (end) break;
  }
  c.pat = (uint16_t) a;
  (void) gliss;
  c.skip_left = c.skip;
}

static void pt2_tick(Player &p, int chip) {
  if (--p.delay_left == 0) {
    p.delay_left = p.delay ? p.delay : 1;
    if (p.pos >= p.pos_count) p.pos = p.loop;
    if (p.ch[0].pat < p.n && p.m[p.ch[0].pat] == 0) {
      p.pos++;
      if (p.pos >= p.pos_count) p.pos = p.loop;
      set_pattern(p, p.pos);
    }
    for (int i = 0; i < 3; i++) {
      if (p.ch[i].skip_left) { p.ch[i].skip_left--; continue; }
      pt2_row(p, p.ch[i], chip);
    }
  }
  int mixer = 0;
  p.noise_add = 0;
  for (int i = 0; i < 3; i++) pt2_emit(p, p.ch[i], i, mixer);
  wr_reg(chip, 7, mixer);
  for (int i = 0; i < 3; i++) {
    wr_reg(chip, i * 2, p.ch[i].tone & 0xff);
    wr_reg(chip, i * 2 + 1, (p.ch[i].tone >> 8) & 15);
    wr_reg(chip, 8 + i, p.ch[i].amp);
  }
  wr_reg(chip, 6, p.noise_add & 31);
}

static int pt2_start(Player &p, const uint8_t *m, uint32_t n) {
  if (!m || n < 0x83) return -1;
  p.kind = K_PT2; p.m = m; p.n = n;
  p.pat_tab = rd16(m + 99);
  p.pos_count = m[1];
  if (!p.pos_count) p.pos_count = 1;
  p.loop = m[2];
  if (p.loop >= p.pos_count) p.loop = 0;
  p.delay = m[0];
  if (!p.delay) p.delay = 1;
  p.delay_left = 1; p.pos = 0;
  p.noise_add = 0; p.env_lo = p.env_hi = 0;
  for (int i = 0; i < 3; i++) {
    Chan &c = p.ch[i];
    memset(&c, 0, sizeof(c));
    c.vol = 15; c.smp = 0; c.orn = 0;
  }
  if ((uint32_t) p.pat_tab + 6 > n) return -1;
  set_pattern(p, 0);
  for (int i = 0; i < 3; i++) {
    Chan &c = p.ch[i];
    uint16_t op = rd16(m + 67);
    uint16_t sp = rd16(m + 3 + 2);
    if ((uint32_t) op + 1 < n) { c.olen = m[op]; c.oloop = m[op + 1]; c.orn = op + 2; if (!c.olen) c.olen = 1; }
    if ((uint32_t) sp + 1 < n) { c.slen = m[sp]; c.sloop = m[sp + 1]; c.smp = sp + 2; if (!c.slen) c.slen = 1; }
    c.spos = c.opos = 0;
  }
  set_title(m, 30, 101);
  return 0;
}

// ---------------------------------------------------------------- STC

static void stc_emit(Player &p, Chan &c, int ci, int &mixer) {
  c.amp = 0;
  if (c.s_tik == 0xff) return;
  if (c.s_tik > 0) c.s_tik--;
  c.spos = (uint8_t) ((c.spos + 1) & 31);
  if (c.s_tik == 0) {
    uint8_t loop = p.m[c.smp + 0x60];
    if (loop) { c.spos = loop & 31; c.s_tik = (uint8_t) (p.m[c.smp + 0x61] + 1); }
    else c.s_tik = 0xff;
  }
  if (c.s_tik == 0xff) return;
  uint32_t a = c.smp + ((c.spos - 1) & 31) * 3;
  if (a + 2 >= p.n) return;
  uint8_t b0 = p.m[a], b1 = p.m[a + 1], b2 = p.m[a + 2];
  int j = clamp_note(c.note + (int8_t) p.m[c.orn + ((c.spos - 1) & 31)] + p.trans);
  int tone;
  if (b1 & 0x20) tone = note_tab[j] + b2 + (((unsigned) (b0 & 0xf0)) << 4);
  else tone = note_tab[j] - b2 - (((unsigned) (b0 & 0xf0)) << 4);
  c.tone = (uint16_t) (tone & 0xfff);
  c.amp = (uint8_t) (b0 & 15);
  if (c.env) c.amp |= 16;
  if (b1 & 0x80) mixer |= 8 << ci;   // noise off
  else wr_reg(0, 6, b1 & 31);
  if (b1 & 0x40) mixer |= 1 << ci;   // tone off
}

static void stc_row(Player &p, Chan &c) {
  uint32_t a = c.pat;
  while (a < p.n) {
    uint8_t v = p.m[a++];
    if (v <= 0x5f) {
      c.note = v; c.s_tik = 32; c.spos = 0;
      break;
    } else if (v >= 0x60 && v <= 0x6f) {
      for (int k = 0; k < 256 && (uint32_t) (0x1b + 0x63 * k + 2) < p.n; k++) {
        if (p.m[0x1b + 0x63 * k] == (v - 0x60)) {
          c.smp = (uint16_t) (0x1c + 0x63 * k);
          break;
        }
      }
    } else if (v >= 0x70 && v <= 0x7f) {
      for (int k = 0; k < 256 && (uint32_t) (p.orn_tab + 0x21 * k + 2) < p.n; k++) {
        if (p.m[p.orn_tab + 0x21 * k] == (v - 0x70)) {
          c.orn = (uint16_t) (p.orn_tab + 0x21 * k + 1);
          c.env = 0;
          break;
        }
      }
    } else if (v == 0x80) {
      c.s_tik = 0xff; break;
    } else if (v == 0x81) {
      break;
    } else if (v == 0x82) {
      for (int k = 0; k < 256 && (uint32_t) (p.orn_tab + 0x21 * k + 2) < p.n; k++) {
        if (p.m[p.orn_tab + 0x21 * k] == 0) { c.orn = (uint16_t) (p.orn_tab + 0x21 * k + 1); break; }
      }
    } else if (v >= 0x83 && v <= 0x8e) {
      c.env = 1;
      wr_reg(0, 13, v - 0x80);
      if (a < p.n) wr_reg(0, 11, p.m[a++]);
      for (int k = 0; k < 256 && (uint32_t) (p.orn_tab + 0x21 * k + 2) < p.n; k++) {
        if (p.m[p.orn_tab + 0x21 * k] == 0) { c.orn = (uint16_t) (p.orn_tab + 0x21 * k + 1); break; }
      }
    } else {
      c.skip = (uint8_t) (v - 0xa1);
    }
  }
  c.pat = (uint16_t) a;
  c.skip_left = c.skip;
}

static void stc_next_pos(Player &p) {
  if (p.pos >= p.m[p.pos_tab]) p.pos = 0;
  else p.pos++;
  p.trans = p.m[p.pos_tab + 2 + p.pos * 2];
  uint8_t pat = p.m[p.pos_tab + 1 + p.pos * 2];
  for (int k = 0; k < 256 && (uint32_t) (p.pat_tab + 7 * k + 6) < p.n; k++) {
    if (p.m[p.pat_tab + 7 * k] == pat) {
      for (int c = 0; c < 3; c++) p.ch[c].pat = rd16(p.m + p.pat_tab + 7 * k + 1 + c * 2);
      break;
    }
  }
}

static void stc_tick(Player &p, int chip) {
  if (--p.delay_left == 0) {
    p.delay_left = p.delay ? p.delay : 1;
    for (int i = 0; i < 3; i++) {
      Chan &c = p.ch[i];
      if (c.skip_left) { c.skip_left--; continue; }
      if (i == 0 && c.pat < p.n && p.m[c.pat] == 0xff) stc_next_pos(p);
      stc_row(p, c);
    }
  }
  int mixer = 0;
  for (int i = 0; i < 3; i++) stc_emit(p, p.ch[i], i, mixer);
  wr_reg(chip, 7, mixer);
  for (int i = 0; i < 3; i++) {
    wr_reg(chip, i * 2, p.ch[i].tone & 0xff);
    wr_reg(chip, i * 2 + 1, (p.ch[i].tone >> 8) & 15);
    wr_reg(chip, 8 + i, p.ch[i].amp);
  }
}

static int stc_start(Player &p, const uint8_t *m, uint32_t n) {
  if (!m || n < 0x1c) return -1;
  p.kind = K_STC; p.m = m; p.n = n;
  p.pos_tab = rd16(m + 1);
  p.orn_tab = rd16(m + 3);
  p.pat_tab = rd16(m + 5);
  p.pos_count = m[p.pos_tab];
  if (!p.pos_count) p.pos_count = 1;
  p.delay = m[0];
  if (!p.delay) p.delay = 1;
  p.delay_left = 1;
  p.pos = 0;
  p.trans = m[p.pos_tab + 2];
  p.noise_add = 0;
  for (int i = 0; i < 3; i++) {
    Chan &c = p.ch[i];
    memset(&c, 0, sizeof(c));
    c.vol = 15; c.smp = 0x1c; c.orn = p.orn_tab + 1;
    c.s_tik = 0xff;
    c.enabled = 1;
  }
  uint8_t pat = m[p.pos_tab + 1];
  for (int k = 0; k < 256 && (uint32_t) (p.pat_tab + 7 * k + 6) < n; k++) {
    if (m[p.pat_tab + 7 * k] == pat) {
      for (int c = 0; c < 3; c++) p.ch[c].pat = rd16(m + p.pat_tab + 7 * k + 1 + c * 2);
      break;
    }
  }
  set_title(m, 18, 7);
  return 0;
}

// ---------------------------------------------------------------- public API

static int start_one(Player &p, int kind, const uint8_t *m, uint32_t n) {
  if (kind == K_PT3) return pt3_start(p, m, n);
  if (kind == K_PT2) return pt2_start(p, m, n);
  return stc_start(p, m, n);
}

static int is_pt3_header(const uint8_t *m, uint32_t n) {
  return n >= 13 && (!memcmp(m, "ProTracker 3.", 13) || !memcmp(m, "Vortex Tracke", 13));
}

static const uint8_t *find_second_pt3(const uint8_t *m, uint32_t n) {
  if (n >= 16 && !memcmp(m + n - 4, "02TS", 4)) {
    uint32_t f = n - 16;
    if (!memcmp(m + f, "PT3!", 4) && !memcmp(m + f + 6, "PT3!", 4)) {
      uint32_t s1 = m[f + 4] | (m[f + 5] << 8);
      uint32_t s2 = m[f + 10] | (m[f + 11] << 8);
      if (s1 && s2 && s1 + s2 == f && is_pt3_header(m, n) && is_pt3_header(m + s1, n - s1))
        return m + s1;
    }
  }
  for (uint32_t i = 0x63; i + 13 <= n; i++)
    if (is_pt3_header(m + i, n - i)) return m + i;
  return 0;
}

extern "C" int ayp_detect(int f, const uint8_t *m, uint32_t n) {
  if (!m) return 0;
  if (f == K_PT3) return n >= 0xcb && (!memcmp(m, "ProTracker 3.", 13) || !memcmp(m, "Vortex Tracke", 13));
  if (f == K_PT2) return n >= 0x83;
  return n >= 0x1c;
}

extern "C" int ayp_start(int f, const uint8_t *m, uint32_t n) {
  g_active = 0; g_nchips = 1; g_ticks = 0; g_length = 0; g_title[0] = 0; g_error[0] = 0;
  memset(g_regs, 0, sizeof(g_regs));
  memset(g_written, 0, sizeof(g_written));
  g_regs[0][7] = g_regs[1][7] = 0xff;
  if (!m) { set_error("empty module"); return -1; }
  if (start_one(g_pl[0], f, m, n) != 0) { set_error("bad module header"); return -1; }
  if (f == K_PT3) {
    const uint8_t *second = find_second_pt3(m, n);
    if (second && second > m && start_one(g_pl[1], f, second, (uint32_t) (n - (second - m))) == 0) g_nchips = 2;
  }
  g_length = estimate_length(g_pl[0]);
  g_kind = f;
  g_active = 1;
  return 0;
}

extern "C" void ayp_tick(void) {
  if (!g_active) return;
  for (int c = 0; c < g_nchips; c++) {
    if (g_kind == K_PT3) pt3_tick(g_pl[c], c);
    else if (g_kind == K_PT2) pt2_tick(g_pl[c], c);
    else stc_tick(g_pl[c], c);
  }
  g_ticks++;
}

extern "C" const uint8_t *ayp_regs(int chip) { return g_regs[chip & 1]; }
extern "C" uint16_t ayp_written(int chip) { return g_written[chip & 1]; }
extern "C" void ayp_clear_written(int chip) { g_written[chip & 1] = 0; }
extern "C" int ayp_channels(void) { return g_nchips * 3; }
extern "C" uint32_t ayp_length(void) { return g_length; }
extern "C" uint32_t ayp_ticks(void) { return g_ticks; }
extern "C" const char *ayp_title(void) { return g_title; }
extern "C" const char *ayp_error(void) { return g_error; }
extern "C" void ayp_stop(void) { g_active = 0; }
