// FPU millicode (fpd.c): the integer side of q800sim's double arithmetic
// (SoftFloat-3 binary64 with the 68881's NaN and underflow rules).
//
// Everything here works on IEEE double bit patterns (u64) in integer
// registers: the conversions between the 68881's 80-bit extended format and
// double that q800sim makes at the memory boundary (to_d / from_d), the
// conversions to and from the integer formats, rounding to an integral value,
// FGETEXP / FGETMAN, FSCALE's scalbn and FSGLMUL / FSGLDIV. The FPU's hardware
// ops (spec FP ops) do the IEEE arithmetic (add, sub, mul, div, sqrt, the
// single conversions). q800sim's fpu.cc has the same C for d_round_pack,
// d_trunc24, d_sglmul and d_sgldiv.
//
// Written in the millicode C subset (compiler/README.md) so that it also
// compiles as C++ for tests/fpd (a host test against q800sim's fpu.cc
// semantics, and against SoftFloat-3): no macros, no floats.
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
const u64 D_DNAN = 0x7fffffffffffffffull;   // the 68881's default NaN: all ones
const u64 D_QBIT = 0x0008000000000000ull;   // a NaN's quiet bit
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
inline u32 d_issnan(u64 d) { return d_isnan(d) && (d & D_QBIT) == 0; }

// sig * 2^(e - 63) (sig's leading one at bit 63), with sticky bits below
// sig, rounded to nbits significant bits (24 or 53) -- fewer on the subnormal
// grid below 2^-1022 -- in mode. flags: OF, UF (tiny before rounding, exact
// or not: the 68881's UNFL), NX. (q800sim fpu.cc has the same code.)
inline u64 d_round_pack(u32 neg, s32 e, u64 sig, u32 sticky, u32 nbits, u32 mode, u32 &flags) {
  u64 sign = 0;
  if (neg) sign = D_SIGN;
  s32 lsb = e - (s32)(nbits - 1);
  if (lsb < -1074) lsb = -1074;
  s32 drop = lsb - (e - 63);
  u64 q = 0;
  u64 rem = 0;
  u32 below = 0;
  u32 inexact = 0;
  u64 half = 0x8000000000000000ull;
  if (drop > 64) {
    below = 1;
    if (sig != 0) inexact = 1;
  } else if (drop == 64) {
    rem = sig;
  } else {
    q = sig >> drop;
    rem = sig & ((1ull << drop) - 1);
    half = 1ull << (drop - 1);
  }
  if (rem != 0 || sticky) inexact = 1;
  u32 up = 0;
  if (inexact) {
    if (mode == 0) {
      if (!below && (rem > half || (rem == half && (sticky || (q & 1) != 0)))) up = 1;
    } else if (mode == 2) {
      if (neg) up = 1;
    } else if (mode == 3) {
      if (!neg) up = 1;
    }
  }
  if (up) q = q + 1;
  if (e < -1022) flags = flags | FL_UF;
  if (inexact) flags = flags | FL_NX;
  if (q == 0) return sign;
  s32 p = 63 - (s32)clz64(q);
  s32 ex = lsb + p;
  if (ex > 1023) {
    flags = flags | FL_OF | FL_NX;
    u32 inf = 0;
    if (mode == 0) inf = 1;
    if (mode == 2 && neg) inf = 1;
    if (mode == 3 && !neg) inf = 1;
    if (inf) return sign | D_INF;
    return sign | 0x7fe0000000000000ull | ((((1ull << (nbits - 1)) - 1) << (53 - nbits)) & D_FRAC);
  }
  if (ex < -1022) return sign | q;
  u64 frac;
  if (p > 52) frac = q >> (p - 52);
  else frac = q << (52 - p);
  return sign | ((u64)(ex + 1023) << 52) | (frac & D_FRAC);
}

// q800sim to_d: an 80-bit extended (sign and exponent se, explicit-integer
// significand m) to double, rounded once in mode (SoftFloat's extF80_to_f64
// after normalizing unnormals and denormals). A NaN keeps its sign and the
// upper 52 bits of its payload (a payload only below them becomes 1: still a
// NaN), signalling or not. No flags (a load sets none).
inline u64 ext_to_d_rm(u32 se, u64 m, u32 mode) {
  u64 sign = 0;
  if (se & 0x8000) sign = D_SIGN;
  u32 e = se & 0x7fff;
  if (e == 0x7fff) {
    if ((m << 1) == 0) return sign | D_INF;
    u64 f = (m >> 11) & D_FRAC;
    if (f == 0) f = 1;
    return sign | D_INF | f;
  }
  if (m == 0) return sign;
  u32 lz = clz64(m);
  u32 fl = 0;
  return d_round_pack(sign != 0, (s32)e - 16383 - (s32)lz, m << lz, 0, 53, mode, fl);
}
// FMOVE.X's and an X operand's load: to nearest
inline u64 ext_to_d(u32 se, u64 m) { return ext_to_d_rm(se, m, 0); }

// q800sim from_d: a double to the extended format, exactly (a NaN's fraction
// is its upper payload, zeros below)
inline void d_to_ext(u64 d, u32 &se, u64 &m) {
  u32 sg = 0;
  if (d & D_SIGN) sg = 0x8000;
  u32 e = d_exp(d);
  u64 f = d & D_FRAC;
  if (d_isnan(d)) {
    se = sg | 0x7fff;
    m = 0x8000000000000000ull | (f << 11);
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

// single to double (exact). A NaN keeps its sign and payload, and a
// signalling one stays signalling (the operation that uses it quiets it)
inline u64 s_to_d(u32 v) {
  u64 sign = 0;
  if (v & 0x80000000u) sign = D_SIGN;
  u32 e = (v >> 23) & 0xff;
  u32 f = v & 0x7fffff;
  if (e == 0xff) {
    if (f == 0) return sign | D_INF;
    return sign | D_INF | ((u64)f << 29);
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

// FINT / FINTRZ: round to an integral value in mode (SoftFloat's
// f64_roundToInt, exact = true: NX when it changed). d is not a NaN.
inline u64 d_rint(u64 d, u32 mode, u32 &flags) {
  if (d_isinf(d) || d_exp(d) >= 1075) return d;
  u32 ix;
  u64 mag = d_rint_mag(d, mode, ix);
  if (ix) flags = flags | FL_NX;
  return mag_to_d((d & D_SIGN) != 0, mag);
}

// q800sim to_int: rounded in mode, INEX2 when it changed the value, OPERR
// for a value outside [lo, hi] (saturating). A NaN gives the top bits bits
// of its quieted extended significand (MC68881UM 6.1.2 / 6.1.3): OPERR for a
// quiet one, SNAN for a signalling one. Returns the integer; fpsr gets the
// 68k exception bits (0x4000 SNAN, 0x2000 OPERR, 0x200 INEX2).
inline s32 d_to_int(u64 d, u32 mode, s32 lo, s32 hi, u32 bits, u32 &fpsr) {
  if (d_isnan(d)) {
    if (d & D_QBIT) fpsr = fpsr | 0x2000;
    else fpsr = fpsr | 0x4000;
    u64 m = 0x8000000000000000ull | D_SIGN >> 1 | ((d & D_FRAC) << 11);
    return (s32)(u32)(m >> (64 - bits));
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

// FSCALE's source chopped to an integer (MC68881UM: rounded toward zero, no
// flag); |d| >= 2^14 gives +-2^14, which always over- or underflows. d is
// finite.
inline s32 d_chop14(u64 d) {
  u32 x = d_exp(d);
  s32 n = 0;
  if (x >= 1023 + 14) n = 16384;
  else if (x >= 1023) n = (s32)(((d & D_FRAC) | D_HIDDEN) >> (52 - (x - 1023)));
  if (d & D_SIGN) n = 0 - n;
  return n;
}

// ilogb of a finite nonzero double
inline s32 d_ilogb(u64 d) {
  u32 e = d_exp(d);
  if (e != 0) return (s32)e - 1023;
  return (s32)(63 - clz64(d & D_FRAC)) - 1074;   // f * 2^-1074
}

// FGETEXP: a zero is the result itself, an infinity the default NaN (the
// caller sets OPERR), else ilogb as a double. d is not a NaN.
inline u64 d_getexp(u64 d) {
  if (d_isinf(d)) return D_DNAN;
  if (d_iszero(d)) return d;
  return i32_to_d(d_ilogb(d));
}

// FGETMAN: scalbn(d, -ilogb(d)), the significand in [1, 2) with d's sign
inline u64 d_getman(u64 d) {
  if (d_isinf(d)) return D_DNAN;
  if (d_iszero(d)) return d;
  u64 sign = d & D_SIGN;
  u32 e = d_exp(d);
  u64 f = d & D_FRAC;
  if (e == 0) {
    u32 lz = clz64(f);
    f = (f << (lz - 11)) & D_FRAC;          // the leading one to bit 52, dropped
  }
  return sign | 0x3ff0000000000000ull | f;
}

// scalbn(d, n) in mode, correctly rounded once; flags OF / UF / NX
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
  s32 xe = x;
  if (n > 4000) xe = 4000;
  else if (n < -4000) xe = -4000;
  else xe = x + n;
  return d_round_pack(neg, xe, mant << 11, 0, 53, mode, flags);
}

// a finite nonzero double's significand truncated to 24 bits (s24, leading
// one at bit 23) and the exponent of that bit
inline u64 d_trunc24(u64 d, s32 &e) {
  u32 x = d_exp(d);
  u64 m = (d & D_FRAC) | D_HIDDEN;
  e = (s32)x - 1023;
  if (x == 0) {
    u64 f = d & D_FRAC;
    u32 lz = clz64(f);
    m = f << (lz - 11);
    e = -1011 - (s32)lz;
  }
  return m >> 29;
}

// FSGLMUL / FSGLDIV (MC68881UM 4.5.5.2, 6.1.4): the operands' mantissas
// truncated to single, the result's rounded to single, the exponent range a
// double's here (extended on a 68881). Finite nonzero operands.
inline u64 d_sglmul(u64 a, u64 b, u32 mode, u32 &flags) {
  s32 ea;
  s32 eb;
  u64 sa = d_trunc24(a, ea);
  u64 sb = d_trunc24(b, eb);
  u64 p = sa * sb;
  u32 lz = clz64(p);
  return d_round_pack(((a ^ b) & D_SIGN) != 0, ea + eb - 46 + (63 - (s32)lz), p << lz, 0, 24, mode, flags);
}
inline u64 d_sgldiv(u64 a, u64 b, u32 mode, u32 &flags) {
  s32 ea;
  s32 eb;
  u64 sa = d_trunc24(a, ea);
  u64 sb = d_trunc24(b, eb);
  u64 n = sa << 40;
  u64 q = n / sb;
  u64 rem = n - q * sb;
  u32 lz = clz64(q);
  return d_round_pack(((a ^ b) & D_SIGN) != 0, ea - eb - 40 + (63 - (s32)lz), q << lz, rem != 0, 24, mode, flags);
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
