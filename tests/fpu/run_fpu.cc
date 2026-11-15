// Differential test of millicode/fpu.c: random FPU-gen instructions and
// machine states, each run by Previous's own FPU code (prev_fpu.c, which
// links cpu/fpp.c) and as compiled millicode on the uisa ISS
// (compiler/tests/iss.h). Data and integer registers, FP registers, FPCR,
// FPSR, FPIAR, the next PC and the exception taken must agree.
//
// Cases the millicode does not handle yet stop with a brk code; those must
// be cases where Previous takes the matching trap (unimplemented instruction
// = vector 11 with an FSAVE frame, unimplemented data type = 55, arithmetic
// = 48-54 or left pending), and are counted rather than compared.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>

static std::map<uint32_t, uint8_t> mem;   // guest memory: written bytes, else a hash of the address
static inline uint8_t membyte(uint32_t a) {
  auto it = mem.find(a);
  if (it != mem.end()) return it->second;
  uint32_t h = a * 0x9e3779b1u;
  return (uint8_t)(h >> 24 ^ h >> 11);
}
static int guestStores;
#define ISS_GUEST_STORE(a, n, v) (guestStores++)
#include "iss.h"
#include "prev_fpu.h"

extern "C" uint8_t ref_membyte(uint32_t a) { return membyte(a); }

struct Entry { const char *name; int start; };
#include "fpu_image.inc"   // FPU_IMAGE[], FPU_WORDS, FPU_ENTRIES[]

static const uint32_t SCRATCH = 0x10000;

static std::mt19937_64 rng(20261115);
static uint64_t r64() { return rng(); }
static uint32_t r32() { return (uint32_t)rng(); }

static void put(uint32_t a, int n, uint64_t v) {
  for (int i = 0; i < n; i++) mem[a + i] = (uint8_t)(v >> (8 * (n - 1 - i)));
}

// extended operands aimed at the edge cases (as tests/softfloat/run_fx80.cc)
static void pickX(uint32_t &se, uint64_t &sig) {
  static const int exps[] = {0, 1, 2, 0x7FFE, 0x7FFF, 0x3FFF, 0x3FFE, 0x4000, 0x3F80, 0x3F81, 0x3F7F,
                             0x3C00, 0x3C01, 0x3BFF, 0x403E, 0x401E, 0x407F, 0x43FE, 0x47FE};
  int e;
  switch (r64() % 6) {
    case 0: e = exps[r64() % (sizeof exps / sizeof exps[0])]; break;
    case 1: case 2: e = 0x3FFF + (int)(r64() % 80) - 40; break;
    default: e = (int)(r64() & 0x7FFF); break;
  }
  switch (r64() % 10) {
    case 0: sig = 0; break;
    case 1: sig = 0x8000000000000000ull; break;
    case 2: sig = r64() >> (r64() % 64); break;                                   // unnormal / denormal
    case 3: sig = 0xC000000000000000ull | (r64() >> (2 + r64() % 62)); break;     // quiet NaN at 7FFF
    case 4: sig = 0x8000000000000000ull | (r64() >> (2 + r64() % 62)); break;     // signaling NaN
    case 5: sig = 0x8000000000000000ull | (r64() & 0xFFFFFF0000000000ull); break;  // exact in single
    default: sig = r64() | 0x8000000000000000ull; break;
  }
  se = (uint32_t)((r64() & 1) << 15 | e);
}
static uint32_t pickS() {
  static const uint32_t e[] = {0, 1, 0x7f, 0x80, 0xfe, 0xff, 0x7e, 0x96};
  uint32_t ex = r64() % 3 ? (uint32_t)(r64() & 0xff) : e[r64() % 8];
  uint32_t f = r64() % 4 == 0 ? 0 : (r32() & 0x7fffff) >> (r64() % 23);
  return (r32() & 0x80000000) | ex << 23 | f;
}
static uint64_t pickD() {
  static const uint64_t e[] = {0, 1, 0x3ff, 0x400, 0x7fe, 0x7ff, 0x3fe, 0x433};
  uint64_t ex = r64() % 3 ? (r64() & 0x7ff) : e[r64() % 8];
  uint64_t f = r64() % 4 == 0 ? 0 : (r64() & 0xfffffffffffffull) >> (r64() % 52);
  return (r64() & 0x8000000000000000ull) | ex << 52 | f;
}
static uint32_t pickI() {
  static const uint32_t sp[] = {0, 1, 0xffffffff, 2, 0x7fffffff, 0x80000000, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff};
  return r64() % 3 ? sp[r64() % 12] : r32() >> (r64() % 32);
}

static const int implOps[] = {0x00, 0x04, 0x18, 0x1a, 0x20, 0x22, 0x23, 0x24, 0x27, 0x28, 0x38, 0x3a, 0x40, 0x41,
                              0x44, 0x45, 0x58, 0x5c, 0x5a, 0x5e, 0x60, 0x62, 0x63, 0x64, 0x66, 0x67, 0x68, 0x6c};
static const int sizeBytes[8] = {4, 4, 12, 12, 2, 8, 1, 0};

// Build one instruction at `pc` and a state for it; returns the opword.
static uint16_t gen(uint32_t pc, ref_state &s) {
  for (int i = 0; i < 16; i++) s.r[i] = r64() % 4 ? pickI() : r32();
  for (int i = 0; i < 8; i++) {
    uint32_t se; uint64_t sig;
    pickX(se, sig);
    s.fp[i][0] = se << 16; s.fp[i][1] = (uint32_t)(sig >> 32); s.fp[i][2] = (uint32_t)sig;
  }
  s.fpcr = (uint32_t)(r64() % 8) << 4;                    // rounding mode and precision
  if (r64() % 8 == 0) s.fpcr |= (uint32_t)(r64() & 0xff) << 8;   // some exceptions enabled
  if (r64() % 16 == 0) s.fpcr = r32() & 0xffff;
  s.fpsr = r32() & 0x0ffffff8;
  s.fpiar = r32();
  s.pc = pc;

  uint16_t cmd, op = 0xF200;
  uint32_t w = (uint32_t)(r64() % 100);
  int opm = w < 85 ? implOps[r64() % (sizeof implOps / sizeof implOps[0])] : (int)(r64() & 0x7f);
  int dst = (int)(r64() & 7);
  if (w < 6) {                                            // FMOVE FPcr, Dn/An
    static const uint16_t sel[] = {0x1000, 0x0800, 0x0400, 0, 0x1800, 0x1c00};
    cmd = (uint16_t)(0x8000 | (r64() & 1) << 13 | sel[r64() % 6]);
    op |= (uint16_t)(r64() % 16);
    put(pc, 2, op); put(pc + 2, 2, cmd);
    return op;
  }
  if (r64() % 3 == 0) {                                   // FPm, FPn
    cmd = (uint16_t)((r64() & 7) << 10 | dst << 7 | opm);
    if (r64() % 64 == 0) cmd = 0x5c00 | (uint16_t)(dst << 7) | (uint16_t)(r64() & 0x7f);   // FMOVECR
    put(pc, 2, op); put(pc + 2, 2, cmd);
    return op;
  }
  int size = r64() % 16 == 0 ? (int)(r64() & 7) : (int)"\0\1\2\4\5\6\0\1\2\5"[r64() % 10];
  cmd = (uint16_t)(0x4000 | size << 10 | dst << 7 | opm);
  int mode = (int)(r64() % 8), reg = (int)(r64() % 8);
  if (mode == 7) reg = (int)(r64() % 6);
  if (r64() % 8 == 0) { mode = 7; reg = 4; }               // more immediates
  op |= (uint16_t)(mode << 3 | reg);
  put(pc, 2, op); put(pc + 2, 2, cmd);
  int nb = sizeBytes[size];
  int step = (reg == 7 && nb == 1) ? 2 : nb;
  uint32_t base = 0x40000 + (r32() & 0xfffe);
  uint32_t ea = 0;
  uint32_t &an = s.r[8 + reg];
  switch (mode) {
    case 2: an = base; ea = base; break;
    case 3: an = base; ea = base; break;
    case 4: an = base; ea = base - step; break;
    case 5: { int16_t d = (int16_t)r32(); put(pc + 4, 2, (uint16_t)d); an = base - d; ea = base; break; }
    case 6: case 7: {
      uint16_t ext = (uint16_t)(r32() & 0xfeff);           // brief format
      if (mode == 6 || reg == 3) {
        if (mode == 6) an = base;
        put(pc + 4, 2, ext);
        uint32_t x = s.r[(ext >> 12) & 15];
        if (!(ext & 0x800)) x = (uint32_t)(int16_t)x;
        ea = (mode == 6 ? an : pc + 4) + (int8_t)ext + (x << ((ext >> 9) & 3));
      } else if (reg == 0) { put(pc + 4, 2, ext); ea = (uint32_t)(int16_t)ext; }
      else if (reg == 1) { ea = base; put(pc + 4, 4, ea); }
      else if (reg == 2) { put(pc + 4, 2, ext); ea = pc + 4 + (int16_t)ext; }
      else if (reg == 4) { ea = pc + 4 + (nb == 1 ? 1 : 0); if (nb == 1) mem[pc + 4] = (uint8_t)r32(); }
      break;
    }
    default: break;
  }
  if (mode >= 2 && nb) {
    switch (size) {
      case 0: put(ea, 4, pickI()); break;
      case 1: put(ea, 4, pickS()); break;
      case 2: case 3: {
        uint32_t se; uint64_t sig;
        pickX(se, sig);
        put(ea, 4, (uint64_t)se << 16 | (r64() % 8 == 0 ? (r32() & 0xffff) : 0));
        put(ea + 4, 8, sig);
        break;
      }
      case 4: put(ea, 2, pickI()); break;
      case 5: put(ea, 8, pickD()); break;
      case 6: put(ea, 1, pickI()); break;
    }
  } else if (mode == 0) {
    if (size == 1) s.r[reg] = pickS();
  }
  return op;
}

int main(int argc, char **argv) {
  long iters = argc > 1 ? atol(argv[1]) : 100000;
  int start = -1;
  for (const Entry &e : FPU_ENTRIES)
    if (!strcmp(e.name, "fpu_gen")) start = e.start;
  if (start < 0) { fprintf(stderr, "run_fpu: no fpu_gen entry\n"); return 2; }
  ref_init();
  long ok = 0, traps = 0, failures = 0, notyet = 0;
  std::map<std::string, long> cls;
  for (long it = 0; it < iters; it++) {
    mem.clear();
    uint32_t pc = 0x1000 + (r32() & 0xfffe);
    ref_state s;
    memset(&s, 0, sizeof s);
    uint16_t op = gen(pc, s);
    ref_state in = s;

    Machine m;
    for (int k = 5; k < 32; k++) m.R[k] = r32();         // garbage in the scratch registers
    for (int i = 0; i < 16; i++) m.R[32 + i] = in.r[i];
    m.R[48] = pc;
    m.R[1] = op;
    m.R[2] = (uint32_t)membyte(pc + 2) << 24 | membyte(pc + 3) << 16 | membyte(pc + 4) << 8 | membyte(pc + 5);
    m.R[3] = (uint32_t)membyte(pc + 6) << 24 | membyte(pc + 7) << 16 | membyte(pc + 8) << 8 | membyte(pc + 9);
    m.R[4] = pc + 4;
    m.csr[0x47] = SCRATCH;
    m.csr[0x20] = in.fpcr; m.csr[0x21] = in.fpsr; m.csr[0x22] = in.fpiar;
    for (int i = 0; i < 8; i++)
      for (int k = 0; k < 3; k++) m.hstore(SCRATCH + 12 * i + 4 * k, 4, in.fp[i][k]);
    guestStores = 0;
    m.run(FPU_IMAGE, FPU_WORDS, start);

    ref_run(op, &s);

    uint16_t cmd = (uint16_t)(membyte(pc + 2) << 8 | membyte(pc + 3));
    char why[256] = "";
    if (!m.err.empty()) {
      snprintf(why, sizeof why, "ISS: %s", m.err.c_str());
    } else if (m.exitv >= 1000) {
      int b = m.exitv - 1000;
      bool agree = (b == 20 && s.vector == 11 && s.fpu_exp_state) || (b == 21 && s.vector == 55) ||
                   (b == 22 && ((s.vector >= 48 && s.vector <= 54) || s.fp_exp_pend)) || b == 23;
      if (!agree) snprintf(why, sizeof why, "brk %d, Previous: vector %d (fsave state %u, pending %u)", b, s.vector,
                           s.fpu_exp_state, s.fp_exp_pend);
      else { if (b == 23) notyet++; else traps++; cls["brk " + std::to_string(b)]++; }
    } else {
      int mv = m.exitv < 0 ? 0 : m.exitv;
      if (mv != s.vector || (mv == 0 && s.fp_exp_pend))
        snprintf(why, sizeof why, "vector %d, Previous %d (pending %u, fsave state %u)", mv, s.vector, s.fp_exp_pend,
                 s.fpu_exp_state);
      for (int i = 0; i < 16 && !*why; i++)
        if (m.R[32 + i] != s.r[i]) snprintf(why, sizeof why, "%c%d %08x, Previous %08x", i < 8 ? 'D' : 'A', i & 7, m.R[32 + i], s.r[i]);
      for (int i = 0; i < 8 && !*why; i++) {
        uint32_t w0 = m.hload(SCRATCH + 12 * i, 4) & 0xffff0000, w1 = m.hload(SCRATCH + 12 * i + 4, 4), w2 = m.hload(SCRATCH + 12 * i + 8, 4);
        if (w0 != (s.fp[i][0] & 0xffff0000) || w1 != s.fp[i][1] || w2 != s.fp[i][2])
          snprintf(why, sizeof why, "FP%d %04x %08x%08x, Previous %04x %08x%08x", i, w0 >> 16, w1, w2, s.fp[i][0] >> 16, s.fp[i][1], s.fp[i][2]);
      }
      if (!*why && m.csr[0x20] != s.fpcr) snprintf(why, sizeof why, "FPCR %08x, Previous %08x", m.csr[0x20], s.fpcr);
      if (!*why && m.csr[0x21] != s.fpsr) snprintf(why, sizeof why, "FPSR %08x, Previous %08x", m.csr[0x21], s.fpsr);
      if (!*why && m.csr[0x22] != s.fpiar) snprintf(why, sizeof why, "FPIAR %08x, Previous %08x", m.csr[0x22], s.fpiar);
      if (!*why && mv == 0 && m.R[48] != s.pc) snprintf(why, sizeof why, "next PC %08x, Previous %08x", m.R[48], s.pc);
      if (!*why && (guestStores || s.nstores)) snprintf(why, sizeof why, "stores %d, Previous %d", guestStores, s.nstores);
      if (!*why) { ok++; cls[mv ? "vector " + std::to_string(mv) : "done"]++; }
    }
    if (*why) {
      if (++failures <= 20) {
        printf("FAIL %04x %04x ext %08x: %s\n", op, cmd, m.R[3], why);
        printf("     fpcr %08x fpsr %08x src(FP%d) %08x %08x%08x dst(FP%d) %08x %08x%08x\n", in.fpcr, in.fpsr,
               (cmd >> 10) & 7, in.fp[(cmd >> 10) & 7][0], in.fp[(cmd >> 10) & 7][1], in.fp[(cmd >> 10) & 7][2],
               (cmd >> 7) & 7, in.fp[(cmd >> 7) & 7][0], in.fp[(cmd >> 7) & 7][1], in.fp[(cmd >> 7) & 7][2]);
      }
    }
  }
  printf("fpu: %ld cases: %ld match (", iters, ok);
  bool first = true;
  for (auto &c : cls) { printf("%s%s %ld", first ? "" : ", ", c.first.c_str(), c.second); first = false; }
  printf("), %ld failures\n", failures);
  return failures ? 1 : 0;
}
