// Allocation benchmark, not run by the tests: SoftFloat-sized (about 1700 values, four
// outer loops around inlined 64-bit divisions). Measure with: build/compiler/ucc -v compiler/tests/bench_alloc.c -o /dev/null

u64 mul64hi(u64 a, u64 b) {
  u64 al = (u32)a, ah = a >> 32, bl = (u32)b, bh = b >> 32;
  u64 m = al * bh + ((al * bl) >> 32);
  u64 n = ah * bl + (u32)m;
  return ah * bh + (m >> 32) + (n >> 32);
}
u64 est(u64 a0, u64 a1, u64 b) {
  if (b <= a0) return 0xffffffffffffffffull;
  u64 b0 = b >> 32;
  u64 z = (b0 << 32 <= a0) ? 0xffffffff00000000ull : (a0 / b0) << 32;
  u64 t = mul64hi(b, z);
  s64 rem = (s64)(a0 - t);
  while (rem < 0) { z -= 0x100000000ull; rem += (s64)(b0 << 32); }
  rem = (rem << 32) | (s64)(a1 >> 32);
  z |= (b0 << 32 <= (u64)rem) ? 0xffffffff : (u64)rem / b0;
  return z;
}
entry void bench_alloc(void) {
  u64 a = ((u64)__rdg(D1) << 32) | __rdg(D0), b = ((u64)__rdg(D3) << 32) | __rdg(D2);
  u64 acc = 0; s64 sacc = 0;
  for (u32 i0 = 0; i0 < (__rdg(D4) & 7); i0++) {
    a = a * 6364136223846793005ull + 1;
    u64 q0 = a / (b | 1) + a % ((b >> 0) | 3);
    s64 s0 = (s64)a / ((s64)b | 5);
    acc = (acc ^ q0) + est(a >> 1, b, b | 0x8000000000000000ull) + (acc << 1);
    sacc += s0 ^ (s64)(acc >> 3);
    if (acc & 1) b = (b << 32) | (u32)acc; else b ^= a >> 5;
  }
  for (u32 i1 = 0; i1 < (__rdg(D4) & 7); i1++) {
    a = a * 6364136223846793007ull + 2;
    u64 q1 = a / (b | 1) + a % ((b >> 7) | 3);
    s64 s1 = (s64)a / ((s64)b | 5);
    acc = (acc ^ q1) + est(a >> 1, b, b | 0x8000000000000000ull) + (acc << 2);
    sacc += s1 ^ (s64)(acc >> 4);
    if (acc & 2) b = (b << 32) | (u32)acc; else b ^= a >> 6;
  }
  for (u32 i2 = 0; i2 < (__rdg(D4) & 7); i2++) {
    a = a * 6364136223846793009ull + 3;
    u64 q2 = a / (b | 1) + a % ((b >> 14) | 3);
    s64 s2 = (s64)a / ((s64)b | 5);
    acc = (acc ^ q2) + est(a >> 1, b, b | 0x8000000000000000ull) + (acc << 3);
    sacc += s2 ^ (s64)(acc >> 5);
    if (acc & 4) b = (b << 32) | (u32)acc; else b ^= a >> 7;
  }
  for (u32 i3 = 0; i3 < (__rdg(D4) & 7); i3++) {
    a = a * 6364136223846793011ull + 4;
    u64 q3 = a / (b | 1) + a % ((b >> 21) | 3);
    s64 s3 = (s64)a / ((s64)b | 5);
    acc = (acc ^ q3) + est(a >> 1, b, b | 0x8000000000000000ull) + (acc << 4);
    sacc += s3 ^ (s64)(acc >> 6);
    if (acc & 8) b = (b << 32) | (u32)acc; else b ^= a >> 8;
  }
  __wrg(D0, (u32)acc); __wrg(D1, (u32)(acc >> 32));
  __wrg(D2, (u32)sacc); __wrg(D3, (u32)(sacc >> 32));
  __wrg(GPC, __next_pc()); __gexit();
}
