// expect: sizeof: only available with --program
entry void err_sizeof(void) {
  u32 a = sizeof(u32);
  __wrg(GPC, __next_pc() + a);
  __gexit();
}
