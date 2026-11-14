// expect: nests 5 calls but the millicode return stack holds 4
u32 f5(u32 x) { return x + 5; }
u32 f4(u32 x) { return f5(x) + 4; }
u32 f3(u32 x) { return f4(x) + 3; }
u32 f2(u32 x) { return f3(x) + 2; }
u32 f1(u32 x) { return f2(x) + 1; }
entry void e(void) { __wrg(D0, f1(__rdg(D1))); __gexit(); }
