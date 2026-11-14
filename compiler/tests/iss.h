// iss.h: a small uisa instruction-set simulator for millicode tests, built
// from the generated decoder and semantics (build/uisa.h, uisa_sem.inc).
// Guest memory reads come from membyte(), which the includer defines.
// Stores are not modelled (a store ends the run with an error).
#ifndef UCC_TEST_ISS_H
#define UCC_TEST_ISS_H

#include <cstdint>
#include <string>
#include <vector>

#include "uisa.h"

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

#endif
