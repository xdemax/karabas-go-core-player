#pragma once
#include <stdint.h>

void mixer_reset(void);
void mixer_write(int16_t l, int16_t r);
void mixer_write_noana(int16_t l, int16_t r);
void mixer_write_it(int16_t l, int16_t r);
