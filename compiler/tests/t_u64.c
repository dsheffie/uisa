// 64-bit arithmetic, logic, shifts, conversions and comparisons.
u64 get64(u32 lo_reg) { return ((u64)__rdg(lo_reg + 1) << 32) | __rdg(lo_reg); }

entry void t_u64(void) {
  u64 a = ((u64)__rdg(D1) << 32) | __rdg(D0);
  u64 b = ((u64)__rdg(D3) << 32) | __rdg(D2);
  s64 sa = (s64)a, sb = (s64)b;
  u32 n = __rdg(D4);
  s32 m = (s32)__rdg(D5);

  u64 s = a + b, d = a - b, p = a * b;
  __wrg(D0, (u32)s ^ (u32)(s >> 32));
  __wrg(D1, (u32)d + (u32)(d >> 32) * 3);
  __wrg(D2, (u32)p);
  __wrg(D3, (u32)(p >> 32));

  // shifts by variable and constant counts
  u32 k = n & 63;
  u64 l = a << k, r = a >> k;
  s64 ar = sa >> k;
  __wrg(D4, (u32)l ^ (u32)(l >> 32) ^ (u32)r ^ (u32)(r >> 40));
  __wrg(D5, (u32)ar + (u32)(ar >> 32));
  __wrg(D6, (u32)((a << 7) >> 39) + (u32)(sb >> 33) + (u32)(b << 32) + (u32)((b >> 31) >> 1));

  // conversions: sign and zero extension, truncation, mixed operands
  s64 e1 = m;                // sign-extends
  u64 e2 = (u32)m;           // zero-extends
  u64 e3 = m + a;            // s32 converted to u64
  s64 e4 = (s64)(s16)n * m;  // s16 -> s64, times s32 -> s64
  u8 t8 = (u8)(a >> 13);
  s16 t16 = (s16)(b >> 3);
  __wrg(D7, (u32)(e1 >> 32) + (u32)(e2 >> 32) * 5 + (u32)e3 + (u32)(e3 >> 32) + (u32)(e4 >> 16) + t8 + t16);

  // comparisons, as values and as branches
  u32 c = 0;
  c |= (a < b) << 0;
  c |= (a <= b) << 1;
  c |= (a > b) << 2;
  c |= (a >= b) << 3;
  c |= (a == b) << 4;
  c |= (a != b) << 5;
  c |= (sa < sb) << 6;
  c |= (sa <= sb) << 7;
  c |= (sa > sb) << 8;
  c |= (sa >= sb) << 9;
  c |= (sa < m) << 10;
  c |= (a < n) << 11;
  c |= (sa < 0) << 12;
  c |= (b == 0x100000000ull) << 13;
  c |= !a << 14;
  c |= (a && b) << 15;
  if (sa < sb) c += 0x10000;
  if (a <= b) c += 0x20000;
  if (sa >= sb) c += 0x40000;
  if (a > b) c += 0x80000;
  if (a == (b ^ 1)) c += 0x100000;
  if (a & b) c += 0x200000;
  if (a != 0 && b != 0) c += 0x400000;
  __wrg(A0, c);

  // logic, negation, complement, compound assignment, ++/--
  u64 x = a;
  x ^= ~b;
  x |= a & 0xff00ff00ff00ull;
  x = -x;
  x += 0xfffffffffull;
  x -= 3000000000;       // an s64 constant
  x *= 5;
  x <<= n & 31;
  x >>= 3;
  u64 y = x++;
  ++x;
  --y;
  __wrg(A1, (u32)x ^ (u32)(x >> 32));
  __wrg(A2, (u32)y + (u32)(y >> 32));
  // 64 x 64 high words from the parts (mulhu path) and a ternary on 64 bits
  u64 lo = (u64)(u32)a * (u32)b;
  u64 big = sa < sb ? a : b + 0x123456789ull;
  __wrg(A3, (u32)(lo >> 32) ^ (u32)lo);
  __wrg(A4, (u32)big ^ (u32)(big >> 32));
  __wrg(A5, (u32)get64(D6) + (u32)(get64(D6) >> 32));
  __wrg(GPC, __next_pc());
  __gexit();
}
