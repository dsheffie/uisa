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
#include "uisa_machine.h"

using namespace uisa;
using namespace uisa_model;

namespace {

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
// millicode spacing: log2 histograms of the run of non-millicode instructions
// before each millicode instruction (count, and instructions in such runs),
// and of millicode flow length (count, and millicode words in such flows)
uint64_t gap_run, gap_hist[40][2], mclen_hist[20][2], mc_by_op[65536];
static int lg2(uint64_t v) { int b = 0; while (v > 1) { v >>= 1; b++; } return b; }
uint64_t exc_counts[256];   // exceptions the model took, by vector
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

// The host's access log as the model's memory: extension words and load
// values come from the log, stores are collected for comparison, and probes
// return the host's MMUSR after the instruction.
struct LogMem : Mem {
  std::map<uint32_t, uint8_t> writes;
  uint32_t oracle_mmusr = 0;
  bool ifetch(uint32_t addr, int n, uint32_t &v) override { return lookup(UISA_LOG_IFETCH, addr, n, v); }
  bool load(int, uint32_t addr, int n, uint32_t &v) override { return lookup(UISA_LOG_READ, addr, n, v); }
  void store(int, uint32_t addr, int n, uint32_t v) override {
    for (int i = 0; i < n; i++) writes[addr + i] = (uint8_t)(v >> (8 * (n - 1 - i)));
  }
  uint32_t probe(int, uint32_t, bool) override { return oracle_mmusr; }
};

uint8_t ccr_of(uint16_t sr) { return sr & 0x1f; }

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

int compare(Machine &m, const LogMem &mem, const uisa_m68k_state *post, uint32_t model_pc);
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
  LogMem mem;
  Flow f = select_flow(cur_op, pre.sr, pre.pc, mem);
  int kind = f.kind;
  kind_totals[kind]++;
  stats[cur_op].kind_count++;
  if (kind == K_MILLICODE) {
    int b = gap_run ? lg2(gap_run) + 1 : 0;
    gap_hist[b][0]++;
    gap_hist[b][1] += gap_run;
    gap_run = 0;
  } else gap_run++;
  if (log_overflow) return;
  if (kind == K_MILLICODE && f.mc_entry < 0) { stats[cur_op].result[R_NATIVE]++; return; }
  if (kind != K_PLA && kind != K_SEQUENCER && kind != K_MILLICODE) return;

  Machine m;
  setup(m, mem, cur_op, pre);
  mem.oracle_mmusr = post->ctl[UISA_CTL_MMUSR];
  uint32_t bfe = (kind == K_MILLICODE && (cur_op & 0xf8c0) == 0xe8c0) ? m.ext16_or_zero(2) : 0;
  uint32_t model_pc = exec_flow(m, f);
  if (kind == K_MILLICODE) {
    mc_by_op[cur_op] += m.mc_insns;
    int lb = m.mc_insns ? lg2(m.mc_insns) + 1 : 0;
    mclen_hist[lb][0]++;
    mclen_hist[lb][1] += m.mc_insns;
    if ((cur_op & 0xf8c0) == 0xe8c0) {
      uint32_t e = bfe;
      uint32_t key = ((cur_op >> 8) & 7) << 20 | ((cur_op >> 3) & 7) << 17 | (e & 0x800 ? 1 : 0) << 16 |
                     (e & 0x20 ? 1 : 0) << 15 | ((e >> 6) & 31) << 10 | (e & 31) << 5 | ((pre.sr >> 13) & 1);
      bf_census[key].first++;
      bf_census[key].second += m.mc_insns;
      bf_pcs[pre.pc]++;
    }
  }
  if (m.fail != R_MATCH) { note(m.fail, m, *post, model_pc); return; }
  if (m.raised) exc_counts[m.vector & 0xff]++;
  note(compare(m, mem, post, model_pc), m, *post, model_pc);
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
  LogMem mem;
  Machine m;
  setup(m, mem, cur_op, pre);
  exec_event(m, exc_vec, exc_level, &exc_fault);
  uint32_t model_pc = m.R[G_PC];
  if (m.fail != R_MATCH) { note(m.fail, m, *post, model_pc, exc_vec); return; }
  note(compare(m, mem, post, model_pc), m, *post, model_pc, exc_vec);
}

}  // extern "C"

namespace {

int compare(Machine &m, const LogMem &mem, const uisa_m68k_state *post, uint32_t model_pc) {
  int result = compare_regs(m, *post, model_pc);
  if (result == R_MATCH) {
    std::map<uint32_t, uint8_t> host;
    for (int e = 0; e < log_n; e++) {
      const LogEntry &L = log_buf[e];
      if (L.kind != UISA_LOG_WRITE) continue;
      for (int i = 0; i < L.size; i++) host[L.addr + i] = (uint8_t)(L.value >> (8 * (L.size - 1 - i)));
    }
    const std::map<uint32_t, uint8_t> &writes = mem.writes;
    if (host != writes) {
      result = R_MISMATCH_STORE;
      char b[128];
      snprintf(b, sizeof b, "model %zu bytes host %zu bytes", writes.size(), host.size());
      m.why = b;
      if (!writes.empty() && !host.empty() && writes.begin()->first != host.begin()->first) {
        snprintf(b, sizeof b, "; first byte model @%08x host @%08x", writes.begin()->first, host.begin()->first);
        m.why += b;
      } else {
        for (auto &w : writes) {
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
  for (size_t i = 0; i < mc_words; i++)
    if (mc_prof[i]) fprintf(f, "# mcprof %zx %llu\n", i * 4, (unsigned long long)mc_prof[i]);
  for (int b = 0; b < 40; b++)
    if (gap_hist[b][0]) fprintf(f, "# gap %d %llu %llu\n", b, (unsigned long long)gap_hist[b][0], (unsigned long long)gap_hist[b][1]);
  for (int b = 0; b < 20; b++)
    if (mclen_hist[b][0]) fprintf(f, "# mclen %d %llu %llu\n", b, (unsigned long long)mclen_hist[b][0], (unsigned long long)mclen_hist[b][1]);
  for (int op = 0; op < 65536; op++)
    if (mc_by_op[op]) fprintf(f, "# mcop %04x %llu %llu\n", op, (unsigned long long)stats[op].kind_count, (unsigned long long)mc_by_op[op]);
  for (auto &kv : bf_census)
    fprintf(f, "# bf %x %llu %llu\n", kv.first, (unsigned long long)kv.second.first, (unsigned long long)kv.second.second);
  for (auto &kv : bf_pcs) if (kv.second >= 1000) fprintf(f, "# bfpc %08x %llu\n", kv.first, (unsigned long long)kv.second);
  fprintf(f, "# details\n");
  for (const std::string &d : details) fprintf(f, "! %s\n", d.c_str());
  fclose(f);
}

}  // extern "C"
