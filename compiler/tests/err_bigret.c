// expect: only inline functions return more than two words
struct t3 { u32 a; u32 b; u32 c; };
t3 f(u32 x) { t3 r; r.a = x; r.b = x; r.c = x; return r; }
entry void e(void) { t3 v = f(1); __wrg(D0, v.a); __gexit(); }
