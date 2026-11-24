// expect: reads entry-ABI registers, so only entry functions can call it
extern u32 g(u32 a) asm("x20 -> x23; reads x2");
u32 h(u32 v) { return g(v); }
entry void e(void) { __wrg(D0, h(1)); __gexit(); }
