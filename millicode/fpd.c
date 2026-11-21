// The 68k FPU on the FP ops (spec FP section): FPU-gen (F200-F23F), FScc /
// FDBcc / FTRAPcc (F240-F27F), FBcc (F280-F2FF), FSAVE (F300-F33F) and
// FRESTORE (F340-F37F).
//
// Compiled by ucc (compiler/). The semantics are q800sim's (fpu.cc), not a
// 68881's: the FP registers hold IEEE doubles (f0-f7), every operation runs
// in double precision on the FP ops under the FPCR's rounding mode, and
// single precision (the FPCR's, or an FS* form) rounds the double result to
// single (double rounding). Extended values are converted at the memory
// boundary (fpd_num.h ext_to_d / d_to_ext). The q800sim quirks matched:
//   - every arithmetic result (and FMOVECR's constant) goes through
//     (float)r whatever the precision: the compiler made round_prec's
//     conversion unconditional, so its overflow / underflow / inexact /
//     invalid flags always land in the FPSR;
//   - FADD and FMUL compute src op dst (the compiler commuted them), FSUB,
//     FDIV, FSGLDIV and FCMP dst op src, FSGLMUL (float)src * (float)dst: it
//     decides which NaN operand the result is (its sign is the FPSR's N);
//   - FSSQRT / FDSQRT (0x41 / 0x45) are FINT: q800sim masks an FS* / FD*
//     opmode with 0x3b;
//   - FSCALE converts its source with fcvtzs (inexact for a fraction,
//     invalid out of the int range), then scalbn;
//   - FINT / FINTRZ raise no inexact (nearbyint / trunc); FGETEXP / FGETMAN
//     of an infinity is a NaN with no invalid;
//   - FCMP of two like-signed infinities is +-0 but keeps the subtraction's
//     invalid flag;
//   - FTRAPcc, FScc etc. as q800sim; enabled exception traps are not taken.
// A NaN result is written to a register as the canonical NaN 0x7ff8...
// (q800sim's from_d), after the condition codes took its sign.
//
// Every load of an extended value is rounded to a double, as q800sim's: FMOVE.X
// (and an X operand) to nearest (to_d before begin_op), FMOVEM.X with the
// FPCR's rounding mode (q800sim sets it around to_d there), neither touching
// the FPSR. Extended precision is double everywhere (user decision).
//
// Out of the slice (brk, a stop for the RTL's front end): the
// transcendentals, FMOD / FREM, packed decimal, full-format index words, and
// the forms q800sim answers with an F-line or illegal exception after
// changing some state.

#include "fpd_num.h"

const u32 BRK_FP_UNIMPL = 30;    // a transcendental, FMOD / FREM (FP2)
const u32 BRK_FP_PACKED = 31;    // packed decimal
const u32 BRK_FP_GAP = 32;       // (unused since q800sim rounds FMOVEM.X loads to doubles)
const u32 BRK_FP_FULLEXT = 33;   // a full-format index extension word
const u32 BRK_FP_FORM = 34;      // a form q800sim raises an exception for with side effects

const u32 FPSR_BSUN = 0x8000;
const u32 FPSR_SNAN = 0x4000;
const u32 FPSR_OPERR = 0x2000;
const u32 FPSR_OVFL = 0x1000;
const u32 FPSR_UNFL = 0x0800;
const u32 FPSR_DZ = 0x0400;
const u32 FPSR_INEX2 = 0x0200;
const u32 FPSR_INEX1 = 0x0100;

// ---------------------------------------------------------------------------
// FP registers

// FPn as bits (the register number at run time: one case per register)
inline u64 fr_get(u32 n) {
  u32 hi;
  u32 lo;
  switch (n & 7) {
    case 0: hi = __fgethi(0); lo = __fgetlo(0); break;
    case 1: hi = __fgethi(1); lo = __fgetlo(1); break;
    case 2: hi = __fgethi(2); lo = __fgetlo(2); break;
    case 3: hi = __fgethi(3); lo = __fgetlo(3); break;
    case 4: hi = __fgethi(4); lo = __fgetlo(4); break;
    case 5: hi = __fgethi(5); lo = __fgetlo(5); break;
    case 6: hi = __fgethi(6); lo = __fgetlo(6); break;
    default: hi = __fgethi(7); lo = __fgetlo(7); break;
  }
  return ((u64)hi << 32) | lo;
}
inline void fr_put(u32 n, u64 v) {
  u32 hi = (u32)(v >> 32);
  u32 lo = (u32)v;
  switch (n & 7) {
    case 0: __fsethi(0, hi); __fsetlo(0, lo); break;
    case 1: __fsethi(1, hi); __fsetlo(1, lo); break;
    case 2: __fsethi(2, hi); __fsetlo(2, lo); break;
    case 3: __fsethi(3, hi); __fsetlo(3, lo); break;
    case 4: __fsethi(4, hi); __fsetlo(4, lo); break;
    case 5: __fsethi(5, hi); __fsetlo(5, lo); break;
    case 6: __fsethi(6, hi); __fsetlo(6, lo); break;
    default: __fsethi(7, hi); __fsetlo(7, lo); break;
  }
}
// FMOVEM.X's staging temporaries f8-f15 (temp n is f8 + n)
inline void ft_put(u32 n, u64 v) {
  u32 hi = (u32)(v >> 32);
  u32 lo = (u32)v;
  switch (n & 7) {
    case 0: __fsethi(8, hi); __fsetlo(8, lo); break;
    case 1: __fsethi(9, hi); __fsetlo(9, lo); break;
    case 2: __fsethi(10, hi); __fsetlo(10, lo); break;
    case 3: __fsethi(11, hi); __fsetlo(11, lo); break;
    case 4: __fsethi(12, hi); __fsetlo(12, lo); break;
    case 5: __fsethi(13, hi); __fsetlo(13, lo); break;
    case 6: __fsethi(14, hi); __fsetlo(14, lo); break;
    default: __fsethi(15, hi); __fsetlo(15, lo); break;
  }
}
// FPn = temp n
inline void ft_commit(u32 n) {
  switch (n & 7) {
    case 0: __fmov_d(0, 8); break;
    case 1: __fmov_d(1, 9); break;
    case 2: __fmov_d(2, 10); break;
    case 3: __fmov_d(3, 11); break;
    case 4: __fmov_d(4, 12); break;
    case 5: __fmov_d(5, 13); break;
    case 6: __fmov_d(6, 14); break;
    default: __fmov_d(7, 15); break;
  }
}

inline void f8_put(u64 v) { __fsethi(8, (u32)(v >> 32)); __fsetlo(8, (u32)v); }
inline void f9_put(u64 v) { __fsethi(9, (u32)(v >> 32)); __fsetlo(9, (u32)v); }
inline u64 f10_get() { return ((u64)__fgethi(10) << 32) | __fgetlo(10); }
inline u64 f11_get() { return ((u64)__fgethi(11) << 32) | __fgetlo(11); }

// ---------------------------------------------------------------------------
// FPSR

// the exception byte from the FP ops' IEEE flags (q800sim end_op)
inline u32 exc_of(u32 fl) {
  u32 x = 0;
  if (fl & FL_NV) x = x | FPSR_OPERR;
  if (fl & FL_OF) x = x | FPSR_OVFL;
  if (fl & FL_UF) x = x | FPSR_UNFL;
  if (fl & FL_DZ) x = x | FPSR_DZ;
  if (fl & FL_NX) x = x | FPSR_INEX2;
  return x;
}
// the accrued byte from the exception byte (q800sim end_op)
inline u32 accrue(u32 fpsr) {
  if (fpsr & (FPSR_BSUN | FPSR_SNAN | FPSR_OPERR)) fpsr = fpsr | 0x80;
  if (fpsr & FPSR_OVFL) fpsr = fpsr | 0x40;
  if ((fpsr & FPSR_UNFL) && (fpsr & FPSR_INEX2)) fpsr = fpsr | 0x20;
  if (fpsr & FPSR_DZ) fpsr = fpsr | 0x10;
  if (fpsr & (FPSR_INEX1 | FPSR_INEX2 | FPSR_OVFL)) fpsr = fpsr | 0x08;
  return fpsr;
}

// FSAVE: a null frame until the FPU is used (q800sim fpu_null)
inline void mark_used() {
  if (__csrr(CSR_FPUSED) == 0) __csrw(CSR_FPUSED, 1);
}

// q800sim fcond: the FPSR's condition codes against an FPcc predicate.
// The signalling predicates (bit 4) set BSUN and its accrued IOP on a NaN.
inline u32 fcond(u32 fpsr, u32 cc) {
  u32 n = (fpsr >> 27) & 1;
  u32 z = (fpsr >> 26) & 1;
  u32 nan = (fpsr >> 24) & 1;
  u32 t = 0;
  switch (cc & 15) {
    case 0: t = 0; break;
    case 1: t = z; break;
    case 2: t = !(nan || z || n); break;
    case 3: t = z || !(nan || n); break;
    case 4: t = n && !(nan || z); break;
    case 5: t = z || (n && !nan); break;
    case 6: t = !(nan || z); break;
    case 7: t = !nan; break;
    case 8: t = nan; break;
    case 9: t = nan || z; break;
    case 10: t = nan || !(n || z); break;
    case 11: t = nan || z || !n; break;
    case 12: t = nan || (n && !z); break;
    case 13: t = nan || z || n; break;
    case 14: t = !z; break;
    default: t = 1; break;
  }
  return t;
}
inline void fcond_bsun(u32 fpsr, u32 cc) {
  if ((cc & 0x10) && (fpsr & 0x01000000)) __csrw(CSR_FPSR, fpsr | FPSR_BSUN | 0x80);
}

// ---------------------------------------------------------------------------
// Effective addresses

// the extension word at byte offset off of the instruction
inline u32 extw(u32 e01, u32 e23, u32 off) {
  if (off == 2) return e01 >> 16;
  if (off == 4) return e01 & 0xffff;
  if (off == 6) return e23 >> 16;
  if (off == 8) return e23 & 0xffff;
  return __ld_16u_gp(__rdg(GPC) + off);
}

// brief-format index (Xn.W/L * scale + d8) from the extension word at off
inline u32 briefIndex(u32 e01, u32 e23, u32 off) {
  u32 ext = extw(e01, e23, off);
  if (ext & 0x100) __brk(BRK_FP_FULLEXT);
  u32 x = __rdg(ext >> 12);
  if (!(ext & 0x800)) x = (s16)x;
  return (x << ((ext >> 9) & 3)) + (s8)ext;
}

// The address of a memory operand of size bytes (q800sim decode_ea), its
// extension words at off; len grows by them. (An)+ / -(An): newA is An's new
// value and wb is set (by size, an A7 byte by 2). #imm: imm is set and the
// address is the immediate's (data at pc + off; a byte is a word's low byte).
// Dn / An (mode 0 / 1) are the caller's.
struct ea_t {
  u32 a;
  u32 len;
  u32 newA;
  u32 wb;
  u32 imm;
};
inline ea_t ea_calc(u32 mode, u32 reg, u32 size, u32 off, u32 e01, u32 e23, u32 pc) {
  ea_t e;
  e.len = off;
  e.newA = 0;
  e.wb = 0;
  e.imm = 0;
  u32 step = size;
  if (reg == 7 && size == 1) step = 2;
  u32 an = __rdg(A0 + reg);
  switch (mode) {
    case 2: e.a = an; break;
    case 3: e.a = an; e.newA = an + step; e.wb = 1; break;
    case 4: e.a = an - step; e.newA = e.a; e.wb = 1; break;
    case 5: e.a = an + (s16)extw(e01, e23, off); e.len = off + 2; break;
    case 6: e.a = an + briefIndex(e01, e23, off); e.len = off + 2; break;
    default:
      switch (reg) {
        case 0: e.a = (s16)extw(e01, e23, off); e.len = off + 2; break;
        case 1: e.a = (extw(e01, e23, off) << 16) | extw(e01, e23, off + 2); e.len = off + 4; break;
        case 2: e.a = pc + off + (s16)extw(e01, e23, off); e.len = off + 2; break;
        case 3: e.a = pc + off + briefIndex(e01, e23, off); e.len = off + 2; break;
        case 4: {
          e.a = pc + off;
          e.imm = 1;
          u32 n = size;
          if (size == 1) {
            n = 2;
            e.a = e.a + 1;
          }
          e.len = off + n;
          break;
        }
        default: __brk(BRK_FP_FORM);
      }
  }
  return e;
}

// guest loads of an operand (program space for an immediate)
inline u32 ld32(ea_t e, u32 k) {
  if (e.imm) return __ld_32_gp(e.a + k);
  return __ld_32_gd(e.a + k);
}

// ---------------------------------------------------------------------------
// FMOVECR's constants (q800sim rom_constant): the doubles a host computes
inline u64 rom_constant(u32 off) {
  switch (off) {
    case 0x00: return 0x400921fb54442d18ull;   // pi
    case 0x0b: return 0x3fd34413509f79ffull;   // log10(2)
    case 0x0c: return 0x4005bf0a8b145769ull;   // e
    case 0x0d: return 0x3ff71547652b82feull;   // log2(e)
    case 0x0e: return 0x3fdbcb7b1526e50eull;   // log10(e)
    case 0x30: return 0x3fe62e42fefa39efull;   // ln(2)
    case 0x31: return 0x40026bb1bbb55516ull;   // ln(10)
    case 0x32: return 0x3ff0000000000000ull;   // 1e0
    case 0x33: return 0x4024000000000000ull;
    case 0x34: return 0x4059000000000000ull;
    case 0x35: return 0x40c3880000000000ull;
    case 0x36: return 0x4197d78400000000ull;
    case 0x37: return 0x4341c37937e08000ull;   // 1e16
    case 0x38: return 0x4693b8b5b5056e17ull;
    case 0x39: return 0x4d384f03e93ff9f5ull;
    case 0x3a: return 0x5a827748f9301d32ull;
    case 0x3b: return 0x75154fdd7f73bf3cull;   // 1e256
    case 0x3c: case 0x3d: case 0x3e: case 0x3f: return D_INF;   // 1e512.. overflow a double
  }
  return 0;
}

// q800sim round_prec, with the conversion its compiler made unconditional:
// (float)r always (its flags count), the result is (double)(float)r for
// single precision (1), else r. r is in f10; the result is returned.
inline u64 round_prec(u32 prec) {
  __fcvt_s_d(11, 10);
  if (prec == 1) {
    __fcvt_d_s(10, 11);
  }
  return f10_get();
}

// ---------------------------------------------------------------------------
// FMOVE / FMOVEM of the control registers (opclass 100 / 101)

inline void ctl_write(u32 w, u32 v) {
  if (w == 4) __csrw(CSR_FPCR, v & 0xfff0);
  else if (w == 2) __csrw(CSR_FPSR, v & 0x0ffffff8);
  else __csrw(CSR_FPIAR, v);
}
inline u32 ctl_read(u32 w) {
  if (w == 4) return __csrr(CSR_FPCR);
  if (w == 2) return __csrr(CSR_FPSR);
  return __csrr(CSR_FPIAR);
}

inline void fmovem_ctl(u32 op, u32 cmd, u32 e01, u32 e23, u32 pc) {
  u32 mode = (op >> 3) & 7;
  u32 reg = op & 7;
  u32 list = (cmd >> 10) & 7;
  u32 to_mem = cmd & 0x2000;
  u32 n = __cpop(list);
  if (n == 0) __brk(BRK_FP_FORM);
  if (mode == 0 || mode == 1) {
    if (n != 1 || (mode == 1 && list != 1)) __brk(BRK_FP_FORM);
    if (to_mem) {
      u32 v = ctl_read(list);
      mark_used();
      if (mode == 0) __wrg(reg, v);
      else __wrg(A0 + reg, v);
    } else {
      u32 v;
      if (mode == 0) v = __rdg(reg);
      else v = __rdg(A0 + reg);
      mark_used();
      ctl_write(list, v);
    }
    __wrg(GPC, pc + 4);
    __gexit();
  }
  if (mode == 7 && reg == 4) {
    if (to_mem) __brk(BRK_FP_FORM);
    // the immediates follow the command word, FPCR first
    u32 a = pc + 4;
    u32 v4 = 0;
    u32 v2 = 0;
    u32 v1 = 0;
    if (list & 4) { v4 = __ld_32_gp(a); a = a + 4; }
    if (list & 2) { v2 = __ld_32_gp(a); a = a + 4; }
    if (list & 1) { v1 = __ld_32_gp(a); a = a + 4; }
    mark_used();
    if (list & 4) ctl_write(4, v4);
    if (list & 2) ctl_write(2, v2);
    if (list & 1) ctl_write(1, v1);
    __wrg(GPC, a);
    __gexit();
  }
  u32 a;
  u32 len = 4;
  u32 an = __rdg(A0 + reg);
  if (mode == 4) a = an - 4 * n;
  else if (mode == 3) a = an;
  else {
    ea_t e = ea_calc(mode, reg, 4, 4, e01, e23, pc);
    a = e.a;
    len = e.len;
  }
  if (to_mem) {
    u32 b = a;
    if (list & 4) { __st_32_gd(b, __csrr(CSR_FPCR)); b = b + 4; }
    if (list & 2) { __st_32_gd(b, __csrr(CSR_FPSR)); b = b + 4; }
    if (list & 1) { __st_32_gd(b, __csrr(CSR_FPIAR)); b = b + 4; }
    mark_used();
  } else {
    u32 b = a;
    u32 v4 = 0;
    u32 v2 = 0;
    u32 v1 = 0;
    if (list & 4) { v4 = __ld_32_gd(b); b = b + 4; }
    if (list & 2) { v2 = __ld_32_gd(b); b = b + 4; }
    if (list & 1) { v1 = __ld_32_gd(b); b = b + 4; }
    mark_used();
    if (list & 4) ctl_write(4, v4);
    if (list & 2) ctl_write(2, v2);
    if (list & 1) ctl_write(1, v1);
  }
  if (mode == 4) __wrg(A0 + reg, a);
  if (mode == 3) __wrg(A0 + reg, an + 4 * n);
  __wrg(GPC, pc + len);
  __gexit();
}

// ---------------------------------------------------------------------------
// FMOVEM of the data registers (opclass 110 / 111): raw 80-bit images in
// memory, doubles in the registers

inline void fmovem_data(u32 op, u32 cmd, u32 e01, u32 e23, u32 pc) {
  u32 mode = (op >> 3) & 7;
  u32 reg = op & 7;
  u32 to_mem = cmd & 0x2000;
  u32 list = cmd & 0xff;
  if (cmd & 0x0800) list = __rdg((cmd >> 4) & 7) & 0xff;   // dynamic list in Dn
  if (mode == 0 || mode == 1 || (mode == 7 && reg == 4)) __brk(BRK_FP_FORM);
  u32 n = __cpop(list);
  u32 a;
  u32 len = 4;
  u32 an = __rdg(A0 + reg);
  if (mode == 4) {
    if (!to_mem) __brk(BRK_FP_FORM);
    a = an - 12 * n;
    // predecrement lists have FP7 in bit 7 ... FP0 in bit 0: reverse
    u32 rl = 0;
    for (u32 i = 0; i < 8; i++)
      if (list & (1u << i)) rl = rl | (0x80u >> i);
    list = rl;
  } else if (mode == 3) {
    a = an;
  } else {
    ea_t e = ea_calc(mode, reg, 4, 4, e01, e23, pc);
    a = e.a;
    len = e.len;
  }
  if (to_mem) {
    u32 b = a;
    for (u32 r = 0; r < 8; r++) {
      if (!(list & (0x80u >> r))) continue;
      u32 se;
      u64 m;
      d_to_ext(fr_get(r), se, m);
      __st_32_gd(b, se << 16);
      __st_32_gd(b + 4, (u32)(m >> 32));
      __st_32_gd(b + 8, (u32)m);
      b = b + 12;
    }
    mark_used();
  } else {
    // load and convert every register into a temporary first: an access
    // fault leaves no register written.  q800sim rounds each image to a
    // double with the FPCR's rounding mode (to_d under it), no FPSR change
    u32 b = a;
    u32 rmode = (__csrr(CSR_FPCR) >> 4) & 3;
    for (u32 r = 0; r < 8; r++) {
      if (!(list & (0x80u >> r))) continue;
      u32 se = __ld_32_gd(b) >> 16;
      u64 m = ((u64)__ld_32_gd(b + 4) << 32) | __ld_32_gd(b + 8);
      ft_put(r, d_canon(ext_to_d_rm(se, m, rmode)));   // (a NaN as from_d stores it)
      b = b + 12;
    }
    mark_used();
    for (u32 r = 0; r < 8; r++)
      if (list & (0x80u >> r)) ft_commit(r);
  }
  if (mode == 4) __wrg(A0 + reg, a);
  if (mode == 3) __wrg(A0 + reg, an + 12 * n);
  __wrg(GPC, pc + len);
  __gexit();
}

// ---------------------------------------------------------------------------
// FMOVE FPn,<ea> (opclass 011): q800sim store_dst

const u32 FMT_L = 0;
const u32 FMT_S = 1;
const u32 FMT_X = 2;
const u32 FMT_P = 3;
const u32 FMT_W = 4;
const u32 FMT_D = 5;
const u32 FMT_B = 6;

inline void fmove_out(u32 op, u32 cmd, u32 e01, u32 e23, u32 pc) {
  u32 mode = (op >> 3) & 7;
  u32 reg = op & 7;
  u32 fmt = (cmd >> 10) & 7;
  if (fmt == FMT_P || fmt == 7) __brk(BRK_FP_PACKED);
  if (mode == 1 || (mode == 7 && reg >= 2)) __brk(BRK_FP_FORM);
  if (mode == 0 && (fmt == FMT_X || fmt == FMT_D)) __brk(BRK_FP_FORM);
  u64 v = fr_get((cmd >> 7) & 7);
  u32 rmode = (__csrr(CSR_FPCR) >> 4) & 3;
  u32 size;
  switch (fmt) {
    case FMT_L: case FMT_S: size = 4; break;
    case FMT_W: size = 2; break;
    case FMT_B: size = 1; break;
    case FMT_D: size = 8; break;
    default: size = 12; break;
  }
  // the value (begin_op .. end_op): the exception byte is this instruction's
  u32 exc = 0;
  u32 w32 = 0;
  if (fmt == FMT_L) w32 = (u32)d_to_int(v, rmode, (s32)0x80000000u, 0x7fffffff, exc);
  else if (fmt == FMT_W) w32 = (u32)d_to_int(v, rmode, -32768, 32767, exc) & 0xffff;
  else if (fmt == FMT_B) w32 = (u32)d_to_int(v, rmode, -128, 127, exc) & 0xff;
  else if (fmt == FMT_S) {
    __csrw(CSR_FFLAGS, 0);
    f8_put(v);
    __fcvt_s_d(10, 8);
    w32 = __fgetlo(10);
    exc = exc_of(__csrr(CSR_FFLAGS));
  }
  ea_t e;
  e.a = 0;
  e.len = 4;
  e.newA = 0;
  e.wb = 0;
  e.imm = 0;
  if (mode != 0) {
    e = ea_calc(mode, reg, size, 4, e01, e23, pc);
    if (fmt == FMT_D) {
      __st_32_gd(e.a, (u32)(v >> 32));
      __st_32_gd(e.a + 4, (u32)v);
    } else if (fmt == FMT_X) {
      u32 se;
      u64 m;
      d_to_ext(v, se, m);
      __st_32_gd(e.a, se << 16);
      __st_32_gd(e.a + 4, (u32)(m >> 32));
      __st_32_gd(e.a + 8, (u32)m);
    } else if (size == 4) {
      __st_32_gd(e.a, w32);
    } else if (size == 2) {
      __st_16_gd(e.a, w32);
    } else {
      __st_8_gd(e.a, w32);
    }
  }
  // commit
  u32 fpsr = (__csrr(CSR_FPSR) & ~0xff00u) | exc;
  __csrw(CSR_FPSR, accrue(fpsr));
  __csrw(CSR_FPIAR, pc);
  mark_used();
  if (mode == 0) {
    u32 d = __rdg(reg);
    if (size == 4) d = w32;
    else if (size == 2) d = (d & 0xffff0000u) | w32;
    else d = (d & 0xffffff00u) | w32;
    __wrg(reg, d);
  }
  if (e.wb) __wrg(A0 + reg, e.newA);
  __wrg(GPC, pc + e.len);
  __gexit();
}

// ---------------------------------------------------------------------------
// FPU-gen: the general instructions

// the opmodes this routine computes (after q800sim's FS* / FD* mask)
inline u32 implemented(u32 o) {
  switch (o) {
    case 0x00: case 0x01: case 0x03: case 0x04: case 0x18: case 0x1a: case 0x1e: case 0x1f:
    case 0x20: case 0x22: case 0x23: case 0x24: case 0x26: case 0x27: case 0x28: case 0x38: case 0x3a:
      return 1;
  }
  return 0;
}
// the opmodes q800sim computes with libm (and FMOD / FREM): FP2
inline u32 transcendental(u32 o) {
  switch (o) {
    case 0x02: case 0x06: case 0x08: case 0x09: case 0x0a: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    case 0x10: case 0x11: case 0x12: case 0x14: case 0x15: case 0x16: case 0x19: case 0x1c: case 0x1d:
    case 0x21: case 0x25: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
      return 1;
  }
  return 0;
}

entry void fpd_gen(void) {
  u32 op = __opword();
  u32 e01 = __ext01();
  u32 e23 = __ext23();
  u32 cmd = e01 >> 16;
  u32 cls = cmd >> 13;
  u32 pc = __rdg(GPC);
  if (cls == 4 || cls == 5) fmovem_ctl(op, cmd, e01, e23, pc);
  if (cls == 6 || cls == 7) fmovem_data(op, cmd, e01, e23, pc);
  if (cls == 3) fmove_out(op, cmd, e01, e23, pc);
  if (cls == 1) __brk(BRK_FP_FORM);
  u32 mode = (op >> 3) & 7;
  u32 reg = op & 7;
  u32 dn = (cmd >> 7) & 7;
  u32 fmt = (cmd >> 10) & 7;
  u32 fpcr = __csrr(CSR_FPCR);
  u32 prec = (fpcr >> 6) & 3;

  if (cls == 2 && fmt == 7) {
    // FMOVECR: the constant, rounded to the FPCR's precision
    u64 c = rom_constant(cmd & 0x7f);
    __csrw(CSR_FFLAGS, 0);
    f8_put(c);
    __fmov_d(10, 8);
    u64 r = round_prec(prec);
    u32 fl = __csrr(CSR_FFLAGS);
    u32 fpsr = (__csrr(CSR_FPSR) & 0xf0ff00ffu) | d_fpcc(r) | exc_of(fl);
    __csrw(CSR_FPSR, accrue(fpsr));
    __csrw(CSR_FPIAR, pc);
    mark_used();
    fr_put(dn, d_canon(r));
    __wrg(GPC, pc + 4);
    __gexit();
  }

  u32 o = cmd & 0x7f;
  if (o & 0x40) {
    if (o & 4) prec = 2;
    else prec = 1;
    o = o & 0x3b;
  }
  if (transcendental(o)) __brk(BRK_FP_UNIMPL);
  if (!implemented(o)) __brk(BRK_FP_FORM);

  // the source operand (q800sim load_src: no flags)
  u64 src = 0;
  ea_t e;
  e.a = 0;
  e.len = 4;
  e.newA = 0;
  e.wb = 0;
  e.imm = 0;
  if (cls == 0) {
    src = fr_get(fmt);
  } else if (mode == 0) {
    u32 d = __rdg(reg);
    if (fmt == FMT_L) src = i32_to_d((s32)d);
    else if (fmt == FMT_S) src = s_to_d(d);
    else if (fmt == FMT_W) src = i32_to_d((s16)d);
    else if (fmt == FMT_B) src = i32_to_d((s8)d);
    else __brk(BRK_FP_FORM);
  } else {
    if (mode == 1) __brk(BRK_FP_FORM);
    if (fmt == FMT_P) __brk(BRK_FP_PACKED);
    u32 size;
    switch (fmt) {
      case FMT_L: case FMT_S: size = 4; break;
      case FMT_W: size = 2; break;
      case FMT_B: size = 1; break;
      case FMT_D: size = 8; break;
      default: size = 12; break;
    }
    e = ea_calc(mode, reg, size, 4, e01, e23, pc);
    if (fmt == FMT_L) src = i32_to_d((s32)ld32(e, 0));
    else if (fmt == FMT_S) src = s_to_d(ld32(e, 0));
    else if (fmt == FMT_X) {
      u32 se = ld32(e, 0) >> 16;
      u64 m = ((u64)ld32(e, 4) << 32) | ld32(e, 8);
      src = ext_to_d(se, m);
    } else if (fmt == FMT_D) {
      src = ((u64)ld32(e, 0) << 32) | ld32(e, 4);
    } else if (fmt == FMT_W) {
      u32 w;
      if (e.imm) w = __ld_16u_gp(e.a);
      else w = __ld_16u_gd(e.a);
      src = i32_to_d((s16)w);
    } else {
      u32 b;
      if (e.imm) b = __ld_8u_gp(e.a);
      else b = __ld_8u_gd(e.a);
      src = i32_to_d((s8)b);
    }
  }
  u64 dst = fr_get(dn);
  u32 rmode = (fpcr >> 4) & 3;

  // the operation (begin_op .. end_op)
  __csrw(CSR_FFLAGS, 0);
  u32 swfl = 0;                // the flags of the operations done in software
  u32 store = 1;
  f8_put(src);
  f9_put(dst);
  switch (o) {
    case 0x00: __fmov_d(10, 8); break;                       // FMOVE
    case 0x01: {                                              // FINT
      u64 r = d_rint(src, rmode, swfl);
      __fsethi(10, (u32)(r >> 32));
      __fsetlo(10, (u32)r);
      break;
    }
    case 0x03: {                                              // FINTRZ
      u64 r = d_rint(src, 1, swfl);
      __fsethi(10, (u32)(r >> 32));
      __fsetlo(10, (u32)r);
      break;
    }
    case 0x04: __fsqrt_d(10, 8); break;                      // FSQRT
    case 0x18: __fabs_d(10, 8); break;                       // FABS
    case 0x1a: __fneg_d(10, 8); break;                       // FNEG
    case 0x1e: {                                              // FGETEXP
      u64 r = d_getexp(src);
      __fsethi(10, (u32)(r >> 32));
      __fsetlo(10, (u32)r);
      break;
    }
    case 0x1f: {                                              // FGETMAN
      u64 r = d_getman(src);
      __fsethi(10, (u32)(r >> 32));
      __fsetlo(10, (u32)r);
      break;
    }
    case 0x20: __fdiv_d(10, 9, 8); break;                    // FDIV: dst / src
    case 0x22: __fadd_d(10, 8, 9); break;                    // FADD: src + dst
    case 0x23: __fmul_d(10, 8, 9); break;                    // FMUL: src * dst
    case 0x24:                                                // FSGLDIV: (float)dst / (float)src
      __fcvt_s_d(12, 9);
      __fcvt_s_d(13, 8);
      __fdiv_s(14, 12, 13);
      __fcvt_d_s(10, 14);
      break;
    case 0x26: {                                              // FSCALE
      u64 r = D_QNAN;
      if (!d_isnan(src) && !d_isinf(src)) {
        s32 k = d_fcvtzs(src, swfl);
        r = d_scalbn(dst, k, rmode, swfl);
      }
      __fsethi(10, (u32)(r >> 32));
      __fsetlo(10, (u32)r);
      break;
    }
    case 0x27:                                                // FSGLMUL: (float)src * (float)dst
      __fcvt_s_d(12, 9);
      __fcvt_s_d(13, 8);
      __fmul_s(14, 13, 12);
      __fcvt_d_s(10, 14);
      break;
    case 0x28: __fsub_d(10, 9, 8); break;                    // FSUB: dst - src
    case 0x38: {                                              // FCMP: dst - src
      store = 0;
      __fsub_d(10, 9, 8);
      if (d_isinf(dst) && d_isinf(src) && ((dst ^ src) & D_SIGN) == 0) {
        __fsethi(10, (u32)(dst >> 32) & 0x80000000u);
        __fsetlo(10, 0);
      }
      break;
    }
    default:                                                  // 0x3a FTST
      store = 0;
      __fmov_d(10, 8);
      break;
  }
  u64 r = round_prec(prec);
  u32 fl = __csrr(CSR_FFLAGS) | swfl;

  // commit: FPSR (quotient cleared, condition codes, exception and accrued
  // bytes), FPIAR, the register, An
  u32 fpsr = (__csrr(CSR_FPSR) & 0xf00000ffu) | d_fpcc(r) | exc_of(fl);
  __csrw(CSR_FPSR, accrue(fpsr));
  __csrw(CSR_FPIAR, pc);
  mark_used();
  if (store) fr_put(dn, d_canon(r));
  if (e.wb) __wrg(A0 + reg, e.newA);
  __wrg(GPC, pc + e.len);
  __gexit();
}

// ---------------------------------------------------------------------------
// FScc / FDBcc / FTRAPcc

entry void fpd_cc(void) {
  u32 op = __opword();
  u32 e01 = __ext01();
  u32 e23 = __ext23();
  u32 pc = __rdg(GPC);
  u32 mode = (op >> 3) & 7;
  u32 reg = op & 7;
  u32 cc = (e01 >> 16) & 0x3f;
  u32 fpsr = __csrr(CSR_FPSR);
  u32 t = fcond(fpsr, cc);
  if (mode == 1) {                                  // FDBcc Dn,<disp16>
    u32 d16 = e01 & 0xffff;
    u32 npc = pc + 6;
    u32 dn = __rdg(reg);
    mark_used();
    fcond_bsun(fpsr, cc);
    if (!t) {
      u32 cnt = (dn - 1) & 0xffff;
      __wrg(reg, (dn & 0xffff0000u) | cnt);
      if (cnt != 0xffff) npc = pc + 4 + (s16)d16;
    }
    __wrg(GPC, npc);
    __gexit();
  }
  if (mode == 7 && reg >= 2 && reg <= 4) {          // FTRAPcc [#imm]
    u32 len = 4;
    if (reg == 2) len = 6;
    if (reg == 3) len = 8;
    mark_used();
    fcond_bsun(fpsr, cc);
    __wrg(GPC, pc + len);
    if (t) __gfault(7);
    __gexit();
  }
  if (mode == 7 && reg >= 2) __brk(BRK_FP_FORM);
  u32 v = 0;                                        // FScc <ea>
  if (t) v = 0xff;
  if (mode == 0) {
    u32 d = __rdg(reg);
    mark_used();
    fcond_bsun(fpsr, cc);
    __wrg(reg, (d & 0xffffff00u) | v);
    __wrg(GPC, pc + 4);
    __gexit();
  }
  ea_t e = ea_calc(mode, reg, 1, 4, e01, e23, pc);
  __st_8_gd(e.a, v);
  mark_used();
  fcond_bsun(fpsr, cc);
  if (e.wb) __wrg(A0 + reg, e.newA);
  __wrg(GPC, pc + e.len);
  __gexit();
}

// ---------------------------------------------------------------------------
// FBcc (FNOP is FBF.W 0)

entry void fpd_bcc(void) {
  u32 op = __opword();
  u32 e01 = __ext01();
  u32 pc = __rdg(GPC);
  u32 cc = op & 0x3f;
  u32 d;
  u32 len;
  if (op & 0x40) {
    d = e01;
    len = 6;
  } else {
    d = (s16)(e01 >> 16);
    len = 4;
  }
  u32 fpsr = __csrr(CSR_FPSR);
  u32 t = fcond(fpsr, cc);
  mark_used();
  fcond_bsun(fpsr, cc);
  if (t) __wrg(GPC, pc + 2 + d);
  else __wrg(GPC, pc + len);
  __gexit();
}

// ---------------------------------------------------------------------------
// FSAVE / FRESTORE (q800sim: a 4-byte null frame until the FPU is used,
// then an idle one; FRESTORE of a null frame resets the FPU)

entry void fpd_save(void) {
  u32 op = __opword();
  u32 e01 = __ext01();
  u32 e23 = __ext23();
  u32 pc = __rdg(GPC);
  u32 mode = (op >> 3) & 7;
  u32 reg = op & 7;
  u32 ctl = mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg <= 1);
  if (!(ctl || mode == 4)) __gfault(4);
  if (!(__rdg(GSR) & 0x2000)) __gfault(8);
  u32 a;
  u32 len = 2;
  u32 an = __rdg(A0 + reg);
  if (mode == 4) a = an - 4;
  else {
    ea_t e = ea_calc(mode, reg, 4, 2, e01, e23, pc);
    a = e.a;
    len = e.len;
  }
  u32 v = 0;
  if (__csrr(CSR_FPUSED)) v = 0x41000000;
  __st_32_gd(a, v);
  if (mode == 4) __wrg(A0 + reg, a);
  __wrg(GPC, pc + len);
  __gexit();
}

entry void fpd_restore(void) {
  u32 op = __opword();
  u32 e01 = __ext01();
  u32 e23 = __ext23();
  u32 pc = __rdg(GPC);
  u32 mode = (op >> 3) & 7;
  u32 reg = op & 7;
  u32 ctl = mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg <= 3);
  if (!(ctl || mode == 3)) __gfault(4);
  if (!(__rdg(GSR) & 0x2000)) __gfault(8);
  u32 a;
  u32 len = 2;
  u32 an = __rdg(A0 + reg);
  if (mode == 3) a = an;
  else {
    ea_t e = ea_calc(mode, reg, 4, 2, e01, e23, pc);
    a = e.a;
    len = e.len;
  }
  u32 v = __ld_32_gd(a);
  if ((v >> 24) == 0) {
    // a null frame: the FPU's reset state (every register a NaN)
    __csrw(CSR_FPUSED, 0);
    __csrw(CSR_FPCR, 0);
    __csrw(CSR_FPSR, 0);
    __csrw(CSR_FPIAR, 0);
    for (u32 r = 0; r < 8; r++) fr_put(r, D_QNAN);
  } else {
    mark_used();
  }
  if (mode == 3) __wrg(A0 + reg, an + 4);
  __wrg(GPC, pc + len);
  __gexit();
}
