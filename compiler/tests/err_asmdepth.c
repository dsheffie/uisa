// expect: nests 5 calls but the millicode return stack holds 4
extern u32 g(u32 a) asm("x20 -> x23; depth 3");
u32 h2(u32 v) { return g(v); }
u32 h1(u32 v) { return h2(v); }
entry void e(void) { __wrg(D0, h1(1)); __gexit(); }
