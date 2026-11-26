// The hardware SID 8580 in the SoC: a small register-level test driver for now.
#pragma once
int sid_hw_present(void);
void sid_test_tone(int on);
int sid_tap_peak(int ms);
