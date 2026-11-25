// The stereo audio FIFO of menu_soc (0x70 / 0x74): the FPGA mixer writes 48 kHz samples; the
// top takes them to the DAC and HDMI.
#pragma once
#include <stdint.h>

#define AUDIO_CAP 8192

int audio_free(void);                     // the free entries of the FIFO
void audio_write(int16_t l, int16_t r);   // one sample (left, right)
void audio_note_written(int n);          // count samples produced by the RTL AY pair
int audio_rate_test(void);                // measure the real drain rate (Hz), -1 on a timeout
uint32_t audio_written(void);             // samples written since the start (for the MIX diagnostic)
int audio_rate(void);
const char *audio_rate_debug(void);   // "R<asked>/<fpga>" for the footer                     // the measured or configured rate (Hz)
int audio_set_rate(int hz);               // set the FPGA mixer rate, returns the effective one
uint16_t audio_drop(void);                // writes attempted while the FIFO was full
uint16_t audio_under(void);               // audio ticks with an empty FIFO
void audio_reset_stats(void);
