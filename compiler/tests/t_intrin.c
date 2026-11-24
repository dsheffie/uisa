// Intrinsics: dynamic guest-register access, bit operations, flags, exits.
entry void t_intrin(void) {
  u32 a = __rdg(D0), b = __rdg(D1);
  if (b == 0x7fff) __gfault(5);
  if (a == 0xffffffff) __brk(9);
  __wrg((a >> 4) & 7, __rdg((b & 7) + 8) + 1);
  __wrg(D2, __clz(a) + __ctz(b) * 64 + __cpop(a ^ b) * 4096);
  __wrg(D3, __rol(a, b) ^ __ror(b, a));
  __wrg(D4, __minu(a, b) + __max(a, b) + __min(a, b) * 3 + __maxu(a, b) * 5);
  __wrg(D5, __mulhu(a, b) ^ __mulh(a, b) ^ __rev8(a));
  __wrg(D6, __bfextui(a, 4, 12) + __mrg8(a, b) + __mrg16(b, a) + __bfextui(b, 28, 8));
  u32 ccr = __rdccr();
  __wrccr((ccr ^ a) & 31);
  __wrg(A5, __opword() + __ext01() * 3 + __ext23() * 5);
  __wrg(GPC, __next_pc());
  __gexit();
}
