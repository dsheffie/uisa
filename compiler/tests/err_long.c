// expect: 'long' is not part of the millicode C subset
entry void err_long(void) {
  long x = 1;
  __wrg(GPC, __next_pc());
  __gexit();
}
