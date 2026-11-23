// AY-family format wrappers. All of them feed the same libayfly engine and the two YM2149s.
#include <string.h>
#include "../fmt.h"
#include "../fpga.h"

static int yes(const uint8_t *p, uint32_t n) { (void) p; (void) n; return 1; }
static const char *err(void) { return pt3_error(); }
static int play_pt1(const play_src_t *s, uint32_t n, play_progress_t p) { return pt1_start(s, n, p); }
static int play_asc(const play_src_t *s, uint32_t n, play_progress_t p) { return asc_start(s, n, p); }
static int play_psc(const play_src_t *s, uint32_t n, play_progress_t p) { return psc_start(s, n, p); }
static int play_sqt(const play_src_t *s, uint32_t n, play_progress_t p) { return sqt_start(s, n, p); }
static int play_stp(const play_src_t *s, uint32_t n, play_progress_t p) { return stp_start(s, n, p); }
static int play_vtx(const play_src_t *s, uint32_t n, play_progress_t p) { return vtx_start(s, n, p); }
static int play_psg(const play_src_t *s, uint32_t n, play_progress_t p) { return psg_start(s, n, p); }

const fmt_module_t fmt_pt1 = { "pt1", "pt1", yes, play_pt1, err };
const fmt_module_t fmt_asc = { "asc", "asc", yes, play_asc, err };
const fmt_module_t fmt_psc = { "psc", "psc", yes, play_psc, err };
const fmt_module_t fmt_sqt = { "sqt", "sqt", yes, play_sqt, err };
const fmt_module_t fmt_stp = { "stp", "stp", yes, play_stp, err };
const fmt_module_t fmt_vtx = { "vtx", "vtx", yes, play_vtx, err };
const fmt_module_t fmt_psg = { "psg", "psg", yes, play_psg, err };
