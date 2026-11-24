// expect: arrays: only available with --program
entry void err_array(void) {
  u32 a[4];
  __wrg(GPC, __next_pc());
  __gexit();
}
