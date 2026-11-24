// expect: recursion is not allowed
u32 even(u32 x) { return x == 0 ? 1 : odd(x - 1); }
u32 odd(u32 x) { return x == 0 ? 0 : even(x - 1); }
entry void e(void) { __wrg(D0, even(__rdg(D1))); __gexit(); }
