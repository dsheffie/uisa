/* Forced into Previous's FPU sources by test_fpu.py: fpp.c logs FPU
 * exceptions with write_log = printf; the test does not want them. */
#include <stdio.h>
#define printf(...) ((void)0)
