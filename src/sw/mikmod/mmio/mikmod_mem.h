#pragma once
#include <stddef.h>
// The memory arena of MikMod: the module structures and samples live in SDRAM.
void mikmod_mem_set(void *base, size_t size);
