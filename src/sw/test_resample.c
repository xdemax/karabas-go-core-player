#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "resample.h"

int test_resample_passthrough(void) {
  rs_t rs;
  rs_init(&rs, 48000);
  
  int16_t l = 100, r = 200;
  int16_t out[2][2];
  
  int n = rs_frame(&rs, l, r, out);
  if (n != 1) {
    printf("FAIL: passthrough count %d\n", n);
    return 0;
  }
  if (out[0][0] != l || out[0][1] != r) {
    printf("FAIL: passthrough values %d,%d expected %d,%d\n", out[0][0], out[0][1], l, r);
    return 0;
  }
  printf("PASS: passthrough\n");
  return 1;
}

int test_resample_32k_to_48k(void) {
  rs_t rs;
  rs_init(&rs, 32000);
  
  int16_t out[2][2];
  int total = 0;
  
  for (int i = 0; i < 192; i++) {
    int16_t l = (int16_t)(32767 * (i + 1) / 192);
    int16_t r = (int16_t)(32767 * (i + 1) / 192);
    int n = rs_frame(&rs, l, r, out);
    total += n;
  }
  
  if (total < 288 || total > 290) {
    printf("FAIL: 32k->48k output length %d (expected ~288)\n", total);
    return 0;
  }
  
  printf("PASS: 32k->48k (output: %d samples)\n", total);
  return 1;
}

int test_resample_24k_to_48k(void) {
  rs_t rs;
  rs_init(&rs, 24000);
  
  int16_t out[2][2];
  int total = 0;
  int prev = 0;
  int monotonic = 1;
  
  for (int i = 0; i < 240; i++) {
    int16_t l = (int16_t)(32767 * (i + 1) / 240);
    int16_t r = (int16_t)(32767 * (i + 1) / 240);
    int n = rs_frame(&rs, l, r, out);
    
    for (int j = 0; j < n; j++) {
      int val = out[j][0];
      if (val < prev - 1) {
        monotonic = 0;
      }
      prev = val;
    }
    total += n;
  }
  
  if (total < 478 || total > 482) {
    printf("FAIL: 24k->48k output length %d (expected ~480)\n", total);
    return 0;
  }
  
  if (!monotonic) {
    printf("WARN: 24k->48k non-monotonic ramp\n");
  }
  
  printf("PASS: 24k->48k (output: %d samples, monotonic: %s)\n", total, monotonic ? "yes" : "no");
  return 1;
}

int main(void) {
  int passed = 0, total = 3;
  
  passed += test_resample_passthrough();
  passed += test_resample_32k_to_48k();
  passed += test_resample_24k_to_48k();
  
  printf("\n%d/%d tests passed\n", passed, total);
  return passed == total ? 0 : 1;
}
