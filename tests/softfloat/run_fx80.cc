// Differential test of millicode/softfloat/softfloat.h: each operation is run
// as compiled millicode on the uisa ISS (compiler/tests/iss.h) and by
// Previous's own SoftFloat (previous/src/softfloat, linked in), with the
// 68040's special flags, on random operands aimed at the edge cases, in
// every rounding mode and precision. The floatx80 result (or int32) and the
// exception flags must agree bit for bit.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <utility>
#include <string>

static inline uint8_t membyte(uint32_t a) { return (uint8_t)a; }   // the ops never load
#include "iss.h"

extern "C" {
#include "softfloat.h"
}

struct Entry { const char *name; int start; };
#include "fx80_image.inc"   // FX80_IMAGE[], FX80_WORDS, FX80_ENTRIES[]

enum Op { ADD, SUB, MUL, DIV, SQRT, MOVE, FROM_I32, TO_I32, NOPS };
static const char *opName[NOPS] = {"fx80_add", "fx80_sub", "fx80_mul", "fx80_div",
                                   "fx80_sqrt", "fx80_move", "fx80_from_i32", "fx80_to_i32"};

static std::mt19937_64 rng(20261114);
static uint64_t r64() { return rng(); }

// sign/exponent and significand aimed at the interesting cases
static floatx80 pickX(const floatx80 *near) {
  floatx80 x;
  static const int exps[] = {0, 1, 2, 0x7FFE, 0x7FFF, 0x3FFF, 0x3FFE, 0x4000, 0x3F80, 0x3F81, 0x3F7F,
                             0x3C00, 0x3C01, 0x3BFF, 0x403E, 0x401E, 0x407F, 0x43FE, 0x47FE, 0x3F6A, 0x3BCD};
  int e;
  switch (r64() % 7) {
    case 0: e = exps[r64() % (sizeof exps / sizeof exps[0])]; break;
    case 1: e = near ? ((near->high & 0x7FFF) + (int)(r64() % 140) - 70) & 0x7FFF : (int)(r64() & 0x7FFF); break;
    case 5:   // 60..130 below the other operand: the 128-bit jamming shifts
      e = near ? ((near->high & 0x7FFF) - 60 - (int)(r64() % 71)) & 0x7FFF : (int)(r64() & 0x7FFF);
      break;
    case 2: e = 0x3FFF + (int)(r64() % 200) - 100; break;
    default: e = (int)(r64() & 0x7FFF); break;
  }
  uint64_t s;
  switch (r64() % 11) {
    case 0: s = 0; break;
    case 1: s = 0x8000000000000000ull; break;
    case 2: s = ~0ull; break;
    case 3: s = r64() >> (r64() % 64); break;                         // unnormal / denormal
    case 4: s = 0xC000000000000000ull | (r64() >> (2 + r64() % 62)); break;   // quiet NaN when e = 0x7FFF
    case 5: s = 0x8000000000000000ull | (r64() >> (2 + r64() % 62)); break;   // signaling NaN
    case 6: s = 0x8000000000000000ull | (r64() & ((1ull << (r64() % 64)) - 1)) | (r64() & 0xFFFFFF0000000000ull); break;
    case 7: s = near ? near->low ^ (1ull << (r64() % 64)) : r64() | 0x8000000000000000ull; break;   // cancellation
    case 8: s = r64() & ((1ull << (1 + r64() % 12)) - 1); break;      // only a few low bits
    default: s = r64() | 0x8000000000000000ull; break;
  }
  x.high = (uint16_t)((r64() & 1) << 15 | e);
  x.low = s;
  return x;
}

static int32_t pickI32() {
  static const int32_t sp[] = {0, 1, -1, 2, 0x7FFFFFFF, (int32_t)0x80000000, 0x40000000, -0x40000000, 3, 1000000};
  if (r64() % 3 == 0) return sp[r64() % 10];
  return (int32_t)(r64() >> (r64() % 64));
}

int main(int argc, char **argv) {
  int iters = argc > 1 ? atoi(argv[1]) : 100000;
  int fails = 0;
  float_status fs;
  memset(&fs, 0, sizeof fs);
  set_special_flags(cmp_signed_nan, &fs);   // fp_init_softfloat(68040)
  set_float_detect_tininess(float_tininess_before_rounding, &fs);
  static const int precs[3] = {80, 64, 32};
  for (int op = 0; op < NOPS; op++) {
    int start = -1;
    for (const Entry &e : FX80_ENTRIES)
      if (!strcmp(e.name, opName[op])) start = e.start;
    if (start < 0) {
      printf("FAIL %s: no entry point\n", opName[op]);
      return 1;
    }
    int ofail = 0;
    unsigned maxSteps = 0;
    for (int it = 0; it < iters && ofail < 5; it++) {
      floatx80 a = pickX(nullptr), b = pickX(&a);
      if (r64() % 16 == 0) {   // a denormal against a small exponent: the 64..127-bit jamming shifts
        a.high = (uint16_t)((a.high & 0x8000) | (60 + r64() % 75));
        b.high &= 0x8000;
        b.low = r64() >> (r64() % 64);
      }
      if (r64() % 2 && (op == ADD || op == SUB || op == MUL || op == DIV) && r64() % 8 == 0) std::swap(a, b);
      int32_t ia = pickI32();
      int mode = (int)(r64() % 4), prec = precs[r64() % 3];
      // Previous
      fs.float_exception_flags = 0;
      fs.float_rounding_mode = mode;
      fs.floatx80_rounding_precision = prec;
      floatx80 z{};
      int32_t zi = 0;
      switch (op) {
        case ADD: z = floatx80_add(a, b, &fs); break;
        case SUB: z = floatx80_sub(a, b, &fs); break;
        case MUL: z = floatx80_mul(a, b, &fs); break;
        case DIV: z = floatx80_div(a, b, &fs); break;
        case SQRT: z = floatx80_sqrt(a, &fs); break;
        case MOVE: z = floatx80_move(a, &fs); break;
        case FROM_I32: z = int32_to_floatx80(ia); break;
        case TO_I32: zi = floatx80_to_int32(a, &fs); break;
      }
      // millicode
      Machine m;
      m.R[32 + 0] = op == FROM_I32 ? (uint32_t)ia : a.high;
      m.R[32 + 1] = (uint32_t)(a.low >> 32);
      m.R[32 + 2] = (uint32_t)a.low;
      m.R[32 + 3] = b.high;
      m.R[32 + 4] = (uint32_t)(b.low >> 32);
      m.R[32 + 5] = (uint32_t)b.low;
      m.R[32 + 6] = mode;
      m.R[32 + 7] = prec;
      for (int k = 5; k < 32; k++) m.R[k] = (uint32_t)r64();   // garbage in the scratch registers
      m.run(FX80_IMAGE, FX80_WORDS, start);
      if (m.steps > maxSteps) maxSteps = m.steps;
      char why[200] = "";
      uint32_t mflags = m.R[32 + 3];
      if (!m.err.empty()) snprintf(why, sizeof why, "%s", m.err.c_str());
      else if (m.exitv != -1) snprintf(why, sizeof why, "exit %d", m.exitv);
      else if (op == TO_I32 ? m.R[32] != (uint32_t)zi
                            : (m.R[32] != z.high || m.R[33] != (uint32_t)(z.low >> 32) || m.R[34] != (uint32_t)z.low))
        snprintf(why, sizeof why, "result %04x %08x%08x, Previous %04x %016llx", m.R[32] & 0xffff, m.R[33], m.R[34],
                 op == TO_I32 ? (unsigned)zi : z.high, (unsigned long long)z.low);
      else if (mflags != fs.float_exception_flags)
        snprintf(why, sizeof why, "flags %02x, Previous %02x", mflags, fs.float_exception_flags);
      if (why[0]) {
        printf("FAIL %s mode %d prec %d: a %04x %016llx b %04x %016llx i %d: %s\n", opName[op], mode, prec, a.high,
               (unsigned long long)a.low, b.high, (unsigned long long)b.low, ia, why);
        ofail++;
      }
    }
    printf("softfloat %-14s %s (%d cases, up to %u steps)\n", opName[op], ofail ? "FAILED" : "ok", iters, maxSteps);
    fails += ofail;
  }
  printf("test_fx80: %d failures\n", fails);
  return fails ? 1 : 0;
}
