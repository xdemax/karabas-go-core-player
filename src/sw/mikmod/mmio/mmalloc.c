// MikMod allocations in the SDRAM arena. The player loads one module at a time, so a bump
// allocator with a reset per load is enough; no module allocation is freed while it plays.
#include <string.h>
#include <stddef.h>
#include "mikmod_mem.h"
#include "mikmod_internals.h"

typedef struct { size_t size; } mblock_t;

static unsigned char *mbase, *mcur, *mend;

void mikmod_mem_set(void *base, size_t size) {
  mbase = mcur = (unsigned char *) base;
  mend = mbase + size;
}

static void *m_alloc(size_t size, int zero) {
  size_t need = sizeof(mblock_t) + size;
  need = (need + 15u) & ~(size_t) 15u;
  if (!mbase || mcur + need > mend) {
    _mm_errno = MMERR_OUT_OF_MEMORY;
    if (_mm_errorhandler) _mm_errorhandler();
    return NULL;
  }
  mblock_t *b = (mblock_t *) mcur;
  mcur += need;
  b->size = size;
  void *p = (void *) (b + 1);
  if (zero) memset(p, 0, size);
  return p;
}

void *MikMod_malloc(size_t size) { return m_alloc(size, 0); }
void *MikMod_calloc(size_t nitems, size_t size) { return m_alloc(nitems * size, 1); }
void *MikMod_realloc(void *data, size_t size) {
  if (!data) return m_alloc(size, 1);
  mblock_t *b = ((mblock_t *) data) - 1;
  void *n = m_alloc(size, 1);
  if (!n) return NULL;
  memcpy(n, data, b->size < size ? b->size : size);
  return n;
}
void MikMod_free(void *data) { (void) data; }
CHAR *MikMod_strdup(const CHAR *s) {
  if (!s) return NULL;
  size_t l = strlen(s) + 1;
  CHAR *d = (CHAR *) m_alloc(l, 0);
  if (d) memcpy(d, s, l);
  return d;
}
