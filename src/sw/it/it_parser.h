/*
 * it_parser.h - Impulse Tracker (.it) parser, integer-only, no libc I/O.
 *
 * Design:
 *  - The whole file lives in memory (SDRAM) and stays valid while the module
 *    is used: packed patterns and zero-copy samples point into it.
 *  - All metadata and decoded sample data are allocated from caller-supplied
 *    arenas (no malloc). Nothing is allocated after it_load() returns.
 *  - Every read is bounds-checked. Patterns are fully validated at load time,
 *    so the runtime row decoder never leaves its pattern buffer.
 *  - Behaviour is selected from Cmwt and header flags, never from a version
 *    string. Cwt is stored for information only.
 */
#ifndef IT_PARSER_H
#define IT_PARSER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- limits ---------------------------------------------------------- */
#define IT_MAX_CHANNELS   64
#define IT_MAX_ORDERS     256
#define IT_MAX_INSTR      255
#define IT_MAX_SAMPLES    4000
#define IT_MAX_PATTERNS   4000
#define IT_MAX_ROWS       1024      /* IT proper: 32..200 */
#define IT_ENV_NODES      25
#define IT_MIDI_CFG_SIZE  4896      /* 9*32 global + 16*32 SFx + 128*32 Zxx */
#define IT_MAX_WARN       32

/* ---- header flags (it_module_t.flags) -------------------------------- */
#define IT_F_STEREO       0x0001
#define IT_F_VOL0MIX      0x0002
#define IT_F_INSTR        0x0004    /* instrument mode (else sample mode) */
#define IT_F_LINEAR       0x0008    /* linear slides                      */
#define IT_F_OLDEFF       0x0010    /* old effects                        */
#define IT_F_COMPATGXX    0x0020    /* link Gxx memory with Exx/Fxx       */
#define IT_F_MIDIPITCH    0x0040
#define IT_F_REQMIDICFG   0x0080
/* special */
#define IT_S_MESSAGE      0x0001
#define IT_S_HISTORY      0x0002
#define IT_S_MIDICFG      0x0008

/* ---- load flags ------------------------------------------------------ */
#define IT_LOAD_STRICT      0x01    /* any corruption -> error, no recovery  */
#define IT_LOAD_NOZEROCOPY  0x02    /* always copy PCM into the sample arena */

/* ---- error / warning codes ------------------------------------------- */
enum {
    IT_OK = 0,
    IT_ERR_ARGS,
    IT_ERR_TOO_SMALL,
    IT_ERR_MAGIC,
    IT_ERR_VERSION,
    IT_ERR_COUNTS,
    IT_ERR_TRUNC,           /* header / offset tables beyond end of file */
    IT_ERR_NOMEM,           /* arena exhausted                           */
    IT_ERR_INSTR,
    IT_ERR_SAMPLE,
    IT_ERR_SAMPLE_DATA,
    IT_ERR_COMPRESS,
    IT_ERR_PATTERN
};

enum {
    IT_W_NONE = 0,
    IT_W_CMWT_UNKNOWN,      /* Cmwt outside 0x100..0x217                 */
    IT_W_ORDERS_CLAMPED,
    IT_W_ORDER_BAD_PAT,     /* index = order position                    */
    IT_W_HISTORY_BAD,
    IT_W_MIDICFG_BAD,
    IT_W_MESSAGE_BAD,
    IT_W_INSTR_SKIPPED,     /* index = instrument number (1-based)       */
    IT_W_ENV_FIXED,
    IT_W_SAMPLE_SKIPPED,    /* index = sample number (1-based)           */
    IT_W_SAMPLE_TRUNC,
    IT_W_LOOP_FIXED,
    IT_W_C5SPEED_ZERO,
    IT_W_IT215_MISMATCH,    /* Cvt delta bit and Cmwt disagree           */
    IT_W_PATTERN_SKIPPED,   /* pattern header unusable -> empty          */
    IT_W_PATTERN_TRUNC,     /* pattern, row = first lost row             */
    IT_W_PATTERN_ROWS,      /* row count outside 32..200                 */
    IT_W_BAD_COMMAND        /* effect > Z, pattern/row/ch = first hit    */
};

typedef struct {
    uint16_t code;
    int16_t  index;
    int16_t  row;
    int16_t  ch;
    uint32_t offset;
} it_warn_t;

typedef struct {
    int       err;          /* IT_OK or IT_ERR_*                         */
    int       pattern;      /* -1 if n/a                                  */
    int       row;
    int       channel;
    int       index;        /* instrument / sample number (1-based)       */
    uint32_t  offset;       /* file offset of the failing object          */
    unsigned  nwarn;        /* stored in warn[]                           */
    unsigned  nwarn_total;  /* may exceed IT_MAX_WARN                     */
    it_warn_t warn[IT_MAX_WARN];
} it_diag_t;

const char *it_err_str(int code);
const char *it_warn_str(int code);

/* ---- arena ------------------------------------------------------------ */
typedef struct {
    uint8_t *base;
    size_t   size;
    size_t   used;
} it_arena_t;

void it_arena_init(it_arena_t *a, void *mem, size_t size);

/* ---- envelopes --------------------------------------------------------- */
#define IT_ENV_ON       0x01
#define IT_ENV_LOOP     0x02
#define IT_ENV_SUSLOOP  0x04
#define IT_ENV_CARRY    0x08
#define IT_ENV_FILTER   0x80    /* pitch envelope slot only: filter env  */

typedef struct {
    int16_t  value;     /* volume: 0..64, pan/pitch: -32..32,
                           filter (IT_ENV_FILTER): 0..64 (file value + 32) */
    uint16_t tick;
} it_env_node_t;

typedef struct {
    uint8_t flags;
    uint8_t num;        /* 0..25; 0 => ON bit cleared by the loader */
    uint8_t lpb, lpe;   /* loop begin/end node indices              */
    uint8_t slb, sle;   /* sustain loop node indices                */
    it_env_node_t node[IT_ENV_NODES];
} it_env_t;

/* ---- instrument -------------------------------------------------------- */
#define IT_NNA_CUT     0
#define IT_NNA_CONT    1
#define IT_NNA_OFF     2
#define IT_NNA_FADE    3
#define IT_DCT_OFF     0
#define IT_DCT_NOTE    1
#define IT_DCT_SAMPLE  2
#define IT_DCT_INST    3
#define IT_DCA_CUT     0
#define IT_DCA_OFF     1
#define IT_DCA_FADE    2

typedef struct {
    char     name[27];
    char     dosname[13];
    uint8_t  present;       /* 1 if a header was loaded                  */
    uint8_t  old_format;    /* Cmwt < 0x200 instrument layout             */
    uint8_t  nna, dct, dca;
    uint8_t  gbv;           /* global volume 0..128                       */
    uint8_t  dfp;           /* raw: bit7 set = no default pan, else 0..64 */
    uint8_t  rv, rp;        /* random volume / pan variation (%)          */
    int8_t   pps;           /* pitch-pan separation                       */
    uint8_t  ppc;           /* pitch-pan centre note                      */
    uint16_t fadeout;       /* raw 0..1024 (old format: raw byte pair)    */
    uint8_t  ifc, ifr;      /* raw initial filter cutoff/resonance, bit7=on */
    uint8_t  mch, mpr;
    uint16_t mbank;
    uint16_t trkvers;
    uint8_t  kb_note[120];  /* note remap                                 */
    uint8_t  kb_smp[120];   /* 1-based sample, 0 = none                   */
    it_env_t vol, pan, pitch;
} it_instrument_t;

/* ---- sample ------------------------------------------------------------ */
#define IT_SMP_DATA      0x01
#define IT_SMP_16BIT     0x02
#define IT_SMP_STEREO    0x04
#define IT_SMP_COMPRESSED 0x08
#define IT_SMP_LOOP      0x10
#define IT_SMP_SUSLOOP   0x20
#define IT_SMP_PINGPONG  0x40
#define IT_SMP_SUSPINGPONG 0x80

#define IT_CVT_SIGNED    0x01
#define IT_CVT_BIGENDIAN 0x02
#define IT_CVT_DELTA     0x04

#define IT_LOOP_NONE     0
#define IT_LOOP_FWD      1
#define IT_LOOP_PINGPONG 2

typedef struct {
    char     name[27];
    char     dosname[13];
    uint8_t  flags;         /* raw Flg                                    */
    uint8_t  cvt;
    uint8_t  gvl;           /* 0..64                                      */
    uint8_t  vol;           /* 0..64                                      */
    uint8_t  dfp;           /* raw: bit7 = use, 0..64                     */
    uint8_t  vis, vid, vir, vit;   /* auto-vibrato speed/depth/rate/wave  */
    uint32_t length;        /* in samples (per channel)                   */
    uint32_t loop_begin, loop_end;      /* validated, end exclusive       */
    uint32_t sus_begin, sus_end;
    uint32_t c5speed;
    uint32_t data_off;      /* raw SamplePointer                          */
    uint8_t  loop_mode;     /* IT_LOOP_* after validation                 */
    uint8_t  sus_mode;
    uint8_t  bits;          /* 8 or 16; 0 if no data                      */
    uint8_t  channels;      /* 1 or 2; 0 if no data                       */
    const void *pcm;        /* int8_t* / int16_t*, signed, native endian   */
    const void *pcm_r;      /* right channel for stereo, else NULL        */
} it_sample_t;

/* ---- pattern ----------------------------------------------------------- */
typedef struct {
    const uint8_t *data;    /* packed data inside the file, NULL if empty */
    uint16_t len;           /* bytes available                            */
    uint16_t rows;          /* nominal row count                          */
    uint16_t valid_rows;    /* rows that decode cleanly (<= rows)         */
} it_pattern_t;

typedef struct {
    uint8_t note;           /* 0..119, 254 cut, 255 off, 120..253 fade    */
    uint8_t inst;           /* 1-based                                    */
    uint8_t vol;            /* raw volume/pan column byte                 */
    uint8_t cmd;            /* 1..26 = A..Z                               */
    uint8_t param;
    uint8_t flags;
} it_cell_t;

#define IT_CF_NOTE 0x01
#define IT_CF_INST 0x02
#define IT_CF_VOL  0x04
#define IT_CF_CMD  0x08

/* ---- module ------------------------------------------------------------ */
typedef struct {
    const uint8_t *file;
    size_t   file_size;

    char     title[27];
    uint16_t ordnum, insnum, smpnum, patnum;
    uint16_t cwt, cmwt, flags, special;
    uint8_t  gv, mv, speed, tempo, sep, pwd;
    uint8_t  chn_pan[IT_MAX_CHANNELS];   /* 0..64, 100 = surround, >=128 off */
    uint8_t  chn_vol[IT_MAX_CHANNELS];   /* 0..64 */

    uint8_t        *orders;      /* ordnum (clamped to IT_MAX_ORDERS) bytes */
    uint16_t        song_len;    /* orders before first 255 (or all)        */
    it_instrument_t *ins;        /* insnum entries, index 0 = instrument 1  */
    it_sample_t     *smp;        /* smpnum entries, index 0 = sample 1      */
    it_pattern_t    *pat;        /* patnum entries                          */

    const uint8_t *message;      /* NOT NUL-terminated, CR-separated        */
    uint16_t       message_len;
    const uint8_t *midi_cfg;     /* IT_MIDI_CFG_SIZE bytes in file, or NULL */

    uint8_t  used_channels;      /* highest channel with data + 1           */
    uint8_t  old_instruments;    /* Cmwt < 0x200                            */
} it_module_t;

void it_arena_init(it_arena_t *a, void *mem, size_t size);

/*
 * Load a module. meta: module structures. pcm: decoded sample data
 * (may be NULL => meta is used). Returns IT_OK or IT_ERR_*.
 * In non-strict mode damaged instruments/samples/patterns are skipped or
 * truncated with a warning in diag; only structural damage fails.
 */
int it_load(const uint8_t *file, size_t size,
            it_arena_t *meta, it_arena_t *pcm,
            uint32_t load_flags, it_diag_t *diag, it_module_t *m);

/* ---- sequential pattern decoder (usable at runtime, no libc) ------------ */
typedef struct {
    const uint8_t *p, *end;
    uint16_t row, rows, valid;
    uint8_t  empty;
    uint8_t  last_ch;
    uint8_t  lastmask[IT_MAX_CHANNELS];
    uint8_t  lastnote[IT_MAX_CHANNELS];
    uint8_t  lastinst[IT_MAX_CHANNELS];
    uint8_t  lastvol[IT_MAX_CHANNELS];
    uint8_t  lastcmd[IT_MAX_CHANNELS];
    uint8_t  lastparam[IT_MAX_CHANNELS];
} it_patdec_t;

#define IT_ROW_OK       0
#define IT_ROW_END      1    /* past last row                     */
#define IT_ROW_CORRUPT  (-1)

void it_patdec_init(it_patdec_t *d, const it_pattern_t *pat);

/*
 * Decode the next row. cells[] entries are valid ONLY for channels whose bit is
 * set in present[ch>>5] (bit ch&31); nothing is cleared, so the per-row cost
 * is proportional to the number of populated cells.
 * Rows past valid_rows are returned as empty rows (IT_ROW_OK).
 */
int it_patdec_row(it_patdec_t *d, it_cell_t cells[IT_MAX_CHANNELS],
                  uint32_t present[2]);

/* Position the decoder so the next it_patdec_row() returns `row`. */
void it_patdec_seek(it_patdec_t *d, const it_pattern_t *pat, unsigned row);

#ifdef __cplusplus
}
#endif
#endif
