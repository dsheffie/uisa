// 64-bit division and remainder, signed and unsigned.
entry void t_u64div(void) {
  u64 a = ((u64)__rdg(D1) << 32) | __rdg(D0);
  u64 b = ((u64)__rdg(D3) << 32) | __rdg(D2);
  u32 sel = __rdg(D4);
  if (sel & 1) b >>= 32;          // also exercise the 32-bit fast path
  if (sel & 2) b &= 0xffff;
  if (b == 0) b = 7;
  s64 sa = (s64)a, sb = (s64)b;
  if (sa == (s64)0x8000000000000000ull && sb == -1) sb = 3;
  u64 q = a / b, r = a % b;
  s64 sq = sa / sb, sr = sa % sb;
  __wrg(D0, (u32)q);
  __wrg(D1, (u32)(q >> 32));
  __wrg(D2, (u32)r);
  __wrg(D3, (u32)(r >> 32));
  __wrg(D4, (u32)sq);
  __wrg(D5, (u32)(sq >> 32));
  __wrg(D6, (u32)sr);
  __wrg(D7, (u32)(sr >> 32));
  // constant divisors: powers of two become shifts and masks
  u64 c = a / 4096 + a % 1024 + a / 1000000007;
  s64 sc = sa / 8 + sa % 16;
  __wrg(A0, (u32)c ^ (u32)(c >> 32));
  __wrg(A1, (u32)sc ^ (u32)(sc >> 32));
  __wrg(GPC, __next_pc());
  __gexit();
}
