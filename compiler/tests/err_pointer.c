// expect: pointers: only available with --program
entry void err_pointer(void) {
  u32 x = 1;
  u32 *p = &x;
  __wrg(D0, *p);
  __gexit();
}
