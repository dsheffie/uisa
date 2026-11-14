// Guest-data loads: displacement folding, absolute addresses, sign extension.
entry void t_mem(void) {
  u32 p = __rdg(A0);
  u32 s = 0;
  for (u32 i = 0; i < 8; i++) s = s * 31 + __ld_8u_gd(p + i);
  __wrg(D0, s);
  __wrg(D1, __ld_32_gd(p + 100) + __ld_16u_gd(p - 2000) + __ld_8s_gd(p + 2047) + __ld_8u_gd(p + 5000));
  __wrg(D2, __ld_32_gd(0x12345678));
  __wrg(D3, __ld_8u_gd(1000) + __ld_8u_gd(-4));
  __wrg(D4, __ld_16u_gd(__rdg(A1) + 3) + __ld_8u_gd(__rdg(A2)));
  __wrg(GPC, __next_pc());
  __gexit();
}
