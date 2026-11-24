// 64-bit parameters and results across calls, loops and switch.
u64 mul64(u64 a, u64 b) { return a * b; }
s64 mix(s64 a, u32 k, s64 b) {
  if (a < b) return mul64(a, b) + k;
  return (a ^ b) >> (k & 63);
}
u64 rotl(u64 x, u32 k) { k &= 63; return k ? (x << k) | (x >> (64 - k)) : x; }
u32 bits(u64 x) {           // population count by loop
  u32 n = 0;
  while (x) { x &= x - 1; n++; }
  return n;
}
u64 sel(u64 v) {
  switch (v) {
    case 0: return 11;
    case 1: return 0x100000000ull;
    case 0xffffffffffffffffull: return 5;
    case 0x7fffffff: return v * 3;
    default: return v + 1;
  }
}

entry void t_u64call(void) {
  u64 a = ((u64)__rdg(D1) << 32) | __rdg(D0);
  s64 b = (s64)(((u64)__rdg(D3) << 32) | __rdg(D2));
  u32 k = __rdg(D4);
  s64 m = mix(a, k, b);
  u64 r = rotl(a, k);
  __wrg(D0, (u32)m);
  __wrg(D1, (u32)(m >> 32));
  __wrg(D2, (u32)r);
  __wrg(D3, (u32)(r >> 32));
  __wrg(D4, bits(a) + bits(b) * 100);
  u64 acc = 1;
  for (u32 i = 0; i < (k & 7); i++) acc = acc * a + i;
  __wrg(D5, (u32)acc ^ (u32)(acc >> 32));
  u64 s = sel(a & 3 ? a : (a >> 1) & 1) + sel((u64)__rdg(D5) - 1) + sel(0x7fffffff);
  __wrg(D6, (u32)s);
  __wrg(D7, (u32)(s >> 32));
  __wrg(GPC, __next_pc());
  __gexit();
}
