// Calls: argument permutations, values live across calls, four-deep chains.
u32 mix(u32 a, u32 b, u32 c) { return a * 3 + b * 5 + c * 7; }
u32 swap2(u32 a, u32 b) { return mix(b, a, a ^ b); }
u32 rot3(u32 a, u32 b, u32 c) { return swap2(c, a) + mix(c, a, b); }
u32 deep(u32 x) { return rot3(x, x + 1, x + 2) ^ x; }
void put(u32 r, u32 v) { __wrg(r, v); }
s16 narrow(u32 x) { return x; }

entry void t_calls(void) {
  u32 a = __rdg(D0), b = __rdg(D1), c = __rdg(D2), d = __rdg(D3);
  u32 x = mix(a, b, c);
  u32 y = mix(c, a, b);
  u32 z = mix(b, c, a);
  u32 w = swap2(d, a);
  __wrg(D4, x + y + z + w + a + b + c + d);
  __wrg(D5, deep(a ^ d));
  put(D6, rot3(b, c, d));
  put(D7, narrow(a) + narrow(b));
  __wrg(GPC, __next_pc());
  __gexit();
}
