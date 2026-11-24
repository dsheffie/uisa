// Included by t_inline.c: SoftFloat-style helpers (out-parameters as
// references, a floatx80 struct). Not a test by itself.
#pragma once

typedef u64 bits64;
typedef u8 flag;

struct floatx80 {
  bits64 low;
  u16 high;
};

inline void add128(bits64 a0, bits64 a1, bits64 b0, bits64 b1, bits64 &z0Ptr, bits64 &z1Ptr) {
  bits64 z1 = a1 + b1;
  z1Ptr = z1;
  z0Ptr = a0 + b0 + (z1 < a1);
}

inline void sub128(bits64 a0, bits64 a1, bits64 b0, bits64 b1, bits64 &z0Ptr, bits64 &z1Ptr) {
  z1Ptr = a1 - b1;
  z0Ptr = a0 - b0 - (a1 < b1);
}

inline void mul64To128(bits64 a, bits64 b, bits64 &z0Ptr, bits64 &z1Ptr) {
  u32 aHigh = a >> 32, aLow = a, bHigh = b >> 32, bLow = b;
  bits64 z1 = (bits64)aLow * bLow;
  bits64 zMiddleA = (bits64)aLow * bHigh;
  bits64 zMiddleB = (bits64)aHigh * bLow;
  bits64 z0 = (bits64)aHigh * bHigh;
  zMiddleA += zMiddleB;
  z0 += ((bits64)(zMiddleA < zMiddleB) << 32) + (zMiddleA >> 32);
  zMiddleA <<= 32;
  z1 += zMiddleA;
  z0 += (z1 < zMiddleA);
  z1Ptr = z1;
  z0Ptr = z0;
}

inline void shift64RightJamming(bits64 a, s32 count, bits64 &zPtr) {
  bits64 z;
  if (count == 0) z = a;
  else if (count < 64) z = (a >> count) | ((a << ((-count) & 63)) != 0);
  else z = (a != 0);
  zPtr = z;
}

inline s32 countLeadingZeros64(bits64 a) {
  if (a >> 32) return __clz((u32)(a >> 32));
  return 32 + __clz((u32)a);
}

inline floatx80 packFloatx80(flag zSign, s32 zExp, bits64 zSig) {
  floatx80 z;
  z.low = zSig;
  z.high = ((u16)zSign << 15) + zExp;
  return z;
}

inline void normalizeFloatx80Subnormal(bits64 aSig, s32 &zExpPtr, bits64 &zSigPtr) {
  s32 shiftCount = countLeadingZeros64(aSig);
  zSigPtr = aSig << shiftCount;
  zExpPtr = 1 - shiftCount;
}
