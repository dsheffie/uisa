// FPU-gen (opwords F200-F23F): the 68040 FPU's general instructions.
//
// Compiled by ucc (compiler/). Implements what a 68040 does in hardware and
// matches Previous (cpu/fpp.c with fpu_no_unimplemented, the configuration a
// NeXT 68040 runs) step for step; tests/fpu runs this against Previous's own
// fpp.c. Done so far:
//   opclass 000/010  FPm or <ea> to FPn: FMOVE, FSQRT, FABS, FNEG, FADD, FSUB,
//                    FMUL, FDIV, FSGLMUL, FSGLDIV, FCMP, FTST and the FS*/FD*
//                    rounding variants, every source format but packed
//   opclass 100/101  FMOVE FPcr to/from Dn or An
// The FP registers live in scratch memory (FPREGS, FMOVEM extended layout:
// sign and exponent in bits 31:16 of the first word, then the significand);
// FPCR, FPSR and FPIAR are CSRs.
//
// Instructions the 68040 does not implement, and the exceptions that need an
// FSAVE frame for the FPSP, are not done yet; they stop with a brk code
// (BRK_*) so the shadow model reports them instead of guessing.

#include "softfloat/softfloat.h"

const u32 FPREGS = 0;     // scratch offset of FP0-FP7, 12 bytes each
const u32 FPRES = 96;     // scratch offset of the arithmetic helpers' result

const u32 BRK_UNIMP_INSN = 20;   // unimplemented FP instruction (vector 11 with an FSAVE frame)
const u32 BRK_UNIMP_DATA = 21;   // unimplemented data type: denormal, unnormal or packed (vector 55)
const u32 BRK_ARITH = 22;        // arithmetic exception: enabled, or the 68040's nonmaskable OVFL/UNFL
const u32 BRK_OPCLASS = 23;      // opclass not done yet (FMOVE to <ea>, FMOVEM)
const u32 BRK_FULL_EXT = 24;     // full-format extension word

const u32 FPSR_CC_N = 0x08000000;
const u32 FPSR_CC_Z = 0x04000000;
const u32 FPSR_CC_I = 0x02000000;
const u32 FPSR_CC_NAN = 0x01000000;
const u32 FPSR_SNAN = 0x4000;
const u32 FPSR_OPERR = 0x2000;
const u32 FPSR_OVFL = 0x1000;
const u32 FPSR_UNFL = 0x0800;
const u32 FPSR_DZ = 0x0400;
const u32 FPSR_INEX2 = 0x0200;
const u32 FPSR_INEX1 = 0x0100;
const u32 FPSR_AE_IOP = 0x80;
const u32 FPSR_AE_OVFL = 0x40;
const u32 FPSR_AE_UNFL = 0x20;
const u32 FPSR_AE_DZ = 0x10;
const u32 FPSR_AE_INEX = 0x08;

// ---------------------------------------------------------------------------
// FP registers and the result slot (scratch memory)

inline u32 fpaddr(u32 n) { return __csrr(CSR_SCRATCH_BASE) + FPREGS + n * 12; }

inline floatx80 ldx(u32 a) {
  floatx80 f;
  f.high = __ld_32_h(a) >> 16;
  f.low = ((u64)__ld_32_h(a + 4) << 32) | __ld_32_h(a + 8);
  return f;
}
inline void stx(u32 a, floatx80 f) {
  __st_32_h(a, (u32)f.high << 16);
  __st_32_h(a + 4, (u32)(f.low >> 32));
  __st_32_h(a + 8, (u32)f.low);
}
inline floatx80 packx(u32 se, u64 sig) {
  floatx80 f;
  f.high = se;
  f.low = sig;
  return f;
}

// ---------------------------------------------------------------------------
// Arithmetic. Each helper computes one operation under the FPCR's rounding
// mode and the given precision (32, 64 or 80), stores the result in the
// result slot and returns SoftFloat's exception flags. They are separate
// functions so each SoftFloat operation is compiled (and allocated) once.

inline void initStatus(float_status &s, u32 rnd, u32 prec) {
  s.float_exception_flags = 0;
  s.float_rounding_mode = rnd;
  s.floatx80_rounding_precision = prec;
}
inline u32 result(floatx80 z, float_status &s) {
  stx(__csrr(CSR_SCRATCH_BASE) + FPRES, z);
  return s.float_exception_flags;
}

u32 op_add(floatx80 a, floatx80 b, u32 rnd, u32 prec) {
  float_status s; initStatus(s, rnd, prec);
  return result(floatx80_add(a, b, s), s);
}
u32 op_sub(floatx80 a, floatx80 b, u32 rnd, u32 prec) {
  float_status s; initStatus(s, rnd, prec);
  return result(floatx80_sub(a, b, s), s);
}
u32 op_mul(floatx80 a, floatx80 b, u32 rnd, u32 prec) {
  float_status s; initStatus(s, rnd, prec);
  return result(floatx80_mul(a, b, s), s);
}
u32 op_div(floatx80 a, floatx80 b, u32 rnd, u32 prec) {
  float_status s; initStatus(s, rnd, prec);
  return result(floatx80_div(a, b, s), s);
}
u32 op_sqrt(floatx80 a, u32 rnd, u32 prec) {
  float_status s; initStatus(s, rnd, prec);
  return result(floatx80_sqrt(a, s), s);
}
u32 op_sglmul(floatx80 a, floatx80 b, u32 rnd, u32 prec) {
  float_status s; initStatus(s, rnd, prec);
  return result(floatx80_sglmul(a, b, s), s);
}
u32 op_sgldiv(floatx80 a, floatx80 b, u32 rnd, u32 prec) {
  float_status s; initStatus(s, rnd, prec);
  return result(floatx80_sgldiv(a, b, s), s);
}
u32 op_cmp(floatx80 a, floatx80 b, u32 rnd, u32 prec) {
  float_status s; initStatus(s, rnd, prec);
  return result(floatx80_cmp(a, b, s), s);
}
// kind: 0 FMOVE, 1 FABS, 2 FNEG, 3 FTST
u32 op_mono(u32 kind, floatx80 a, u32 rnd, u32 prec) {
  float_status s; initStatus(s, rnd, prec);
  floatx80 z;
  switch (kind) {
    case 0: z = floatx80_move(a, s); break;
    case 1: z = floatx80_abs(a, s); break;
    case 2: z = floatx80_neg(a, s); break;
    default: z = floatx80_tst(a, s); break;
  }
  return result(z, s);
}

// ---------------------------------------------------------------------------
// Operand classes (Previous's softfloat.h definitions)

inline u32 isUnnormal(floatx80 a) {
  u32 e = a.high & 0x7fff;
  return e > 0 && e < 0x7fff && !(a.low >> 63);
}
inline u32 isDenormal(floatx80 a) {
  return (a.high & 0x7fff) == 0 && !(a.low >> 63) && (a.low << 1) != 0;
}
inline u32 isZero(floatx80 a) { return (a.high & 0x7fff) < 0x7fff && a.low == 0; }

// The 68040 has no denormal or unnormal arithmetic: an unnormal zero becomes
// a true zero, anything else is an unimplemented data type (`bad` is set).
inline floatx80 fixOperand(floatx80 a, u32 &bad) {
  if (isUnnormal(a) || isDenormal(a)) {
    if (isZero(a)) return packx(a.high & 0x8000, 0);
    bad = 1;
  }
  return a;
}

// Single and double to extended, keeping denormals as unnormalized values
// (Previous's float32/64_to_floatx80_allowunnormal).
inline floatx80 fromSingle(u32 w) {
  u32 se = (w >> 16) & 0x8000;
  u32 e = (w >> 23) & 0xff;
  u64 sig = (u64)(w & 0x7fffff) << 40;
  if (e == 0xff) return packx(se | 0x7fff, sig);
  if (e == 0) {
    if (sig == 0) return packx(se, 0);
    return packx(se | 0x3f81, sig);
  }
  return packx(se | (e + 0x3f80), sig | (1ull << 63));
}
inline floatx80 fromDouble(u32 hi, u32 lo) {
  u32 se = (hi >> 16) & 0x8000;
  u32 e = (hi >> 20) & 0x7ff;
  u64 frac = ((u64)(hi & 0xfffff) << 32) | lo;
  if (e == 0x7ff) return packx(se | 0x7fff, frac << 11);
  if (e == 0) {
    if (frac == 0) return packx(se, 0);
    return packx(se | 0x3c01, frac << 11);
  }
  return packx(se | (e + 0x3c00), (frac | (1ull << 52)) << 11);
}

// ---------------------------------------------------------------------------
// Effective addresses

// The extension word at byte offset `off` of the instruction.
inline u32 extw(u32 e01, u32 e23, u32 off) {
  if (off == 2) return e01 >> 16;
  if (off == 4) return e01 & 0xffff;
  if (off == 6) return e23 >> 16;
  if (off == 8) return e23 & 0xffff;
  return __ld_16u_gp(__rdg(GPC) + off);
}

// Brief-format index (Xn.W/L * scale + d8) from the extension word at `off`.
inline u32 briefIndex(u32 e01, u32 e23, u32 off) {
  u32 ext = extw(e01, e23, off);
  if (ext & 0x100) __brk(BRK_FULL_EXT);
  u32 x = __rdg(ext >> 12);
  if (!(ext & 0x800)) x = (s16)x;
  return (x << ((ext >> 9) & 3)) + (s8)ext;
}

// ---------------------------------------------------------------------------
// opclass 100/101 with a register EA: FMOVE FPcr,Dn/An and Dn/An,FPcr

inline void fmoveCtlReg(u32 op, u32 cmd) {
  u32 bits = cmd & 0x1c00;
  u32 r = op & 7;
  if ((op & 0x38) == 0) {                    // Dn: one register, none = FPIAR
    if (bits != 0 && bits != 0x1000 && bits != 0x0800 && bits != 0x0400) __gfault(11);
    if (cmd & 0x2000) {
      u32 v;
      if (bits == 0x1000) v = __csrr(CSR_FPCR);
      else if (bits == 0x0800) v = __csrr(CSR_FPSR);
      else v = __csrr(CSR_FPIAR);
      __wrg(r, v);
    } else {
      u32 v = __rdg(r);
      if (bits == 0x1000) __csrw(CSR_FPCR, v & 0xffff);
      else if (bits == 0x0800) __csrw(CSR_FPSR, v & 0x0ffffff8);
      else __csrw(CSR_FPIAR, v);
    }
  } else {                                   // An: FPIAR only
    if (bits != 0 && bits != 0x0400) __gfault(11);
    if (cmd & 0x2000) __wrg(A0 + r, __csrr(CSR_FPIAR));
    else __csrw(CSR_FPIAR, __rdg(A0 + r));
  }
}

// ---------------------------------------------------------------------------

// The opmodes a 68040 executes; everything else valid traps to the FPSP.
inline u32 implemented(u32 opm) {
  switch (opm) {
    case 0x00: case 0x04: case 0x18: case 0x1a: case 0x20: case 0x22: case 0x23: case 0x24:
    case 0x27: case 0x28: case 0x38: case 0x3a: case 0x40: case 0x41: case 0x44: case 0x45:
    case 0x58: case 0x5c: case 0x5a: case 0x5e: case 0x60: case 0x62: case 0x63: case 0x64:
    case 0x66: case 0x67: case 0x68: case 0x6c:
      return 1;
  }
  return 0;
}

// Opmodes with no instruction: F-line (vector 11), or illegal for 78-7F.
inline void checkOpmode(u32 opm) {
  switch (opm) {
    case 0x05: case 0x07: case 0x0b: case 0x13: case 0x17: case 0x1b: case 0x29: case 0x2a:
    case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f: case 0x39: case 0x3b: case 0x3c:
    case 0x3d: case 0x3e: case 0x3f: case 0x42: case 0x43: case 0x46: case 0x47: case 0x48:
    case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: case 0x50:
    case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x59:
    case 0x5b: case 0x5d: case 0x5f: case 0x61: case 0x65: case 0x69: case 0x6a: case 0x6b:
    case 0x6d: case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74:
    case 0x75: case 0x76: case 0x77:
      __gfault(11);
  }
  if (opm >= 0x78) __gfault(4);
}

entry void fpu_gen(void) {
  u32 op = __opword();
  u32 e01 = __ext01();
  u32 e23 = __ext23();
  u32 cmd = e01 >> 16;
  u32 opclass = cmd >> 13;
  u32 pc = __rdg(GPC);
  u32 len = 4;                               // instruction length, from the operand fetched

  if (opclass == 4 || opclass == 5) {
    if ((op & 0x30) != 0) __brk(BRK_OPCLASS);   // FMOVEM of control registers to/from memory
    fmoveCtlReg(op, cmd);
    __wrg(GPC, pc + 4);
    __gexit();
  }
  if (opclass != 0 && opclass != 2) __brk(BRK_OPCLASS);
  if ((cmd & 0xfc00) == 0x5c00) __brk(BRK_UNIMP_INSN);   // FMOVECR
  u32 opm = cmd & 0x7f;
  checkOpmode(opm);
  u32 impl = implemented(opm);

  // From here every exit leaves the exception byte cleared and FPIAR set.
  __csrw(CSR_FPSR, __csrr(CSR_FPSR) & 0x0fff00f8);
  __csrw(CSR_FPIAR, pc);

  // Source operand
  u32 bad = 0;                          // unimplemented data type
  floatx80 src;
  u32 size = (cmd >> 10) & 7;
  u32 mode = (op >> 3) & 7;
  u32 reg = op & 7;
  u32 newA = 0;                              // (An)+ / -(An): the address register's new value
  u32 wbA = 0;
  if (!(cmd & 0x4000)) {
    src = fixOperand(ldx(fpaddr((cmd >> 10) & 7)), bad);
  } else if (mode == 0) {
    u32 d = __rdg(reg);
    if (size == 0) src = int32_to_floatx80(d);
    else if (size == 4) src = int32_to_floatx80((s16)d);
    else if (size == 6) src = int32_to_floatx80((s8)d);
    else if (size == 1) src = fixOperand(fromSingle(d), bad);
    else {
      if (size != 3 && !impl) __brk(BRK_UNIMP_INSN);
      __gfault(11);                          // no Dn source of this size
    }
  } else if (mode == 1) {
    if (!impl) __brk(BRK_UNIMP_INSN);
    __gfault(11);
  } else {
    u32 bytes;                               // operand size in memory
    switch (size) {
      case 0: case 1: bytes = 4; break;
      case 2: case 3: bytes = 12; break;
      case 4: bytes = 2; break;
      case 5: bytes = 8; break;
      case 6: bytes = 1; break;
      default: bytes = 0; break;
    }
    u32 step = bytes;
    if (reg == 7 && bytes == 1) step = 2;    // A7 stays word aligned
    u32 a;
    u32 imm = 0;
    u32 an = __rdg(A0 + reg);
    switch (mode) {
      case 2: a = an; break;
      case 3: a = an; newA = an + step; wbA = 1; break;
      case 4: a = an - step; newA = a; wbA = 1; break;
      case 5: a = an + (s16)extw(e01, e23, 4); len = 6; break;
      case 6: a = an + briefIndex(e01, e23, 4); len = 6; break;
      default:
        switch (reg) {
          case 0: a = (s16)extw(e01, e23, 4); len = 6; break;
          case 1: a = (extw(e01, e23, 4) << 16) | extw(e01, e23, 6); len = 8; break;
          case 2: a = pc + 4 + (s16)extw(e01, e23, 4); len = 6; break;
          case 3: a = pc + 4 + briefIndex(e01, e23, 4); len = 6; break;
          case 4: a = pc + 4; imm = 1; len = 4 + (bytes == 1 ? 2 : bytes); break;
          default: __gfault(11);
        }
    }
    if (bytes == 0) __gfault(11);
    if (imm && bytes == 1) a = a + 1;        // #imm.B: the low byte of a word
    switch (size) {
      case 0:
        src = int32_to_floatx80(imm ? __ld_32_gp(a) : __ld_32_gd(a));
        break;
      case 1:
        src = fixOperand(fromSingle(imm ? __ld_32_gp(a) : __ld_32_gd(a)), bad);
        break;
      case 2: case 3: {
        u32 w0, w1, w2;
        if (imm) { w0 = __ld_32_gp(a); w1 = __ld_32_gp(a + 4); w2 = __ld_32_gp(a + 8); }
        else { w0 = __ld_32_gd(a); w1 = __ld_32_gd(a + 4); w2 = __ld_32_gd(a + 8); }
        if (size == 3) bad = 1;           // packed decimal
        src = fixOperand(packx(w0 >> 16, ((u64)w1 << 32) | w2), bad);
        break;
      }
      case 4:
        src = int32_to_floatx80((s16)(imm ? __ld_16u_gp(a) : __ld_16u_gd(a)));
        break;
      case 5: {
        u32 hi, lo;
        if (imm) { hi = __ld_32_gp(a); lo = __ld_32_gp(a + 4); }
        else { hi = __ld_32_gd(a); lo = __ld_32_gd(a + 4); }
        src = fixOperand(fromDouble(hi, lo), bad);
        break;
      }
      default:
        src = int32_to_floatx80((s8)(imm ? __ld_8u_gp(a) : __ld_8u_gd(a)));
        break;
    }
  }

  u32 n = (cmd >> 7) & 7;
  u32 dstA = fpaddr(n);
  floatx80 dst = ldx(dstA);
  if ((opm & 0x30) == 0x20 || opm == 0x38) dst = fixOperand(dst, bad);   // dyadic
  if (!impl) __brk(BRK_UNIMP_INSN);
  if (bad) __brk(BRK_UNIMP_DATA);

  // Rounding mode and precision from FPCR; FS*/FD* force single/double.
  u32 fpcr = __csrr(CSR_FPCR);
  u32 rnd;
  switch ((fpcr >> 4) & 3) {
    case 0: rnd = float_round_nearest_even; break;
    case 1: rnd = float_round_to_zero; break;
    case 2: rnd = float_round_down; break;
    default: rnd = float_round_up; break;
  }
  u32 prec;
  switch ((fpcr >> 6) & 3) {
    case 0: prec = 80; break;
    case 1: prec = 32; break;
    default: prec = 64; break;
  }
  if (opm & 0x40) prec = (opm & 4) ? 64 : 32;

  u32 flags;
  switch (opm) {
    case 0x00: case 0x40: case 0x44: flags = op_mono(0, src, rnd, prec); break;
    case 0x18: case 0x58: case 0x5c: flags = op_mono(1, src, rnd, prec); break;
    case 0x1a: case 0x5a: case 0x5e: flags = op_mono(2, src, rnd, prec); break;
    case 0x3a: flags = op_mono(3, src, rnd, prec); break;
    case 0x04: case 0x41: case 0x45: flags = op_sqrt(src, rnd, prec); break;
    case 0x20: case 0x60: case 0x64: flags = op_div(dst, src, rnd, prec); break;
    case 0x22: case 0x62: case 0x66: flags = op_add(dst, src, rnd, prec); break;
    case 0x23: case 0x63: case 0x67: flags = op_mul(dst, src, rnd, prec); break;
    case 0x24: flags = op_sgldiv(dst, src, rnd, prec); break;
    case 0x27: flags = op_sglmul(dst, src, rnd, prec); break;
    case 0x28: case 0x68: case 0x6c: flags = op_sub(dst, src, rnd, prec); break;
    default: flags = op_cmp(dst, src, rnd, prec); break;   // 0x38
  }
  floatx80 z = ldx(__csrr(CSR_SCRATCH_BASE) + FPRES);

  // FPSR: condition codes from the result, exception byte from the flags,
  // then the accrued byte.
  u32 fpsr = __csrr(CSR_FPSR) & 0x00fffff8;
  if (z.high & 0x8000) fpsr |= FPSR_CC_N;
  if ((z.high & 0x7fff) == 0x7fff && (z.low << 1) != 0) fpsr |= FPSR_CC_NAN;
  else if (isZero(z)) fpsr |= FPSR_CC_Z;
  else if ((z.high & 0x7fff) == 0x7fff) fpsr |= FPSR_CC_I;
  if (flags & float_flag_signaling) fpsr |= FPSR_SNAN;
  if (flags & float_flag_invalid) fpsr |= FPSR_OPERR;
  if (flags & float_flag_divbyzero) fpsr |= FPSR_DZ;
  if (flags & float_flag_overflow) fpsr |= FPSR_OVFL;
  if (flags & float_flag_underflow) fpsr |= FPSR_UNFL;
  if (flags & float_flag_inexact) fpsr |= FPSR_INEX2;
  if (flags & float_flag_decimal) fpsr |= FPSR_INEX1;
  if (fpsr & FPSR_OVFL) fpsr |= FPSR_AE_OVFL;
  if (fpsr & (FPSR_OVFL | FPSR_INEX2 | FPSR_INEX1)) fpsr |= FPSR_AE_INEX;
  if (fpsr & (FPSR_SNAN | FPSR_OPERR)) fpsr |= FPSR_AE_IOP;
  if ((fpsr & FPSR_UNFL) && (fpsr & FPSR_INEX2)) fpsr |= FPSR_AE_UNFL;
  if (fpsr & FPSR_DZ) fpsr |= FPSR_AE_DZ;

  // An enabled exception, or overflow/underflow (which a 68040 always hands
  // to the FPSP), needs an FSAVE frame: not done yet.
  if ((fpsr & fpcr & 0xff00) || (fpsr & (FPSR_OVFL | FPSR_UNFL))) __brk(BRK_ARITH);

  // commit
  __csrw(CSR_FPSR, fpsr);
  if (opm != 0x38 && opm != 0x3a) stx(dstA, z);
  if (wbA) __wrg(A0 + reg, newA);
  __wrg(GPC, pc + len);
  __gexit();
}
