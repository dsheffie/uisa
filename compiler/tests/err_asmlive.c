// expect: is still needed after the call to fix, which overwrites it
extern u32 fix(u32 a) asm("x20 -> x23; reads x4; clobbers x4");
entry void e(void) { __wrg(D0, fix(1)); __wrg(GPC, __next_pc()); __gexit(); }
