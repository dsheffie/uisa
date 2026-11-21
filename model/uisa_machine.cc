// The uisa execution core (see uisa_machine.h).

#include "uisa_machine.h"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "uisa.h"

using namespace uisa;

namespace uisa_model {

#include "uisa_flows.inc"
#include "uisa_millicode.inc"

enum { IK_CONST, IK_D16, IK_D8, IK_ABSW, IK_ABSL, IK_IMMB, IK_IMMW, IK_IMML, IK_BRW, IK_BRL, IK_PC16, IK_PC8,
       IK_BFB, IK_BFM1, IK_BFM2, IK_BFM3, IK_BFM4, IK_BFR, IK_BFI };

const char *const R_NAMES[R_NUM] = {"match", "reg", "ccr", "pc", "sr", "ctl", "store", "load-not-in-log",
                                    "ext-not-in-log", "full-ext", "unsupported", "mc-unhandled", "exception",
                                    "native", "aborted", "fp"};

uint64_t pla_uops_total, seq_uops_total, mc_insns_total;
static uint64_t mc_prof_counts[sizeof MC_IMAGE / sizeof MC_IMAGE[0]];
uint64_t *const mc_prof = mc_prof_counts;
const size_t mc_words = sizeof MC_IMAGE / sizeof MC_IMAGE[0];
const int32_t *const mc_entry_table = MC_ENTRY;

// Address space of each memory op, from its mnemonic's suffix:
// 0 hidden, 1 gdata, 2 gprog, 3 galt (spec/uisa.yaml `spaces`).
namespace {
struct SpaceTable {
  uint8_t space[UISA_NUM_OPS];
  bool probe_w[UISA_NUM_OPS];
  SpaceTable() {
    for (unsigned i = 0; i < UISA_NUM_OPS; i++) {
      const char *m = OPS[i].mnem;
      size_t n = strlen(m);
      space[i] = 1;
      if (n > 3 && !strcmp(m + n - 3, ".gp")) space[i] = 2;
      else if (n > 3 && !strcmp(m + n - 3, ".ga")) space[i] = 3;
      else if (n > 2 && !strcmp(m + n - 2, ".h")) space[i] = 0;
      probe_w[i] = !strncmp(m, "probe.w", 7);
    }
  }
};
const SpaceTable SPACES;
}  // namespace

void Machine::failf(int r, const char *fmt, uint32_t v) {
  if (fail != R_MATCH) return;
  fail = r;
  if (fmt) {
    char b[96];
    snprintf(b, sizeof b, fmt, v);
    why = b;
  }
}

int Machine::fc_space(int space, bool write) const {
  bool s = R[G_SR] & 0x2000;
  switch (space) {
  case 2: return s ? 6 : 2;
  case 3: return (int)(csr[write ? UISA_CTL_DFC : UISA_CTL_SFC] & 7);
  default: return s ? 5 : 1;
  }
}

uint32_t Machine::ext16(int off) {
  uint32_t v;
  if (!mem->ifetch(gpc_insn + off, 2, v)) {
    if (pnr_seq && !raised && !pnr_violation) { pnr_violation = true; pnr_fault_mpc = cur_pc; }
    failf(R_EXT_NOT_IN_LOG);
    return 0;
  }
  return v;
}
uint32_t Machine::ext32(int off) {
  uint32_t v;
  if (!mem->ifetch(gpc_insn + off, 4, v)) {
    if (pnr_seq && !raised && !pnr_violation) { pnr_violation = true; pnr_fault_mpc = cur_pc; }
    failf(R_EXT_NOT_IN_LOG);
    return 0;
  }
  return v;
}
uint32_t Machine::ext16_or_zero(int off) {
  uint32_t v;
  return mem->ifetch(gpc_insn + off, 2, v) ? v : 0;
}

uint32_t Machine::imm(const uisa_fuop &u) {
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

uint32_t Machine::imm_at(const uisa_fuop &u) {
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

uint32_t Machine::load(int fc, uint32_t addr, int n) {
  uint32_t v;
  if (!mem->load(fc, addr, n, v)) {
    if (pnr_seq && !raised && !pnr_violation) { pnr_violation = true; pnr_fault_mpc = cur_pc; }
    char b[64];
    snprintf(b, sizeof b, "load %d @%08x", n, addr);
    if (fail == R_MATCH) { fail = R_LOAD_NOT_IN_LOG; why = b; }
    return 0;
  }
  return v;
}

// Index register value * scale from an index extension word
uint32_t Machine::index_reg(uint32_t w) {
  uint32_t x = R[G_D + ((w & 0x8000) ? 8 : 0) + ((w >> 12) & 7)];
  if (!(w & 0x800)) x = (uint32_t)(int32_t)(int16_t)x;
  return x << ((w >> 9) & 3);
}
// brief-format index: Xn.W/L * scale, d8 separately (full format: see full_ea)
uint32_t Machine::index(int off, uint32_t *d8) {
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

// Bytes of displacement that follow a full-format word w, or -1 if reserved.
int Machine::full_disp_bytes(uint32_t w) {
  static const int bd_len[4] = {-1, 0, 2, 4};
  int n = bd_len[(w >> 4) & 3];
  int iis = w & 7, is = (w >> 6) & 1;
  if (n < 0 || (w & 8) || iis == 4 || (is && iis > 3)) return -1;
  static const int od_len[4] = {0, 0, 2, 4};
  return n + od_len[iis & 3];
}
// Note an index word at flow offset `off` if it is full format; call in
// instruction order before the flow runs.
void Machine::scan_full(int off) {
  uint32_t w;
  if (full_n >= 2 || !mem->ifetch(gpc_insn + adj(off), 2, w) || !(w & 0x100)) return;
  int n = full_disp_bytes(w);
  if (n < 0) { failf(R_UNSUPPORTED_UOP, "reserved full-format extension word %04x", w); return; }
  full_off[full_n] = off;
  full_bytes[full_n++] = n;
  full_extra += n;
}
// Extension-word offset after the length decoder's adjustment
int Machine::adj(int off) const {
  int a = off;
  for (int i = 0; i < full_n; i++)
    if (off > full_off[i]) a += full_bytes[i];
  return a;
}
bool Machine::is_full(int off) const {
  for (int i = 0; i < full_n; i++)
    if (off == full_off[i]) return true;
  return false;
}

// Effective address from the full-format word at `off`; `base` is An, or the
// address of the extension word for PC-relative modes.
uint32_t Machine::full_ea(int off, uint32_t base) {
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
  int fc = fc_space(1, false);
  if (iis < 4) return load(fc, base + bd + x, 4) + od;   // pre-indexed
  return load(fc, base + bd, 4) + x + od;                // post-indexed
}

void Machine::step(const uisa_fuop &u) {
  if (u.op == UISA_PSEUDO_IDX) {
    int off = adj(u.ioff);
    R[u.rd] = is_full(u.ioff) ? full_ea(off, R[u.rs1]) : R[u.rs1] + index(off, nullptr);
    return;
  }
  const opinfo &o = OPS[u.op];
  uint32_t immv = imm(u);
  uint32_t a = R[u.rs1];
  uint32_t b = o.reads_rs2 ? R[u.rs2] : immv;
  int space = SPACES.space[u.op];
  if (o.is_probe) {
    int fc = fc_space(space, true);
    uint32_t mmusr = mem->probe(fc, a + immv, SPACES.probe_w[u.op]);
    R[u.rd] = mmusr;
    if (o.writes_u) {                            // .us: u = ok; .ua: u = u && ok
      // ok (spec probe_ok): with translation off (TC.E = 0) an MMUSR of 0
      // means no TTR matched, so the access is untranslated and succeeds;
      // a TTR match (TC.E either way) has T and R set and W from the TTR
      bool ok = PROBE_OK(mmusr, SPACES.probe_w[u.op], !(fc & 4), (csr[UISA_CTL_TC] & 0x8000) != 0);
      U = o.reads_u ? (U && ok) : ok;
    }
  } else if ((o.is_load || o.is_store) && o.reads_u && !U) {
    // ld.c / st.c / stu.c with u clear: no access at all (no translation, no
    // fault, nothing logged); a load writes rd = 0, a store writes nothing
    if (o.is_load) R[u.rd] = 0;
  } else if (o.is_load && o.is_store) {         // cas: compare-and-swap (as SPARC CASA)
    int fc = fc_space(space, true);
    uint32_t addr = a + immv, n = o.mem_bytes;
    uint32_t m = n == 4 ? 0xffffffffu : (1u << (8 * n)) - 1;
    uint32_t old = load(fc, addr, (int)n);
    if (fail != R_MATCH) return;
    if (old == (R[u.rs2] & m)) store(fc, addr, (int)n, R[u.rd] & m);
    R[u.rd] = old;
  } else if (o.is_load) {
    uint32_t addr = a + immv, v = load(fc_space(space, false), addr, o.mem_bytes);
    if (o.load_signed) v = (uint32_t)SX(v, 8 * o.mem_bytes);
    R[u.rd] = v;
  } else if (o.is_store) {
    int fc = fc_space(space, true);
    if (u.op == UISA_GPUSHR) {
      immv += full_extra;                        // the return address: the true length
      store(fc, a - 4, 4, gpc_insn + immv);
      R[u.rd] = a - 4;
    } else if (o.wb == 1) {
      store(fc, a + immv, o.mem_bytes, R[u.rs2]);
      R[u.rd] = a + immv;
    } else if (o.wb == 2) {
      store(fc, a, o.mem_bytes, R[u.rs2]);
      R[u.rd] = a + immv;
    } else {
      store(fc, a + immv, o.mem_bytes, R[u.rd]);
    }
  } else if (o.unit == UNIT_FPU) {
    fpop(u);
  } else if (o.gform != GF_NONE) {
    gop_out g = gop_eval(o.gfn, o.gsize_bits, a, b, X, N, Z, V, C);
    if (o.writes_rd) R[u.rd] = gop_writeback(o.gform, o.gsize_bits, R[u.rd], g.r);
    if (g.T) trap_vec = g.T;
    if (o.flags_on) {
      if (o.writes_nzvc) { N = g.N; Z = g.Z; V = g.V; C = g.C; }
      if (o.writes_x) X = g.X;
    }
  } else {
    base(u, a, b, immv);
  }
  if (o.lock_begin) locked = true;
  if (o.lock_end || u.op == UISA_UNLOCK) locked = false;   // stu.c.*: whether or not it stored
  R[0] = 0;
}


// ---- floating point (spec FP ops) ------------------------------------------
// IEEE 754 binary64 / binary32 on the host, under the FPCR's rounding mode,
// with the spec's NaN rules applied explicitly (an AArch64 host already
// follows them; its double arithmetic is q800sim's). The exceptions the host
// raises are ORed into fflags (CSR 0x23: NV 4, DZ 3, OF 2, UF 1, NX 0):
// underflow is the host's (AArch64: tininess before rounding).
namespace {
inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
inline bool nan64(uint64_t x) { return (x & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; }
inline bool snan64(uint64_t x) { return nan64(x) && !(x & 0x0008000000000000ull); }
inline bool nan32(uint32_t x) { return (x & 0x7fffffffu) > 0x7f800000u; }
inline bool snan32(uint32_t x) { return nan32(x) && !(x & 0x00400000u); }
// the NaN result of a two-operand op with a NaN operand
inline uint64_t pick_nan64(uint64_t a, uint64_t b) {
  if (snan64(a)) return a | 0x0008000000000000ull;
  if (snan64(b)) return b | 0x0008000000000000ull;
  return nan64(a) ? a : b;
}
inline uint32_t pick_nan32(uint32_t a, uint32_t b) {
  if (snan32(a)) return a | 0x00400000u;
  if (snan32(b)) return b | 0x00400000u;
  return nan32(a) ? a : b;
}
const uint64_t DNAN64 = 0x7ff8000000000000ull;
const uint32_t DNAN32 = 0x7fc00000u;
}  // namespace

void Machine::fpop(const uisa_fuop &u) {
  static const int modes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD};
  const uint64_t a = F[u.rs1 & 15], b = F[u.rs2 & 15];
  uint64_t r = 0;
  bool wf = true;            // writes an FP register
  if (u.rd > 15 && u.op != UISA_FGETHI && u.op != UISA_FGETLO) { failf(R_UNSUPPORTED_UOP, "FP register %u", u.rd); return; }
  if (u.rs1 > 15 && u.op != UISA_FSETHI && u.op != UISA_FSETLO) { failf(R_UNSUPPORTED_UOP, "FP register %u", u.rs1); return; }
  switch (u.op) {
  case UISA_FMOV_D: r = a; break;
  case UISA_FNEG_D: r = a ^ 0x8000000000000000ull; break;
  case UISA_FABS_D: r = a & 0x7fffffffffffffffull; break;
  case UISA_FSETHI: r = (F[u.rd & 15] & 0xffffffffull) | ((uint64_t)R[u.rs1] << 32); break;
  case UISA_FSETLO: r = (F[u.rd & 15] & 0xffffffff00000000ull) | R[u.rs1]; break;
  case UISA_FGETHI: wf = false; if (u.rd) R[u.rd] = (uint32_t)(a >> 32); break;
  case UISA_FGETLO: wf = false; if (u.rd) R[u.rd] = (uint32_t)a; break;
  default: {
    std::feclearexcept(FE_ALL_EXCEPT);
    std::fesetround(modes[(csr[0x20] >> 4) & 3]);
    volatile double da = u2d(a), db = u2d(b);
    volatile float fa = u2f((uint32_t)a), fb = u2f((uint32_t)b);
    switch (u.op) {
    case UISA_FADD_D: r = d2u(da + db); break;
    case UISA_FSUB_D: r = d2u(da - db); break;
    case UISA_FMUL_D: r = d2u(da * db); break;
    case UISA_FDIV_D: r = d2u(da / db); break;
    case UISA_FSQRT_D: r = d2u(std::sqrt((double)da)); break;
    case UISA_FMUL_S: r = f2u(fa * fb); break;
    case UISA_FDIV_S: r = f2u(fa / fb); break;
    case UISA_FCVT_S_D: r = f2u((float)da); break;
    case UISA_FCVT_D_S: r = d2u((double)fa); break;
    default: failf(R_UNSUPPORTED_UOP, "FP op %u", u.op); break;
    }
    int fe = std::fetestexcept(FE_ALL_EXCEPT);
    std::fesetround(FE_TONEAREST);
    // the NaN rules, whatever the host's
    switch (u.op) {
    case UISA_FADD_D: case UISA_FSUB_D: case UISA_FMUL_D: case UISA_FDIV_D:
      if (nan64(a) || nan64(b)) r = pick_nan64(a, b);
      else if (nan64(r)) r = DNAN64;
      break;
    case UISA_FSQRT_D:
      if (nan64(a)) r = a | 0x0008000000000000ull;
      else if (nan64(r)) r = DNAN64;
      break;
    case UISA_FMUL_S: case UISA_FDIV_S:
      if (nan32((uint32_t)a) || nan32((uint32_t)b)) r = pick_nan32((uint32_t)a, (uint32_t)b);
      else if (nan32((uint32_t)r)) r = DNAN32;
      break;
    case UISA_FCVT_S_D:
      if (nan64(a)) r = (uint32_t)((a >> 32) & 0x80000000u) | 0x7fc00000u | (uint32_t)((a >> 29) & 0x3fffffu);
      break;
    case UISA_FCVT_D_S:
      if (nan32((uint32_t)a))
        r = ((uint64_t)(a & 0x80000000u) << 32) | 0x7ff8000000000000ull | ((uint64_t)(a & 0x3fffffu) << 29);
      break;
    }
    csr[0x23] |= ((fe & FE_INVALID) ? 16u : 0u) | ((fe & FE_DIVBYZERO) ? 8u : 0u) | ((fe & FE_OVERFLOW) ? 4u : 0u) |
                 ((fe & FE_UNDERFLOW) ? 2u : 0u) | ((fe & FE_INEXACT) ? 1u : 0u);
    break;
  }
  }
  if (wf) F[u.rd & 15] = r;
}

void Machine::base(const uisa_fuop &u, uint32_t a, uint32_t b, uint32_t IMM) {
  const opinfo &o = OPS[u.op];
  uint32_t r = 0, pc = cur_pc, npc = cur_pc + 4;
  uint32_t OLD = R[u.rd];
  const bool xin = X, nin = N, zin = Z, vin = V, cin = C, uin = U;
  (void)OLD; (void)IMM; (void)xin; (void)nin; (void)zin; (void)vin; (void)cin; (void)uin;
#define GPC_INSN gpc_insn
#define GNPC_SET(v) do { gnpc = (v); gnpc_set = true; } while (0)
// rdg/wrg reach only the guest registers, indices 0-17 (D0-D7, A0-A7, gpc,
// gsr); 18-31 is a millicode fault
#define GREG_READ(i) greg_read(i)
#define GREG_WRITE(i, v) greg_write((i), (v))
#define CSR_READ(i) csr[(i) & 0xff]
#define CSR_WRITE(i, v) (csr[(i) & 0xff] = (v))
#define CSTACK_PUSH(v) cstack_push(v)
#define CSTACK_POP() cstack_pop()
#define GSTORE(sp, addr, n, v) store(fc_space((sp), true), (addr), (n), (v))
#define GDATA 1
// No caches in the functional model; an ATC flush goes to the memory, which
// may cache translations. (probe's table walk is the hardware walker's: the
// memory supplies its result.)
#define MMU_FLUSH(addr, how) ((void)(addr), mem->mmu_flush())
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

bool Machine::guest_write(const uisa_fuop &u) const {
  const opinfo &o = OPS[u.op];
  // guest-space store (gpushr, cas included); a conditional store (st.c,
  // stu.c) only if it stored: u was set (the store does not change u)
  if (o.is_store) return SPACES.space[u.op] != 0 && (!o.reads_u || U);
  if (o.writes_rd && u.rd >= G_D && u.rd <= G_SR) return true;    // D0-D7, A0-A7, gpc, gsr
  if ((o.writes_nzvc || o.writes_x) && o.flags_on) return true;    // the flags
  if (u.op == UISA_WRG) return true;
  // VBR..MSP, the FP control registers (not fflags, the FP ops' scratch flags)
  if ((u.op == UISA_CSRW || u.op == UISA_CSRS || u.op == UISA_CSRC) && (u.imm & 0xff) < 0x40 &&
      (u.imm & 0xff) != 0x23)
    return true;
  if (o.unit == UNIT_FPU && (o.fpregs & 1) && u.rd < 8) return true;   // FP0-FP7
  return false;
}

void Machine::cstack_push(uint32_t v) {
  if (cstack.size() >= 4) failf(R_UNSUPPORTED_UOP, "millicode call stack overflow");
  else cstack.push_back(v);
}
uint32_t Machine::cstack_pop() {
  if (cstack.empty()) { failf(R_UNSUPPORTED_UOP, "millicode call stack underflow"); return 0; }
  uint32_t v = cstack.back();
  cstack.pop_back();
  return v;
}

// ---- millicode -----------------------------------------------------------
// Enter the exception entry point (spec entry `exception`).
void Machine::raise(uint32_t vec) {
  raised = true;
  vector = vec;
  R[1] = vec;
  R[2] = R[G_PC];
  R[3] = gpc_insn;
  R[4] = 0;
  cstack.clear();
}
void Machine::run_millicode(uint32_t entry, int ilen) {
  R[1] = cur_op;
  R[2] = (ext16_or_zero(2) << 16) | ext16_or_zero(4);
  R[3] = (ext16_or_zero(6) << 16) | ext16_or_zero(8);
  R[4] = gpc_insn + ilen;
  run_from(MC_BASE + entry);
}

namespace {
// millicode words decoded once
struct McDecoded {
  dec_t d[sizeof MC_IMAGE / sizeof MC_IMAGE[0]];
  McDecoded() { for (size_t i = 0; i < sizeof MC_IMAGE / sizeof MC_IMAGE[0]; i++) d[i] = decode(MC_IMAGE[i]); }
};
const McDecoded &mc_decoded() { static McDecoded t; return t; }
}  // namespace

void Machine::run_from(uint32_t mpc) {
  const uint32_t n_words = sizeof MC_IMAGE / sizeof MC_IMAGE[0];
  const McDecoded &mcd = mc_decoded();
  while (!done && fail == R_MATCH) {
    uint32_t i = (mpc - MC_BASE) / 4;
    if (i >= n_words || ++mc_insns > 10000) { failf(R_UNSUPPORTED_UOP, "millicode ran away at %08x", mpc); return; }
    mc_prof_counts[i]++;
    const dec_t &d = mcd.d[i];
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
    mc_seq++;
    step(u);
    if (!pnr_seq && !raised && fail == R_MATCH && guest_write(u)) { pnr_seq = mc_seq; pnr_mpc = mpc; }
    mpc = next_pc;
  }
}

// ---- MOVEM sequencer ---------------------------------------------------
uint32_t Machine::ea_control(int mode, int reg, int off) {
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
void Machine::run_movem() {
  uint32_t mask = ext16(2);
  int size = (cur_op & 0x40) ? 4 : 2;
  int mode = (cur_op >> 3) & 7, reg = cur_op & 7;
  if (fail != R_MATCH) return;
  int fc = fc_space(1, false);
  if (!(cur_op & 0x400)) {                       // registers to memory
    if (mode == 4) {                             // -(An): mask bit 0 = A7 ... bit 15 = D0
      uint32_t a = R[G_A + reg], init = a;
      for (int i = 0; i < 16; i++) {
        if (!(mask & (1u << i))) continue;
        int r = 15 - i;
        a -= size;
        uint32_t v = (r == 8 + reg) ? init - size : R[G_D + r];   // 68020+: An is stored decremented
        store(fc, a, size, v);
        seq_uops_total++;
      }
      R[G_A + reg] = a;
    } else {
      uint32_t a = ea_control(mode, reg, 4);
      for (int i = 0; i < 16; i++) {
        if (!(mask & (1u << i))) continue;
        store(fc, a, size, R[G_D + i]);
        a += size;
        seq_uops_total++;
      }
    }
  } else {                                       // memory to registers
    uint32_t a = (mode == 3) ? R[G_A + reg] : ea_control(mode, reg, 4);
    for (int i = 0; i < 16 && fail == R_MATCH; i++) {
      if (!(mask & (1u << i))) continue;
      uint32_t v = load(fc, a, size);
      R[G_D + i] = (size == 2) ? (uint32_t)(int32_t)(int16_t)v : v;
      a += size;
      seq_uops_total++;
    }
    if (mode == 3) R[G_A + reg] = a;            // the post-increment wins over a loaded An
  }
  seq_uops_total++;                              // address setup / writeback
}

// ---- flow selection and running ------------------------------------------

Flow select_flow(uint16_t cur_op, uint16_t sr, uint32_t pc, Mem &mem) {
  Flow f;
  f.kind = FLOW_KIND[cur_op];
  f.uops = &FLOW_UOPS[FLOW_START[cur_op]];
  f.len = FLOW_LEN[cur_op];
  f.xreg = f.xreg2 = 0;
  f.ilen = FLOW_ILEN[cur_op];
  f.mc_entry = MC_ENTRY[cur_op];
  if (XSEL_INDEX[cur_op] < 0) return f;
  auto pick = [&](int x) {
    f.uops = &FLOW_UOPS[XSEL_START[x]];
    f.len = XSEL_LEN[x];
    f.kind = K_PLA;
  };
  // second-level decode: a bitfield with immediate offset and width runs
  // one of the opword's XSEL flows, chosen by the bytes the field touches
  uint32_t e;
  if ((cur_op & 0xf8c0) == 0xe8c0 && mem.ifetch(pc + 2, 2, e) && !(e & 0x820)) {
    uint32_t o = (e >> 6) & 31, w = (e & 31) ? (e & 31) : 32, n = ((o & 7) + w + 7) >> 3;
    int v;
    if ((cur_op & 0x38) == 0) v = (o == 0 && w == 32) ? 4 : 0;
    else v = (w == 32 && (o & 7) == 0) ? 4 : n <= 4 ? (int)n - 1 : -1;
    if (v >= 0) {
      pick(XSEL_INDEX[cur_op] * XSEL_NV + v);
      f.xreg = G_D + ((e >> 12) & 7);
    }
  }
  // MOVE SR,Dn and MOVE Dn,SR in supervisor state with M = 0; MOVE #imm,SR
  // also needs M = 0 in the immediate, and picks by its S bit
  bool sup = (sr & 0x3000) == 0x2000;
  int sv = -1;
  if ((cur_op & 0xfff8) == 0x40c0 && (sr & 0x2000)) sv = 0;
  if ((cur_op & 0xfff8) == 0x46c0 && sup) sv = 0;
  if (cur_op == 0x46fc && sup && mem.ifetch(pc + 2, 2, e) && !(e & 0x1000)) sv = e & 0x2000 ? 0 : 1;
  if (sv >= 0) pick(XSEL_INDEX[cur_op] * XSEL_NV + sv);
  // MULU.L / MULS.L: by extension bits 11:10 (signed, 64-bit result)
  if ((cur_op & 0xffc0) == 0x4c00 && mem.ifetch(pc + 2, 2, e)) {
    pick(XSEL_INDEX[cur_op] * XSEL_NV + ((e >> 10) & 3));
    f.xreg = G_D + ((e >> 12) & 7);
    f.xreg2 = G_D + (e & 7);
  }
  // DIVU.L / DIVS.L with a 32-bit dividend: by bit 11 (signed), plus 4 when
  // Dr = Dq (quotient only); a 64-bit dividend (bit 10) stays in millicode
  if ((cur_op & 0xffc0) == 0x4c40 && mem.ifetch(pc + 2, 2, e)) {
    int x = XSEL_INDEX[cur_op] * XSEL_NV + (((e >> 10) & 3) | (((e >> 12) & 7) == (e & 7) ? 4 : 0));
    if (XSEL_LEN[x]) {
      pick(x);
      f.xreg = G_D + ((e >> 12) & 7);
      f.xreg2 = G_D + (e & 7);
    }
  }
  // CAS Dc,Du,<ea>: one flow; Du is extension bits 8:6, Dc bits 2:0
  if ((cur_op & 0xf9c0) == 0x08c0 && mem.ifetch(pc + 2, 2, e)) {
    int x = XSEL_INDEX[cur_op] * XSEL_NV;
    if (XSEL_LEN[x]) {
      pick(x);
      f.xreg = G_D + ((e >> 6) & 7);
      f.xreg2 = G_D + (e & 7);
    }
    return f;
  }
  // MOVES in supervisor state: load into Dn, load into An, or store
  // (size 3 in this opword range is CAS.L, above)
  if ((cur_op & 0xff00) == 0x0e00 && (sr & 0x2000) && mem.ifetch(pc + 2, 2, e)) {
    pick(XSEL_INDEX[cur_op] * XSEL_NV + (e & 0x800 ? 2 : e & 0x8000 ? 1 : 0));
    f.xreg = G_D + ((e >> 12) & 15);
  }
  // MOVEC in supervisor state with M = 0: one flow per control register
  if ((cur_op & 0xfffe) == 0x4e7a && sup && mem.ifetch(pc + 2, 2, e) && !(e & 0x7f8)) {
    int x = XSEL_INDEX[cur_op] * XSEL_NV + (((e >> 8) & 8) | (e & 7));
    if (XSEL_LEN[x]) {
      pick(x);
      f.xreg = G_D + ((e >> 12) & 15);
    }
  }
  return f;
}

void setup(Machine &m, Mem &mem, uint16_t op, const uisa_m68k_state &pre) {
  m.mem = &mem;
  memset(m.R, 0, sizeof m.R);
  memset(m.csr, 0, sizeof m.csr);
  for (int i = 0; i < 16; i++) m.R[G_D + i] = pre.r[i];
  for (int i = 0; i < UISA_CTL_NUM; i++) m.csr[i] = pre.ctl[i];
  m.R[G_PC] = pre.pc;
  m.R[G_SR] = pre.sr & 0xff00;
  m.X = (pre.sr >> 4) & 1; m.N = (pre.sr >> 3) & 1; m.Z = (pre.sr >> 2) & 1;
  m.V = (pre.sr >> 1) & 1; m.C = pre.sr & 1;
  m.U = false;
  m.locked = false;
  memset(m.F, 0, sizeof m.F);
  m.fp_valid = pre.fp_valid == UISA_FP_VALID;
  if (m.fp_valid) {
    for (int i = 0; i < 8; i++) m.F[i] = pre.fp[i];
    m.csr[0x20] = pre.fpcr;
    m.csr[0x21] = pre.fpsr;
    m.csr[0x22] = pre.fpiar;
    m.csr[0x24] = pre.fpused;
  }
  m.cur_op = op;
  m.gpc_insn = pre.pc;
  m.gnpc_set = false;
}

uint32_t exec_flow(Machine &m, const Flow &f) {
  uint32_t model_pc;
  if (f.kind == K_PLA) {
    const uisa_fuop *u = f.uops;
    pla_uops_total += f.len;
    for (int k = 0; k < f.len; k++)
      if (u[k].op == UISA_PSEUDO_IDX) m.scan_full(u[k].ioff);
    for (int k = 0; k < f.len && m.fail == R_MATCH; k++) {
      uisa_fuop x = u[k];
      if (x.rd == G_XREG) x.rd = (uint8_t)f.xreg;     // the front end's register substitution
      if (x.rs1 == G_XREG) x.rs1 = (uint8_t)f.xreg;
      if (x.rs2 == G_XREG) x.rs2 = (uint8_t)f.xreg;
      if (x.rd == G_XREG2) x.rd = (uint8_t)f.xreg2;
      if (x.rs1 == G_XREG2) x.rs1 = (uint8_t)f.xreg2;
      if (x.rs2 == G_XREG2) x.rs2 = (uint8_t)f.xreg2;
      if (x.op == UISA_GFAULT || x.op == UISA_GFAULTR) {
        // a decode-time exception (ILLEGAL, LINE-A, LINE-F flows): the
        // instruction does not retire; the frame stacks its own PC
        m.R[G_PC] = m.gpc_insn;
        m.raise(x.op == UISA_GFAULT ? x.imm : m.R[x.rs1]);
        m.run_from(MC_BASE + MC_EP_EXCEPTION);
        mc_insns_total += m.mc_insns;
        return m.R[G_PC];
      }
      m.step(x);
    }
    model_pc = m.gnpc_set ? m.gnpc : m.gpc_insn + f.ilen + m.full_extra;
    if (m.fail == R_MATCH && m.trap_vec) {
      // a trap (zero divide): the instruction has retired, so the exception
      // stacks the next PC (and, in the format 2 frame, this instruction's)
      m.R[G_PC] = model_pc;
      m.raise(m.trap_vec);
      m.run_from(MC_BASE + MC_EP_EXCEPTION);
      mc_insns_total += m.mc_insns;
      model_pc = m.R[G_PC];
    }
  } else if (f.kind == K_SEQUENCER) {
    m.run_movem();
    model_pc = m.gpc_insn + f.ilen + m.full_extra;
  } else {
    m.run_millicode((uint32_t)f.mc_entry, f.ilen);
    mc_insns_total += m.mc_insns;
    model_pc = m.R[G_PC];
  }
  return model_pc;
}

void exec_event(Machine &m, int vector, int level, const uisa_fault *f) {
  if (vector == 2) {
    m.csr[0x41] = f->addr;
    m.csr[0x42] = f->ssw;
    m.csr[0x43] = m.gpc_insn;
    m.csr[0x48] = f->ea;
    m.csr[0x49] = f->wb2_status;
    m.csr[0x4a] = f->wb2_addr;
    m.csr[0x4b] = f->wb3_status;
    m.csr[0x4c] = f->wb3_data;
    for (int i = 0; i < 4; i++) m.csr[0x4d + i] = f->pd[i];
    m.raise(2);
    m.run_from(MC_BASE + MC_EP_EXCEPTION);
  } else {
    m.R[1] = level;
    m.R[2] = vector;
    m.run_from(MC_BASE + MC_EP_INTERRUPT);
  }
  mc_insns_total += m.mc_insns;
}

void writeback(const Machine &m, uint32_t model_pc, uisa_m68k_state &s) {
  for (int i = 0; i < 16; i++) s.r[i] = m.R[G_D + i];
  s.pc = model_pc;
  s.sr = (uint16_t)((m.R[G_SR] & 0xff00) | CCR_PACK(m.X, m.N, m.Z, m.V, m.C));
  for (int i = 0; i < UISA_CTL_NUM; i++) s.ctl[i] = m.csr[i];
  if (m.fp_valid) {
    s.fp_valid = UISA_FP_VALID;
    for (int i = 0; i < 8; i++) s.fp[i] = m.F[i];
    s.fpcr = m.csr[0x20];
    s.fpsr = m.csr[0x21];
    s.fpiar = m.csr[0x22];
    s.fpused = m.csr[0x24];
  }
}

int active_stack(uint16_t sr) {
  if (!(sr & 0x2000)) return UISA_CTL_USP;
  return (sr & 0x1000) ? UISA_CTL_MSP : UISA_CTL_ISP;
}

int compare_regs(const Machine &m, const uisa_m68k_state &post, uint32_t model_pc) {
  for (int i = 0; i < 16; i++)
    if (m.R[G_D + i] != post.r[i]) return R_MISMATCH_REG;
  if (CCR_PACK(m.X, m.N, m.Z, m.V, m.C) != (post.sr & 0x1f)) return R_MISMATCH_CCR;
  if ((m.R[G_SR] & 0xff00) != (post.sr & 0xff00)) return R_MISMATCH_SR;
  if (model_pc != post.pc) return R_MISMATCH_PC;
  for (int i = 0; i < UISA_CTL_NUM; i++)
    if (i != active_stack(post.sr) && m.csr[i] != post.ctl[i]) return R_MISMATCH_CTL;
  if (m.fp_valid && post.fp_valid == UISA_FP_VALID) {
    for (int i = 0; i < 8; i++)
      if (m.F[i] != post.fp[i]) return R_MISMATCH_FP;
    if (m.csr[0x20] != post.fpcr || m.csr[0x21] != post.fpsr || m.csr[0x22] != post.fpiar ||
        m.csr[0x24] != post.fpused)
      return R_MISMATCH_FP;
  }
  return R_MATCH;
}

}  // namespace uisa_model
