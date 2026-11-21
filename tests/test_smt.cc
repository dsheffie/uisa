// The symbolic uop semantics (build/uisa_smt.inc, from tools/gen_smt.py)
// against the concrete ones: every base op through the execution core's
// Machine::step, every guest op family at each size through gop_eval, on
// random and boundary inputs. z3 folds the symbolic results of constant
// inputs to numerals, which must equal the concrete results.
#include <cstdio>
#include <cstring>
#include <vector>

#include "uisa_machine.h"
#include "uisa_symsem.h"

using namespace uisa;
using namespace uisa_model;

static uint32_t rng = 0x2468ace1;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint32_t rnd_val() {
  switch (rnd() % 7) {
  case 0: return rnd() % 16;
  case 1: return 0u - (rnd() % 16);
  case 2: return 0x80000000u + (rnd() % 4) - 2;
  case 3: return rnd() >> (rnd() % 32);
  case 4: return (rnd() % 2) ? 0x7fffu + (rnd() % 3) - 1 : 0xffu + (rnd() % 3) - 1;
  default: return rnd();
  }
}

static uint64_t num(const Sym &s) {
  z3::expr e = s.e.simplify();
  if (!e.is_numeral()) { printf("non-constant result: %s\n", e.to_string().c_str()); return ~0ull; }
  return e.get_numeral_uint64();
}

struct NullMem : Mem {
  bool ifetch(uint32_t, int, uint32_t &v) override { v = 0; return true; }
  bool load(int, uint32_t, int, uint32_t &v) override { v = 0; return true; }
  void store(int, uint32_t, int, uint32_t) override {}
  uint32_t probe(int, uint32_t, bool) override { return 0; }
};

struct TestSem : SymSem {
  Sym csr[256], greg[32], gnpc, gnpc_set;
  std::vector<Sym> cstack;
  static int idx(const Sym &i, int n) {
    z3::expr e = usym::conv(i, usym::U32).e.simplify();
    return e.is_numeral() ? (int)(e.get_numeral_uint64() % n) : -1;
  }
  Sym CSR_READ(const Sym &i) override { return csr[idx(i, 256)]; }
  void CSR_WRITE(const Sym &i, const Sym &v) override { csr[idx(i, 256)] = v; }
  Sym GREG_READ(const Sym &i) override { return greg[idx(i, 32)]; }
  void GREG_WRITE(const Sym &i, const Sym &v) override { greg[idx(i, 32)] = v; }
  void CSTACK_PUSH(const Sym &v) override { cstack.push_back(v); }
  Sym CSTACK_POP() override { Sym v = cstack.back(); cstack.pop_back(); return v; }
  void GNPC_SET(const Sym &v) override { gnpc = v; gnpc_set = Sym(true); }
  void GSTORE(int, const Sym &, int, const Sym &) override {}
};

static int fails = 0;
static void check(const char *what, const char *name, uint64_t sym, uint64_t ref, uint32_t a, uint32_t b) {
  if (sym == ref) return;
  if (fails++ < 20)
    printf("FAIL %s %s: symbolic %llx concrete %llx (a %08x b %08x)\n", name, what, (unsigned long long)sym,
           (unsigned long long)ref, a, b);
}

int main(int argc, char **argv) {
  int trials = argc > 1 ? atoi(argv[1]) : 200;
  int nbase = 0, nguest = 0;
  for (unsigned op = 1; op < UISA_NUM_OPS; op++) {
    const opinfo &o = OPS[op];
    if (o.is_load || o.is_store || o.is_probe || o.gform != GF_NONE) continue;
    {   // does it have semantics?
      TestSem t;
      t.cstack.push_back(Sym(0u));
      if (!t.base(op)) continue;
    }
    nbase++;
    for (int k = 0; k < trials; k++) {
      uint32_t a = rnd_val(), b2 = rnd_val(), old = rnd_val(), imm = rnd() % 4 ? rnd() % 64 : rnd_val();
      if (op == UISA_RDG || op == UISA_WRG) a %= 18;   // 18-31 is a millicode fault (the models' greg hooks)
      uint32_t pc = (rnd() % 4096) * 4, gpc = rnd() & ~1u, ret = (rnd() % 4096) * 4;
      bool fl[5], uin = rnd() & 1;
      for (bool &f : fl) f = rnd() & 1;
      uint32_t csr[256], greg[32];
      for (uint32_t &c : csr) c = rnd_val();
      for (uint32_t &g : greg) g = rnd_val();
      // concrete: the execution core
      NullMem nm;
      Machine m;
      m.mem = &nm;
      memset(m.R, 0, sizeof m.R);
      memcpy(m.csr, csr, sizeof csr);
      for (int i = 0; i < 32; i++) m.R[32 + i] = greg[i];
      m.R[1] = a; m.R[2] = b2; m.R[3] = old;
      m.X = fl[0]; m.N = fl[1]; m.Z = fl[2]; m.V = fl[3]; m.C = fl[4];
      m.U = uin;
      m.gpc_insn = gpc; m.gnpc = 0; m.gnpc_set = false;
      m.cur_pc = pc; m.next_pc = pc + 4;
      m.cstack.push_back(ret);
      uisa_fuop u = {(uint16_t)op, 3, 1, 2, 0, 0, (int32_t)imm};
      m.step(u);
      // symbolic
      TestSem t;
      for (int i = 0; i < 256; i++) t.csr[i] = Sym(csr[i]);
      for (int i = 0; i < 32; i++) t.greg[i] = Sym(greg[i]);
      uint32_t bv = o.reads_rs2 ? b2 : imm;
      t.a = Sym(a); t.b = Sym(bv); t.IMM = Sym(imm); t.OLD = Sym(old); t.r = Sym(0u);
      t.pc = Sym(pc); t.npc = Sym(pc + 4); t.GPC_INSN = Sym(gpc);
      t.xin = t.X = Sym(fl[0]); t.nin = t.N = Sym(fl[1]); t.zin = t.Z = Sym(fl[2]);
      t.vin = t.V = Sym(fl[3]); t.cin = t.C = Sym(fl[4]);
      t.uin = t.U = Sym(uin);
      t.gnpc = Sym(0u); t.gnpc_set = Sym(false);
      t.cstack.push_back(Sym(ret));
      t.base(op);
      const char *nm_ = o.mnem;
      if (o.writes_rd) check("r", nm_, num(t.r), m.R[3], a, bv);
      check("npc", nm_, num(t.npc), m.next_pc, a, bv);
      check("X", nm_, num(t.X), m.X, a, bv); check("N", nm_, num(t.N), m.N, a, bv);
      check("Z", nm_, num(t.Z), m.Z, a, bv); check("V", nm_, num(t.V), m.V, a, bv);
      check("C", nm_, num(t.C), m.C, a, bv);
      check("U", nm_, num(t.U), m.U, a, bv);
      for (int i = 0; i < 256; i++) check("csr", nm_, num(t.csr[i]), m.csr[i], a, bv);
      for (int i = 0; i < 32; i++) check("greg", nm_, num(t.greg[i]), m.R[32 + i], a, bv);
      check("gnpc_set", nm_, num(t.gnpc_set), m.gnpc_set, a, bv);
      if (m.gnpc_set) check("gnpc", nm_, num(t.gnpc), m.gnpc, a, bv);
      check("cstack", nm_, t.cstack.size(), m.cstack.size(), a, bv);
    }
  }
  // guest ops: every family at every size
  for (unsigned fn = 0; fn < 64; fn++) {
    for (unsigned n : {8u, 16u, 32u}) {
      for (int k = 0; k < trials; k++) {
        uint32_t a = rnd_val(), b = rnd() % 3 ? rnd_val() : rnd() % 70;
        bool fl[5];
        for (bool &f : fl) f = rnd() & 1;
        gop_out g = gop_eval(fn, n, a, b, fl[0], fl[1], fl[2], fl[3], fl[4]);
        SymGop s;
        sym_gop_eval(fn, n, Sym(a), Sym(b), Sym(fl[0]), Sym(fl[1]), Sym(fl[2]), Sym(fl[3]), Sym(fl[4]), s);
        char name[32];
        snprintf(name, sizeof name, "gop%u.%u", fn, n);
        check("r", name, num(s.r), g.r, a, b);
        check("N", name, num(s.N), g.N, a, b); check("Z", name, num(s.Z), g.Z, a, b);
        check("V", name, num(s.V), g.V, a, b); check("C", name, num(s.C), g.C, a, b);
        check("X", name, num(s.X), g.X, a, b); check("T", name, num(s.T), g.T, a, b);
        if (k == 0 && n == 8) nguest++;
      }
    }
  }
  // the probe success rule (spec probe_ok) that the .us/.ua probes write to
  // u: symbolic against the C++ PROBE_OK, on random MMUSR values and every
  // combination of the B, S, W, T and R bits
  int nprobe = 0;
  for (int k = 0; k < trials * 4 + 64; k++) {
    uint32_t mmusr = k < 64 ? (uint32_t)(((k & 32) ? 0x800 : 0) | ((k & 16) ? 0x80 : 0) | ((k & 8) ? 4 : 0) |
                                         (k & 3)) | ((k & 4) ? 0x1000u : 0)
                            : rnd();
    if (k >= 64 && (k & 7) == 0) mmusr = 0;     // translation-off case: MMUSR 0
    for (int wu = 0; wu < 8; wu++) {
      bool w = wu & 1, user = wu & 2, tce = wu & 4;
      bool ref = PROBE_OK(mmusr, w, user, tce);
      check("probe_ok", "PROBE_OK", num(sym_probe_ok(Sym(mmusr), Sym(w), Sym(user), Sym(tce))), ref, mmusr, wu);
      nprobe++;
    }
  }
  // spot checks of the rule itself: T or R, not B; probe.w needs W clear; user needs S clear
  check("probe_ok", "unmapped", PROBE_OK(0, false, false, true), 0, 0, 0);
  check("probe_ok", "resident", PROBE_OK(1, true, true, true), 1, 1, 3);
  check("probe_ok", "transparent", PROBE_OK(2, false, false, true), 1, 2, 0);
  check("probe_ok", "bus error", PROBE_OK(0x801, false, false, true), 0, 0x801, 0);
  check("probe_ok", "write-protected read", PROBE_OK(5, false, false, true), 1, 5, 0);
  check("probe_ok", "write-protected write", PROBE_OK(5, true, false, true), 0, 5, 1);
  check("probe_ok", "supervisor page, supervisor", PROBE_OK(0x81, false, false, true), 1, 0x81, 0);
  check("probe_ok", "supervisor page, user", PROBE_OK(0x81, false, true, true), 0, 0x81, 2);
  check("probe_ok", "TC.E = 0, no TTR match", PROBE_OK(0, true, true, false), 1, 0, 3);
  check("probe_ok", "TC.E = 0, write-protecting TTR, write", PROBE_OK(0x7, true, false, false), 0, 7, 1);
  check("probe_ok", "TC.E = 0, TTR match, read", PROBE_OK(0x7, false, false, false), 1, 7, 0);
  printf("test_smt: %d probe_ok cases\n", nprobe);
  printf("test_smt: %d base ops, guest fn codes 0-63 x 3 sizes, %d trials each: %d failures\n", nbase, trials, fails); (void)nguest;
  return fails != 0;
}
