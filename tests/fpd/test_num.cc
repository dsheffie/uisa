// Host test of millicode/fpd_num.h (the FPU millicode's integer helpers)
// against Berkeley SoftFloat-3, q800sim's FPU arithmetic (the softfloat
// submodule of the q800sim checkout beside this one): the conversions
// to_d / from_d (q800sim's code, on SoftFloat's extF80_to_f64), round to
// integral (f64_roundToInt), q800sim's to_int, FSCALE's chop and scalbn
// (q800sim's SoftFloat steps), FGETEXP / FGETMAN, and FSGLMUL / FSGLDIV
// (SoftFloat's extF80 multiply / divide at rounding precision 32: a single
// mantissa, the extended exponent range), in every rounding mode, results and
// exception flags. Portable: no host floating point is the reference.
//
//   c++ -std=c++17 -O1 -Imillicode -I$(Q800SIM)/softfloat/source/include -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 \
//       -o build/test_fpd_num tests/fpd/test_num.cc $(Q800SIM)/softfloat/build/Linux-x86_64-GCC/softfloat.a
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
extern "C" {
#include "softfloat.h"
}

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
static inline u32 __clz(u32 x) { return x ? __builtin_clz(x) : 32; }
#include "fpd_num.h"

static float64_t F(u64 x) { float64_t f; f.v = x; return f; }
static u32 sf_flags() {
  u32 f = softfloat_exceptionFlags;
  return ((f & softfloat_flag_invalid) ? FL_NV : 0) | ((f & softfloat_flag_infinite) ? FL_DZ : 0) |
         ((f & softfloat_flag_overflow) ? FL_OF : 0) | ((f & softfloat_flag_underflow) ? FL_UF : 0) |
         ((f & softfloat_flag_inexact) ? FL_NX : 0);
}
static void sf_begin(u32 mode) {
  softfloat_exceptionFlags = 0;
  softfloat_roundingMode = (uint_fast8_t)mode;
  softfloat_detectTininess = softfloat_tininess_beforeRounding;
}
static u32 tiny64(u64 r) { return (d_exp(r) == 0 && (r & D_FRAC)) ? FL_UF : 0; }

// q800sim fpu.cc's to_d / from_d (SoftFloat), verbatim but for the types
struct fpx_t { uint16_t se = 0; uint64_t m = 0; };
static u64 to_d(const fpx_t &x, int rm) {
  uint64_t sign = (x.se & 0x8000) ? D_SIGN : 0;
  int e = x.se & 0x7fff;
  if (e == 0x7fff) {
    if (!(x.m << 1)) return sign | D_INF;
    uint64_t f = (x.m >> 11) & D_FRAC;
    return sign | D_INF | (f ? f : 1);
  }
  if (x.m == 0) return sign;
  int lz = __builtin_clzll(x.m);
  uint64_t m = x.m << lz;
  int ex = e - lz;
  if (ex < 1) { ex = 1; m = 1ull << 63; }
  extFloat80_t a;
  a.signExp = (uint16_t)((x.se & 0x8000) | ex);
  a.signif = m;
  softfloat_roundingMode = (uint_fast8_t)rm;
  return extF80_to_f64(a).v;
}
static fpx_t from_d(uint64_t d) {
  fpx_t x;
  uint16_t sg = (d & D_SIGN) ? 0x8000 : 0;
  int e = (int)d_exp(d);
  uint64_t f = d & D_FRAC;
  if (e == 0x7ff) { x.se = (uint16_t)(sg | 0x7fff); x.m = f ? (1ull << 63) | (f << 11) : 0; return x; }
  if (e == 0 && f == 0) { x.se = sg; x.m = 0; return x; }
  if (e != 0) { x.se = (uint16_t)(sg | (e - 1023 + 16383)); x.m = (1ull << 63) | (f << 11); return x; }
  int lz = __builtin_clzll(f);
  x.m = f << lz;
  x.se = (uint16_t)(sg | (16383 - 1011 - lz));
  return x;
}
static u64 pow2(int k) { return (uint64_t)(0x3ff + k) << 52; }
// q800sim's FSCALE (musl's scalbn steps on SoftFloat's multiply)
static u64 sf_scalbn(u64 y, int n) {
  float64_t v = F(y);
  if (n > 1023) {
    v = f64_mul(v, F(pow2(1023)));
    n -= 1023;
    if (n > 1023) { v = f64_mul(v, F(pow2(1023))); n -= 1023; if (n > 1023) n = 1023; }
  } else if (n < -1022) {
    v = f64_mul(v, F(pow2(-969)));
    n += 969;
    if (n < -1022) { v = f64_mul(v, F(pow2(-969))); n += 969; if (n < -1022) n = -1022; }
  }
  return f64_mul(v, F(pow2(n))).v;
}
// q800sim's to_int (FPSR bits: 0x4000 SNAN, 0x2000 OPERR, 0x200 INEX2)
static s64 sf_to_int(u64 v, s64 lo, s64 hi, int bits, u32 &fpsr) {
  if (d_isnan(v)) {
    fpsr |= (v & D_QBIT) ? 0x2000 : 0x4000;
    return (s64)(from_d(v | D_QBIT).m >> (64 - bits));
  }
  bool neg = v & D_SIGN;
  if (d_isinf(v) || d_exp(v) >= 1023 + 62) { fpsr |= 0x2000; return neg ? lo : hi; }
  float64_t r = f64_roundToInt(F(v), softfloat_roundingMode, true);
  if (softfloat_exceptionFlags & softfloat_flag_inexact) fpsr |= 0x200;
  s64 n = f64_to_i64(r, softfloat_round_minMag, false);
  if (n < lo) { fpsr |= 0x2000; return lo; }
  if (n > hi) { fpsr |= 0x2000; return hi; }
  return n;
}
// FSGLMUL / FSGLDIV through SoftFloat's extended arithmetic at rounding
// precision 32 (the operands truncated to 24 bits first), for normal operands
// and results in the double's normal range (then extF80_to_f64 is exact)
static extFloat80_t ext_of(u64 d) { fpx_t x = from_d(d); extFloat80_t a; a.signExp = x.se; a.signif = x.m; return a; }
static u64 sf_sgl(u64 a, u64 b, bool div) {
  extFloat80_t ea = ext_of(a & ~0x1fffffffull), eb = ext_of(b & ~0x1fffffffull);
  extF80_roundingPrecision = 32;
  extFloat80_t r = div ? extF80_div(ea, eb) : extF80_mul(ea, eb);
  extF80_roundingPrecision = 80;
  return extF80_to_f64(r).v;
}

static std::mt19937_64 rng(1);
static u64 rnd_d() {
  u64 s = (rng() & 1) ? D_SIGN : 0;
  switch (rng() % 12) {
  case 0: return s | (rng() & D_FRAC);                                   // subnormal
  case 1: return s | ((u64)(1 + rng() % 3) << 52) | (rng() & D_FRAC);   // smallest normals
  case 2: return s | ((u64)(0x7fe - rng() % 3) << 52) | (rng() & D_FRAC);
  case 3: return s | D_INF | ((rng() % 3) ? (rng() & D_FRAC) : 0);     // inf / NaN
  case 4: {                                                               // halves, quarters
    u64 v = i32_to_d((s32)(rng() % 2000000));
    if (v) v -= ((rng() & 1) ? 1ull : 2ull) << 52;
    return v | s;
  }
  case 5: return s | ((u64)(1023 - 60 + rng() % 140) << 52) | (rng() & 0xffffffff00000000ull);
  case 6: return s | ((u64)(1023 + rng() % 70) << 52) | (rng() & D_FRAC);
  case 7: return s | ((u64)(1023 - 60 + rng() % 120) << 52) | ((rng() & 1) ? (rng() & D_FRAC) : (rng() & 0xfffff00000000ull));
  case 8: return s;
  default: return rng();
  }
}

int main(int argc, char **argv) {
  long n = argc > 1 ? atol(argv[1]) : 200000;
  long fails = 0;
  auto bad = [&](const char *what, u64 a, u64 got, u64 want, int mode = -1) {
    if (fails++ < 20) printf("FAIL %s: %016llx mode %d: got %016llx want %016llx\n", what, (unsigned long long)a, mode,
                             (unsigned long long)got, (unsigned long long)want);
  };
  for (long i = 0; i < n; i++) {
    // ext_to_d / d_to_ext
    {
      fpx_t x;
      x.se = (uint16_t)rng();
      switch (rng() % 6) {
      case 0: x.se = (x.se & 0x8000) | (uint16_t)(15360 - 60 + rng() % 120); break;   // around the subnormal limit
      case 1: x.se = (x.se & 0x8000) | (uint16_t)(16383 - 1100 + rng() % 2200); break;
      case 2: x.se = (x.se & 0x8000) | 0x7fff; break;
      case 3: x.se = (x.se & 0x8000) | (uint16_t)(16383 + 1000 + rng() % 40); break;   // overflow
      }
      x.m = rng();
      if (rng() % 4 == 0) x.m &= ~0x7ffull;
      if (rng() % 8 == 0) x.m = 0;
      if (rng() % 8 == 0) x.m |= 0x8000000000000000ull;
      if (rng() % 8 == 0) x.m &= 0xc0000000000007ffull;                                 // NaN payloads below the top 52
      if (rng() % 4 == 0) x.m = ((x.m | 0x8000000000000000ull) & ~0x3ffull) | 0x400;   // ties
      for (u32 mode = 0; mode < 4; mode++) {
        u64 wm = to_d(x, (int)mode);
        u64 gm = ext_to_d_rm(x.se, x.m, mode);
        if (gm != wm) bad("ext_to_d_rm", ((u64)x.se << 48) ^ x.m, gm, wm, (int)mode);
      }
      u64 d = rnd_d();
      fpx_t w = from_d(d);
      u32 se;
      u64 m;
      d_to_ext(d, se, m);
      if (se != w.se || m != w.m) bad("d_to_ext", d, ((u64)se << 48) ^ m, ((u64)w.se << 48) ^ w.m);
      if (ext_to_d(se, m) != d) bad("ext_to_d(d_to_ext)", d, ext_to_d(se, m), d);
    }
    for (u32 mode = 0; mode < 4; mode++) {
      u64 d = rnd_d();
      // round to integral (FINT / FINTRZ: inexact when it changed)
      if (!d_isnan(d)) {
        sf_begin(mode);
        u64 want = f64_roundToInt(F(d), (uint_fast8_t)mode, true).v;
        u32 wf = sf_flags();
        u32 fl = 0;
        u64 got = d_rint(d, mode, fl);
        if (got != want || fl != wf) bad("rint", d, got ^ ((u64)fl << 59), want ^ ((u64)wf << 59), (int)mode);
      }
      // q800sim to_int (L, W, B)
      static const s32 lo[3] = {INT32_MIN, INT16_MIN, INT8_MIN}, hi[3] = {INT32_MAX, INT16_MAX, INT8_MAX};
      static const u32 bits[3] = {32, 16, 8};
      for (int k = 0; k < 3; k++) {
        sf_begin(mode);
        u32 wfpsr = 0;
        s32 wv = (s32)sf_to_int(d, lo[k], hi[k], (int)bits[k], wfpsr);
        u32 gfpsr = 0;
        s32 gv = d_to_int(d, mode, lo[k], hi[k], bits[k], gfpsr);
        if (gv != wv || gfpsr != wfpsr) bad("to_int", d, ((u64)gfpsr << 32) | (u32)gv, ((u64)wfpsr << 32) | (u32)wv, (int)mode);
      }
      // FSCALE: the chop, then scalbn
      if (!d_isnan(d) && !d_isinf(d)) {
        s32 want = (d_exp(d) >= 1023 + 14) ? ((d & D_SIGN) ? -16384 : 16384)
                                           : (s32)f64_to_i32(F(d), softfloat_round_minMag, false);
        if (d_chop14(d) != want) bad("chop14", d, (u32)d_chop14(d), (u32)want);
      }
      u64 x = rnd_d();
      if (!d_isnan(x) && !d_isinf(x) && !d_iszero(x)) {
        s32 k = (s32)(rng() % 4400) - 2200;
        if (rng() % 8 == 0) k = (rng() & 1) ? 16384 : -16384;
        if (rng() % 4 == 0) k = (s32)(rng() % 120) - 60;
        sf_begin(mode);
        u64 want = sf_scalbn(x, k);
        u32 wf = sf_flags() | tiny64(want);
        u32 fl = 0;
        u64 got = d_scalbn(x, k, mode, fl);
        if (got != want || fl != wf) bad("scalbn", x, got ^ ((u64)fl << 59), want ^ ((u64)wf << 59), (int)(mode * 100000 + (k & 0xffff)));
      }
      // FSGLMUL / FSGLDIV, normal operands and results
      u64 a = rnd_d(), b = rnd_d();
      if (d_exp(a) != 0 && d_exp(a) != 0x7ff && d_exp(b) != 0 && d_exp(b) != 0x7ff) {
        for (int dv = 0; dv < 2; dv++) {
          int ee = dv ? (int)d_exp(a) - (int)d_exp(b) : (int)d_exp(a) + (int)d_exp(b) - 2046;
          if (ee < -1000 || ee > 1000) continue;
          sf_begin(mode);
          u64 want = sf_sgl(a, b, dv);
          u32 wf = sf_flags() & FL_NX;
          u32 fl = 0;
          u64 got = dv ? d_sgldiv(a, b, mode, fl) : d_sglmul(a, b, mode, fl);
          if (got != want || fl != wf) bad(dv ? "sgldiv" : "sglmul", a ^ (b >> 32), got ^ ((u64)fl << 59), want ^ ((u64)wf << 59), (int)mode);
        }
      }
    }
    // FGETEXP / FGETMAN, int32 and single to double
    {
      u64 d = rnd_d();
      if (!d_isnan(d)) {
        u64 we, wm;
        if (d_isinf(d)) we = wm = D_DNAN;
        else if (d_iszero(d)) we = wm = d;
        else {
          int e = d_exp(d) ? (int)d_exp(d) - 1023 : (63 - __builtin_clzll(d & D_FRAC)) - 1074;
          we = i32_to_f64(e).v;
          sf_begin(0);
          wm = sf_scalbn(d, -e);   // exact
        }
        if (d_getexp(d) != we) bad("getexp", d, d_getexp(d), we);
        if (d_getman(d) != wm) bad("getman", d, d_getman(d), wm);
      }
      s32 iv = (s32)rng();
      if (rng() & 1) iv >>= rng() % 31;
      if (i32_to_d(iv) != i32_to_f64(iv).v) bad("i32_to_d", (u32)iv, i32_to_d(iv), i32_to_f64(iv).v);
      u32 sv = (u32)rng();
      if (rng() % 3 == 0) sv &= 0x807fffffu;
      if (rng() % 5 == 0) sv |= 0x7f800000u;
      u64 ws;
      if ((sv & 0x7f800000u) == 0x7f800000u && (sv & 0x7fffff))
        ws = ((u64)(sv >> 31) << 63) | D_INF | ((u64)(sv & 0x7fffff) << 29);   // signalling stays signalling
      else {
        float32_t f;
        f.v = sv;
        ws = f32_to_f64(f).v;
      }
      if (s_to_d(sv) != ws) bad("s_to_d", sv, s_to_d(sv), ws);
    }
  }
  printf("test_fpd_num: %ld cases, %ld failures\n", n, fails);
  return fails != 0;
}
