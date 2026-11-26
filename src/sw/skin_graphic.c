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
#define LIST_FONT_STEPS 8
static const font_t *const list_fonts[] = { &f26, &f27, &f28, &f29, &f30, &f32, &f34, &f36 };
static const int list_rows_h[] = { 38, 39, 40, 41, 42, 45, 47, 50 };
static int cur_font_step;
static const font_t *list_font(void) { int s = cur_font_step; if (s < 0) s = 0; else if (s > LIST_FONT_STEPS - 1) s = LIST_FONT_STEPS - 1; return list_fonts[s]; }
static int list_row_h(void) { int s = cur_font_step; if (s < 0) s = 0; else if (s > LIST_FONT_STEPS - 1) s = LIST_FONT_STEPS - 1; return list_rows_h[s]; }

#define PX      (RIGHT_X + 20)   // the right panel content: x = 420, width = 360
#define PW      (RIGHT_W - 40)
#define SP_Y    270              // the spectrum label
#define SP_BASE 462
#define SP_H    160
#define SP_BARS 60               // 2 px bars, 2 px gaps
#define SP_BW   3
#define SP_GAP  2
#define SP_STEP (SP_BW + SP_GAP)
#define SP_W    (SP_BARS * SP_BW + (SP_BARS - 1) * SP_GAP)
#define SP_X    (PX + (PW - SP_W) / 2)
#define SP_CAP  2
#define SP_SEG  6
#define SP_SEG_STEP (SP_SEG + 1)
// The UI used to show only the middle (yellow) third: the raw bands stay around
// 85..170, which is exactly the yellow colour zone. Use that zone as the full scale.
#define SP_LO   85
#define SP_HI   170

static uint8_t c_bg, c_panel, c_head, c_sel, c_accent, c_line, c_vu, c_hot, c_spec, c_grid, c_yellow, c_red, c_vud;
static ramp_t r_text, r_dim, r_accent, r_ptext, r_pdim, r_paccent, r_pyellow, r_pgreen;

static int last_sel = -1, last_top = -1, last_now = -1, last_paused = -1, last_font_step = -1;
static int load_x = -1, load_phase;
static char last_path[FS_PATH_MAX];
static int marq_off, marq_dir = 1;
static uint32_t marq_t;
static int last_prog = -1, disp_vu_l, disp_vu_r;
static int disp_ch[32], disp_bands[SP_BARS];
static int peak_bands[SP_BARS], cap_lo[SP_BARS], cap_hi[SP_BARS], peak_hold[SP_BARS];
static int spec_off;
static int chan_off;
static uint32_t last_elapsed = 0xFFFFFFFFu;
static char last_title[40], last_format[8], last_status[32], last_mix[40], last_meta[48], last_warn[48];

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
  c_vud  = gfx_color(0x1f7a4d);
  c_grid = gfx_color(0x1a2433);
  c_yellow = gfx_color(0xffd24a);
  c_red = gfx_color(0xff5a5a);
  r_text = gfx_ramp_over(0xe8eef8, 0x0b0f18);
  r_dim = gfx_ramp_over(0x8a97ab, 0x0b0f18);
  r_accent = gfx_ramp_over(0x59b0ff, 0x0b0f18);
  r_ptext = gfx_ramp_over(0xe8eef8, 0x101724);
  r_pdim = gfx_ramp_over(0x8a97ab, 0x101724);
  r_paccent = gfx_ramp_over(0x59b0ff, 0x101724);
  r_pyellow = gfx_ramp_over(0xf2d64b, 0x101724);
  r_pgreen  = gfx_ramp_over(0x39d98a, 0x101724);
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
  return gfx_text_width(list_font(), nm);
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

/* Fit a name into a pixel width (not a character count): with the larger list font a fixed
 * character limit still ran past the panel edge. */
static void fit_px(char *dst, int n, const char *src, const font_t *f, int maxw) {
  int len = (int) strlen(src);
  if (len > n - 1) len = n - 1;
  memcpy(dst, src, (size_t) len);
  dst[len] = 0;
  while (len > 0 && gfx_text_width(f, dst) > maxw) dst[--len] = 0;
}

static void draw_row(const player_view_t *v, int k) {
  if (k < v->top || k >= v->top + ui_list_rows()) return;
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
    // The name slides inside its own viewport: clip on both edges, so it never runs into
    // the selection border on the left or the size column / right panel on the right.
    gfx_clip(NAME_X, y - 2, avail, rh + 4);
    gfx_text_left(lf, NAME_X - off, y + rh / 2, nm, r_text);
    gfx_clip_off();
  } else {
    char fit[FS_NAME_MAX + 2];
    fit_px(fit, (int) sizeof(fit), nm, lf, avail);
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
  const int rows = ui_list_rows();
  for (int i = 0; i < rows; i++) {
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
  load_x = -1;
  load_phase = 0;
  disp_vu_l = disp_vu_r = 0;
  last_elapsed = 0xFFFFFFFFu;
  last_paused = -1;

  if (!v->now) {
    int cx = RIGHT_X + RIGHT_W / 2;
    gfx_text_center(&f26, cx, 262, "KARABAS GO", r_paccent);
    gfx_text_center(&f26, cx, 294, "MUSIC PLAYER", r_paccent);
    /* The grouped format list, one group per line (label: extensions). */
    const char *f = v->formats && v->formats[0] ? v->formats : "mod, xm";
    int fy = 322, fn = 0;
    char line[64];
    const char *p = f;
    while (*p && fn < 16) {
      int k = 0;
      while (*p && *p != '\n' && k < (int) sizeof(line) - 1) line[k++] = *p++;
      line[k] = 0;
      if (*p == '\n') p++;
      if (k) {
        char *colon = strchr(line, ':');
        if (colon) {
          *colon = 0;
          const char *ext = colon + 1;
          while (*ext == ' ') ext++;
          int wl = gfx_text_width(&f20, line) + 5 + gfx_text_width(&f20, ext);
          int x = cx - wl / 2;
          gfx_text_left(&f20, x, fy, line, r_pyellow);
          gfx_text_left(&f20, x + gfx_text_width(&f20, line) + 5, fy, ext, r_pgreen);
        } else {
          gfx_text_center(&f20, cx, fy, line, r_pdim);
        }
        fy += 12;
        fn++;
      }
    }
    gfx_text_center(&f20, cx, fy + 10, "Enter a file to play", r_pdim);
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

  /* A compatibility note takes the place of the engine line (the same font): it must
   * not draw over the frame below the panel. */
  const char *w = v->warning ? v->warning : "";
  if (w[0]) {
    copy_fit(b, w, sizeof(b) - 1);
  } else {
    q = str_cpy(b, v->engine ? v->engine : "");
    if (v->version && v->version[0]) { *q++ = ' '; q = str_cpy(q, v->version); }
    q = str_cpy(q, "  AUD ");
    q = u2s(q, v->aud_rate, 0);
    q = str_cpy(q, v->loading ? "  LOADING" : (v->paused ? "  PAUSED" : "  PLAYING"));
  }
  copy_fit(last_warn, w, sizeof(last_warn));
  gfx_fill(PX, 525, PW, 24, c_panel);
  gfx_text_left(&f20, PX, 536, b, w[0] ? r_pyellow : r_pdim);
}

static void update_progress(const player_view_t *v) {
  if (v->loading && v->total_pos && v->cur >= v->total_pos) {
    /* Bytes are in (100%) but the module is still being parsed: a percentage bar
     * would sit full, so run an indeterminate segment instead. */
    int seg = PW / 4, span = PW - seg;
    if (span < 1) span = 1;
    load_phase += 6;
    int t = load_phase % (2 * span);
    int x = t < span ? t : 2 * span - t;
    if (load_x >= 0) gfx_fill(PX + load_x, 114, seg, 6, c_bg);
    gfx_fill(PX + x, 114, seg, 6, c_accent);
    load_x = x;
    last_prog = -1;
    return;
  }
  if (load_x >= 0) { gfx_fill(PX + load_x, 114, PW / 4, 6, c_bg); load_x = -1; }
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

  /* Take the two corner pixels off the rows above and below every gap, so each
   * segment reads as a rounded pill. Only inside the bar, otherwise short bars would
   * pay for dozens of draws in the empty area above them. */
  for (int i = 0; i < SP_BARS; i++) {
    int mh = disp_bands[i] * SP_H / 255;
    if (mh < SP_SEG) continue;
    int top = SP_BASE - mh;
    int bx = SP_X + i * SP_STEP;
    for (int y = SP_BASE - SP_SEG; y > SP_BASE - SP_H; y -= SP_SEG_STEP) {
      if (y - 1 < top) continue;
      gfx_fill(bx, y - 1, 1, 1, c_panel);
      gfx_fill(bx + SP_BW - 1, y - 1, 1, 1, c_panel);
      gfx_fill(bx, y + 1, 1, 1, c_panel);
      gfx_fill(bx + SP_BW - 1, y + 1, 1, 1, c_panel);
    }
  }
}

static void update_spectrum_alt(const player_view_t *v);

static int spec_style_prev = -1;

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

  if (v->spectrum_style != spec_style_prev) {
    /* Start the new look from a clean panel: the incremental path and the full redraw
     * styles would otherwise leave pieces of each other behind. */
    spec_style_prev = v->spectrum_style;
    gfx_fill(SP_X, SP_BASE - SP_H - 2, SP_W, SP_H + 26, c_panel);
    draw_spectrum_grid();
    for (int i = 0; i < SP_BARS; i++) {
      disp_bands[i] = 0; peak_bands[i] = 0; cap_lo[i] = cap_hi[i] = 0; peak_hold[i] = 0;
    }
  }
  if (v->spectrum_style) { update_spectrum_alt(v); return; }

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

    /* Interpolate between the 16 measured bands: 60 bars drawn from the nearest band
     * alone looked like separate sticks, not a spectrum. */
    int si = (i * (FMT_BANDS - 1)) / (SP_BARS - 1);
    int t = v->bands[si];
    {
      int num = (i * (FMT_BANDS - 1)) % (SP_BARS - 1);
      if (num && si + 1 < FMT_BANDS) {
        int nx = v->bands[si + 1];
        t += (nx - t) * num / (SP_BARS - 1);
      }
    }
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
    /* the cap is a separate blue marker so the level and the peak never blend together */
    if (peak > 3 && hi > lo) {
      gfx_fill(bx, SP_BASE - hi, SP_BW, hi - lo, c_spec);
      if (lo > mh + 2) gfx_fill(bx, SP_BASE - lo + 1, SP_BW, 1, c_vud);
    }
    cap_lo[i] = lo;
    cap_hi[i] = hi;
  }
  draw_spectrum_segments();

  /* A short dim reflection under the baseline makes the panel read as a lit surface
   * instead of bars floating in the dark. */
  gfx_fill(SP_X, SP_BASE + 2, SP_W, 22, c_panel);
  for (int i = 0; i < SP_BARS; i++) {
    int mh = disp_bands[i] * SP_H / 255;
    int rh = mh / 3;
    if (rh > 18) rh = 18;
    if (rh <= 0) continue;
    gfx_fill(SP_X + i * SP_STEP, SP_BASE + 2, SP_BW, rh, c_vud);
    if (rh > 4) gfx_fill(SP_X + i * SP_STEP, SP_BASE + 2 + rh - 2, SP_BW, 2, c_grid);
  }
}

/* The other two looks redraw the whole panel: the clear is one rect fill and the bars are
 * a handful of fills each, which stays cheap at the panel refresh rate. */
static void spec_erase_col(int bx, int y0, int y1) {
  if (y1 <= y0) return;
  gfx_fill(bx, y0, SP_BW, y1 - y0, c_panel);
  int gy[3] = { SP_BASE - 40, SP_BASE - 80, SP_BASE - 120 };
  for (int g = 0; g < 3; g++)
    if (gy[g] >= y0 && gy[g] < y1) gfx_fill(bx, gy[g], SP_BW, 1, c_grid);
}

static void spec_zone_fill(int bx, int y_lo, int y_hi) {
  int t1 = SP_H / 3, t2 = SP_H * 2 / 3;
  int a = y_lo, b = y_hi;
  int e = b < t1 ? b : t1;
  if (e > a) gfx_fill(bx, SP_BASE - e, SP_BW, e - a, c_vu);
  a = a > t1 ? a : t1; e = b < t2 ? b : t2;
  if (e > a) gfx_fill(bx, SP_BASE - e, SP_BW, e - a, c_yellow);
  a = a > t2 ? a : t2;
  if (b > a) gfx_fill(bx, SP_BASE - b, SP_BW, b - a, c_red);
}

/* The other looks use the same incremental scheme as the LED one, with the cap attached
 * to the bar (as in the LED bars) instead of a separate floating marker, so the erase of
 * the bar always covers the cap it drew. */
static void update_spectrum_alt(const player_view_t *v) {
  int style = v->spectrum_style;
  int center = SP_BASE - SP_H / 2;
  int t1 = SP_H / 3, t2 = SP_H * 2 / 3;
  for (int i = 0; i < SP_BARS; i++) {
    int si = (i * (FMT_BANDS - 1)) / (SP_BARS - 1);
    int t = v->bands[si];
    int num = (i * (FMT_BANDS - 1)) % (SP_BARS - 1);
    if (num && si + 1 < FMT_BANDS) t += (v->bands[si + 1] - t) * num / (SP_BARS - 1);
    t = t * (70 + (i * 30) / (SP_BARS - 1)) / 100;
    t = t * 3 / 4;
    t = (t - SP_LO) * 255 / (SP_HI - SP_LO);
    if (t < 0) t = 0; else if (t > 255) t = 255;

    int old = disp_bands[i];
    int nv = old;
    if (t > nv) nv = t;
    else { nv -= 14; if (nv < t) nv = t; }
    disp_bands[i] = nv;
    int peak = peak_bands[i];
    if (nv > peak) { peak = nv; peak_hold[i] = 6; }
    else if (peak_hold[i] > 0) peak_hold[i]--;
    else { peak -= 4; if (peak < nv) peak = nv; }
    if (peak < 0) peak = 0;
    peak_bands[i] = peak;

    int bx = SP_X + i * SP_STEP;
    int oh = old * SP_H / 255;
    int nh = nv * SP_H / 255;

    if (style == 1) {
      if (cap_hi[i] > cap_lo[i])
        spec_erase_col(bx, SP_BASE - cap_hi[i], SP_BASE - cap_lo[i]);
      if (nh > oh) spec_zone_fill(bx, oh, nh);
      else if (nh < oh) spec_erase_col(bx, SP_BASE - oh, SP_BASE - nh);

      uint8_t tc = nh <= t1 ? c_vu : (nh <= t2 ? c_yellow : c_red);
      if (nh > 0) gfx_fill(bx, SP_BASE - nh, SP_BW, 1, tc);

      gfx_fill(bx, SP_BASE + 2, SP_BW, 18, c_panel);
      int rh = nh / 3; if (rh > 18) rh = 18;
      if (rh > 0) gfx_fill(bx, SP_BASE + 2, SP_BW, rh, c_vud);

      if (nh > 2) {
        int hi = nh + 2 < SP_H ? nh + 2 : SP_H;
        gfx_fill(bx, SP_BASE - hi, SP_BW, hi - nh, c_spec);
        cap_lo[i] = nh; cap_hi[i] = hi;
      } else cap_lo[i] = cap_hi[i] = 0;
    } else {
      int ohh = oh / 2, nhh = nh / 2;
      if (cap_lo[i] > 0) {
        spec_erase_col(bx, center - cap_lo[i], center - cap_lo[i] + 1);
        spec_erase_col(bx, center + cap_lo[i] - 1, center + cap_lo[i]);
        cap_lo[i] = 0;
      }
      if (nhh > ohh) {
        uint8_t cu = nhh > t1 / 2 ? c_yellow : c_vu;
        gfx_fill(bx, center - nhh, SP_BW, nhh - ohh, cu);
        gfx_fill(bx, center + ohh, SP_BW, nhh - ohh, c_vud);
      } else if (nhh < ohh) {
        spec_erase_col(bx, center - ohh, center - nhh);
        spec_erase_col(bx, center + nhh, center + ohh);
      }
      if (nhh > 1) {
        gfx_fill(bx, center - nhh - 1, SP_BW, 1, c_spec);
        gfx_fill(bx, center + nhh, SP_BW, 1, c_spec);
        cap_lo[i] = nhh + 1;
      } else cap_lo[i] = 0;
    }
  }
}

static void update_player_dynamic(const player_view_t *v, int force) {
  if (!v->now) return;
  update_progress(v);
  update_meter(v->peak_l, 132, &disp_vu_l);
  update_meter(v->peak_r, 154, &disp_vu_r);
  update_channels(v);
  update_spectrum(v);
  const char *wnow = v->warning ? v->warning : "";
  int warn_changed = strcmp(wnow, last_warn) != 0;
  if (force || v->elapsed != last_elapsed || warn_changed) {
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
  if (v->font_step != last_font_step) {           /* Shift+= / Shift+-: repaint now */
    last_font_step = v->font_step;
    cur_font_step = v->font_step;
    changed = 1;
    draw_left_base(v);
    draw_list_area(v);
    last_top = v->top;
    last_sel = v->sel;
    update_footer(v);
    return;
  }
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
