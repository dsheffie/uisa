// Integer arithmetic, C conversions and comparisons over every type.
entry void t_arith(void) {
  u32 a = __rdg(D0), b = __rdg(D1), c = __rdg(D2);
  s32 sa = a, sb = b;
  u8 a8 = a; s8 b8 = b; u16 a16 = a; s16 b16 = b;
  __wrg(D0, a + b * c - (a ^ c));
  __wrg(D1, (sa >> (b & 31)) + (a >> (c & 31)) + (a << (sb & 15)));
  __wrg(D2, a8 * b8 + a16 * b16);
  __wrg(D3, (u8)(a8 + 200) + (s8)(b8 - 100) + (u16)(a16 * 3) + (s16)(b16 << 2));
  __wrg(D4, (sa < sb) + 2 * (a < b) + 4 * (b8 < a8) + 8 * (a16 >= b16) + 16 * (sa != (s32)c) +
                32 * (a8 == (u8)c) + 64 * (sb <= -5) + 128 * (b > 0x80000000u) + 256 * (b16 > 100) +
                512 * (a8 <= 17) + 1024 * (sa >= 40000) + 2048 * (a > 0xfffff000));
  __wrg(D5, ~a & (b | 0x12345678) ^ -sa);
  u32 q = c ? a / c : 7, r = c ? a % c : 9;
  s32 sq = (sb != 0 && !(sa == (s32)0x80000000 && sb == -1)) ? sa / sb : 3;
  s32 sr = (sb != 0 && !(sa == (s32)0x80000000 && sb == -1)) ? sa % sb : 4;
  __wrg(D6, q * 3 + r);
  __wrg(D7, sq ^ sr);
  __wrg(A0, a * 0x10001 + 0x12345678 - b * 8 + c / 4 + (a & 0xffff) + (b & 0xff00ff) + (c & 0xff));
  __wrg(A1, (a8 > 127) + (b8 < 0) * 2 + (s16)a16 + (u32)b16 + (u16)b8 + (s8)a16);
  u16 w = a16;
  w += b16;
  w <<= 3;
  s8 sb8 = b8;
  sb8 -= 7;
  sb8 *= 3;
  u32 post = a++;
  u32 pre = ++b;
  __wrg(A2, w + sb8 * 65536 + post + pre * 3 + a + b);
  __wrg(A3, (sa > 0) == (sb > 0) ? sa - sb : sa + sb);
  __wrg(A4, 0xdeadbeef);
  __wrg(A5, -1);
  __wrg(A6, (a | 0x80000000) >> 31 << 4 | (sa >> 31) & 0x0f00);
  __wrg(GPC, __next_pc());
  __gexit();
}
