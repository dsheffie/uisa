// expect: arrays: only available with --program
struct s { u32 a[4]; };
entry void err_arrayfield(void) {
  __wrg(GPC, __next_pc());
  __gexit();
}
