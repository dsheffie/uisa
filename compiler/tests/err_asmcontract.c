// expect: 1 argument registers for 2 argument words
extern u32 g(u32 a, u32 b) asm("x20 -> x23");
entry void e(void) { __wrg(D0, g(1, 2)); __gexit(); }
