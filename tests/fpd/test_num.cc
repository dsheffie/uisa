// Host test of millicode/fpd_num.h (the FPU millicode's integer helpers)
// against the host double arithmetic q800sim's fpu.cc uses: the conversions
// to_d / from_d, nearbyint / trunc, q800sim's to_int, the fcvtzs that
// FSCALE's (int)trunc(src) compiles to, FGETEXP / FGETMAN and scalbn, in
// every rounding mode, results and exception flags. Needs an AArch64 host
// (q800sim's: tininess before rounding, fcvtzs flags).
//
//   c++ -std=c++17 -O1 -Imillicode -o build/test_fpd_num tests/fpd/test_num.cc && ./build/test_fpd_num [n]
#pragma STDC FENV_ACCESS ON
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
static inline u32 __clz(u32 x) { return x ? __builtin_clz(x) : 32; }
#include "fpd_num.h"

static double u2d(u64 u) { double d; memcpy(&d, &u, 8); return d; }
static u64 d2u(double d) { u64 u; memcpy(&u, &d, 8); return u; }
static const int MODES[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD};
static u32 host_flags() {
  int fe = std::fetestexcept(FE_ALL_EXCEPT);
  return ((fe & FE_INVALID) ? FL_NV : 0) | ((fe & FE_DIVBYZERO) ? FL_DZ : 0) | ((fe & FE_OVERFLOW) ? FL_OF : 0) |
         ((fe & FE_UNDERFLOW) ? FL_UF : 0) | ((fe & FE_INEXACT) ? FL_NX : 0);
}

// q800sim fpu.cc's to_d / from_d, verbatim
struct fpx_t { uint16_t se = 0; uint64_t m = 0; };
static double to_d(const fpx_t &x) {
  bool neg = x.se & 0x8000;
  int e = x.se & 0x7fff;
  double r;
  if (e == 0x7fff) r = (x.m << 1) ? NAN : INFINITY;
  else if (x.m == 0) r = 0.0;
  else r = ldexp((double)x.m, e - 16383 - 63);
  return neg ? -r : r;
}
static fpx_t from_d(double d) {
  fpx_t x;
  bool neg = std::signbit(d);
  if (std::isnan(d)) { x.se = 0x7fff; x.m = 0xffffffffffffffffull; return x; }
  if (std::isinf(d)) { x.se = (uint16_t)((neg ? 0x8000 : 0) | 0x7fff); x.m = 0; return x; }
  if (d == 0) { x.se = neg ? 0x8000 : 0; x.m = 0; return x; }
  int e;
  double f = frexp(fabs(d), &e);
  x.m = (uint64_t)ldexp(f, 64);
  x.se = (uint16_t)((neg ? 0x8000 : 0) | (uint16_t)(e - 1 + 16383));
  return x;
}

static std::mt19937_64 rng(1);
// doubles aimed at the edges: denormals, the normal limits, NaNs, integers, halves
static u64 rnd_d() {
  u64 s = (rng() & 1) ? D_SIGN : 0;
  switch (rng() % 12) {
  case 0: return s | (rng() & D_FRAC);                                   // subnormal
  case 1: return s | ((u64)(1 + rng() % 3) << 52) | (rng() & D_FRAC);   // smallest normals
  case 2: return s | ((u64)(0x7fe - rng() % 3) << 52) | (rng() & D_FRAC);
  case 3: return s | D_INF | ((rng() % 3) ? (rng() & D_FRAC) : 0);     // inf / NaN
  case 4: return d2u((double)(int64_t)(rng() % 2000000) * ((rng() & 1) ? 0.5 : 0.25)) | s;
  case 5: return d2u(std::ldexp((double)(rng() & 0xffffffffull), (int)(rng() % 80) - 60)) | s;
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
      if (rng() % 4 == 0) x.m = (x.m | 0x8000000000000000ull) & ~0x3ffull | 0x400;   // ties
      u64 want = d2u(to_d(x));
      u64 got = ext_to_d(x.se, x.m);
      if (got != want) bad("ext_to_d", ((u64)x.se << 48) ^ x.m, got, want);
      u64 d = rnd_d();
      fpx_t w = from_d(u2d(d));
      u32 se;
      u64 m;
      d_to_ext(d, se, m);
      if (se != w.se || m != w.m) bad("d_to_ext", d, ((u64)se << 48) ^ m, ((u64)w.se << 48) ^ w.m);
      if (!d_isnan(d) && ext_to_d(se, m) != d) bad("ext_to_d(d_to_ext)", d, ext_to_d(se, m), d);
      // FMOVEM.X's load: to_d under the FPCR's rounding mode
      for (u32 mode = 0; mode < 4; mode++) {
        std::fesetround(MODES[mode]);
        u64 wm = d2u(to_d(x));
        std::fesetround(FE_TONEAREST);
        u64 gm = ext_to_d_rm(x.se, x.m, mode);
        if (gm != wm) bad("ext_to_d_rm", ((u64)x.se << 48) ^ x.m, gm, wm, (int)mode);
      }
    }
    for (u32 mode = 0; mode < 4; mode++) {
      u64 d = rnd_d();
      // nearbyint / trunc
      std::fesetround(MODES[mode]);
      std::feclearexcept(FE_ALL_EXCEPT);
      volatile double vd = u2d(d);
      u64 want = d2u(std::nearbyint(vd));
      u32 wf = host_flags();
      std::fesetround(FE_TONEAREST);
      u32 fl = 0;
      u64 got = d_rint(d, mode, fl);
      if (got != want || fl != wf) bad("nearbyint", d, got ^ ((u64)fl << 59), want ^ ((u64)wf << 59), mode);
      std::feclearexcept(FE_ALL_EXCEPT);
      want = d2u(std::trunc(vd));
      wf = host_flags();
      fl = 0;
      got = d_rint(d, 1, fl);
      if (got != want || fl != wf) bad("trunc", d, got, want, mode);
      // q800sim to_int (L, W, B)
      static const s32 lo[3] = {INT32_MIN, INT16_MIN, INT8_MIN}, hi[3] = {INT32_MAX, INT16_MAX, INT8_MAX};
      for (int k = 0; k < 3; k++) {
        std::fesetround(MODES[mode]);
        u32 wfpsr = 0;
        s32 wv;
        double v = u2d(d);
        if (std::isnan(v)) { wfpsr |= 0x2000; wv = hi[k]; }
        else {
          double r = std::nearbyint(v);
          if (r != v) wfpsr |= 0x200;
          if (r < (double)lo[k]) { wfpsr |= 0x2000; wv = lo[k]; }
          else if (r > (double)hi[k]) { wfpsr |= 0x2000; wv = hi[k]; }
          else wv = (s32)(int64_t)r;
        }
        std::fesetround(FE_TONEAREST);
        u32 gfpsr = 0;
        s32 gv = d_to_int(d, mode, lo[k], hi[k], gfpsr);
        if (gv != wv || gfpsr != wfpsr) bad("to_int", d, ((u64)gfpsr << 32) | (u32)gv, ((u64)wfpsr << 32) | (u32)wv, mode);
      }
      if (!d_isnan(d) && !d_isinf(d)) {
        // fcvtzs (FSCALE's (int) conversion)
        std::feclearexcept(FE_ALL_EXCEPT);
        volatile s32 wi;
        asm volatile("fcvtzs %w0, %d1" : "=r"(wi) : "w"(vd));
        wf = host_flags();
        fl = 0;
        s32 gi = d_fcvtzs(d, fl);
        if (gi != wi || fl != wf) bad("fcvtzs", d, ((u64)fl << 32) | (u32)gi, ((u64)wf << 32) | (u32)wi, mode);
      }
      // scalbn (FSCALE): dst finite, n in a range that reaches over- and underflow
      u64 x = rnd_d();
      s32 k = (s32)(rng() % 4400) - 2200;
      if (rng() % 8 == 0) k = (rng() & 1) ? INT32_MAX : INT32_MIN;
      if (rng() % 4 == 0) k = (s32)(rng() % 120) - 60;
      std::fesetround(MODES[mode]);
      std::feclearexcept(FE_ALL_EXCEPT);
      volatile double vx = u2d(d_canon(x));
      want = d2u(std::scalbn(vx, k));
      wf = host_flags();
      std::fesetround(FE_TONEAREST);
      fl = 0;
      got = d_scalbn(d_canon(x), k, mode, fl);
      if (got != want || fl != wf) bad("scalbn", d_canon(x), got ^ ((u64)fl << 59), want ^ ((u64)wf << 59), mode * 100000 + (k & 0xffff));
    }
    // FGETEXP / FGETMAN (q800sim's code)
    {
      u64 d = rnd_d();
      double s = u2d(d);
      double we = (s == 0 || std::isinf(s) || std::isnan(s)) ? (std::isinf(s) ? NAN : s) : (double)ilogb(s);
      double wm = (s == 0 || std::isinf(s) || std::isnan(s)) ? (std::isinf(s) ? NAN : s) : scalbn(s, -ilogb(s));
      if (d_getexp(d) != d2u(we)) bad("getexp", d, d_getexp(d), d2u(we));
      if (d_getman(d) != d2u(wm)) bad("getman", d, d_getman(d), d2u(wm));
      // int32 and single to double
      s32 iv = (s32)rng();
      if (rng() & 1) iv >>= rng() % 31;
      if (i32_to_d(iv) != d2u((double)iv)) bad("i32_to_d", (u32)iv, i32_to_d(iv), d2u((double)iv));
      u32 sv = (u32)rng();
      if (rng() % 3 == 0) sv &= 0x807fffffu;
      if (rng() % 5 == 0) sv |= 0x7f800000u;
      float f;
      memcpy(&f, &sv, 4);
      volatile float vf = f;
      if (s_to_d(sv) != d2u((double)vf)) bad("s_to_d", sv, s_to_d(sv), d2u((double)vf));
    }
  }
  printf("test_fpd_num: %ld cases, %ld failures\n", n, fails);
  return fails != 0;
}
