// expect: has no field
struct s { u32 a; };
entry void e(void) { s v; v.a = 1; __wrg(D0, v.b); __gexit(); }
