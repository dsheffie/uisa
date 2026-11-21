// uisa execution engine (see uisa_engine.h).

#include "uisa_engine.h"

#include "uisa_machine.h"

namespace uisa_model {

namespace {

// The host's memory with this instruction's stores held back: loads see
// earlier stores of the same instruction (millicode builds exception frames
// and MOVEM writes before it reads), stores are collected in order.
struct EngineMem : Mem {
  EngineHost &host;
  int prog_fc;
  std::vector<EngineStore> &stores;
  const Machine *m = nullptr;
  EngineMem(EngineHost &h, int pfc, std::vector<EngineStore> &st) : host(h), prog_fc(pfc), stores(st) {}

  bool ifetch(uint32_t addr, int n, uint32_t &v) override { return host.read(prog_fc, addr, n, v); }
  bool load(int fc, uint32_t addr, int n, uint32_t &v) override {
    if (!host.read(fc, addr, n, v)) return false;
    for (const EngineStore &s : stores) {          // overlay buffered bytes, oldest first
      for (int i = 0; i < n; i++) {
        uint32_t off = addr + (uint32_t)i - s.addr;
        if (off >= s.size || s.fc != fc) continue;
        uint8_t byte = (uint8_t)(s.value >> (8 * (s.size - 1 - off)));
        int sh = 8 * (n - 1 - i);
        v = (v & ~(0xffu << sh)) | ((uint32_t)byte << sh);
      }
    }
    return true;
  }
  void store(int fc, uint32_t addr, int n, uint32_t v) override {
    stores.push_back({(uint8_t)fc, (uint8_t)n, addr, n == 4 ? v : v & ((1u << (8 * n)) - 1), m ? m->mc_seq : 0});
  }
  uint32_t probe(int fc, uint32_t addr, bool write) override { return host.probe(fc, addr, write); }
  void mmu_flush() override { host.mmu_flush(); }
};

}  // namespace

const char *engine_kind_name(int kind) {
  static const char *const n[K_NUM] = {"illegal", "pla", "sequencer", "millicode", "unmapped"};
  return kind >= 0 && kind < K_NUM ? n[kind] : "?";
}

int engine_step(uint16_t op, uisa_m68k_state &s, EngineHost &host, EngineResult &r) {
  r.stores.clear();
  r.why.clear();
  r.raised = false;
  r.vector = 0;
  r.uops = 0;
  r.ilen = 0;
  r.full_ext = false;
  r.fail = R_MATCH;
  r.pnr_seq = r.pnr_mpc = r.pnr_fault_mpc = 0;
  r.pnr_violation = false;
  EngineMem mem(host, (s.sr & 0x2000) ? 6 : 2, r.stores);
  Flow f = select_flow(op, s.sr, s.pc, mem);
  r.kind = f.kind;
  if ((f.kind != K_PLA && f.kind != K_SEQUENCER && f.kind != K_MILLICODE) ||
      (f.kind == K_MILLICODE && f.mc_entry < 0)) {
    r.fail = f.kind == K_MILLICODE ? R_NATIVE : R_UNSUPPORTED_UOP;
    r.why = f.kind == K_MILLICODE ? "no millicode routine" : engine_kind_name(f.kind);
    return r.status = E_FALLBACK;
  }
  // the FPU routines need the host's FP state (uisa_m68k_state fp_valid)
  if ((op & 0xfe00) == 0xf200 && s.fp_valid != UISA_FP_VALID) {
    r.fail = R_NATIVE;
    r.why = "coprocessor 1 without the host's FP state";
    return r.status = E_FALLBACK;
  }
  Machine m;
  setup(m, mem, op, s);
  mem.m = &m;
  uint64_t seq0 = seq_uops_total;
  uint32_t model_pc = exec_flow(m, f);
  r.uops = f.kind == K_PLA ? (uint32_t)f.len + m.mc_insns
         : f.kind == K_SEQUENCER ? (uint32_t)(seq_uops_total - seq0) : m.mc_insns;
  // length: the length decoder's (flow length plus full-format words); in
  // millicode, x4 is the next sequential PC, corrected by the EA helpers
  if (f.kind == K_MILLICODE) {
    uint32_t mc = m.R[4] - s.pc;
    r.ilen = (mc >= 2 && mc <= 22 && !(mc & 1)) ? mc : (uint32_t)f.ilen;
  } else {
    r.ilen = (uint32_t)f.ilen + (uint32_t)m.full_extra;
  }
  r.full_ext = r.ilen > (uint32_t)f.ilen;
  r.pnr_seq = m.pnr_seq;
  r.pnr_mpc = m.pnr_mpc;
  r.pnr_violation = m.pnr_violation;
  r.pnr_fault_mpc = m.pnr_fault_mpc;
  if (m.fail != R_MATCH) {
    r.fail = m.fail;
    r.why = m.why;
    r.stores.clear();
    return r.status = (m.fail == R_LOAD_NOT_IN_LOG || m.fail == R_EXT_NOT_IN_LOG) ? E_FAULT : E_FALLBACK;
  }
  r.raised = m.raised;
  r.vector = m.vector;
  writeback(m, model_pc, s);
  return r.status = E_DONE;
}

}  // namespace uisa_model
