// Shadow functional model: run each 68k instruction's uisa flow and compare
// it with the host emulator's execution. See uisa_shadow.h.

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
  uint8_t ik;      // immediate kind (IK_*), bit 7 negates
  int8_t ioff;     // extension-word offset from the instruction address
  int32_t imm;
};

#include "uisa_flows.inc"

enum { K_ILLEGAL, K_PLA, K_SEQUENCER, K_MILLICODE, K_UNMAPPED, K_NUM };
enum { IK_CONST, IK_D16, IK_D8, IK_ABSW, IK_ABSL, IK_IMMB, IK_IMMW, IK_IMML, IK_BRW, IK_BRL, IK_PC16, IK_PC8 };

// Outcomes of a shadowed instruction.
enum {
  R_MATCH,
  R_MISMATCH_REG,
  R_MISMATCH_CCR,
  R_MISMATCH_PC,
  R_MISMATCH_SR,
  R_MISMATCH_STORE,
  R_LOAD_NOT_IN_LOG,    // the flow loaded from an address the host never read
  R_EXT_NOT_IN_LOG,     // an extension word the host never fetched
  R_FULL_EXT,           // full-format extension word: not modelled yet
  R_UNSUPPORTED_UOP,    // a uop the model cannot run in a PLA flow
  R_ABORTED,            // pre without post: the host took a fault mid-instruction
  R_NUM
};
const char *R_NAMES[R_NUM] = {"match", "reg", "ccr", "pc", "sr", "store", "load-not-in-log",
                              "ext-not-in-log", "full-ext", "unsupported-uop", "aborted"};

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
std::vector<std::string> details;
const size_t MAX_DETAILS = 4000;
int details_per_op[65536];

bool lookup(int kind, uint32_t addr, int n, uint32_t &out) {
  // assemble big-endian bytes from logged accesses of `kind`; later entries win
  out = 0;
  for (int i = 0; i < n; i++) {
    uint32_t a = addr + i;
    bool found = false;
    uint8_t byte = 0;
    for (int e = 0; e < log_n; e++) {
      const LogEntry &L = log_buf[e];
      if (L.kind != kind || a < L.addr || a >= L.addr + L.size) continue;
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
  bool N, Z, V, C, X;
  uint32_t gpc_insn;
  uint32_t gnpc;
  bool gnpc_set;
  std::map<uint32_t, uint8_t> writes;
  int fail = R_MATCH;
  std::string why;

  uint32_t ext16(int off) {
    uint32_t v;
    if (!lookup(UISA_LOG_IFETCH, gpc_insn + off, 2, v)) { fail = R_EXT_NOT_IN_LOG; return 0; }
    return v;
  }
  uint32_t ext32(int off) {
    uint32_t v;
    if (!lookup(UISA_LOG_IFETCH, gpc_insn + off, 4, v)) { fail = R_EXT_NOT_IN_LOG; return 0; }
    return v;
  }
  uint32_t imm(const uisa_fuop &u) {
    uint32_t v;
    switch (u.ik & 0x7f) {
    case IK_CONST: v = (uint32_t)u.imm; break;
    case IK_D16: case IK_ABSW: case IK_IMMW: v = (uint32_t)(int32_t)(int16_t)ext16(u.ioff); break;
    case IK_D8: v = (uint32_t)(int32_t)(int8_t)(ext16(u.ioff) & 0xff); break;
    case IK_ABSL: case IK_IMML: v = ext32(u.ioff); break;
    case IK_IMMB: v = ext16(u.ioff) & 0xff; break;
    case IK_BRW: v = 2 + (uint32_t)(int32_t)(int16_t)ext16(u.ioff); break;
    case IK_BRL: v = 2 + ext32(u.ioff); break;
    case IK_PC16: v = u.ioff + (uint32_t)(int32_t)(int16_t)ext16(u.ioff); break;
    case IK_PC8: v = u.ioff + (uint32_t)(int32_t)(int8_t)(ext16(u.ioff) & 0xff); break;
    default: fail = R_UNSUPPORTED_UOP; v = 0; break;
    }
    return (u.ik & 0x80) ? (uint32_t)(0u - v) : v;
  }
  uint32_t load(uint32_t addr, int n) {
    uint32_t v;
    if (!lookup(UISA_LOG_READ, addr, n, v)) {
      if (fail == R_MATCH) {
        fail = R_LOAD_NOT_IN_LOG;
        char b[64];
        snprintf(b, sizeof b, "load %d @%08x", n, addr);
        why = b;
      }
      return 0;
    }
    return v;
  }
  void store(uint32_t addr, int n, uint32_t v) {
    for (int i = 0; i < n; i++) writes[addr + i] = (uint8_t)(v >> (8 * (n - 1 - i)));
  }

  void step(const uisa_fuop &u) {
    if (u.op == UISA_PSEUDO_IDX) {
      uint32_t w = ext16(u.ioff);
      if (w & 0x100) { fail = R_FULL_EXT; return; }
      uint32_t x = R[32 + ((w & 0x8000) ? 8 : 0) + ((w >> 12) & 7)];
      if (!(w & 0x800)) x = (uint32_t)(int32_t)(int16_t)x;
      R[u.rd] = R[u.rs1] + (x << ((w >> 9) & 3));
      return;
    }
    const opinfo &o = OPS[u.op];
    uint32_t immv = imm(u);
    uint32_t a = R[u.rs1];
    uint32_t b = o.reads_rs2 ? R[u.rs2] : immv;
    if (o.is_load && !o.is_probe) {
      uint32_t addr = a + immv, v = load(addr, o.mem_bytes);
      if (o.load_signed) v = (uint32_t)SX(v, 8 * o.mem_bytes);
      R[u.rd] = v;
    } else if (o.is_store) {
      if (u.op == UISA_GPUSHR) {
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
    uint32_t r = 0, npc = 0, pc = 0;
    uint32_t OLD = R[u.rd];
    const bool xin = X, nin = N, zin = Z, vin = V, cin = C;
    (void)npc; (void)pc; (void)OLD; (void)IMM; (void)xin; (void)nin; (void)zin; (void)vin; (void)cin;
#define GPC_INSN gpc_insn
#define GNPC_SET(v) do { gnpc = (v); gnpc_set = true; } while (0)
#define GREG_READ(i) R[32 + (i)]
#define GREG_WRITE(i, v) (R[32 + (i)] = (v))
#define CSR_READ(i) (fail = R_UNSUPPORTED_UOP, 0u)
#define CSR_WRITE(i, v) (fail = R_UNSUPPORTED_UOP)
#define CSTACK_PUSH(v) (fail = R_UNSUPPORTED_UOP)
#define CSTACK_POP() (fail = R_UNSUPPORTED_UOP, 0u)
#define GSTORE(sp, addr, n, v) store((addr), (n), (v))
#define GDATA 1
    switch (u.op) {
#include "uisa_sem.inc"
    default:
      fail = R_UNSUPPORTED_UOP;
      why = std::string("no semantics for ") + o.mnem;
      return;
    }
    if (o.writes_rd) R[u.rd] = r;
  }
};

uint8_t ccr_of(uint16_t sr) { return sr & 0x1f; }

void note(int result, const Machine &m, const uisa_m68k_state &post, int ilen) {
  OpStats &s = stats[cur_op];
  s.result[result]++;
  if (result == R_MATCH || details.size() >= MAX_DETAILS || details_per_op[cur_op] >= 3) return;
  details_per_op[cur_op]++;
  char b[512];
  int n = snprintf(b, sizeof b, "%04x pc=%08x %s", cur_op, pre.pc, R_NAMES[result]);
  if (!m.why.empty()) n += snprintf(b + n, sizeof b - n, " (%s)", m.why.c_str());
  if (result == R_MISMATCH_REG) {
    for (int i = 0; i < 16; i++)
      if (m.R[32 + i] != post.r[i])
        n += snprintf(b + n, sizeof b - n, " %c%d: model %08x host %08x pre %08x", i < 8 ? 'd' : 'a', i & 7,
                      m.R[32 + i], post.r[i], pre.r[i]);
  } else if (result == R_MISMATCH_CCR) {
    n += snprintf(b + n, sizeof b - n, " ccr: model %02x host %02x pre %02x",
                  (unsigned)CCR_PACK(m.X, m.N, m.Z, m.V, m.C), ccr_of(post.sr), ccr_of(pre.sr));
  } else if (result == R_MISMATCH_PC) {
    n += snprintf(b + n, sizeof b - n, " next pc: model %08x host %08x (len %d)",
                  m.gnpc_set ? m.gnpc : pre.pc + ilen, post.pc, ilen);
  }
  details.push_back(b);
}

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
  kind_totals[kind]++;
  stats[cur_op].kind_count++;
  if (kind != K_PLA || log_overflow) return;

  Machine m;
  memset(m.R, 0, sizeof m.R);
  for (int i = 0; i < 16; i++) m.R[32 + i] = pre.r[i];
  m.R[48] = pre.pc;                       // gpc
  m.R[49] = pre.sr & 0xff00;              // gsr system byte
  m.X = (pre.sr >> 4) & 1; m.N = (pre.sr >> 3) & 1; m.Z = (pre.sr >> 2) & 1;
  m.V = (pre.sr >> 1) & 1; m.C = pre.sr & 1;
  m.gpc_insn = pre.pc;
  m.gnpc_set = false;
  const uisa_fuop *u = &FLOW_UOPS[FLOW_START[cur_op]];
  for (int k = 0; k < FLOW_LEN[cur_op] && m.fail == R_MATCH; k++) m.step(u[k]);
  int ilen = FLOW_ILEN[cur_op];
  if (m.fail != R_MATCH) { note(m.fail, m, *post, ilen); return; }

  int result = R_MATCH;
  for (int i = 0; i < 16 && result == R_MATCH; i++)
    if (m.R[32 + i] != post->r[i]) result = R_MISMATCH_REG;
  if (result == R_MATCH && CCR_PACK(m.X, m.N, m.Z, m.V, m.C) != ccr_of(post->sr)) result = R_MISMATCH_CCR;
  if (result == R_MATCH && (post->sr & 0xff00) != (pre.sr & 0xff00)) result = R_MISMATCH_SR;
  if (result == R_MATCH && (m.gnpc_set ? m.gnpc : pre.pc + ilen) != post->pc) result = R_MISMATCH_PC;
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
      snprintf(b, sizeof b, "model %zu bytes host %zu bytes%s", m.writes.size(), host.size(),
               m.writes.empty() || host.empty() ? "" : "");
      m.why = b;
      if (!m.writes.empty() && !host.empty() && m.writes.begin()->first != host.begin()->first) {
        snprintf(b, sizeof b, "; first byte model @%08x host @%08x", m.writes.begin()->first, host.begin()->first);
        m.why += b;
      }
    }
  }
  note(result, m, *post, ilen);
}

uint64_t uisa_shadow_count(uint16_t opword, int result) { return stats[opword].result[result]; }

void uisa_shadow_dump(void) {
  if (report_path.empty()) return;
  FILE *f = fopen(report_path.c_str(), "w");
  if (!f) return;
  fprintf(f, "# uisa shadow report\n# kinds:");
  const char *kn[K_NUM] = {"illegal", "pla", "sequencer", "millicode", "unmapped"};
  for (int k = 0; k < K_NUM; k++) fprintf(f, " %s=%llu", kn[k], (unsigned long long)kind_totals[k]);
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
  fprintf(f, "# details\n");
  for (const std::string &d : details) fprintf(f, "! %s\n", d.c_str());
  fclose(f);
}

}  // extern "C"
