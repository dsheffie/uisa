// softfloat.h: extended-precision (floatx80) arithmetic for the 68040 FPU
// millicode, in the millicode C subset (compiler/README.md).
//
// A port of the floatx80 core of the SoftFloat that Previous uses
// (previous/src/softfloat: QEMU's SoftFloat 2a with WinUAE's SOFTFLOAT_68K
// changes), built the way Previous builds it for a 68040: SOFTFLOAT_68K
// defined, special flags = cmp_signed_nan only (so inf_clear_intbit() and
// faddsub_swap_inf() are false). Results and exception flags must match
// Previous bit for bit; tests/softfloat checks that against Previous's own
// code. The control flow, names and constants follow the original so the two
// can be read side by side; pointer out-parameters are references and
// `float_status *` is `float_status &`. Everything is inline: SoftFloat nests
// helpers deeper than the millicode return stack.
//
// Not ported yet: saveFloatx80Internal() (the unrounded result the 68040's
// FPSP exception frames need when an FP exception is enabled), and the
// operations beyond add/sub/mul/div/sqrt, rounding to precision and int32
// conversions.
//
// SoftFloat is derived from release 2a of the SoftFloat IEC/IEEE
// Floating-point Arithmetic Package, written by John R. Hauser (ICSI,
// funded in part by the NSF under grant MIP-9311980); this is a derivative
// work under its terms. Later QEMU contributions are under the BSD license
// (Copyright (c) 2006, Fabrice Bellard) or GPL-v2-or-later, as in the
// original files.
#pragma once

typedef u8 flag;
typedef s8 int8_t;
typedef s16 int16_t;
typedef s32 int32_t;
typedef s64 int64_t;
typedef u8 uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;

struct floatx80 {
  uint64_t low;
  uint16_t high;
};

struct float_status {
  uint8_t float_exception_flags;
  int8_t float_rounding_mode;
  int8_t floatx80_rounding_precision;
};

// softfloat.h enums
const int float_round_nearest_even = 0;
const int float_round_down = 1;
const int float_round_up = 2;
const int float_round_to_zero = 3;

const int float_flag_invalid = 0x01;
const int float_flag_denormal = 0x02;
const int float_flag_divbyzero = 0x04;
const int float_flag_overflow = 0x08;
const int float_flag_underflow = 0x10;
const int float_flag_inexact = 0x20;
const int float_flag_signaling = 0x40;
const int float_flag_decimal = 0x80;

const uint64_t floatx80_default_infinity_low = 0x0000000000000000ull;

// the 68040's special flags (fp_init_softfloat): only cmp_signed_nan
inline flag inf_clear_intbit(float_status &status) { return 0; }
inline flag faddsub_swap_inf(float_status &status) { return 0; }

inline void float_raise(uint8_t flags, float_status &status) {
  status.float_exception_flags |= flags;
}

// ---------------------------------------------------------------------------
// softfloat-macros.h
// ---------------------------------------------------------------------------
inline void shift64RightJamming(uint64_t a, int count, uint64_t &zPtr) {
  uint64_t z;
  if (count == 0) {
    z = a;
  } else if (count < 64) {
    z = (a >> count) | ((a << ((-count) & 63)) != 0);
  } else {
    z = (a != 0);
  }
  zPtr = z;
}

inline void shift64ExtraRightJamming(uint64_t a0, uint64_t a1, int count, uint64_t &z0Ptr, uint64_t &z1Ptr) {
  uint64_t z0, z1;
  int8_t negCount = (-count) & 63;
  if (count == 0) {
    z1 = a1;
    z0 = a0;
  } else if (count < 64) {
    z1 = (a0 << negCount) | (a1 != 0);
    z0 = a0 >> count;
  } else {
    if (count == 64) {
      z1 = a0 | (a1 != 0);
    } else {
      z1 = ((a0 | a1) != 0);
    }
    z0 = 0;
  }
  z1Ptr = z1;
  z0Ptr = z0;
}

inline void shift128Right(uint64_t a0, uint64_t a1, int count, uint64_t &z0Ptr, uint64_t &z1Ptr) {
  uint64_t z0, z1;
  int8_t negCount = (-count) & 63;
  if (count == 0) {
    z1 = a1;
    z0 = a0;
  } else if (count < 64) {
    z1 = (a0 << negCount) | (a1 >> count);
    z0 = a0 >> count;
  } else {
    z1 = (count < 128) ? (a0 >> (count & 63)) : 0;
    z0 = 0;
  }
  z1Ptr = z1;
  z0Ptr = z0;
}

inline void shift128RightJamming(uint64_t a0, uint64_t a1, int count, uint64_t &z0Ptr, uint64_t &z1Ptr) {
  uint64_t z0, z1;
  int8_t negCount = (-count) & 63;
  if (count == 0) {
    z1 = a1;
    z0 = a0;
  } else if (count < 64) {
    z1 = (a0 << negCount) | (a1 >> count) | ((a1 << negCount) != 0);
    z0 = a0 >> count;
  } else {
    if (count == 64) {
      z1 = a0 | (a1 != 0);
    } else if (count < 128) {
      z1 = (a0 >> (count & 63)) | (((a0 << negCount) | a1) != 0);
    } else {
      z1 = ((a0 | a1) != 0);
    }
    z0 = 0;
  }
  z1Ptr = z1;
  z0Ptr = z0;
}

inline void shortShift128Left(uint64_t a0, uint64_t a1, int count, uint64_t &z0Ptr, uint64_t &z1Ptr) {
  z1Ptr = a1 << count;
  z0Ptr = (count == 0) ? a0 : (a0 << count) | (a1 >> ((-count) & 63));
}

inline void add128(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1, uint64_t &z0Ptr, uint64_t &z1Ptr) {
  uint64_t z1;
  z1 = a1 + b1;
  z1Ptr = z1;
  z0Ptr = a0 + b0 + (z1 < a1);
}

inline void add192(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t b0, uint64_t b1, uint64_t b2,
                   uint64_t &z0Ptr, uint64_t &z1Ptr, uint64_t &z2Ptr) {
  uint64_t z0, z1, z2;
  uint8_t carry0, carry1;
  z2 = a2 + b2;
  carry1 = (z2 < a2);
  z1 = a1 + b1;
  carry0 = (z1 < a1);
  z0 = a0 + b0;
  z1 += carry1;
  z0 += (z1 < carry1);
  z0 += carry0;
  z2Ptr = z2;
  z1Ptr = z1;
  z0Ptr = z0;
}

inline void sub128(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1, uint64_t &z0Ptr, uint64_t &z1Ptr) {
  z1Ptr = a1 - b1;
  z0Ptr = a0 - b0 - (a1 < b1);
}

inline void sub192(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t b0, uint64_t b1, uint64_t b2,
                   uint64_t &z0Ptr, uint64_t &z1Ptr, uint64_t &z2Ptr) {
  uint64_t z0, z1, z2;
  uint8_t borrow0, borrow1;
  z2 = a2 - b2;
  borrow1 = (a2 < b2);
  z1 = a1 - b1;
  borrow0 = (a1 < b1);
  z0 = a0 - b0;
  z0 -= (z1 < borrow1);
  z1 -= borrow1;
  z0 -= borrow0;
  z2Ptr = z2;
  z1Ptr = z1;
  z0Ptr = z0;
}

// 64 x 64 -> 128 with the hardware's mulhu instead of four partial products
inline void mul64To128(uint64_t a, uint64_t b, uint64_t &z0Ptr, uint64_t &z1Ptr) {
  uint32_t aHigh = a >> 32, aLow = a, bHigh = b >> 32, bLow = b;
  uint64_t z1, zMiddleA, zMiddleB, z0;
  z1 = (uint64_t)aLow * bLow;
  zMiddleA = (uint64_t)aLow * bHigh;
  zMiddleB = (uint64_t)aHigh * bLow;
  z0 = (uint64_t)aHigh * bHigh;
  zMiddleA += zMiddleB;
  z0 += ((uint64_t)(zMiddleA < zMiddleB) << 32) + (zMiddleA >> 32);
  zMiddleA <<= 32;
  z1 += zMiddleA;
  z0 += (z1 < zMiddleA);
  z1Ptr = z1;
  z0Ptr = z0;
}

inline uint64_t estimateDiv128To64(uint64_t a0, uint64_t a1, uint64_t b) {
  uint64_t b0, b1;
  uint64_t rem0, rem1, term0, term1;
  uint64_t z;
  if (b <= a0) return 0xFFFFFFFFFFFFFFFFull;
  b0 = b >> 32;
  z = (b0 << 32 <= a0) ? 0xFFFFFFFF00000000ull : (a0 / b0) << 32;
  mul64To128(b, z, term0, term1);
  sub128(a0, a1, term0, term1, rem0, rem1);
  while (((int64_t)rem0) < 0) {
    z -= 0x100000000ull;
    b1 = b << 32;
    add128(rem0, rem1, b0, b1, rem0, rem1);
  }
  rem0 = (rem0 << 32) | (rem1 >> 32);
  z |= (b0 << 32 <= rem0) ? 0xFFFFFFFF : rem0 / b0;
  return z;
}

// the original's two 16-entry tables, as switches (no arrays in the subset)
inline uint16_t sqrtOddAdjustments(int i) {
  switch (i) {
    case 0: return 0x0004; case 1: return 0x0022; case 2: return 0x005D; case 3: return 0x00B1;
    case 4: return 0x011D; case 5: return 0x019F; case 6: return 0x0236; case 7: return 0x02E0;
    case 8: return 0x039C; case 9: return 0x0468; case 10: return 0x0545; case 11: return 0x0631;
    case 12: return 0x072B; case 13: return 0x0832; case 14: return 0x0946; default: return 0x0A67;
  }
}
inline uint16_t sqrtEvenAdjustments(int i) {
  switch (i) {
    case 0: return 0x0A2D; case 1: return 0x08AF; case 2: return 0x075A; case 3: return 0x0629;
    case 4: return 0x051A; case 5: return 0x0429; case 6: return 0x0356; case 7: return 0x029E;
    case 8: return 0x0200; case 9: return 0x0179; case 10: return 0x0109; case 11: return 0x00AF;
    case 12: return 0x0068; case 13: return 0x0034; case 14: return 0x0012; default: return 0x0002;
  }
}

inline uint32_t estimateSqrt32(int aExp, uint32_t a) {
  int8_t index;
  uint32_t z;
  index = (a >> 27) & 15;
  if (aExp & 1) {
    z = 0x4000 + (a >> 17) - sqrtOddAdjustments((int)index);
    z = ((a / z) << 14) + (z << 15);
    a >>= 1;
  } else {
    z = 0x8000 + (a >> 17) - sqrtEvenAdjustments((int)index);
    z = a / z + z;
    z = (0x20000 <= z) ? 0xFFFF8000 : (z << 15);
    if (z <= a) return (uint32_t)(((int32_t)a) >> 1);
  }
  return ((uint32_t)((((uint64_t)a) << 31) / z)) + (z >> 1);
}

// the original's table-driven versions, with the hardware's clz
inline int8_t countLeadingZeros32(uint32_t a) { return a ? __clz(a) : 32; }
inline int8_t countLeadingZeros64(uint64_t a) {
  if (a >> 32) return __clz((uint32_t)(a >> 32));
  return 32 + countLeadingZeros32((uint32_t)a);
}

// ---------------------------------------------------------------------------
// floatx80 basics (softfloat.h, softfloat.c, softfloat-specialize.h)
// ---------------------------------------------------------------------------
inline uint64_t extractFloatx80Frac(floatx80 a) { return a.low; }
inline int32_t extractFloatx80Exp(floatx80 a) { return a.high & 0x7FFF; }
inline flag extractFloatx80Sign(floatx80 a) { return a.high >> 15; }

inline floatx80 packFloatx80(flag zSign, int32_t zExp, uint64_t zSig) {
  floatx80 z;
  z.low = zSig;
  z.high = (((uint16_t)zSign) << 15) + zExp;
  return z;
}

inline floatx80 floatx80_default_nan(float_status &status) {
  floatx80 r;
  r.high = 0x7FFF;
  r.low = 0xFFFFFFFFFFFFFFFFull;
  return r;
}

inline flag floatx80_invalid_encoding(floatx80 a) {
  return (a.low & (1ull << 63)) == 0 && (a.high & 0x7FFF) != 0 && (a.high & 0x7FFF) != 0x7FFF;
}

inline flag floatx80_is_nan(floatx80 a) {
  return ((a.high & 0x7FFF) == 0x7FFF) && (uint64_t)(a.low << 1);
}

inline flag floatx80_is_signaling_nan(floatx80 a) {
  uint64_t aLow;
  aLow = a.low & ~0x4000000000000000ull;
  return ((a.high & 0x7FFF) == 0x7FFF) && (uint64_t)(aLow << 1) && (a.low == aLow);
}

// SOFTFLOAT_68K: the exponent of a normalized subnormal is -shiftCount
inline void normalizeFloatx80Subnormal(uint64_t aSig, int32_t &zExpPtr, uint64_t &zSigPtr) {
  int8_t shiftCount;
  shiftCount = countLeadingZeros64(aSig);
  zSigPtr = aSig << shiftCount;
  zExpPtr = -shiftCount;
}

// SOFTFLOAT_68K
inline floatx80 propagateFloatx80NaN(floatx80 a, floatx80 b, float_status &status) {
  flag aIsNaN, aIsSignalingNaN, bIsSignalingNaN;
  aIsNaN = floatx80_is_nan(a);
  aIsSignalingNaN = floatx80_is_signaling_nan(a);
  bIsSignalingNaN = floatx80_is_signaling_nan(b);
  a.low |= 0x4000000000000000ull;
  b.low |= 0x4000000000000000ull;
  if (aIsSignalingNaN | bIsSignalingNaN) float_raise(float_flag_signaling, status);
  return aIsNaN ? a : b;
}

inline floatx80 propagateFloatx80NaNOneArg(floatx80 a, float_status &status) {
  if (floatx80_is_signaling_nan(a)) float_raise(float_flag_signaling, status);
  a.low |= 0x4000000000000000ull;
  return a;
}

// ---------------------------------------------------------------------------
// rounding (the SOFTFLOAT_68K roundAndPackFloatx80)
// ---------------------------------------------------------------------------
inline floatx80 roundAndPackFloatx80(int8_t roundingPrecision, flag zSign, int32_t zExp, uint64_t zSig0,
                                     uint64_t zSig1, float_status &status) {
  int8_t roundingMode;
  flag roundNearestEven, increment;
  uint64_t roundMask, roundBits;
  uint64_t roundIncrement;
  int32_t expOffset;

  roundingMode = status.float_rounding_mode;
  roundNearestEven = (roundingMode == float_round_nearest_even);
  if (roundingPrecision == 80) goto precision80;
  if (roundingPrecision == 64) {
    roundIncrement = 0x0000000000000400ull;
    roundMask = 0x00000000000007FFull;
    expOffset = 0x3C00;
  } else if (roundingPrecision == 32) {
    roundIncrement = 0x0000008000000000ull;
    roundMask = 0x000000FFFFFFFFFFull;
    expOffset = 0x3F80;
  } else {
    goto precision80;
  }
  zSig0 |= (zSig1 != 0);
  if (!roundNearestEven) {
    if (roundingMode == float_round_to_zero) {
      roundIncrement = 0;
    } else {
      roundIncrement = roundMask;
      if (zSign) {
        if (roundingMode == float_round_up) roundIncrement = 0;
      } else {
        if (roundingMode == float_round_down) roundIncrement = 0;
      }
    }
  }
  roundBits = zSig0 & roundMask;
  if (((0x7FFE - expOffset) < zExp) || ((zExp == (0x7FFE - expOffset)) && (zSig0 + roundIncrement < zSig0))) {
    float_raise(float_flag_overflow, status);
    if (zSig0 & roundMask) float_raise(float_flag_inexact, status);
    if ((roundingMode == float_round_to_zero) || (zSign && (roundingMode == float_round_up)) ||
        (!zSign && (roundingMode == float_round_down))) {
      return packFloatx80(zSign, 0x7FFE - expOffset, ~roundMask);
    }
    return packFloatx80(zSign, 0x7FFF, floatx80_default_infinity_low);
  }
  if (zExp < (expOffset + 1)) {
    float_raise(float_flag_underflow, status);
    shift64RightJamming(zSig0, -(zExp - (expOffset + 1)), zSig0);
    zExp = expOffset + 1;
    roundBits = zSig0 & roundMask;
    if (roundBits) float_raise(float_flag_inexact, status);
    zSig0 += roundIncrement;
    roundIncrement = roundMask + 1;
    if (roundNearestEven && (roundBits << 1 == roundIncrement)) {
      roundMask |= roundIncrement;
    }
    zSig0 &= ~roundMask;
    return packFloatx80(zSign, zExp, zSig0);
  }
  if (roundBits) {
    float_raise(float_flag_inexact, status);
  }
  zSig0 += roundIncrement;
  if (zSig0 < roundIncrement) {
    ++zExp;
    zSig0 = 0x8000000000000000ull;
  }
  roundIncrement = roundMask + 1;
  if (roundNearestEven && (roundBits << 1 == roundIncrement)) {
    roundMask |= roundIncrement;
  }
  zSig0 &= ~roundMask;
  if (zSig0 == 0) zExp = 0;
  return packFloatx80(zSign, zExp, zSig0);
precision80:
  increment = ((int64_t)zSig1 < 0);
  if (!roundNearestEven) {
    if (roundingMode == float_round_to_zero) {
      increment = 0;
    } else {
      if (zSign) {
        increment = (roundingMode == float_round_down) && zSig1;
      } else {
        increment = (roundingMode == float_round_up) && zSig1;
      }
    }
  }
  if (0x7FFE <= (uint32_t)zExp) {
    if ((0x7FFE < zExp) || ((zExp == 0x7FFE) && (zSig0 == 0xFFFFFFFFFFFFFFFFull) && increment)) {
      roundMask = 0;
      float_raise(float_flag_overflow, status);
      if ((zSig0 & roundMask) || zSig1) float_raise(float_flag_inexact, status);
      if ((roundingMode == float_round_to_zero) || (zSign && (roundingMode == float_round_up)) ||
          (!zSign && (roundingMode == float_round_down))) {
        return packFloatx80(zSign, 0x7FFE, ~roundMask);
      }
      return packFloatx80(zSign, 0x7FFF, floatx80_default_infinity_low);
    }
    if (zExp < 0) {
      float_raise(float_flag_underflow, status);
      shift64ExtraRightJamming(zSig0, zSig1, -zExp, zSig0, zSig1);
      zExp = 0;
      if (zSig1) float_raise(float_flag_inexact, status);
      if (roundNearestEven) {
        increment = ((int64_t)zSig1 < 0);
      } else {
        if (zSign) {
          increment = (roundingMode == float_round_down) && zSig1;
        } else {
          increment = (roundingMode == float_round_up) && zSig1;
        }
      }
      if (increment) {
        ++zSig0;
        zSig0 &= ~(((uint64_t)(zSig1 << 1) == 0) & roundNearestEven);
      }
      return packFloatx80(zSign, zExp, zSig0);
    }
  }
  if (zSig1) {
    float_raise(float_flag_inexact, status);
  }
  if (increment) {
    ++zSig0;
    if (zSig0 == 0) {
      ++zExp;
      zSig0 = 0x8000000000000000ull;
    } else {
      zSig0 &= ~(((uint64_t)(zSig1 << 1) == 0) & roundNearestEven);
    }
  } else {
    if (zSig0 == 0) zExp = 0;
  }
  return packFloatx80(zSign, zExp, zSig0);
}

inline floatx80 normalizeRoundAndPackFloatx80(int8_t roundingPrecision, flag zSign, int32_t zExp, uint64_t zSig0,
                                              uint64_t zSig1, float_status &status) {
  int8_t shiftCount;
  if (zSig0 == 0) {
    zSig0 = zSig1;
    zSig1 = 0;
    zExp -= 64;
  }
  shiftCount = countLeadingZeros64(zSig0);
  shortShift128Left(zSig0, zSig1, shiftCount, zSig0, zSig1);
  zExp -= shiftCount;
  return roundAndPackFloatx80(roundingPrecision, zSign, zExp, zSig0, zSig1, status);
}

// ---------------------------------------------------------------------------
// conversions
// ---------------------------------------------------------------------------
inline floatx80 int32_to_floatx80(int32_t a) {
  flag zSign;
  uint32_t absA;
  int8_t shiftCount;
  uint64_t zSig;
  if (a == 0) return packFloatx80(0, 0, 0);
  zSign = (a < 0);
  absA = zSign ? -a : a;
  shiftCount = countLeadingZeros32(absA) + 32;
  zSig = absA;
  return packFloatx80(zSign, 0x403E - shiftCount, zSig << shiftCount);
}

inline int32_t roundAndPackInt32(flag zSign, uint64_t absZ, float_status &status) {
  int8_t roundingMode;
  flag roundNearestEven;
  int8_t roundIncrement, roundBits;
  int32_t z;
  roundingMode = status.float_rounding_mode;
  roundNearestEven = (roundingMode == float_round_nearest_even);
  switch (roundingMode) {
    case float_round_nearest_even:
      roundIncrement = 0x40;
      break;
    case float_round_to_zero:
      roundIncrement = 0;
      break;
    case float_round_up:
      roundIncrement = zSign ? 0 : 0x7f;
      break;
    default:   // float_round_down
      roundIncrement = zSign ? 0x7f : 0;
      break;
  }
  roundBits = absZ & 0x7F;
  absZ = (absZ + roundIncrement) >> 7;
  absZ &= ~(((roundBits ^ 0x40) == 0) & roundNearestEven);
  z = (int32_t)absZ;
  if (zSign) z = -z;
  if ((absZ >> 32) || (z && ((z < 0) ^ zSign))) {
    float_raise(float_flag_invalid, status);
    return zSign ? (int32_t)0x80000000 : 0x7FFFFFFF;
  }
  if (roundBits) {
    status.float_exception_flags |= float_flag_inexact;
  }
  return z;
}

// SOFTFLOAT_68K
inline int32_t floatx80_to_int32(floatx80 a, float_status &status) {
  flag aSign;
  int32_t aExp, shiftCount;
  uint64_t aSig;
  aSig = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  aSign = extractFloatx80Sign(a);
  if (aExp == 0x7FFF) {
    if ((uint64_t)(aSig << 1)) {
      a = propagateFloatx80NaNOneArg(a, status);
      if (a.low == aSig) float_raise(float_flag_invalid, status);
      return (int32_t)(a.low >> 32);
    }
    float_raise(float_flag_invalid, status);
    return aSign ? (int32_t)0x80000000 : 0x7FFFFFFF;
  }
  shiftCount = 0x4037 - aExp;
  if (shiftCount <= 0) shiftCount = 1;
  shift64RightJamming(aSig, shiftCount, aSig);
  return roundAndPackInt32(aSign, aSig, status);
}

// ---------------------------------------------------------------------------
// add, sub
// ---------------------------------------------------------------------------
inline floatx80 addFloatx80Sigs(floatx80 a, floatx80 b, flag zSign, float_status &status) {
  int32_t aExp, bExp, zExp;
  uint64_t aSig, bSig, zSig0, zSig1;
  int32_t expDiff;

  aSig = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  bSig = extractFloatx80Frac(b);
  bExp = extractFloatx80Exp(b);
  if (aExp == 0) {
    if (aSig == 0) {
      aExp = -64;
    } else {
      normalizeFloatx80Subnormal(aSig, aExp, aSig);
    }
  }
  if (bExp == 0) {
    if (bSig == 0) {
      bExp = -64;
    } else {
      normalizeFloatx80Subnormal(bSig, bExp, bSig);
    }
  }
  expDiff = aExp - bExp;
  if (0 < expDiff) {
    if (aExp == 0x7FFF) {
      if ((uint64_t)(aSig << 1)) return propagateFloatx80NaN(a, b, status);
      return inf_clear_intbit(status) ? packFloatx80(extractFloatx80Sign(a), aExp, 0) : a;
    }
    shift64ExtraRightJamming(bSig, 0, expDiff, bSig, zSig1);
    zExp = aExp;
  } else if (expDiff < 0) {
    if (bExp == 0x7FFF) {
      if ((uint64_t)(bSig << 1)) return propagateFloatx80NaN(a, b, status);
      if (inf_clear_intbit(status)) bSig = 0;
      return packFloatx80(zSign, bExp, bSig);
    }
    shift64ExtraRightJamming(aSig, 0, -expDiff, aSig, zSig1);
    zExp = bExp;
  } else {
    if (aExp == 0x7FFF) {
      if ((uint64_t)((aSig | bSig) << 1)) {
        return propagateFloatx80NaN(a, b, status);
      }
      if (inf_clear_intbit(status)) return packFloatx80(extractFloatx80Sign(a), aExp, 0);
      return faddsub_swap_inf(status) ? b : a;
    }
    zSig1 = 0;
    zSig0 = aSig + bSig;
    zExp = aExp;
    if (aSig == 0 && bSig == 0) return packFloatx80(zSign, 0, 0);
    if (aSig == 0 || bSig == 0) goto roundAndPack;
    goto shiftRight1;
  }
  zSig0 = aSig + bSig;
  if ((int64_t)zSig0 < 0) goto roundAndPack;
shiftRight1:
  shift64ExtraRightJamming(zSig0, zSig1, 1, zSig0, zSig1);
  zSig0 |= 0x8000000000000000ull;
  ++zExp;
roundAndPack:
  return roundAndPackFloatx80(status.floatx80_rounding_precision, zSign, zExp, zSig0, zSig1, status);
}

inline floatx80 subFloatx80Sigs(floatx80 a, floatx80 b, flag zSign, float_status &status) {
  int32_t aExp, bExp, zExp;
  uint64_t aSig, bSig, zSig0, zSig1;
  int32_t expDiff;

  aSig = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  bSig = extractFloatx80Frac(b);
  bExp = extractFloatx80Exp(b);
  expDiff = aExp - bExp;
  if (0 < expDiff) goto aExpBigger;
  if (expDiff < 0) goto bExpBigger;
  if (aExp == 0x7FFF) {
    if ((uint64_t)((aSig | bSig) << 1)) {
      return propagateFloatx80NaN(a, b, status);
    }
    float_raise(float_flag_invalid, status);
    return floatx80_default_nan(status);
  }
  zSig1 = 0;
  if (bSig < aSig) goto aBigger;
  if (aSig < bSig) goto bBigger;
  return packFloatx80(status.float_rounding_mode == float_round_down, 0, 0);
bExpBigger:
  if (bExp == 0x7FFF) {
    if ((uint64_t)(bSig << 1)) return propagateFloatx80NaN(a, b, status);
    if (inf_clear_intbit(status)) bSig = 0;
    return packFloatx80(zSign ^ 1, bExp, bSig);
  }
  shift128RightJamming(aSig, 0, -expDiff, aSig, zSig1);
bBigger:
  sub128(bSig, 0, aSig, zSig1, zSig0, zSig1);
  zExp = bExp;
  zSign ^= 1;
  goto normalizeRoundAndPack;
aExpBigger:
  if (aExp == 0x7FFF) {
    if ((uint64_t)(aSig << 1)) return propagateFloatx80NaN(a, b, status);
    return inf_clear_intbit(status) ? packFloatx80(extractFloatx80Sign(a), aExp, 0) : a;
  }
  shift128RightJamming(bSig, 0, expDiff, bSig, zSig1);
aBigger:
  sub128(aSig, 0, bSig, zSig1, zSig0, zSig1);
  zExp = aExp;
normalizeRoundAndPack:
  return normalizeRoundAndPackFloatx80(status.floatx80_rounding_precision, zSign, zExp, zSig0, zSig1, status);
}

inline floatx80 floatx80_add(floatx80 a, floatx80 b, float_status &status) {
  flag aSign, bSign;
  if (floatx80_invalid_encoding(a) || floatx80_invalid_encoding(b)) {
    float_raise(float_flag_invalid, status);
    return floatx80_default_nan(status);
  }
  aSign = extractFloatx80Sign(a);
  bSign = extractFloatx80Sign(b);
  if (aSign == bSign) {
    return addFloatx80Sigs(a, b, aSign, status);
  } else {
    return subFloatx80Sigs(a, b, aSign, status);
  }
}

inline floatx80 floatx80_sub(floatx80 a, floatx80 b, float_status &status) {
  flag aSign, bSign;
  if (floatx80_invalid_encoding(a) || floatx80_invalid_encoding(b)) {
    float_raise(float_flag_invalid, status);
    return floatx80_default_nan(status);
  }
  aSign = extractFloatx80Sign(a);
  bSign = extractFloatx80Sign(b);
  if (aSign == bSign) {
    return subFloatx80Sigs(a, b, aSign, status);
  } else {
    return addFloatx80Sigs(a, b, aSign, status);
  }
}

// ---------------------------------------------------------------------------
// mul, div, sqrt
// ---------------------------------------------------------------------------
inline floatx80 floatx80_mul(floatx80 a, floatx80 b, float_status &status) {
  flag aSign, bSign, zSign;
  int32_t aExp, bExp, zExp;
  uint64_t aSig, bSig, zSig0, zSig1;

  if (floatx80_invalid_encoding(a) || floatx80_invalid_encoding(b)) {
    float_raise(float_flag_invalid, status);
    return floatx80_default_nan(status);
  }
  aSig = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  aSign = extractFloatx80Sign(a);
  bSig = extractFloatx80Frac(b);
  bExp = extractFloatx80Exp(b);
  bSign = extractFloatx80Sign(b);
  zSign = aSign ^ bSign;
  if (aExp == 0x7FFF) {
    if ((uint64_t)(aSig << 1) || ((bExp == 0x7FFF) && (uint64_t)(bSig << 1))) {
      return propagateFloatx80NaN(a, b, status);
    }
    if ((bExp | bSig) == 0) goto invalid;
    if (inf_clear_intbit(status)) aSig = 0;
    return packFloatx80(zSign, aExp, aSig);
  }
  if (bExp == 0x7FFF) {
    if ((uint64_t)(bSig << 1)) {
      return propagateFloatx80NaN(a, b, status);
    }
    if ((aExp | aSig) == 0) {
    invalid:
      float_raise(float_flag_invalid, status);
      return floatx80_default_nan(status);
    }
    if (inf_clear_intbit(status)) bSig = 0;
    return packFloatx80(zSign, bExp, bSig);
  }
  if (aExp == 0) {
    if (aSig == 0) return packFloatx80(zSign, 0, 0);
    normalizeFloatx80Subnormal(aSig, aExp, aSig);
  }
  if (bExp == 0) {
    if (bSig == 0) return packFloatx80(zSign, 0, 0);
    normalizeFloatx80Subnormal(bSig, bExp, bSig);
  }
  zExp = aExp + bExp - 0x3FFE;
  mul64To128(aSig, bSig, zSig0, zSig1);
  if (0 < (int64_t)zSig0) {
    shortShift128Left(zSig0, zSig1, 1, zSig0, zSig1);
    --zExp;
  }
  return roundAndPackFloatx80(status.floatx80_rounding_precision, zSign, zExp, zSig0, zSig1, status);
}

inline floatx80 floatx80_div(floatx80 a, floatx80 b, float_status &status) {
  flag aSign, bSign, zSign;
  int32_t aExp, bExp, zExp;
  uint64_t aSig, bSig, zSig0, zSig1;
  uint64_t rem0, rem1, rem2, term0, term1, term2;

  if (floatx80_invalid_encoding(a) || floatx80_invalid_encoding(b)) {
    float_raise(float_flag_invalid, status);
    return floatx80_default_nan(status);
  }
  aSig = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  aSign = extractFloatx80Sign(a);
  bSig = extractFloatx80Frac(b);
  bExp = extractFloatx80Exp(b);
  bSign = extractFloatx80Sign(b);
  zSign = aSign ^ bSign;
  if (aExp == 0x7FFF) {
    if ((uint64_t)(aSig << 1)) {
      return propagateFloatx80NaN(a, b, status);
    }
    if (bExp == 0x7FFF) {
      if ((uint64_t)(bSig << 1)) {
        return propagateFloatx80NaN(a, b, status);
      }
      goto invalid;
    }
    if (inf_clear_intbit(status)) aSig = 0;
    return packFloatx80(zSign, aExp, aSig);
  }
  if (bExp == 0x7FFF) {
    if ((uint64_t)(bSig << 1)) {
      return propagateFloatx80NaN(a, b, status);
    }
    return packFloatx80(zSign, 0, 0);
  }
  if (bExp == 0) {
    if (bSig == 0) {
      if ((aExp | aSig) == 0) {
      invalid:
        float_raise(float_flag_invalid, status);
        return floatx80_default_nan(status);
      }
      float_raise(float_flag_divbyzero, status);
      return packFloatx80(zSign, 0x7FFF, floatx80_default_infinity_low);
    }
    normalizeFloatx80Subnormal(bSig, bExp, bSig);
  }
  if (aExp == 0) {
    if (aSig == 0) return packFloatx80(zSign, 0, 0);
    normalizeFloatx80Subnormal(aSig, aExp, aSig);
  }
  zExp = aExp - bExp + 0x3FFE;
  rem1 = 0;
  if (bSig <= aSig) {
    shift128Right(aSig, 0, 1, aSig, rem1);
    ++zExp;
  }
  zSig0 = estimateDiv128To64(aSig, rem1, bSig);
  mul64To128(bSig, zSig0, term0, term1);
  sub128(aSig, rem1, term0, term1, rem0, rem1);
  while ((int64_t)rem0 < 0) {
    --zSig0;
    add128(rem0, rem1, 0, bSig, rem0, rem1);
  }
  zSig1 = estimateDiv128To64(rem1, 0, bSig);
  if ((uint64_t)(zSig1 << 1) <= 8) {
    mul64To128(bSig, zSig1, term1, term2);
    sub128(rem1, 0, term1, term2, rem1, rem2);
    while ((int64_t)rem1 < 0) {
      --zSig1;
      add128(rem1, rem2, 0, bSig, rem1, rem2);
    }
    zSig1 |= ((rem1 | rem2) != 0);
  }
  return roundAndPackFloatx80(status.floatx80_rounding_precision, zSign, zExp, zSig0, zSig1, status);
}

inline floatx80 floatx80_sqrt(floatx80 a, float_status &status) {
  flag aSign;
  int32_t aExp, zExp;
  uint64_t aSig0, aSig1, zSig0, zSig1, doubleZSig0;
  uint64_t rem0, rem1, rem2, rem3, term0, term1, term2, term3;

  if (floatx80_invalid_encoding(a)) {
    float_raise(float_flag_invalid, status);
    return floatx80_default_nan(status);
  }
  aSig0 = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  aSign = extractFloatx80Sign(a);
  if (aExp == 0x7FFF) {
    if ((uint64_t)(aSig0 << 1)) return propagateFloatx80NaNOneArg(a, status);
    if (!aSign) return inf_clear_intbit(status) ? packFloatx80(aSign, aExp, 0) : a;
    goto invalid;
  }
  if (aSign) {
    if ((aExp | aSig0) == 0) return a;
  invalid:
    float_raise(float_flag_invalid, status);
    return floatx80_default_nan(status);
  }
  if (aExp == 0) {
    if (aSig0 == 0) return packFloatx80(0, 0, 0);
    normalizeFloatx80Subnormal(aSig0, aExp, aSig0);
  }
  zExp = ((aExp - 0x3FFF) >> 1) + 0x3FFF;
  zSig0 = estimateSqrt32(aExp, aSig0 >> 32);
  shift128Right(aSig0, 0, 2 + (aExp & 1), aSig0, aSig1);
  zSig0 = estimateDiv128To64(aSig0, aSig1, zSig0 << 32) + (zSig0 << 30);
  doubleZSig0 = zSig0 << 1;
  mul64To128(zSig0, zSig0, term0, term1);
  sub128(aSig0, aSig1, term0, term1, rem0, rem1);
  while ((int64_t)rem0 < 0) {
    --zSig0;
    doubleZSig0 -= 2;
    add128(rem0, rem1, zSig0 >> 63, doubleZSig0 | 1, rem0, rem1);
  }
  zSig1 = estimateDiv128To64(rem1, 0, doubleZSig0);
  if ((zSig1 & 0x3FFFFFFFFFFFFFFFull) <= 5) {
    if (zSig1 == 0) zSig1 = 1;
    mul64To128(doubleZSig0, zSig1, term1, term2);
    sub128(rem1, 0, term1, term2, rem1, rem2);
    mul64To128(zSig1, zSig1, term2, term3);
    sub192(rem1, rem2, 0, 0, term2, term3, rem1, rem2, rem3);
    while ((int64_t)rem1 < 0) {
      --zSig1;
      shortShift128Left(0, zSig1, 1, term2, term3);
      term3 |= 1;
      term2 |= doubleZSig0;
      add192(rem1, rem2, rem3, 0, term2, term3, rem1, rem2, rem3);
    }
    zSig1 |= ((rem1 | rem2 | rem3) != 0);
  }
  shortShift128Left(0, zSig1, 1, zSig0, zSig1);
  zSig0 |= doubleZSig0;
  return roundAndPackFloatx80(status.floatx80_rounding_precision, 0, zExp, zSig0, zSig1, status);
}

// FMOVE/FSMOVE/FDMOVE: round to the current precision (floatx80_move, SOFTFLOAT_68K)
inline floatx80 floatx80_move(floatx80 a, float_status &status) {
  flag aSign;
  int32_t aExp;
  uint64_t aSig;
  aSig = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  aSign = extractFloatx80Sign(a);
  if (aExp == 0x7FFF) {
    if ((uint64_t)(aSig << 1)) return propagateFloatx80NaNOneArg(a, status);
    return inf_clear_intbit(status) ? packFloatx80(aSign, aExp, 0) : a;
  }
  if (aExp == 0) {
    if (aSig == 0) return a;
    return normalizeRoundAndPackFloatx80(status.floatx80_rounding_precision, aSign, aExp, aSig, 0, status);
  }
  return roundAndPackFloatx80(status.floatx80_rounding_precision, aSign, aExp, aSig, 0, status);
}

// FSGLMUL/FSGLDIV rounding (SOFTFLOAT_68K roundSigAndPackFloatx80). Previous
// detects tininess before rounding, so isTiny is always true here.
inline floatx80 roundSigAndPackFloatx80(int8_t roundingPrecision, flag zSign, int32_t zExp, uint64_t zSig0,
                                        uint64_t zSig1, float_status &status) {
  int8_t roundingMode;
  flag roundNearestEven;
  uint64_t roundMask, roundBits;
  uint64_t roundIncrement;

  roundingMode = status.float_rounding_mode;
  roundNearestEven = (roundingMode == float_round_nearest_even);
  if (roundingPrecision == 32) {
    roundIncrement = 0x0000008000000000ull;
    roundMask = 0x000000FFFFFFFFFFull;
  } else if (roundingPrecision == 64) {
    roundIncrement = 0x0000000000000400ull;
    roundMask = 0x00000000000007FFull;
  } else {
    return roundAndPackFloatx80(80, zSign, zExp, zSig0, zSig1, status);
  }
  zSig0 |= (zSig1 != 0);
  if (!roundNearestEven) {
    if (roundingMode == float_round_to_zero) {
      roundIncrement = 0;
    } else {
      roundIncrement = roundMask;
      if (zSign) {
        if (roundingMode == float_round_up) roundIncrement = 0;
      } else {
        if (roundingMode == float_round_down) roundIncrement = 0;
      }
    }
  }
  roundBits = zSig0 & roundMask;

  if (0x7FFE <= (uint32_t)zExp) {
    if ((0x7FFE < zExp) || ((zExp == 0x7FFE) && (zSig0 + roundIncrement < zSig0))) {
      float_raise(float_flag_overflow, status);
      if (zSig0 & roundMask) float_raise(float_flag_inexact, status);
      if ((roundingMode == float_round_to_zero) || (zSign && (roundingMode == float_round_up)) ||
          (!zSign && (roundingMode == float_round_down))) {
        return packFloatx80(zSign, 0x7FFE, 0xFFFFFFFFFFFFFFFFull);
      }
      return packFloatx80(zSign, 0x7FFF, floatx80_default_infinity_low);
    }
    if (zExp < 0) {
      float_raise(float_flag_underflow, status);   // isTiny: tininess before rounding
      shift64RightJamming(zSig0, -zExp, zSig0);
      zExp = 0;
      roundBits = zSig0 & roundMask;
      if (roundBits) float_raise(float_flag_inexact, status);
      zSig0 += roundIncrement;
      if (roundNearestEven && (roundBits == roundIncrement)) {
        roundMask |= roundIncrement << 1;
      }
      zSig0 &= ~roundMask;
      return packFloatx80(zSign, zExp, zSig0);
    }
  }
  if (roundBits) {
    float_raise(float_flag_inexact, status);
  }
  zSig0 += roundIncrement;
  if (zSig0 < roundIncrement) {
    ++zExp;
    zSig0 = 0x8000000000000000ull;
  }
  roundIncrement = roundMask + 1;
  if (roundNearestEven && (roundBits << 1 == roundIncrement)) {
    roundMask |= roundIncrement;
  }
  zSig0 &= ~roundMask;
  if (zSig0 == 0) zExp = 0;
  return packFloatx80(zSign, zExp, zSig0);
}

inline floatx80 floatx80_sglmul(floatx80 a, floatx80 b, float_status &status) {
  flag aSign, bSign, zSign;
  int32_t aExp, bExp, zExp;
  uint64_t aSig, bSig, zSig0, zSig1;

  aSig = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  aSign = extractFloatx80Sign(a);
  bSig = extractFloatx80Frac(b);
  bExp = extractFloatx80Exp(b);
  bSign = extractFloatx80Sign(b);
  zSign = aSign ^ bSign;
  if (aExp == 0x7FFF) {
    if ((uint64_t)(aSig << 1) || ((bExp == 0x7FFF) && (uint64_t)(bSig << 1))) {
      return propagateFloatx80NaN(a, b, status);
    }
    if ((bExp | bSig) == 0) goto invalid;
    if (inf_clear_intbit(status)) aSig = 0;
    return packFloatx80(zSign, aExp, aSig);
  }
  if (bExp == 0x7FFF) {
    if ((uint64_t)(bSig << 1)) return propagateFloatx80NaN(a, b, status);
    if ((aExp | aSig) == 0) {
    invalid:
      float_raise(float_flag_invalid, status);
      return floatx80_default_nan(status);
    }
    if (inf_clear_intbit(status)) bSig = 0;
    return packFloatx80(zSign, bExp, bSig);
  }
  if (aExp == 0) {
    if (aSig == 0) return packFloatx80(zSign, 0, 0);
    normalizeFloatx80Subnormal(aSig, aExp, aSig);
  }
  if (bExp == 0) {
    if (bSig == 0) return packFloatx80(zSign, 0, 0);
    normalizeFloatx80Subnormal(bSig, bExp, bSig);
  }
  aSig &= 0xFFFFFF0000000000ull;
  bSig &= 0xFFFFFF0000000000ull;
  zExp = aExp + bExp - 0x3FFE;
  mul64To128(aSig, bSig, zSig0, zSig1);
  if (0 < (int64_t)zSig0) {
    shortShift128Left(zSig0, zSig1, 1, zSig0, zSig1);
    --zExp;
  }
  return roundSigAndPackFloatx80(32, zSign, zExp, zSig0, zSig1, status);
}

inline floatx80 floatx80_sgldiv(floatx80 a, floatx80 b, float_status &status) {
  flag aSign, bSign, zSign;
  int32_t aExp, bExp, zExp;
  uint64_t aSig, bSig, zSig0, zSig1;
  uint64_t rem0, rem1, rem2, term0, term1, term2;

  aSig = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  aSign = extractFloatx80Sign(a);
  bSig = extractFloatx80Frac(b);
  bExp = extractFloatx80Exp(b);
  bSign = extractFloatx80Sign(b);
  zSign = aSign ^ bSign;
  if (aExp == 0x7FFF) {
    if ((uint64_t)(aSig << 1)) return propagateFloatx80NaN(a, b, status);
    if (bExp == 0x7FFF) {
      if ((uint64_t)(bSig << 1)) return propagateFloatx80NaN(a, b, status);
      goto invalid;
    }
    if (inf_clear_intbit(status)) aSig = 0;
    return packFloatx80(zSign, aExp, aSig);
  }
  if (bExp == 0x7FFF) {
    if ((uint64_t)(bSig << 1)) return propagateFloatx80NaN(a, b, status);
    return packFloatx80(zSign, 0, 0);
  }
  if (bExp == 0) {
    if (bSig == 0) {
      if ((aExp | aSig) == 0) {
      invalid:
        float_raise(float_flag_invalid, status);
        return floatx80_default_nan(status);
      }
      float_raise(float_flag_divbyzero, status);
      return packFloatx80(zSign, 0x7FFF, floatx80_default_infinity_low);
    }
    normalizeFloatx80Subnormal(bSig, bExp, bSig);
  }
  if (aExp == 0) {
    if (aSig == 0) return packFloatx80(zSign, 0, 0);
    normalizeFloatx80Subnormal(aSig, aExp, aSig);
  }

  zExp = aExp - bExp + 0x3FFE;
  rem1 = 0;
  if (bSig <= aSig) {
    shift128Right(aSig, 0, 1, aSig, rem1);
    ++zExp;
  }
  zSig0 = estimateDiv128To64(aSig, rem1, bSig);
  mul64To128(bSig, zSig0, term0, term1);
  sub128(aSig, rem1, term0, term1, rem0, rem1);
  while ((int64_t)rem0 < 0) {
    --zSig0;
    add128(rem0, rem1, 0, bSig, rem0, rem1);
  }
  zSig1 = estimateDiv128To64(rem1, 0, bSig);
  if ((uint64_t)(zSig1 << 1) <= 8) {
    mul64To128(bSig, zSig1, term1, term2);
    sub128(rem1, 0, term1, term2, rem1, rem2);
    while ((int64_t)rem1 < 0) {
      --zSig1;
      add128(rem1, rem2, 0, bSig, rem1, rem2);
    }
    zSig1 |= ((rem1 | rem2) != 0);
  }
  return roundSigAndPackFloatx80(32, zSign, zExp, zSig0, zSig1, status);
}

inline floatx80 floatx80_abs(floatx80 a, float_status &status) {
  int32_t aExp;
  uint64_t aSig;
  aSig = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  if (aExp == 0x7FFF) {
    if ((uint64_t)(aSig << 1)) return propagateFloatx80NaNOneArg(a, status);
    if (inf_clear_intbit(status)) aSig = 0;
    return packFloatx80(0, aExp, aSig);
  }
  if (aExp == 0) {
    if (aSig == 0) return packFloatx80(0, 0, 0);
    normalizeFloatx80Subnormal(aSig, aExp, aSig);
  }
  return roundAndPackFloatx80(status.floatx80_rounding_precision, 0, aExp, aSig, 0, status);
}

inline floatx80 floatx80_neg(floatx80 a, float_status &status) {
  flag aSign;
  int32_t aExp;
  uint64_t aSig;
  aSig = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  aSign = extractFloatx80Sign(a);
  if (aExp == 0x7FFF) {
    if ((uint64_t)(aSig << 1)) return propagateFloatx80NaNOneArg(a, status);
    if (inf_clear_intbit(status)) aSig = 0;
    return packFloatx80(!aSign, aExp, aSig);
  }
  aSign = !aSign;
  if (aExp == 0) {
    if (aSig == 0) return packFloatx80(aSign, 0, 0);
    normalizeFloatx80Subnormal(aSig, aExp, aSig);
  }
  return roundAndPackFloatx80(status.floatx80_rounding_precision, aSign, aExp, aSig, 0, status);
}

// FCMP: the result is a value whose sign/zero/infinity/NaN give the
// condition codes. The 68040 sets the special flag cmp_signed_nan.
inline flag fcmp_signed_nan(float_status &status) { return 1; }

inline floatx80 floatx80_cmp(floatx80 a, floatx80 b, float_status &status) {
  flag aSign, bSign;
  int32_t aExp, bExp;
  uint64_t aSig, bSig;
  aSig = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  aSign = extractFloatx80Sign(a);
  bSig = extractFloatx80Frac(b);
  bExp = extractFloatx80Exp(b);
  bSign = extractFloatx80Sign(b);
  if ((aExp == 0x7FFF && (uint64_t)(aSig << 1)) || (bExp == 0x7FFF && (uint64_t)(bSig << 1))) {
    // 68040 FCMP -NaN return N flag set
    if (fcmp_signed_nan(status)) return propagateFloatx80NaN(a, b, status);
    return propagateFloatx80NaN(packFloatx80(0, aExp, aSig), packFloatx80(0, bExp, bSig), status);
  }
  if (bExp < aExp) return packFloatx80(aSign, 0x3FFF, 0x8000000000000000ull);
  if (aExp < bExp) return packFloatx80(bSign ^ 1, 0x3FFF, 0x8000000000000000ull);
  if (aExp == 0x7FFF) {
    if (aSign == bSign) return packFloatx80(aSign, 0, 0);
    return packFloatx80(aSign, 0x3FFF, 0x8000000000000000ull);
  }
  if (bSig < aSig) return packFloatx80(aSign, 0x3FFF, 0x8000000000000000ull);
  if (aSig < bSig) return packFloatx80(bSign ^ 1, 0x3FFF, 0x8000000000000000ull);
  if (aSig == 0) return packFloatx80(aSign, 0, 0);
  if (aSign == bSign) return packFloatx80(0, 0, 0);
  return packFloatx80(aSign, 0x3FFF, 0x8000000000000000ull);
}

inline floatx80 floatx80_tst(floatx80 a, float_status &status) {
  int32_t aExp;
  uint64_t aSig;
  aSig = extractFloatx80Frac(a);
  aExp = extractFloatx80Exp(a);
  if (aExp == 0x7FFF && (uint64_t)(aSig << 1)) return propagateFloatx80NaNOneArg(a, status);
  return a;
}
