// The graphic skin: the SD2 file list on the left, the player and the spectrum analyzer on the
// right. The player core knows nothing about this layout; it only fills a player_view_t.
#include <string.h>
#include "mmio.h"
#include "ui_skin.h"
#include "ui.h"
#include "gfx.h"
#include "fonts.h"

#define HDR_H   48
#define FOOT_H  40
#define BODY_Y  HDR_H
#define BODY_H  (FB_H - HDR_H - FOOT_H)
#define LEFT_W  400
#define NAME_X  20
#define NAME_PAD 16
#define NAME_SIZE_W 96
#define MARQ_MS 50
#define MARQ_STEP 2
#define RIGHT_X LEFT_W
#define RIGHT_W (FB_W - LEFT_W)
#define LIST_Y  88
#define ROW_H   38
/* File list font ladder: Shift+= makes it bigger, Shift+- smaller (fonts are discrete). */
static const font_t *const list_fonts[] = { &f26, &f27, &f28, &f29, &f30 };
static const int list_rows_h[] = { 38, 39, 40, 41, 42 };
static int cur_font_step;
static const font_t *list_font(void) { int s = cur_font_step; if (s < 0) s = 0; else if (s > 4) s = 4; return list_fonts[s]; }
static int list_row_h(void) { int s = cur_font_step; if (s < 0) s = 0; else if (s > 4) s = 4; return list_rows_h[s]; }
#define ROWS    ((FB_H - LIST_Y - FOOT_H) / ROW_H)

#define PX      (RIGHT_X + 20)   // the right panel content: x = 420, width = 360
#define PW      (RIGHT_W - 40)
#define SP_Y    270              // the spectrum label
#define SP_BASE 462
#define SP_H    160
#define SP_BARS 60               // 2 px bars, 2 px gaps
#define SP_BW   2
#define SP_GAP  2
#define SP_STEP (SP_BW + SP_GAP)
#define SP_W    (SP_BARS * SP_BW + (SP_BARS - 1) * SP_GAP)
#define SP_X    (PX + (PW - SP_W) / 2)
#define SP_CAP  4
#define SP_SEG  4                // one segment is cap-sized
#define SP_SEG_STEP (SP_SEG + 1)
// The UI used to show only the middle (yellow) third: the raw bands stay around
// 85..170, which is exactly the yellow colour zone. Use that zone as the full scale.
#define SP_LO   85
#define SP_HI   170

static uint8_t c_bg, c_panel, c_head, c_sel, c_accent, c_line, c_vu, c_hot, c_spec, c_grid, c_yellow, c_red;
static ramp_t r_text, r_dim, r_accent, r_ptext, r_pdim, r_paccent;

static int last_sel = -1, last_top = -1, last_now = -1, last_paused = -1;
static char last_path[FS_PATH_MAX];
static int marq_off, marq_dir = 1;
static uint32_t marq_t;
static int last_prog = -1, disp_vu_l, disp_vu_r;
static int disp_ch[32], disp_bands[SP_BARS];
static int peak_bands[SP_BARS], cap_lo[SP_BARS], cap_hi[SP_BARS], peak_hold[SP_BARS];
static int spec_off;
static int chan_off;
static uint32_t last_elapsed = 0xFFFFFFFFu;
static char last_title[40], last_format[8], last_status[32], last_mix[40], last_meta[48];

static void col_init(void) {
  c_bg = gfx_color(0x0b0f18);
  c_panel = gfx_color(0x101724);
  c_head = gfx_color(0x141b28);
  c_sel = gfx_color(0x1f3350);
  c_accent = gfx_color(0x59b0ff);
  c_line = gfx_color(0x223047);
  c_vu = gfx_color(0x39d98a);
  c_hot = gfx_color(0xffb454);
  c_spec = gfx_color(0x59b0ff);
  c_grid = gfx_color(0x1a2433);
  c_yellow = gfx_color(0xffd24a);
  c_red = gfx_color(0xff5a5a);
  r_text = gfx_ramp_over(0xe8eef8, 0x0b0f18);
  r_dim = gfx_ramp_over(0x8a97ab, 0x0b0f18);
  r_accent = gfx_ramp_over(0x59b0ff, 0x0b0f18);
  r_ptext = gfx_ramp_over(0xe8eef8, 0x101724);
  r_pdim = gfx_ramp_over(0x8a97ab, 0x101724);
  r_paccent = gfx_ramp_over(0x59b0ff, 0x101724);
}

static void copy_fit(char *d, const char *s, int max) {
  int i = 0;
  for (; s && s[i] && i < max - 1; i++) d[i] = s[i];
  d[i] = 0;
}

static char *put_time(char *p, uint32_t sec) {
  p = u2s(p, sec / 60u, 0);
  *p++ = ':';
  if (sec % 60u < 10u) *p++ = '0';
  p = u2s(p, sec % 60u, 0);
  return p;
}

// ------------------------------------------------------------------ header / list

static void draw_header(const player_view_t *v) {
  gfx_fill(0, 0, FB_W, HDR_H, c_head);
  gfx_text_left(&f26, 20, HDR_H / 2, "MUSIC PLAYER", r_accent);
  char s[48], *q = s;
  q = str_cpy(q, "SD2 ");
  q = str_cpy(q, v->skin ? v->skin : "");
  gfx_text_right(&f20, FB_W - 20, HDR_H / 2, s, r_dim);
  gfx_fill(0, HDR_H - 1, FB_W, 1, c_line);
}

static void draw_left_base(const player_view_t *v) {
  gfx_fill(0, BODY_Y, LEFT_W - 1, BODY_H, c_bg);
  char p[56];
  const char *path = v->path ? v->path : "/";
  int n = (int) strlen(path);
  if (n > 50) path += n - 50;
  copy_fit(p, path, 52);
  gfx_text_left(&f20, 20, BODY_Y + 18, p, r_dim);
  gfx_fill(12, BODY_Y + 34, LEFT_W - 24, 1, c_line);
}

static int name_width(const player_view_t *v, int k) {
  if (k < 0 || k >= v->nent) return 0;
  const fs_entry_t *e = &v->ent[k];
  char nm[FS_NAME_MAX + 2];
  copy_fit(nm, e->name, sizeof(nm));
  int j = (int) strlen(nm);
  if (e->dir && j < (int) sizeof(nm) - 1) { nm[j++] = '/'; nm[j] = 0; }
  return gfx_text_width(&f20, nm);
}

static int name_avail(const fs_entry_t *e) {
  int right = LEFT_W - NAME_PAD;
  if (!e->dir) right -= (NAME_SIZE_W * list_row_h()) / 38;
  int w = right - NAME_X;
  return w > 0 ? w : 0;
}

static void reset_marquee(void) {
  marq_off = 0;
  marq_dir = 1;
  marq_t = R_US;
}

int ui_list_rows(void) { return (FB_H - LIST_Y - FOOT_H) / list_row_h(); }

static void draw_row(const player_view_t *v, int k) {
  if (k < v->top || k >= v->top + ROWS) return;
  int r = k - v->top;
  const int rh = list_row_h();
  const font_t *lf = list_font();
  int y = LIST_Y + r * rh;
  gfx_fill(0, y - 2, LEFT_W - 1, rh + 2, c_bg);
  if (k == v->sel) gfx_solid_rect(8, y + 1, LEFT_W - 16, rh - 4, c_sel);
  if (k < 0 || k >= v->nent) return;

  const fs_entry_t *e = &v->ent[k];
  char nm[FS_NAME_MAX + 2];
  copy_fit(nm, e->name, sizeof(nm));
  int j = (int) strlen(nm);
  if (e->dir && j < (int) sizeof(nm) - 1) { nm[j++] = '/'; nm[j] = 0; }
  int avail = name_avail(e);
  int w = gfx_text_width(lf, nm);
  int off = (k == v->sel && w > avail) ? marq_off : 0;
  if (off > w - avail) off = w - avail;
  if (off > 0) {
    // Keep the size column and the panel clean while the long name slides.
    gfx_keep(NAME_X + avail, y - 2, LEFT_W - (NAME_X + avail), rh + 2);
    gfx_text_left(lf, NAME_X - off, y + rh / 2, nm, r_text);
    gfx_keep_off();
  } else {
    char fit[40];
    copy_fit(fit, nm, 34);
    gfx_text_left(lf, NAME_X, y + rh / 2, fit, r_text);
  }
  if (!e->dir) {
    char b[16], *z = b;
    z = u2s(z, (e->size + 1023u) / 1024u, 1);
    *z++ = 'K'; *z = 0;
    gfx_text_right(lf, LEFT_W - 16, y + rh / 2, b, r_dim);
  }
}

static void marquee_tick(const player_view_t *v) {
  if (v->sel < 0 || v->sel >= v->nent) { reset_marquee(); return; }
  int avail = name_avail(&v->ent[v->sel]);
  int over = name_width(v, v->sel) - avail;
  if (over <= 0) { reset_marquee(); return; }
  if ((uint32_t) (R_US - marq_t) < MARQ_MS) return;
  marq_t = R_US;
  marq_off += marq_dir * MARQ_STEP;
  if (marq_off >= over) { marq_off = over; marq_dir = -1; }
  else if (marq_off <= 0) { marq_off = 0; marq_dir = 1; }
  draw_row(v, v->sel);
}

static void draw_list_area(const player_view_t *v) {
  cur_font_step = v->font_step;
  gfx_fill(0, LIST_Y - 4, LEFT_W - 1, (BODY_Y + BODY_H) - (LIST_Y - 4), c_bg);
  if (v->idle) v->idle();
  if (!v->nent) {
    gfx_text_center(&f20, LEFT_W / 2, 300, "no mod / xm / s3m / it files here", r_dim);
    return;
  }
  for (int i = 0; i < ROWS; i++) {
    int k = v->top + i;
    if (k >= v->nent) break;
    draw_row(v, k);
    if (v->idle && (i & 1)) v->idle();   // refill the audio FIFO while the list is painted
  }
}

// ------------------------------------------------------------------ the right panel: static parts

static void draw_spectrum_grid(void) {
  for (int g = 1; g <= 3; g++) gfx_fill(SP_X, SP_BASE - SP_H * g / 4, SP_W, 1, c_grid);
  gfx_fill(SP_X, SP_BASE + 1, SP_W, 1, c_line);
}

static void draw_channels_tracks(const player_view_t *v) {
  int n = v->channels;
  if (n < 0) n = 0;
  if (n > 32) n = 32;
  if (!n) return;
  int bw = 8, gap = 2;
  int total = n * bw + (n - 1) * gap;
  if (total > PW) { bw = 5; gap = 1; total = n * bw + (n - 1) * gap; }
  int x0 = PX + (PW - total) / 2;
  int base = 254, h = 52;
  for (int c = 0; c < n; c++) {
    int bx = x0 + c * (bw + gap);
    gfx_fill(bx, base - h, bw, h, c_grid);
  }
}

static void draw_meter_static(const char *lab, int y) {
  gfx_text_left(&f20, PX, y + 7, lab, r_pdim);
  gfx_fill(PX + 20, y, PW - 20, 14, c_bg);
}

static void update_player_dynamic(const player_view_t *v, int force);

static void draw_player_static(const player_view_t *v) {
  gfx_fill(RIGHT_X, BODY_Y, RIGHT_W, BODY_H, c_panel);
  gfx_fill(RIGHT_X - 1, BODY_Y, 1, BODY_H, c_line);

  for (int i = 0; i < 32; i++) disp_ch[i] = 0;
  for (int i = 0; i < SP_BARS; i++) {
    disp_bands[i] = 0;
    peak_bands[i] = 0;
    cap_lo[i] = cap_hi[i] = 0;
    peak_hold[i] = 0;
  }
  spec_off = 0;
  chan_off = 0;
  last_prog = -1;
  disp_vu_l = disp_vu_r = 0;
  last_elapsed = 0xFFFFFFFFu;
  last_paused = -1;

  if (!v->now) {
    int cx = RIGHT_X + RIGHT_W / 2;
    gfx_text_center(&f26, cx, 262, "KARABAS GO", r_paccent);
    gfx_text_center(&f26, cx, 294, "MUSIC PLAYER", r_paccent);
    char b[128], *q = b;
    *q++ = '(';
    q = str_cpy(q, v->formats && v->formats[0] ? v->formats : "mod, xm");
    *q++ = ')';
    *q = 0;
    gfx_text_center(&f20, cx, 334, b, r_pdim);
    gfx_text_center(&f20, cx, 382, "Enter a file to play", r_pdim);
    last_now = 0;
    return;
  }

  char b[64], *q = b;
  q = str_cpy(q, v->format ? v->format : "");
  q = str_cpy(q, "  ");
  q = str_cpy(q, v->engine ? v->engine : "");
  gfx_text_left(&f20, PX, 64, b, r_pdim);        // format/engine (the time is on the right)

  char t[40];
  copy_fit(t, v->title ? v->title : "", 30);
  gfx_text_left(&f26, PX, 90, t, r_paccent);

  gfx_fill(PX, 114, PW, 6, c_bg);
  draw_meter_static("L", 132);
  draw_meter_static("R", 154);

  gfx_text_left(&f20, PX, 190, "CHANNELS", r_pdim);
  if (v->spectrum) draw_channels_tracks(v);

  gfx_text_left(&f20, PX, SP_Y, "SPECTRUM", r_pdim);
  draw_spectrum_grid();

  // The static line 1 is drawn here; the order/row and the engine lines are redrawn by the
  // dynamic part (they include values that change).
  q = str_cpy(b, "CH ");
  q = u2s(q, (uint32_t) (v->channels < 0 ? 0 : v->channels), 0);
  q = str_cpy(q, "  SMP ");
  q = u2s(q, (uint32_t) (v->samples < 0 ? 0 : v->samples), 0);
  q = str_cpy(q, "  SPD ");
  q = u2s(q, (uint32_t) (v->speed < 0 ? 0 : v->speed), 0);
  q = str_cpy(q, "  BPM ");
  u2s(q, (uint32_t) (v->bpm < 0 ? 0 : v->bpm), 0);
  gfx_text_left(&f20, PX, 492, b, r_ptext);

  last_now = 1;
  copy_fit(last_title, v->title ? v->title : "", sizeof(last_title));
  copy_fit(last_format, v->format ? v->format : "", sizeof(last_format));
  update_player_dynamic(v, 1);
}

// ------------------------------------------------------------------ the right panel: dynamic parts

static void update_time(const player_view_t *v) {
  char tm[32], *p = put_time(tm, v->elapsed);
  if (v->total) { *p++ = ' '; *p++ = '/'; *p++ = ' '; put_time(p, v->total); }
  gfx_fill(PX + PW - 150, 52, 150, 26, c_panel);
  gfx_text_right(&f20, PX + PW, 64, tm, r_pdim);
}

static void update_info(const player_view_t *v) {
  char b[72], *q;
  q = str_cpy(b, "ORD ");
  q = u2s(q, (uint32_t) (v->order < 0 ? 0 : v->order), 0);
  q = str_cpy(q, "  PAT ");
  q = u2s(q, (uint32_t) (v->pattern < 0 ? 0 : v->pattern), 0);
  q = str_cpy(q, "  ROW ");
  q = u2s(q, (uint32_t) (v->row < 0 ? 0 : v->row), 0);
  q = str_cpy(q, "  VOL ");
  u2s(q, (uint32_t) (v->volume < 0 ? 0 : v->volume), 0);
  gfx_fill(PX, 503, PW, 22, c_panel);
  gfx_text_left(&f20, PX, 514, b, r_ptext);

  q = str_cpy(b, v->engine ? v->engine : "");
  if (v->version && v->version[0]) { *q++ = ' '; q = str_cpy(q, v->version); }
  q = str_cpy(q, "  AUD ");
  q = u2s(q, v->aud_rate, 0);
  q = str_cpy(q, v->loading ? "  LOADING" : (v->paused ? "  PAUSED" : "  PLAYING"));
  gfx_fill(PX, 525, PW, 24, c_panel);
  gfx_text_left(&f20, PX, 536, b, r_pdim);
}

static void update_progress(const player_view_t *v) {
  int nw = (v->has_pos && v->total_pos) ? (int) ((uint64_t) PW * v->cur / v->total_pos) : 0;
  if (nw < 0) nw = 0;
  if (nw > PW) nw = PW;
  int old = last_prog < 0 ? 0 : last_prog;
  if (nw > old) gfx_fill(PX + old, 114, nw - old, 6, c_accent);
  else if (nw < old) gfx_fill(PX + nw, 114, old - nw, 6, c_bg);
  last_prog = nw;
}

static void update_meter(int32_t peak, int y, int *disp) {
  int t = (int) (peak >> 7);
  if (t < 0) t = 0;
  if (t > 255) t = 255;
  int old = *disp;
  int n = old;
  if (t > n) n = t;
  else { n -= 12; if (n < t) n = t; }     // a smooth fall, ~0.7 s at 30 Hz
  *disp = n;
  int bw = PW - 20;
  int ow = bw * old / 255, nw = bw * n / 255;
  int hot = bw * 3 / 4;
  if (nw > ow) {
    int a = ow < hot ? ow : hot, b = nw < hot ? nw : hot;
    if (b > a) gfx_fill(PX + 20 + a, y, b - a, 14, c_vu);
    if (nw > hot) {
      int s = ow > hot ? ow : hot;
      gfx_fill(PX + 20 + s, y, nw - s, 14, c_hot);
    }
  } else if (nw < ow) {
    gfx_fill(PX + 20 + nw, y, ow - nw, 14, c_bg);
  }
}

static void update_channels(const player_view_t *v) {
  if (!v->spectrum) {
    if (!chan_off) {
      chan_off = 1;
      gfx_fill(PX, 202, PW, 58, c_panel);
      gfx_text_center(&f20, PX + PW / 2, 230, "CHANNELS OFF", r_pdim);
    }
    return;
  }
  if (chan_off) {
    chan_off = 0;
    gfx_fill(PX, 202, PW, 58, c_panel);
    draw_channels_tracks(v);
  }
  int n = v->channels;
  if (n < 0) n = 0;
  if (n > 32) n = 32;
  if (!n) return;
  int bw = 8, gap = 2;
  int total = n * bw + (n - 1) * gap;
  if (total > PW) { bw = 5; gap = 1; total = n * bw + (n - 1) * gap; }
  int x0 = PX + (PW - total) / 2;
  int base = 254, h = 52;
  for (int c = 0; c < n; c++) {
    int bx = x0 + c * (bw + gap);
    int t = v->vu[c];
    int old = disp_ch[c];
    int nv = old;
    if (t > nv) nv = t;
    else { nv -= 10; if (nv < t) nv = t; }
    disp_ch[c] = nv;
    int oh = h * old / 255;
    int nh = h * nv / 255;
    if (nh > oh) gfx_fill(bx, base - nh, bw, nh - oh, c_vu);
    else if (nh < oh) gfx_fill(bx, base - oh, bw, oh - nh, c_grid);
  }
}

static void draw_spectrum_off(const player_view_t *v) {
  gfx_fill(SP_X - 4, SP_BASE - SP_H - 2, SP_W + 8, SP_H + 4, c_panel);
  int cx = PX + PW / 2;
  gfx_text_center(&f26, cx, SP_BASE - 92, "THIS TRACK BROKE", r_paccent);
  gfx_text_center(&f26, cx, SP_BASE - 62, "MY ANALYZER", r_paccent);
  char b[40], *q = u2s(b, (uint32_t) (v->channels < 0 ? 0 : v->channels), 0);
  q = str_cpy(q, " CHANNELS / SPECTRUM OFF");
  gfx_text_center(&f20, cx, SP_BASE - 24, b, r_pdim);
}

// Cut the solid bars into cap-sized segments. The background gaps are drawn over
// every bar; this is one pass over the spectrum, not per-bar rectangle drawing.
static void draw_spectrum_segments(void) {
  for (int y = SP_BASE - SP_SEG; y > SP_BASE - SP_H; y -= SP_SEG_STEP)
    gfx_fill(SP_X, y, SP_W, 1, c_panel);
}

static void update_spectrum(const player_view_t *v) {
  if (!v->spectrum) {
    if (!spec_off) {
      spec_off = 1;
      for (int i = 0; i < SP_BARS; i++) {
        disp_bands[i] = 0;
        peak_bands[i] = 0;
        cap_lo[i] = cap_hi[i] = 0;
        peak_hold[i] = 0;
      }
      draw_spectrum_off(v);
    }
    return;
  }
  if (spec_off) {
    spec_off = 0;
    gfx_fill(SP_X - 4, SP_BASE - SP_H - 2, SP_W + 8, SP_H + 4, c_panel);
    draw_spectrum_grid();
  }

  int gy[3] = { SP_BASE - 40, SP_BASE - 80, SP_BASE - 120 };
  for (int i = 0; i < SP_BARS; i++) {
    int bx = SP_X + i * SP_STEP;

    // erase the old peak cap (it is above the bar, so this does not touch the bar)
    if (cap_hi[i] > cap_lo[i]) {
      int top = SP_BASE - cap_hi[i], hgt = cap_hi[i] - cap_lo[i];
      gfx_fill(bx, top, SP_BW, hgt, c_panel);
      for (int g = 0; g < 3; g++)
        if (gy[g] >= top && gy[g] < top + hgt) gfx_fill(bx, gy[g], SP_BW, 1, c_grid);
    }

    int si = (i * (FMT_BANDS - 1)) / (SP_BARS - 1);
    int t = v->bands[si];
    // spectrum sensitivity: 75% overall, with a gentle low-frequency tilt (70%..100%)
    t = t * (70 + (i * 30) / (SP_BARS - 1)) / 100;
    t = t * 3 / 4;
    // Zoom into the band values that used to sit in the yellow zone: the yellow
    // bottom is the new zero, the yellow top the new full scale.
    t = (t - SP_LO) * 255 / (SP_HI - SP_LO);
    if (t < 0) t = 0; else if (t > 255) t = 255;
    int old = disp_bands[i];
    int nv = old;
    if (t > nv) nv = t;
    else { nv -= 14; if (nv < t) nv = t; }   // peak attack, smooth release
    disp_bands[i] = nv;

    int oh = old * SP_H / 255;
    int nh = nv * SP_H / 255;
    if (nh > oh) {
      // green / yellow / red thirds of the bar height
      int t1 = SP_H / 3, t2 = SP_H * 2 / 3;
      if (oh < t1) {
        int a = oh, b = nh < t1 ? nh : t1;
        if (b > a) gfx_fill(bx, SP_BASE - b, SP_BW, b - a, c_vu);
      }
      if (nh > t1) {
        int a = oh > t1 ? oh : t1, b = nh < t2 ? nh : t2;
        if (b > a) gfx_fill(bx, SP_BASE - b, SP_BW, b - a, c_yellow);
      }
      if (nh > t2) {
        int a = oh > t2 ? oh : t2;
        gfx_fill(bx, SP_BASE - nh, SP_BW, nh - a, c_red);
      }
    } else if (nh < oh) {
      int top = SP_BASE - oh, hgt = oh - nh;
      gfx_fill(bx, top, SP_BW, hgt, c_panel);
      for (int g = 0; g < 3; g++)
        if (gy[g] >= top && gy[g] < top + hgt) gfx_fill(bx, gy[g], SP_BW, 1, c_grid);
    }

    // the peak cap: ~1/10 of the scale, above the bar. It follows a rising bar at once,
    // holds for a few frames, then falls slower than the bar.
    int peak = peak_bands[i];
    if (nv > peak) { peak = nv; peak_hold[i] = 6; }
    else if (peak_hold[i] > 0) peak_hold[i]--;
    else { peak -= 4; if (peak < nv) peak = nv; }
    if (peak < 0) peak = 0;
    peak_bands[i] = peak;
    int mh = nv * SP_H / 255;
    int ph = peak * SP_H / 255;
    // a 1 px dark gap keeps the cap visible when it sits exactly on the bar top
    int lo = ph > mh ? ph : mh + 1;
    int hi = lo + SP_CAP;
    if (hi > SP_H) hi = SP_H;
    // the cap takes the colour of the zone it is in (green / yellow / red)
    if (hi > lo) {
      int t1 = SP_H / 3, t2 = SP_H * 2 / 3;
      int a = lo, b = hi < t1 ? hi : t1;
      if (b > a) gfx_fill(bx, SP_BASE - b, SP_BW, b - a, c_vu);
      a = lo > t1 ? lo : t1; b = hi < t2 ? hi : t2;
      if (b > a) gfx_fill(bx, SP_BASE - b, SP_BW, b - a, c_yellow);
      a = lo > t2 ? lo : t2;
      if (hi > a) gfx_fill(bx, SP_BASE - hi, SP_BW, hi - a, c_red);
    }
    cap_lo[i] = lo;
    cap_hi[i] = hi;
  }
  draw_spectrum_segments();
}

static void update_player_dynamic(const player_view_t *v, int force) {
  if (!v->now) return;
  update_progress(v);
  update_meter(v->peak_l, 132, &disp_vu_l);
  update_meter(v->peak_r, 154, &disp_vu_r);
  update_channels(v);
  update_spectrum(v);
  if (force || v->elapsed != last_elapsed) {
    update_time(v);
    update_info(v);
    last_elapsed = v->elapsed;
  } else if (v->paused != last_paused) {
    update_info(v);
  }
  last_paused = v->paused;
}

// ------------------------------------------------------------------ footer

static void footer_meta(const player_view_t *v, char *out, int n) {
  int i = 0;
  // When a live diagnostic string exists it wins the footer slot: the build id is long
  // enough to push it out of the visible area.
  const char *b = (v->debug && v->debug[0]) ? "" : (v->build ? v->build : "");
  for (; b[0] && i < n - 1; b++) out[i++] = *b;
  if (v->debug && v->debug[0]) {
    const char *d = v->debug;
    for (; d[0] && i < n - 1; d++) out[i++] = *d;
  }
  out[i] = 0;
}

static void draw_footer_static(const player_view_t *v) {
  gfx_fill(0, FB_H - FOOT_H, FB_W, FOOT_H, c_head);
  gfx_fill(0, FB_H - FOOT_H, FB_W, 1, c_line);
  gfx_text_left(&f20, 20, FB_H - FOOT_H / 2, "Up/Dn L/R Enter Esc BkSp | Space +/- Tab F12", r_dim);
  gfx_text_left(&f20, 20, FB_H - FOOT_H / 2 + 12, v->logging ? "LOG:ON" : "LOG:OFF",
                v->logging ? r_accent : r_dim);
  char meta[48];
  footer_meta(v, meta, sizeof(meta));
  gfx_text_left(&f20, 400, FB_H - FOOT_H / 2, meta, r_dim);
  copy_fit(last_meta, meta, sizeof(last_meta));
  char st[32];
  copy_fit(st, v->status ? v->status : "", 28);
  gfx_text_right(&f20, FB_W - 20, FB_H - FOOT_H + 12, st, v->playing ? r_accent : r_dim);
  copy_fit(last_status, st, sizeof(last_status));
  const char *mx = v->mix ? v->mix : "";
  gfx_text_right(&f20, FB_W - 20, FB_H - 10, mx, r_dim);
  copy_fit(last_mix, mx, sizeof(last_mix));
}

static void update_footer(const player_view_t *v) {
  char st[32];
  copy_fit(st, v->status ? v->status : "", 28);
  const char *mx = v->mix ? v->mix : "";
  char meta[48];
  footer_meta(v, meta, sizeof(meta));
  if (strcmp(st, last_status) == 0 && strcmp(mx, last_mix) == 0 &&
      strcmp(meta, last_meta) == 0) return;
  gfx_fill(390, FB_H - FOOT_H + 2, FB_W - 390, FOOT_H - 4, c_head);
  gfx_text_left(&f20, 400, FB_H - FOOT_H / 2, meta, r_dim);
  gfx_text_right(&f20, FB_W - 20, FB_H - FOOT_H + 12, st, v->playing ? r_accent : r_dim);
  gfx_text_right(&f20, FB_W - 20, FB_H - 10, mx, r_dim);
  copy_fit(last_status, st, sizeof(last_status));
  copy_fit(last_mix, mx, sizeof(last_mix));
  copy_fit(last_meta, meta, sizeof(last_meta));
}

// ------------------------------------------------------------------ the skin entry points

static void draw_full(const player_view_t *v) {
  scr_begin();
  col_init();
  gfx_fill(0, 0, FB_W, FB_H, c_bg);
  draw_header(v);
  draw_left_base(v);
  draw_list_area(v);
  draw_player_static(v);
  draw_footer_static(v);
  scr_end();
  last_top = v->top;
  last_sel = v->sel;
  last_now = v->now;
  last_paused = v->now ? v->paused : -1;
  copy_fit(last_path, v->path ? v->path : "", sizeof(last_path));
  reset_marquee();
}

// The live refresh draws into the visible buffer, without a clear or a buffer swap. Static text
// is left alone; only the progress bar, the meters, the channel bars and the spectrum are
// changed, and only by their deltas.
static void update_list(const player_view_t *v) {
  gfx_target(v->fb);
  col_init();
  int changed = strcmp(v->path ? v->path : "", last_path) != 0 ||
                v->top != last_top || v->sel != last_sel;
  if (changed) reset_marquee();
  if (strcmp(v->path ? v->path : "", last_path)) {
    draw_left_base(v);
    draw_list_area(v);
    copy_fit(last_path, v->path ? v->path : "", sizeof(last_path));
  } else if (v->top != last_top) {
    draw_list_area(v);
  } else if (v->sel != last_sel) {
    draw_row(v, last_sel);
    draw_row(v, v->sel);
  } else {
    marquee_tick(v);
  }
  last_top = v->top;
  last_sel = v->sel;
  update_footer(v);
}

static void update_direct(const player_view_t *v, int footer) {
  update_list(v);

  if (!v->now) {
    if (last_now != 0) draw_player_static(v);
  } else if (last_now != 1 ||
             strcmp(v->title ? v->title : "", last_title) ||
             strcmp(v->format ? v->format : "", last_format)) {
    draw_player_static(v);
  } else {
    update_player_dynamic(v, 0);
  }
  if (footer) update_footer(v);
  last_now = v->now;
}

static void update(const player_view_t *v) { update_direct(v, 1); }
static void progress(const player_view_t *v) { update_direct(v, 0); }
static int init(void) { return 0; }

const ui_skin_t skin_graphic = { "graphic", init, draw_full, update, progress, update_list, NULL, NULL };
