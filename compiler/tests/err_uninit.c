// expect: 'y' may be used before it is assigned
entry void e(void) {
  u32 y;
  if (__rdg(D0)) y = 3;
  __wrg(D1, y);
  __gexit();
}
