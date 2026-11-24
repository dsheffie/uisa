// expect: global variables are only available with --program
u32 counter = 3;
entry void err_globalvar(void) {
  __wrg(GPC, __next_pc());
  __gexit();
}
