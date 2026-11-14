// Differential test of ucc: every test program runs natively (host.h) and as
// compiled millicode on a small uisa ISS built from the generated decoder and
// semantics (build/uisa.h, build/uisa_sem.inc), on many random guest states.
// Guest registers, flags and the exit reason must agree.
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "host.h"
#include "uisa.h"

namespace host {
uint32_t G[18], E[5], CCR;
}

struct UccTest {
  const char *name;
  const uint32_t *image;
  int words;
  int start;            // word index of the entry function
  void (*native)();
};

#include "ucc_tests.inc"

using namespace uisa;

enum { G_D = 32 };

struct Machine {
  uint32_t R[64] = {}, csr[256] = {};
  bool N = 0, Z = 0, V = 0, C = 0, X = 0;
  std::vector<uint32_t> cstack;
  uint32_t cur_pc = 0, next_pc = 0;
  int exitv = 0;
  std::string err;
  unsigned steps = 0, maxDepth = 0;

  uint32_t load(uint32_t a, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | membyte(a + i);
    return v;
  }
  void cstack_push(uint32_t v) {
    if (cstack.size() >= 4) err = "return stack overflow";
    cstack.push_back(v);
    if (cstack.size() > maxDepth) maxDepth = cstack.size();
  }
  uint32_t cstack_pop() {
    if (cstack.empty()) { err = "return stack underflow"; return 0; }
    uint32_t v = cstack.back();
    cstack.pop_back();
    return v;
  }
  void base(const dec_t &d, uint32_t a, uint32_t b, uint32_t IMM) {
    const opinfo &o = OPS[d.op];
    uint32_t r = 0, pc = cur_pc, npc = cur_pc + 4;
    uint32_t OLD = R[d.rd];
    const bool xin = X, nin = N, zin = Z, vin = V, cin = C;
    (void)OLD; (void)IMM; (void)xin; (void)nin; (void)zin; (void)vin; (void)cin; (void)pc;
#define GPC_INSN R[48]
#define GNPC_SET(v) (void)(v)
#define GREG_READ(i) R[G_D + (i)]
#define GREG_WRITE(i, v) (R[G_D + (i)] = (v))
#define CSR_READ(i) csr[(i) & 0xff]
#define CSR_WRITE(i, v) (csr[(i) & 0xff] = (v))
#define CSTACK_PUSH(v) cstack_push(v)
#define CSTACK_POP() cstack_pop()
#define GSTORE(sp, addr, n, v) (void)0
#define GDATA 1
    switch (d.op) {
#include "uisa_sem.inc"
      default:
        err = std::string("no semantics for ") + o.mnem;
        return;
    }
    if (o.writes_rd) R[d.rd] = r;
    next_pc = npc;
  }
  void run(const uint32_t *img, int n, int start) {
    uint32_t mpc = start * 4;
    while (err.empty()) {
      uint32_t i = mpc / 4;
      if (i >= (uint32_t)n || ++steps > 2000000) { err = "ran away"; return; }
      dec_t d = decode(img[i]);
      const opinfo &o = OPS[d.op];
      if (d.op == UISA_ILLEGAL) { err = "illegal word"; return; }
      if (d.op == UISA_GEXIT) { exitv = -1; return; }
      if (d.op == UISA_GFAULT) { exitv = d.imm; return; }
      if (d.op == UISA_BRK) { exitv = 1000 + d.imm; return; }
      uint32_t a = R[d.rs1];
      uint32_t b = o.reads_rs2 ? R[d.rs2] : d.imm;
      cur_pc = mpc;
      next_pc = mpc + 4;
      if (o.is_load && !o.is_probe) {
        uint32_t v = load(a + d.imm, o.mem_bytes);
        if (o.load_signed) v = (uint32_t)SX(v, 8 * o.mem_bytes);
        R[d.rd] = v;
      } else if (o.is_store) {
        err = "store";
      } else if (o.gform != GF_NONE) {
        gop_out g = gop_eval(o.gfn, o.gsize_bits, a, b, X, N, Z, V, C);
        if (o.writes_rd) R[d.rd] = gop_writeback(o.gform, o.gsize_bits, R[d.rd], g.r);
        if (o.flags_on) {
          if (o.writes_nzvc) { N = g.N; Z = g.Z; V = g.V; C = g.C; }
          if (o.writes_x) X = g.X;
        }
      } else {
        base(d, a, b, d.imm);
      }
      R[0] = 0;
      mpc = next_pc;
    }
  }
};

static uint32_t pick(std::mt19937 &rng) {
  static const uint32_t special[] = {0, 1, 2, 3, 7, 8, 31, 32, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff,
                                     0x10000, 0x7fffffff, 0x80000000u, 0xfffffffeu, 0xffffffffu};
  switch (rng() % 4) {
    case 0: return special[rng() % (sizeof special / sizeof special[0])];
    case 1: return rng() % 64;
    default: return rng();
  }
}

int main(int argc, char **argv) {
  int iters = argc > 1 ? atoi(argv[1]) : 3000;
  int fails = 0;
  for (const UccTest &t : TESTS) {
    std::mt19937 rng(12345);
    int tfail = 0;
    unsigned maxSteps = 0, maxDepth = 0;
    for (int it = 0; it < iters && tfail < 3; it++) {
      uint32_t G0[18], E0[5], ccr0 = rng() & 31;
      for (auto &g : G0) g = pick(rng);
      E0[0] = 0;
      E0[1] = rng() & 0xffff;
      for (int k = 2; k < 5; k++) E0[k] = pick(rng);
      // native
      memcpy(host::G, G0, sizeof G0);
      memcpy(host::E, E0, sizeof E0);
      host::CCR = ccr0;
      int hexit = 0;
      try {
        t.native();
        hexit = -2;   // returned: not possible for an entry function
      } catch (host::Exit &e) {
        hexit = e.vector;
      }
      // compiled
      Machine m;
      for (int k = 0; k < 18; k++) m.R[32 + k] = G0[k];
      for (int k = 1; k < 5; k++) m.R[k] = E0[k];
      for (int k = 5; k < 32; k++) m.R[k] = rng();   // garbage in the scratch registers
      CCR_UNPACK(ccr0, m.X, m.N, m.Z, m.V, m.C);
      m.run(t.image, t.words, t.start);
      if (m.steps > maxSteps) maxSteps = m.steps;
      if (m.maxDepth > maxDepth) maxDepth = m.maxDepth;
      std::string why = m.err;
      if (why.empty() && m.exitv != hexit) why = "exit " + std::to_string(m.exitv) + " vs native " + std::to_string(hexit);
      for (int k = 0; k < 18 && why.empty() && hexit == -1; k++)
        if (m.R[32 + k] != host::G[k]) {
          char b[96];
          snprintf(b, sizeof b, "guest reg %d: ucc %08x native %08x", k, m.R[32 + k], host::G[k]);
          why = b;
        }
      uint32_t mccr = CCR_PACK(m.X, m.N, m.Z, m.V, m.C);
      if (why.empty() && hexit == -1 && mccr != host::CCR) why = "ccr";
      if (!why.empty()) {
        printf("FAIL %s iteration %d: %s\n  in:", t.name, it, why.c_str());
        for (int k = 0; k < 18; k++) printf(" %08x", G0[k]);
        printf("\n  entry: %08x %08x %08x %08x\n", E0[1], E0[2], E0[3], E0[4]);
        tfail++;
      }
    }
    printf("ucc test %-12s %s (%d words, up to %u steps, call depth %u)\n", t.name, tfail ? "FAILED" : "ok",
           t.words, maxSteps, maxDepth);
    fails += tfail;
  }
  printf("test_ucc: %d failures\n", fails);
  return fails ? 1 : 0;
}
