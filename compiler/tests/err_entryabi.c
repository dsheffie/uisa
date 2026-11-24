// expect: only available in an entry function
u32 f(void) { return __opword(); }
entry void e(void) { __wrg(D0, f()); __gexit(); }
