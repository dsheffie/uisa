// expect: calls itself
inline u32 f(u32 x) { return x ? f(x - 1) : 0; }
entry void e(void) { __wrg(D0, f(__rdg(D1))); __gexit(); }
