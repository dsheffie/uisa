// inline functions, reference parameters, structs, typedefs and #include.
#include "softhelp.h"
#include "softhelp.h"   // a second include is skipped

struct pair64 {
  u32 lo;
  u32 hi;
};

// a real (called) function taking a struct and returning a two-word struct
pair64 swapHalves(pair64 p, u32 k) {
  pair64 r;
  r.lo = p.hi + k;
  r.hi = p.lo ^ k;
  return r;
}

inline u32 nested(u32 x, u32 &acc) {   // inline calling inline, with early returns
  if (x == 0) return 7;
  bits64 h, l;
  mul64To128(x, x, h, l);
  acc += (u32)l;
  if (x & 1) return (u32)(l >> 32);
  return (u32)h + x;
}

entry void t_inline(void) {
  bits64 a = ((bits64)__rdg(D1) << 32) | __rdg(D0);
  bits64 b = ((bits64)__rdg(D3) << 32) | __rdg(D2);
  bits64 z0, z1;
  add128(a, b, b, a, z0, z1);
  __wrg(D0, (u32)z0 ^ (u32)(z0 >> 32));
  __wrg(D1, (u32)z1 ^ (u32)(z1 >> 32));
  sub128(a, b, b, a, z0, z1);
  __wrg(D2, (u32)z0 + (u32)(z1 >> 32));
  mul64To128(a, b, z0, z1);
  __wrg(D3, (u32)z0 ^ (u32)(z0 >> 32));
  __wrg(D4, (u32)z1 ^ (u32)(z1 >> 32));
  bits64 j;
  shift64RightJamming(a, (s32)(__rdg(D4) & 127), j);
  __wrg(D5, (u32)j ^ (u32)(j >> 32));

  floatx80 f = packFloatx80(__rdg(D5) & 1, __rdg(D5) & 0x7fff, a);
  floatx80 g = f;                  // struct copy
  g.low += 1;                      // field assignment
  s32 e;
  bits64 sig;
  normalizeFloatx80Subnormal(b | 1, e, sig);
  __wrg(D6, f.high + (u32)(g.low >> 40) + (u32)e + (u32)(sig >> 33));

  pair64 p;
  p.lo = __rdg(D6);
  p.hi = __rdg(D7);
  pair64 q = swapHalves(p, __rdg(A0));
  pair64 r = (q.lo & 1) ? q : swapHalves(q, 3);
  u32 acc = 0;
  u32 n = nested(__rdg(A1) & 0xffff, acc) + nested(__rdg(A2), acc) + nested(0, acc);
  __wrg(D7, r.lo ^ r.hi);
  __wrg(A0, n);
  __wrg(A1, acc);
  __wrg(GPC, __next_pc());
  __gexit();
}
