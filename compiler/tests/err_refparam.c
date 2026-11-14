// expect: only inline functions take references
void f(u32 &x) { x = 1; }
entry void e(void) { u32 y; f(y); __gexit(); }
