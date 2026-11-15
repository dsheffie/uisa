// Shadow functional model: run each 68k instruction as uisa code (a PLA flow,
// the MOVEM sequencer, or millicode) and compare with the host emulator's
// execution. See uisa_shadow.h.

#include "uisa_shadow.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "uisa.h"

using namespace uisa;

namespace {

const uint16_t UISA_PSEUDO_IDX = 0xffff;

struct uisa_fuop {
  uint16_t op;
  uint8_t rd, rs1, rs2;
  uint8_t ik;      // immediate kind (IK_*), bit 7 negates, bit 6 adds the bitfield byte offset
  int8_t ioff;     // extension-word offset from the instruction address
  int32_t imm;
};

#include "uisa_flows.inc"
#include "uisa_millicode.inc"

enum { K_ILLEGAL, K_PLA, K_SEQUENCER, K_MILLICODE, K_UNMAPPED, K_NUM };
enum { IK_CONST, IK_D16, IK_D8, IK_ABSW, IK_ABSL, IK_IMMB, IK_IMMW, IK_IMML, IK_BRW, IK_BRL, IK_PC16, IK_PC8,
       IK_BFB, IK_BFM1, IK_BFM2, IK_BFM3, IK_BFM4, IK_BFR, IK_BFI };
enum { G_BFX = 60 };   // flow register: the data register in bits 14:12 of a bitfield extension word
enum { G_D = 32, G_A = 40, G_PC = 48, G_SR = 49 };   // guest registers in the uisa namespace

// Outcomes of a shadowed instruction.
enum {
  R_MATCH,
  R_MISMATCH_REG,
  R_MISMATCH_CCR,
  R_MISMATCH_PC,
  R_MISMATCH_SR,
  R_MISMATCH_CTL,
  R_MISMATCH_STORE,
  R_LOAD_NOT_IN_LOG,    // the model loaded from an address the host never read
  R_EXT_NOT_IN_LOG,     // an extension word the host never fetched
  R_FULL_EXT,           // full-format extension word: not modelled yet
  R_UNSUPPORTED_UOP,    // a uop or addressing form the model cannot run
  R_MC_UNHANDLED,       // millicode reached brk: a form its routine does not handle yet
  R_EXCEPTION,          // unused (exceptions are now modelled and compared)
  R_NATIVE,             // millicode operation with no routine yet (not compared)
  R_ABORTED,            // pre without post: the host took a fault mid-instruction
  R_NUM
};
const char *R_NAMES[R_NUM] = {"match", "reg", "ccr", "pc", "sr", "ctl", "store", "load-not-in-log",
                              "ext-not-in-log", "full-ext", "unsupported", "mc-unhandled", "exception",
                              "native", "aborted"};

struct LogEntry { uint8_t kind, size; uint32_t addr, value; };

struct OpStats {
  uint64_t kind_count = 0;
  uint64_t result[R_NUM] = {};
};

std::string report_path;
bool pending = false;
uint16_t cur_op;
uisa_m68k_state pre;
LogEntry log_buf[256];
int log_n;
bool log_overflow;
OpStats stats[65536];
uint64_t kind_totals[K_NUM];
uint64_t pla_uops_total;   // PLA uops issued (bitfield XSEL flows included)
uint64_t mc_insns_total, seq_uops_total;
uint64_t exc_counts[256];   // exceptions the model took, by vector
uint64_t mc_prof[sizeof MC_IMAGE / sizeof MC_IMAGE[0]];   // executions per millicode word
// bitfield census: key = op(3) mode(3) Do Dw off(5) wid(5) super; value = count, mc insns
std::map<uint32_t, std::pair<uint64_t, uint64_t>> bf_census;
std::map<uint32_t, uint64_t> bf_pcs;
std::vector<std::string> details;
const size_t MAX_DETAILS = 4000;
int details_per_op[65536];
OpStats exc_stats[256];      // exceptions taken outside instructions, by vector
int exc_details[256];
bool exc_pending = false;
int exc_vec;

bool lookup(int kind, uint32_t addr, int n, uint32_t &out) {
  // assemble big-endian bytes from logged accesses of `kind`; later entries win
  out = 0;
  for (int i = 0; i < n; i++) {
    uint32_t a = addr + i;
    bool found = false;
    uint8_t byte = 0;
    for (int e = 0; e < log_n; e++) {
      const LogEntry &L = log_buf[e];
      if (L.kind != kind || a - L.addr >= (uint32_t)L.size) continue;   // wraps at 2^32
      byte = (uint8_t)(L.value >> (8 * (L.size - 1 - (a - L.addr))));
      found = true;
    }
    if (!found) return false;
    out = (out << 8) | byte;
  }
  return true;
}

struct Machine {
  uint32_t R[64];
  uint32_t csr[256];
  bool N, Z, V, C, X;
  uint32_t gpc_insn;
  uint32_t gnpc;
  bool gnpc_set;
  std::map<uint32_t, uint8_t> writes;
  int fail = R_MATCH;
  std::string why;
  // millicode
  uint32_t cur_pc = 0, next_pc = 0;
  std::vector<uint32_t> cstack;
  bool done = false, raised = false;
  uint32_t vector = 0;
  uint32_t oracle_mmusr = 0;   // the host's MMUSR after the instruction (probe result)
  uint32_t mc_insns = 0;

  void failf(int r, const char *fmt = nullptr, uint32_t v = 0) {
    if (fail != R_MATCH) return;
    fail = r;
    if (fmt) {
      char b[96];
      snprintf(b, sizeof b, fmt, v);
      why = b;
    }
  }
  uint32_t ext16(int off) {
    uint32_t v;
    if (!lookup(UISA_LOG_IFETCH, gpc_insn + off, 2, v)) { failf(R_EXT_NOT_IN_LOG); return 0; }
    return v;
  }
  uint32_t ext32(int off) {
    uint32_t v;
    if (!lookup(UISA_LOG_IFETCH, gpc_insn + off, 4, v)) { failf(R_EXT_NOT_IN_LOG); return 0; }
    return v;
  }
  uint32_t ext16_or_zero(int off) {
    uint32_t v;
    return lookup(UISA_LOG_IFETCH, gpc_insn + off, 2, v) ? v : 0;
  }
  uint32_t imm(const uisa_fuop &u) {
    int k = u.ik & 0x3f;
    if (k == IK_CONST) return (uint32_t)u.imm;
    uint32_t v;
    if (k == IK_D8 && is_full(u.ioff)) v = 0;                                   // the full EA includes bd
    else if (k == IK_PC8 && is_full(u.ioff)) v = (uint32_t)adj(u.ioff);          // base = the extension word
    else {
      uisa_fuop a = u;
      a.ioff = (int8_t)adj(u.ioff);
      v = imm_at(a);
    }
    if (u.ik & 0x40) v += (ext16(2) >> 9) & 3;     // + bitfield offset / 8
    return v + (uint32_t)u.imm;
  }
  uint32_t imm_at(const uisa_fuop &u) {
    uint32_t v;
    switch (u.ik & 0x3f) {
    case IK_CONST: v = (uint32_t)u.imm; break;
    case IK_D16: case IK_ABSW: case IK_IMMW: v = (uint32_t)(int32_t)(int16_t)ext16(u.ioff); break;
    case IK_D8: v = (uint32_t)(int32_t)(int8_t)(ext16(u.ioff) & 0xff); break;
    case IK_ABSL: case IK_IMML: v = ext32(u.ioff); break;
    case IK_IMMB: v = ext16(u.ioff) & 0xff; break;
    case IK_BRW: v = 2 + (uint32_t)(int32_t)(int16_t)ext16(u.ioff); break;
    case IK_BRL: v = 2 + ext32(u.ioff); break;
    case IK_PC16: v = u.ioff + (uint32_t)(int32_t)(int16_t)ext16(u.ioff); break;
    case IK_PC8: v = u.ioff + (uint32_t)(int32_t)(int8_t)(ext16(u.ioff) & 0xff); break;
    case IK_BFB: v = (ext16(u.ioff) >> 9) & 3; break;
    case IK_BFM1: case IK_BFM2: case IK_BFM3: case IK_BFM4: {
      uint32_t e = ext16(u.ioff), n = (u.ik & 0x3f) - IK_BFM1 + 1;
      v = BF_PACK(32 - 8 * n + ((e >> 6) & 7), e & 31);
      break;
    }
    case IK_BFR: { uint32_t e = ext16(u.ioff); v = BF_PACK((e >> 6) & 31, e & 31); break; }
    case IK_BFI: { uint32_t w = ext16(u.ioff) & 31; v = BF_PACK(32 - (w ? w : 32), w); break; }
    default: failf(R_UNSUPPORTED_UOP); v = 0; break;
    }
    return (u.ik & 0x80) ? (uint32_t)(0u - v) : v;
  }
  uint32_t load(uint32_t addr, int n) {
    uint32_t v;
    if (!lookup(UISA_LOG_READ, addr, n, v)) {
      char b[64];
      snprintf(b, sizeof b, "load %d @%08x", n, addr);
      if (fail == R_MATCH) { fail = R_LOAD_NOT_IN_LOG; why = b; }
      return 0;
    }
    return v;
  }
  void store(uint32_t addr, int n, uint32_t v) {
    for (int i = 0; i < n; i++) writes[addr + i] = (uint8_t)(v >> (8 * (n - 1 - i)));
  }
  // Index register value * scale from an index extension word
  uint32_t index_reg(uint32_t w) {
    uint32_t x = R[G_D + ((w & 0x8000) ? 8 : 0) + ((w >> 12) & 7)];
    if (!(w & 0x800)) x = (uint32_t)(int32_t)(int16_t)x;
    return x << ((w >> 9) & 3);
  }
  // brief-format index: Xn.W/L * scale, d8 separately (full format: see full_ea)
  uint32_t index(int off, uint32_t *d8) {
    uint32_t w = ext16(off);
    if (w & 0x100) { failf(R_FULL_EXT); return 0; }
    if (d8) *d8 = (uint32_t)(int32_t)(int8_t)(w & 0xff);
    return index_reg(w);
  }

  // ---- full-format extension words (68020+) --------------------------------
  // The front end's length decoder sees a full-format word (bit 8 set), so the
  // instruction is longer than its opword says and later extension words move.
  // The flow's index uop expands into the full address computation: base
  // (suppressible), base displacement, index (suppressible), and an optional
  // memory-indirect load with outer displacement (pre- or post-indexed).
  int full_n = 0;
  int full_off[2];       // flow offsets (as if brief) of full-format words in this instruction
  int full_bytes[2];     // bytes of base / outer displacement after each
  int full_extra = 0;    // total extra length

  // Bytes of displacement that follow a full-format word w, or -1 if reserved.
  static int full_disp_bytes(uint32_t w) {
    static const int bd_len[4] = {-1, 0, 2, 4};
    int n = bd_len[(w >> 4) & 3];
    int iis = w & 7, is = (w >> 6) & 1;
    if (n < 0 || (w & 8) || iis == 4 || (is && iis > 3)) return -1;
    static const int od_len[4] = {0, 0, 2, 4};
    return n + od_len[iis & 3];
  }
  // Note an index word at flow offset `off` if it is full format; call in
  // instruction order before the flow runs.
  void scan_full(int off) {
    uint32_t w;
    if (full_n >= 2 || !lookup(UISA_LOG_IFETCH, gpc_insn + adj(off), 2, w) || !(w & 0x100)) return;
    int n = full_disp_bytes(w);
    if (n < 0) { failf(R_UNSUPPORTED_UOP, "reserved full-format extension word %04x", w); return; }
    full_off[full_n] = off;
    full_bytes[full_n++] = n;
    full_extra += n;
  }
  // Extension-word offset after the length decoder's adjustment
  int adj(int off) const {
    int a = off;
    for (int i = 0; i < full_n; i++)
      if (off > full_off[i]) a += full_bytes[i];
    return a;
  }
  bool is_full(int off) const {
    for (int i = 0; i < full_n; i++)
      if (off == full_off[i]) return true;
    return false;
  }

  // Effective address from the full-format word at `off`; `base` is An, or the
  // address of the extension word for PC-relative modes.
  uint32_t full_ea(int off, uint32_t base) {
    uint32_t w = ext16(off);
    int p = off + 2;
    uint32_t bd = 0, od = 0;
    switch ((w >> 4) & 3) {
    case 2: bd = (uint32_t)(int32_t)(int16_t)ext16(p); p += 2; break;
    case 3: bd = ext32(p); p += 4; break;
    }
    if (w & 0x80) base = 0;                       // BS: base suppressed
    uint32_t x = (w & 0x40) ? 0 : index_reg(w);   // IS: index suppressed
    int iis = w & 7;
    if (iis == 0) return base + bd + x;           // no memory indirection
    switch (iis & 3) {
    case 2: od = (uint32_t)(int32_t)(int16_t)ext16(p); break;
    case 3: od = ext32(p); break;
    }
    if (iis < 4) return load(base + bd + x, 4) + od;   // pre-indexed
    return load(base + bd, 4) + x + od;                // post-indexed
  }

  void step(const uisa_fuop &u) {
    if (u.op == UISA_PSEUDO_IDX) {
      int off = adj(u.ioff);
      R[u.rd] = is_full(u.ioff) ? full_ea(off, R[u.rs1]) : R[u.rs1] + index(off, nullptr);
      return;
    }
    const opinfo &o = OPS[u.op];
    uint32_t immv = imm(u);
    uint32_t a = R[u.rs1];
    uint32_t b = o.reads_rs2 ? R[u.rs2] : immv;
    if (o.is_probe) {
      R[u.rd] = oracle_mmusr;                    // the table walker's result comes from the host
    } else if (o.is_load) {
      uint32_t addr = a + immv, v = load(addr, o.mem_bytes);
      if (o.load_signed) v = (uint32_t)SX(v, 8 * o.mem_bytes);
      R[u.rd] = v;
    } else if (o.is_store) {
      if (u.op == UISA_GPUSHR) {
        immv += full_extra;                        // the return address: the true length
        store(a - 4, 4, gpc_insn + immv);
        R[u.rd] = a - 4;
      } else if (o.wb == 1) {
        store(a + immv, o.mem_bytes, R[u.rs2]);
        R[u.rd] = a + immv;
      } else if (o.wb == 2) {
        store(a, o.mem_bytes, R[u.rs2]);
        R[u.rd] = a + immv;
      } else {
        store(a + immv, o.mem_bytes, R[u.rd]);
      }
    } else if (o.gform != GF_NONE) {
      gop_out g = gop_eval(o.gfn, o.gsize_bits, a, b, X, N, Z, V, C);
      if (o.writes_rd) R[u.rd] = gop_writeback(o.gform, o.gsize_bits, R[u.rd], g.r);
      if (o.flags_on) {
        if (o.writes_nzvc) { N = g.N; Z = g.Z; V = g.V; C = g.C; }
        if (o.writes_x) X = g.X;
      }
    } else {
      base(u, a, b, immv);
    }
    R[0] = 0;
  }

  void base(const uisa_fuop &u, uint32_t a, uint32_t b, uint32_t IMM) {
    const opinfo &o = OPS[u.op];
    uint32_t r = 0, pc = cur_pc, npc = cur_pc + 4;
    uint32_t OLD = R[u.rd];
    const bool xin = X, nin = N, zin = Z, vin = V, cin = C;
    (void)OLD; (void)IMM; (void)xin; (void)nin; (void)zin; (void)vin; (void)cin;
#define GPC_INSN gpc_insn
#define GNPC_SET(v) do { gnpc = (v); gnpc_set = true; } while (0)
#define GREG_READ(i) R[G_D + (i)]
#define GREG_WRITE(i, v) (R[G_D + (i)] = (v))
#define CSR_READ(i) csr[(i) & 0xff]
#define CSR_WRITE(i, v) (csr[(i) & 0xff] = (v))
#define CSTACK_PUSH(v) cstack_push(v)
#define CSTACK_POP() cstack_pop()
#define GSTORE(sp, addr, n, v) store((addr), (n), (v))
#define GDATA 1
// No ATC or caches in the functional model: flushes have no visible effect.
// (probe's table walk is the hardware walker's; step() takes its result from
// the host, so the shadow checks the millicode around it, not the walk.)
#define MMU_FLUSH(addr, how) ((void)(addr))
#define CACHE_OP(addr, how, push) ((void)(addr))
    switch (u.op) {
#include "uisa_sem.inc"
    default:
      failf(R_UNSUPPORTED_UOP);
      why = std::string("no semantics for ") + o.mnem;
      return;
    }
    if (o.writes_rd) R[u.rd] = r;
    next_pc = npc;
  }
  void cstack_push(uint32_t v) {
    if (cstack.size() >= 4) failf(R_UNSUPPORTED_UOP, "millicode call stack overflow");
    else cstack.push_back(v);
  }
  uint32_t cstack_pop() {
    if (cstack.empty()) { failf(R_UNSUPPORTED_UOP, "millicode call stack underflow"); return 0; }
    uint32_t v = cstack.back();
    cstack.pop_back();
    return v;
  }

  // ---- millicode -----------------------------------------------------------
  // Enter the exception entry point (spec entry `exception`).
  void raise(uint32_t vec) {
    raised = true;
    vector = vec;
    R[1] = vec;
    R[2] = R[G_PC];
    R[3] = gpc_insn;
    R[4] = 0;
    cstack.clear();
  }
  void run_millicode(uint32_t entry, int ilen) {
    R[1] = cur_op;
    R[2] = (ext16_or_zero(2) << 16) | ext16_or_zero(4);
    R[3] = (ext16_or_zero(6) << 16) | ext16_or_zero(8);
    R[4] = gpc_insn + ilen;
    run_from(MC_BASE + entry);
  }
  void run_from(uint32_t mpc) {
    const uint32_t n_words = sizeof MC_IMAGE / sizeof MC_IMAGE[0];
    while (!done && fail == R_MATCH) {
      uint32_t i = (mpc - MC_BASE) / 4;
      if (i >= n_words || ++mc_insns > 10000) { failf(R_UNSUPPORTED_UOP, "millicode ran away at %08x", mpc); return; }
      mc_prof[i]++;
      uint32_t w = MC_IMAGE[i];
      dec_t d = decode(w);
      const opinfo &o = OPS[d.op];
      if (d.op == UISA_ILLEGAL) { failf(R_UNSUPPORTED_UOP, "illegal millicode word at %08x", mpc); return; }
      if (d.op == UISA_GEXIT) { done = true; return; }
      if (d.op == UISA_GFAULT || d.op == UISA_GFAULTR) {
        uint32_t vec = d.op == UISA_GFAULT ? d.imm : R[d.rs1];
        if (raised) { failf(R_UNSUPPORTED_UOP, "exception %u during exception entry", vec); return; }
        raise(vec);
        mpc = MC_BASE + MC_EP_EXCEPTION;
        continue;
      }
      if (d.op == UISA_BRK || d.op == UISA_HALT) {
        char b[64];
        snprintf(b, sizeof b, "brk %u at millicode %08x", d.imm, mpc);
        failf(R_MC_UNHANDLED);
        why = b;
        return;
      }
      if ((o.is_load || o.is_store) && d.sp == 0) { failf(R_UNSUPPORTED_UOP, "hidden-memory access at %08x", mpc); return; }
      uisa_fuop u = {(uint16_t)d.op, d.rd, d.rs1, d.rs2, IK_CONST, 0, (int32_t)d.imm};
      cur_pc = mpc;
      next_pc = mpc + 4;
      step(u);
      mpc = next_pc;
    }
  }

  // ---- MOVEM sequencer ---------------------------------------------------
  uint32_t ea_control(int mode, int reg, int off) {
    uint32_t d8;
    switch (mode) {
    case 2: return R[G_A + reg];
    case 5: return R[G_A + reg] + (uint32_t)(int32_t)(int16_t)ext16(off);
    case 6:
      if (ext16(off) & 0x100) { scan_full(off); return full_ea(off, R[G_A + reg]); }
      { uint32_t x = index(off, &d8); return R[G_A + reg] + x + d8; }
    case 7:
      switch (reg) {
      case 0: return (uint32_t)(int32_t)(int16_t)ext16(off);
      case 1: return ext32(off);
      case 2: return gpc_insn + off + (uint32_t)(int32_t)(int16_t)ext16(off);
      case 3:
        if (ext16(off) & 0x100) { scan_full(off); return full_ea(off, gpc_insn + off); }
        { uint32_t x = index(off, &d8); return gpc_insn + off + x + d8; }
      }
    }
    failf(R_UNSUPPORTED_UOP, "movem addressing mode %u", mode);
    return 0;
  }
  void run_movem() {
    uint32_t mask = ext16(2);
    int size = (cur_op & 0x40) ? 4 : 2;
    int mode = (cur_op >> 3) & 7, reg = cur_op & 7;
    if (fail != R_MATCH) return;
    if (!(cur_op & 0x400)) {                       // registers to memory
      if (mode == 4) {                             // -(An): mask bit 0 = A7 ... bit 15 = D0
        uint32_t a = R[G_A + reg], init = a;
        for (int i = 0; i < 16; i++) {
          if (!(mask & (1u << i))) continue;
          int r = 15 - i;
          a -= size;
          uint32_t v = (r == 8 + reg) ? init - size : R[G_D + r];   // 68020+: An is stored decremented
          store(a, size, v);
          seq_uops_total++;
        }
        R[G_A + reg] = a;
      } else {
        uint32_t a = ea_control(mode, reg, 4);
        for (int i = 0; i < 16; i++) {
          if (!(mask & (1u << i))) continue;
          store(a, size, R[G_D + i]);
          a += size;
          seq_uops_total++;
        }
      }
    } else {                                       // memory to registers
      uint32_t a = (mode == 3) ? R[G_A + reg] : ea_control(mode, reg, 4);
      for (int i = 0; i < 16 && fail == R_MATCH; i++) {
        if (!(mask & (1u << i))) continue;
        uint32_t v = load(a, size);
        R[G_D + i] = (size == 2) ? (uint32_t)(int32_t)(int16_t)v : v;
        a += size;
        seq_uops_total++;
      }
      if (mode == 3) R[G_A + reg] = a;            // the post-increment wins over a loaded An
    }
    seq_uops_total++;                              // address setup / writeback
  }
};

uint8_t ccr_of(uint16_t sr) { return sr & 0x1f; }

int active_stack(uint16_t sr) {
  if (!(sr & 0x2000)) return UISA_CTL_USP;
  return (sr & 0x1000) ? UISA_CTL_MSP : UISA_CTL_ISP;
}

// Count a result for the current instruction (vec < 0) or exception event
// (vector vec), and keep a few details of each kind of failure.
void note(int result, const Machine &m, const uisa_m68k_state &post, uint32_t model_pc, int vec = -1) {
  OpStats &s = vec < 0 ? stats[cur_op] : exc_stats[vec & 0xff];
  int &shown = vec < 0 ? details_per_op[cur_op] : exc_details[vec & 0xff];
  s.result[result]++;
  if (result == R_MATCH || result == R_NATIVE || details.size() >= MAX_DETAILS || shown >= 3) return;
  shown++;
  char b[640];
  int n = vec < 0 ? snprintf(b, sizeof b, "%04x pc=%08x %s", cur_op, pre.pc, R_NAMES[result])
                  : snprintf(b, sizeof b, "exc%d pc=%08x %s", vec, pre.pc, R_NAMES[result]);
  if (!m.why.empty()) n += snprintf(b + n, sizeof b - n, " (%s)", m.why.c_str());
  if (result == R_MISMATCH_REG) {
    for (int i = 0; i < 16; i++)
      if (m.R[G_D + i] != post.r[i])
        n += snprintf(b + n, sizeof b - n, " %c%d: model %08x host %08x pre %08x", i < 8 ? 'd' : 'a', i & 7,
                      m.R[G_D + i], post.r[i], pre.r[i]);
  } else if (result == R_MISMATCH_CCR) {
    n += snprintf(b + n, sizeof b - n, " ccr: model %02x host %02x pre %02x",
                  (unsigned)CCR_PACK(m.X, m.N, m.Z, m.V, m.C), ccr_of(post.sr), ccr_of(pre.sr));
  } else if (result == R_MISMATCH_SR) {
    n += snprintf(b + n, sizeof b - n, " sr: model %04x host %04x pre %04x", m.R[G_SR] & 0xff00,
                  post.sr & 0xff00, pre.sr & 0xff00);
  } else if (result == R_MISMATCH_PC) {
    n += snprintf(b + n, sizeof b - n, " next pc: model %08x host %08x", model_pc, post.pc);
  } else if (result == R_MISMATCH_CTL) {
    for (int i = 0; i < UISA_CTL_NUM; i++)
      if (i != active_stack(post.sr) && m.csr[i] != post.ctl[i])
        n += snprintf(b + n, sizeof b - n, " ctl%d: model %08x host %08x pre %08x", i, m.csr[i], post.ctl[i], pre.ctl[i]);
  }
  details.push_back(b);
}

void setup(Machine &m);
int compare(Machine &m, const uisa_m68k_state *post, uint32_t model_pc);
int exc_level;
uisa_fault exc_fault;

}  // namespace

extern "C" {

int uisa_shadow_logging;

void uisa_shadow_init(const char *path) { report_path = path ? path : ""; }

void uisa_shadow_pre(uint16_t opword, const uisa_m68k_state *s) {
  if (pending) stats[cur_op].result[R_ABORTED]++;
  cur_op = opword;
  pre = *s;
  log_n = 0;
  log_overflow = false;
  pending = true;
  uisa_shadow_logging = 1;
}

void uisa_shadow_log(int kind, uint32_t addr, int size, uint32_t value) {
  if (log_n >= (int)(sizeof log_buf / sizeof log_buf[0])) { log_overflow = true; return; }
  log_buf[log_n++] = {(uint8_t)kind, (uint8_t)size, addr, value};
}

void uisa_shadow_post(const uisa_m68k_state *post) {
  uisa_shadow_logging = 0;
  if (!pending) return;
  pending = false;
  int kind = FLOW_KIND[cur_op];
  const uisa_fuop *flow = &FLOW_UOPS[FLOW_START[cur_op]];
  int flow_len = FLOW_LEN[cur_op];
  uint32_t bfx = 0;
  if (XSEL_INDEX[cur_op] >= 0) {
    // second-level decode: a bitfield with immediate offset and width runs
    // one of the opword's XSEL flows, chosen by the bytes the field touches
    uint32_t e;
    if (lookup(UISA_LOG_IFETCH, pre.pc + 2, 2, e) && !(e & 0x820)) {
      uint32_t o = (e >> 6) & 31, w = (e & 31) ? (e & 31) : 32, n = ((o & 7) + w + 7) >> 3;
      int v;
      if ((cur_op & 0x38) == 0) v = (o == 0 && w == 32) ? 4 : 0;
      else v = (w == 32 && (o & 7) == 0) ? 4 : n <= 4 ? (int)n - 1 : -1;
      if (v >= 0) {
        int x = XSEL_INDEX[cur_op] * 5 + v;
        flow = &FLOW_UOPS[XSEL_START[x]];
        flow_len = XSEL_LEN[x];
        kind = K_PLA;
        bfx = G_D + ((e >> 12) & 7);
      }
    }
  }
  kind_totals[kind]++;
  stats[cur_op].kind_count++;
  if (log_overflow) return;
  if (kind == K_MILLICODE && MC_ENTRY[cur_op] < 0) { stats[cur_op].result[R_NATIVE]++; return; }
  if (kind != K_PLA && kind != K_SEQUENCER && kind != K_MILLICODE) return;

  Machine m;
  setup(m);
  m.oracle_mmusr = post->ctl[UISA_CTL_MMUSR];
  int ilen = FLOW_ILEN[cur_op];
  uint32_t model_pc;
  if (kind == K_PLA) {
    const uisa_fuop *u = flow;
    pla_uops_total += flow_len;
    for (int k = 0; k < flow_len; k++)
      if (u[k].op == UISA_PSEUDO_IDX) m.scan_full(u[k].ioff);
    for (int k = 0; k < flow_len && m.fail == R_MATCH; k++) {
      uisa_fuop x = u[k];
      if (x.rd == G_BFX) x.rd = (uint8_t)bfx;     // the front end's register substitution
      if (x.rs1 == G_BFX) x.rs1 = (uint8_t)bfx;
      if (x.rs2 == G_BFX) x.rs2 = (uint8_t)bfx;
      m.step(x);
    }
    model_pc = m.gnpc_set ? m.gnpc : pre.pc + ilen + m.full_extra;
  } else if (kind == K_SEQUENCER) {
    m.run_movem();
    model_pc = pre.pc + ilen + m.full_extra;
  } else {
    m.run_millicode((uint32_t)MC_ENTRY[cur_op], ilen);
    mc_insns_total += m.mc_insns;
    if ((cur_op & 0xf8c0) == 0xe8c0) {
      uint32_t e = m.R[2] >> 16;
      uint32_t key = ((cur_op >> 8) & 7) << 20 | ((cur_op >> 3) & 7) << 17 | (e & 0x800 ? 1 : 0) << 16 |
                     (e & 0x20 ? 1 : 0) << 15 | ((e >> 6) & 31) << 10 | (e & 31) << 5 | ((pre.sr >> 13) & 1);
      bf_census[key].first++;
      bf_census[key].second += m.mc_insns;
      bf_pcs[pre.pc]++;
    }
    model_pc = m.R[G_PC];
  }
  if (m.fail != R_MATCH) { note(m.fail, m, *post, model_pc); return; }
  if (m.raised) exc_counts[m.vector & 0xff]++;
  note(compare(m, post, model_pc), m, *post, model_pc);
}

uint64_t uisa_shadow_count(uint16_t opword, int result) { return stats[opword].result[result]; }
uint64_t uisa_shadow_kind_total(int kind) { return kind >= 0 && kind < K_NUM ? kind_totals[kind] : 0; }
uint64_t uisa_shadow_exc_count(int vector, int result) { return exc_stats[vector & 0xff].result[result]; }

void uisa_shadow_exc_pre(int vector, int level, const uisa_m68k_state *s, const uisa_fault *f) {
  if (pending) { stats[cur_op].result[R_ABORTED]++; pending = false; }
  if (exc_pending) exc_stats[exc_vec & 0xff].result[R_ABORTED]++;
  exc_pending = true;
  exc_vec = vector;
  exc_level = level;
  if (f) exc_fault = *f; else memset(&exc_fault, 0, sizeof exc_fault);
  pre = *s;
  log_n = 0;
  log_overflow = false;
  uisa_shadow_logging = 1;
}

void uisa_shadow_exc_post(const uisa_m68k_state *post) {
  uisa_shadow_logging = 0;
  if (!exc_pending) return;
  exc_pending = false;
  exc_stats[exc_vec & 0xff].kind_count++;
  if (log_overflow) return;
  Machine m;
  setup(m);
  if (exc_vec == 2) {
    m.csr[0x41] = exc_fault.addr;
    m.csr[0x42] = exc_fault.ssw;
    m.csr[0x43] = pre.pc;
    m.csr[0x48] = exc_fault.ea;
    m.csr[0x49] = exc_fault.wb2_status;
    m.csr[0x4a] = exc_fault.wb2_addr;
    m.csr[0x4b] = exc_fault.wb3_status;
    m.csr[0x4c] = exc_fault.wb3_data;
    for (int i = 0; i < 4; i++) m.csr[0x4d + i] = exc_fault.pd[i];
    m.raise(2);
    m.run_from(MC_BASE + MC_EP_EXCEPTION);
  } else {
    m.R[1] = exc_level;
    m.R[2] = exc_vec;
    m.run_from(MC_BASE + MC_EP_INTERRUPT);
  }
  mc_insns_total += m.mc_insns;
  uint32_t model_pc = m.R[G_PC];
  if (m.fail != R_MATCH) { note(m.fail, m, *post, model_pc, exc_vec); return; }
  note(compare(m, post, model_pc), m, *post, model_pc, exc_vec);
}

}  // extern "C"

namespace {

void setup(Machine &m) {
  memset(m.R, 0, sizeof m.R);
  memset(m.csr, 0, sizeof m.csr);
  for (int i = 0; i < 16; i++) m.R[G_D + i] = pre.r[i];
  for (int i = 0; i < UISA_CTL_NUM; i++) m.csr[i] = pre.ctl[i];
  m.R[G_PC] = pre.pc;
  m.R[G_SR] = pre.sr & 0xff00;
  m.X = (pre.sr >> 4) & 1; m.N = (pre.sr >> 3) & 1; m.Z = (pre.sr >> 2) & 1;
  m.V = (pre.sr >> 1) & 1; m.C = pre.sr & 1;
  m.gpc_insn = pre.pc;
  m.gnpc_set = false;
}

int compare(Machine &m, const uisa_m68k_state *post, uint32_t model_pc) {
  int result = R_MATCH;
  for (int i = 0; i < 16 && result == R_MATCH; i++)
    if (m.R[G_D + i] != post->r[i]) result = R_MISMATCH_REG;
  if (result == R_MATCH && CCR_PACK(m.X, m.N, m.Z, m.V, m.C) != ccr_of(post->sr)) result = R_MISMATCH_CCR;
  if (result == R_MATCH && (m.R[G_SR] & 0xff00) != (post->sr & 0xff00)) result = R_MISMATCH_SR;
  if (result == R_MATCH && model_pc != post->pc) result = R_MISMATCH_PC;
  if (result == R_MATCH)
    for (int i = 0; i < UISA_CTL_NUM; i++)
      if (i != active_stack(post->sr) && m.csr[i] != post->ctl[i]) result = R_MISMATCH_CTL;
  if (result == R_MATCH) {
    std::map<uint32_t, uint8_t> host;
    for (int e = 0; e < log_n; e++) {
      const LogEntry &L = log_buf[e];
      if (L.kind != UISA_LOG_WRITE) continue;
      for (int i = 0; i < L.size; i++) host[L.addr + i] = (uint8_t)(L.value >> (8 * (L.size - 1 - i)));
    }
    if (host != m.writes) {
      result = R_MISMATCH_STORE;
      char b[128];
      snprintf(b, sizeof b, "model %zu bytes host %zu bytes", m.writes.size(), host.size());
      m.why = b;
      if (!m.writes.empty() && !host.empty() && m.writes.begin()->first != host.begin()->first) {
        snprintf(b, sizeof b, "; first byte model @%08x host @%08x", m.writes.begin()->first, host.begin()->first);
        m.why += b;
      } else {
        for (auto &w : m.writes) {
          auto h = host.find(w.first);
          if (h != host.end() && h->second != w.second) {
            snprintf(b, sizeof b, "; @%08x model %02x host %02x", w.first, w.second, h->second);
            m.why += b;
            break;
          }
        }
      }
    }
  }
  return result;
}

}  // namespace

extern "C" {

void uisa_shadow_dump(void) {
  if (report_path.empty()) return;
  FILE *f = fopen(report_path.c_str(), "w");
  if (!f) return;
  fprintf(f, "# uisa shadow report\n# kinds:");
  const char *kn[K_NUM] = {"illegal", "pla", "sequencer", "millicode", "unmapped"};
  for (int k = 0; k < K_NUM; k++) fprintf(f, " %s=%llu", kn[k], (unsigned long long)kind_totals[k]);
  fprintf(f, "\n# millicode instructions executed: %llu; sequencer uops: %llu\n",
          (unsigned long long)mc_insns_total, (unsigned long long)seq_uops_total);
  uint64_t all_insns = kind_totals[K_PLA] + kind_totals[K_SEQUENCER] + kind_totals[K_MILLICODE];
  uint64_t all_uops = pla_uops_total + seq_uops_total + mc_insns_total;
  fprintf(f, "# pla uops: %llu; uops per instruction: %.3f\n", (unsigned long long)pla_uops_total,
          all_insns ? (double)all_uops / all_insns : 0.0);
  fprintf(f, "# exceptions taken by vector:");
  for (int v = 0; v < 256; v++)
    if (exc_counts[v]) fprintf(f, " %d=%llu", v, (unsigned long long)exc_counts[v]);
  fprintf(f, "\n# opword count");
  for (int r = 0; r < R_NUM; r++) fprintf(f, " %s", R_NAMES[r]);
  fprintf(f, "\n");
  for (int op = 0; op < 65536; op++) {
    const OpStats &s = stats[op];
    if (!s.kind_count && !s.result[R_ABORTED]) continue;
    fprintf(f, "%04x %llu", op, (unsigned long long)s.kind_count);
    for (int r = 0; r < R_NUM; r++) fprintf(f, " %llu", (unsigned long long)s.result[r]);
    fprintf(f, "\n");
  }
  for (int v = 0; v < 256; v++) {
    const OpStats &s = exc_stats[v];
    if (!s.kind_count && !s.result[R_ABORTED]) continue;
    fprintf(f, "# event %d %llu", v, (unsigned long long)s.kind_count);
    for (int r = 0; r < R_NUM; r++) fprintf(f, " %llu", (unsigned long long)s.result[r]);
    fprintf(f, "\n");
  }
  for (size_t i = 0; i < sizeof mc_prof / sizeof mc_prof[0]; i++)
    if (mc_prof[i]) fprintf(f, "# mcprof %zx %llu\n", i * 4, (unsigned long long)mc_prof[i]);
  for (auto &kv : bf_census)
    fprintf(f, "# bf %x %llu %llu\n", kv.first, (unsigned long long)kv.second.first, (unsigned long long)kv.second.second);
  for (auto &kv : bf_pcs) if (kv.second >= 1000) fprintf(f, "# bfpc %08x %llu\n", kv.first, (unsigned long long)kv.second);
  fprintf(f, "# details\n");
  for (const std::string &d : details) fprintf(f, "! %s\n", d.c_str());
  fclose(f);
}

}  // extern "C"
