// Many 64-bit divisions per run, with dividends and divisors of every width,
// so divlu's digit corrections and the wide-divisor fixup are exercised.
u64 next(u64 x) { return x * 6364136223846793005ull + 1442695040888963407ull; }

entry void t_u64div2(void) {
  u64 seed = ((u64)__rdg(D1) << 32) | __rdg(D0);
  u64 acc = 0, sacc = 0;
  for (u32 i = 0; i < 48; i++) {
    seed = next(seed);
    u64 n = seed >> (u32)(seed & 31);
    seed = next(seed);
    u64 d = seed >> (u32)((seed >> 7) & 63);
    if (seed & 0x100) d |= 0x8000000000000000ull >> (u32)(seed & 63);   // near-power-of-two shapes
    if (seed & 0x200) d = (d << 32) | 0xffffffff;
    if (d == 0) d = 1;
    if ((seed & 0xc00) == 0xc00) {   // dividend's high word just below a 32-bit divisor:
      d = (u32)d | 0x10000;          // the quotient digit estimate reaches 2^16
      n = ((d - 1 - ((seed >> 20) & 0xff)) << 32) | (n & 0xffffffff);
    }
    u64 q = n / d, r = n % d;
    acc = (acc ^ q) * 31 + r;
    s64 sn = (s64)n, sd = (s64)d;
    if (sd == -1) sd = 5;
    sacc = (sacc ^ (u64)(sn / sd)) * 17 + (u64)(sn % sd);
  }
  __wrg(D0, (u32)acc);
  __wrg(D1, (u32)(acc >> 32));
  __wrg(D2, (u32)sacc);
  __wrg(D3, (u32)(sacc >> 32));
  __wrg(GPC, __next_pc());
  __gexit();
}
