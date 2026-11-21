// FPU millicode (fpd.c): the integer side of q800sim's double arithmetic.
//
// Everything here works on IEEE double bit patterns (u64) in integer
// registers: the conversions between the 68881's 80-bit extended format and
// double that q800sim makes at the memory boundary (to_d / from_d), the
// conversions to and from the integer formats, rounding to an integral value,
// FGETEXP / FGETMAN and FSCALE's scalbn. The FPU's hardware ops (spec FP ops)
// do the IEEE arithmetic (add, sub, mul, div, sqrt, the single conversions).
//
// Written in the millicode C subset (compiler/README.md) so that it also
// compiles as C++ for tests/fpd (a host test against q800sim's fpu.cc
// semantics): no macros, no floats.
//
// Rounding modes are the 68881's FPCR encoding: 0 to nearest, 1 toward zero,
// 2 toward minus infinity, 3 toward plus infinity. Exception flags are the
// fflags CSR's: NV 16, DZ 8, OF 4, UF 2, NX 1.

const u32 FL_NV = 16;
const u32 FL_DZ = 8;
const u32 FL_OF = 4;
const u32 FL_UF = 2;
const u32 FL_NX = 1;

const u64 D_SIGN = 0x8000000000000000ull;
const u64 D_QNAN = 0x7ff8000000000000ull;   // the default NaN (q800sim's NAN), and the canonical register NaN
const u64 D_INF = 0x7ff0000000000000ull;
const u64 D_FRAC = 0x000fffffffffffffull;
const u64 D_HIDDEN = 0x0010000000000000ull;

inline u32 clz64(u64 x) {
  u32 hi = (u32)(x >> 32);
  if (hi != 0) return __clz(hi);
  u32 lo = (u32)x;
  if (lo != 0) return 32 + __clz(lo);
  return 64;
}

inline u32 d_isnan(u64 d) { return (d & ~D_SIGN) > D_INF; }
inline u32 d_isinf(u64 d) { return (d & ~D_SIGN) == D_INF; }
inline u32 d_iszero(u64 d) { return (d & ~D_SIGN) == 0; }
inline u32 d_exp(u64 d) { return (u32)(d >> 52) & 0x7ff; }

// a NaN result as an FP register holds it: q800sim's from_d makes every NaN
// se = 0x7fff, m = all ones, which to_d reads back as NAN
inline u64 d_canon(u64 d) {
  if (d_isnan(d)) return D_QNAN;
  return d;
}

// mag * 2^(e - 52) with mag in [2^52, 2^53), or a smaller mag (then e is the
// minimum exponent): pack, rounding the bits shifted out below the
// subnormal range.  e is the unbiased exponent of bit 52 of mag.  Rounding
// mode mode, sign neg; flags gets OF / UF / NX.  (mag carries no fraction:
// the callers round to 53 bits first or have exactly 53.)  Used for the
// exact results of scalbn and for to_d's ldexp.
inline u64 d_pack(u32 neg, s32 e, u64 mag, u32 mode, u32 extra, u32 &flags) {
  // mag: the 53-bit significand; extra: 1 if bits below it were lost already
  // (the caller's sticky; not used for underflow, only for an exact check)
  u64 sign = 0;
  if (neg) sign = D_SIGN;
  s32 be = e + 1023;
  if (be >= 0x7ff) {
    flags = flags | FL_OF | FL_NX;
    u32 inf = 0;
    if (mode == 0) inf = 1;
    if (mode == 2 && neg) inf = 1;
    if (mode == 3 && !neg) inf = 1;
    if (inf) return sign | D_INF;
    return sign | 0x7fefffffffffffffull;
  }
  if (be >= 1) return sign | ((u64)be << 52) | (mag & D_FRAC);
  // subnormal: shift right by 1 - be with rounding (tininess before rounding)
  u32 sh = (u32)(1 - be);
  u64 q;
  u64 rem;
  u64 half;
  if (sh >= 64) {
    q = 0;
    rem = mag;
    half = 0xffffffffffffffffull;          // mag < 2^53 is below half an ulp: never a tie
  } else {
    q = mag >> sh;
    rem = mag & ((1ull << sh) - 1);
    half = 1ull << (sh - 1);
  }
  u32 inexact = 0;
  if (rem != 0 || extra) inexact = 1;
  u32 up = 0;
  if (inexact) {
    if (mode == 0) {
      if (rem > half || (rem == half && ((q & 1) != 0 || extra))) up = 1;
    } else if (mode == 2) {
      if (neg) up = 1;
    } else if (mode == 3) {
      if (!neg) up = 1;
    }
    flags = flags | FL_UF | FL_NX;
  }
  if (up) q = q + 1;
  return sign | q;
}

// q800sim to_d: an 80-bit extended (sign and exponent se, explicit-integer
// significand m) to double. (double)m rounds the significand to 53 bits in
// the host's rounding mode, which is to nearest wherever to_d runs on a
// memory or immediate operand (before begin_op sets the FPCR's), then ldexp
// scales it, rounding again (to nearest) below the normal range. No flags:
// q800sim discards them (begin_op clears the host's).
inline u64 ext_to_d(u32 se, u64 m) {
  u64 sign = 0;
  if (se & 0x8000) sign = D_SIGN;
  u32 e = se & 0x7fff;
  if (e == 0x7fff) {
    if ((m << 1) != 0) return sign | D_QNAN;
    return sign | D_INF;
  }
  if (m == 0) return sign;
  u32 lz = clz64(m);
  u64 n = m << lz;
  s32 x = (s32)e - 16383 - (s32)lz;   // n * 2^(x - 63)
  // (double)m: the top 53 bits, to nearest even
  u64 mant = n >> 11;
  u64 rb = n & 0x7ff;
  if (rb > 0x400 || (rb == 0x400 && (mant & 1) != 0)) mant = mant + 1;
  if (mant == (1ull << 53)) {
    mant = mant >> 1;
    x = x + 1;
  }
  u32 fl = 0;
  return d_pack(sign != 0, x, mant, 0, 0, fl);
}

// q800sim to_d under a host rounding mode mode (FMOVEM.X's loads: q800sim sets
// the FPCR's mode around to_d there). Both roundings -- (double)m to 53 bits
// and ldexp below the normal range -- are of the magnitude (to_d negates
// after), so toward -inf and toward +inf act on |x|. No flags (FMOVEM sets
// none).
inline u64 ext_to_d_rm(u32 se, u64 m, u32 mode) {
  u64 sign = 0;
  if (se & 0x8000) sign = D_SIGN;
  u32 e = se & 0x7fff;
  if (e == 0x7fff) {
    if ((m << 1) != 0) return sign | D_QNAN;
    return sign | D_INF;
  }
  if (m == 0) return sign;
  u32 lz = clz64(m);
  u64 n = m << lz;
  s32 x = (s32)e - 16383 - (s32)lz;
  u64 mant = n >> 11;
  u64 rb = n & 0x7ff;
  u32 up = 0;
  if (mode == 0) {
    if (rb > 0x400 || (rb == 0x400 && (mant & 1) != 0)) up = 1;
  } else if (mode == 3) {
    if (rb != 0) up = 1;                    // toward +inf, of the magnitude
  }
  if (up) mant = mant + 1;
  if (mant == (1ull << 53)) {
    mant = mant >> 1;
    x = x + 1;
  }
  u32 fl = 0;
  return sign | d_pack(0, x, mant, mode, 0, fl);
}

// q800sim from_d: a double to the extended format, exactly (a NaN becomes
// the canonical se = 0x7fff, m = all ones, its sign dropped)
inline void d_to_ext(u64 d, u32 &se, u64 &m) {
  u32 sg = 0;
  if (d & D_SIGN) sg = 0x8000;
  u32 e = d_exp(d);
  u64 f = d & D_FRAC;
  if (d_isnan(d)) {
    se = 0x7fff;
    m = 0xffffffffffffffffull;
    return;
  }
  if (e == 0x7ff) {
    se = sg | 0x7fff;
    m = 0;
    return;
  }
  if (e == 0 && f == 0) {
    se = sg;
    m = 0;
    return;
  }
  if (e != 0) {
    se = sg | (e - 1023 + 16383);
    m = 0x8000000000000000ull | (f << 11);
    return;
  }
  u32 lz = clz64(f);
  m = f << lz;
  se = sg | (u32)(16383 - 1011 - (s32)lz);
}

// int32 to double (exact)
inline u64 i32_to_d(s32 v) {
  if (v == 0) return 0;
  u64 sign = 0;
  u32 a = (u32)v;
  if (v < 0) {
    sign = D_SIGN;
    a = 0 - a;
  }
  u32 lz = __clz(a);
  u64 mant = (u64)a << (21 + lz);          // bit 52 set
  u32 e = 31 - lz;
  return sign | ((u64)(e + 1023) << 52) | (mant & D_FRAC);
}

// single to double (exact): (double)bits2f(v). A signaling NaN is quieted
// (the conversion's invalid flag is discarded: q800sim converts before
// begin_op)
inline u64 s_to_d(u32 v) {
  u64 sign = 0;
  if (v & 0x80000000u) sign = D_SIGN;
  u32 e = (v >> 23) & 0xff;
  u32 f = v & 0x7fffff;
  if (e == 0xff) {
    if (f == 0) return sign | D_INF;
    return sign | D_QNAN | ((u64)(f & 0x3fffff) << 29);
  }
  if (e == 0) {
    if (f == 0) return sign;
    u32 lz = __clz(f) - 8;                 // f's leading one to bit 23
    u64 mant = (u64)(f << lz) << 29;       // bit 52 set
    u32 be = 1023 - 126 - lz;
    return sign | ((u64)be << 52) | (mant & D_FRAC);
  }
  return sign | ((u64)(e + 896) << 52) | ((u64)f << 29);
}

// the magnitude of d rounded to an integral value in mode (nearbyint: no
// inexact flag), for finite d with |d| < 2^63; inexact is set when it
// differs from d
inline u64 d_rint_mag(u64 d, u32 mode, u32 &inexact) {
  u32 neg = 0;
  if (d & D_SIGN) neg = 1;
  u32 e = d_exp(d);
  inexact = 0;
  if (d_iszero(d)) return 0;
  u64 mant = (d & D_FRAC) | D_HIDDEN;
  if (e == 0) mant = d & D_FRAC;
  if (e >= 1075) return mant << (e - 1075);
  // value = mant * 2^(e - 1075); sh = 1075 - e >= 1
  u32 sh = 1075 - e;
  if (e == 0) sh = 1074;
  u64 ip;
  u64 rem;
  u64 half;
  if (sh >= 64) {
    ip = 0;
    rem = mant;
    half = 0xffffffffffffffffull;          // rem < half always (a tie is impossible)
  } else {
    ip = mant >> sh;
    rem = mant & ((1ull << sh) - 1);
    half = 1ull << (sh - 1);
  }
  if (rem == 0) return ip;
  inexact = 1;
  u32 up = 0;
  if (mode == 0) {
    if (rem > half || (rem == half && (ip & 1) != 0)) up = 1;
  } else if (mode == 2) {
    if (neg) up = 1;
  } else if (mode == 3) {
    if (!neg) up = 1;
  }
  if (up) ip = ip + 1;
  return ip;
}

// a nonnegative integer below 2^53 to double, with sign
inline u64 mag_to_d(u32 neg, u64 a) {
  u64 sign = 0;
  if (neg) sign = D_SIGN;
  if (a == 0) return sign;
  u32 lz = clz64(a);
  u64 mant = a << lz;                       // bit 63 set
  u32 e = 63 - lz;
  return sign | ((u64)(e + 1023) << 52) | ((mant >> 11) & D_FRAC);
}

// FINT / FINTRZ: nearbyint (mode) / trunc. A NaN is quieted (invalid on a
// signaling one: frinti / frintz); no inexact.
inline u64 d_rint(u64 d, u32 mode, u32 &flags) {
  if (d_isnan(d)) {
    if ((d & 0x0008000000000000ull) == 0) flags = flags | FL_NV;
    return d | 0x0008000000000000ull;
  }
  if (d_isinf(d) || d_exp(d) >= 1075) return d;
  u32 ix;
  u64 mag = d_rint_mag(d, mode, ix);
  return mag_to_d((d & D_SIGN) != 0, mag);
}

// q800sim to_int: nearbyint in mode, INEX2 when it changed the value, OPERR
// for a NaN or a value outside [lo, hi] (saturating). Returns the integer;
// fpsr gets the 68k exception bits (0x2000 OPERR, 0x200 INEX2).
inline s32 d_to_int(u64 d, u32 mode, s32 lo, s32 hi, u32 &fpsr) {
  if (d_isnan(d)) {
    fpsr = fpsr | 0x2000;
    return hi;
  }
  u32 neg = 0;
  if (d & D_SIGN) neg = 1;
  if (d_isinf(d) || d_exp(d) >= 1023 + 62) {
    fpsr = fpsr | 0x2000;                   // |d| >= 2^62: integral and out of range
    if (neg) return lo;
    return hi;
  }
  u32 ix;
  u64 mag = d_rint_mag(d, mode, ix);
  if (ix) fpsr = fpsr | 0x200;
  s64 r = (s64)mag;
  if (neg) r = 0 - r;
  if (r < (s64)lo) {
    fpsr = fpsr | 0x2000;
    return lo;
  }
  if (r > (s64)hi) {
    fpsr = fpsr | 0x2000;
    return hi;
  }
  return (s32)r;
}

// the AArch64 fcvtzs w, d that q800sim's FSCALE compiles (int)trunc(src)
// to: toward zero, saturating; invalid out of range, else inexact if a
// fraction was dropped. d is finite.
inline s32 d_fcvtzs(u64 d, u32 &flags) {
  u32 neg = 0;
  if (d & D_SIGN) neg = 1;
  if (d_exp(d) >= 1023 + 32) {              // |d| >= 2^32
    flags = flags | FL_NV;
    if (neg) return (s32)0x80000000u;
    return 0x7fffffff;
  }
  u32 ix;
  u64 mag = d_rint_mag(d, 1, ix);
  if (!neg && mag > 0x7fffffffull) {
    flags = flags | FL_NV;
    return 0x7fffffff;
  }
  if (neg && mag > 0x80000000ull) {
    flags = flags | FL_NV;
    return (s32)0x80000000u;
  }
  if (ix) flags = flags | FL_NX;
  if (neg) return (s32)(0 - (u32)mag);
  return (s32)(u32)mag;
}

// ilogb of a finite nonzero double
inline s32 d_ilogb(u64 d) {
  u32 e = d_exp(d);
  if (e != 0) return (s32)e - 1023;
  return (s32)(63 - clz64(d & D_FRAC)) - 1074;   // f * 2^-1074
}

// FGETEXP: 0, inf and NaN as q800sim (an infinity gives NAN, no flag; a
// zero or a NaN is the result itself), else ilogb as a double
inline u64 d_getexp(u64 d) {
  if (d_isinf(d)) return D_QNAN;
  if (d_iszero(d) || d_isnan(d)) return d;
  return i32_to_d(d_ilogb(d));
}

// FGETMAN: scalbn(d, -ilogb(d)), the significand in [1, 2) with d's sign
inline u64 d_getman(u64 d) {
  if (d_isinf(d)) return D_QNAN;
  if (d_iszero(d) || d_isnan(d)) return d;
  u64 sign = d & D_SIGN;
  u32 e = d_exp(d);
  u64 f = d & D_FRAC;
  if (e == 0) {
    u32 lz = clz64(f);
    f = (f << (lz - 11)) & D_FRAC;          // the leading one to bit 52, dropped
  }
  return sign | 0x3ff0000000000000ull | f;
}

// scalbn(d, n) in mode, correctly rounded once (libm's); flags OF / UF / NX
inline u64 d_scalbn(u64 d, s32 n, u32 mode, u32 &flags) {
  if (d_isnan(d) || d_isinf(d) || d_iszero(d)) return d;
  u32 neg = 0;
  if (d & D_SIGN) neg = 1;
  u32 e = d_exp(d);
  u64 mant = (d & D_FRAC) | D_HIDDEN;
  s32 x = (s32)e - 1023;
  if (e == 0) {
    u64 f = d & D_FRAC;
    u32 lz = clz64(f);
    mant = f << (lz - 11);
    x = -1011 - (s32)lz;
  }
  // x + n, clamped far outside the range (an int n may be +-2^31)
  s32 xe = x;
  if (n > 4000) xe = 4000;
  else if (n < -4000) xe = -4000;
  else xe = x + n;
  return d_pack(neg, xe, mant, mode, 0, flags);
}

// the FPSR condition codes of a result: N (sign), and NAN, I or Z
inline u32 d_fpcc(u64 d) {
  u32 cc = 0;
  if (d & D_SIGN) cc = 0x08000000;
  if (d_isnan(d)) cc = cc | 0x01000000;
  else if (d_isinf(d)) cc = cc | 0x02000000;
  else if (d_iszero(d)) cc = cc | 0x04000000;
  return cc;
}
