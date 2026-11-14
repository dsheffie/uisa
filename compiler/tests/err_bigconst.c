// expect: does not fit 64 bits
entry void e(void) { u64 x = 18446744073709551616; __wrg(D0, (u32)x); __gexit(); }
