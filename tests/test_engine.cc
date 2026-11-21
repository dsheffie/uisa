// The execution engine against a toy host: memory with per-page read and
// write permission. Checks the point-of-no-return tracking: a fault after a
// millicode routine's first guest-visible write is reported. Also the
// millicode predicate u on the execution core: the probe forms that write
// it, and the conditional loads, stores and branches that read it.
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "uisa.h"
#include "uisa_engine.h"
#include "uisa_machine.h"

using namespace uisa;
using namespace uisa_model;

static int fails = 0;

struct Host : EngineHost {
  std::map<uint32_t, uint8_t> mem;
  std::map<uint32_t, int> perm;     // page -> 1 read, 2 write (default: both)
  int prm(uint32_t a) const { auto p = perm.find(a >> 12); return p == perm.end() ? 3 : p->second; }
  bool read(int, uint32_t va, int n, uint32_t &v) override {
    v = 0;
    for (int i = 0; i < n; i++) {
      if (!(prm(va + i) & 1)) return false;
      auto m = mem.find(va + i);
      v = (v << 8) | (m == mem.end() ? 0 : m->second);
    }
    return true;
  }
  bool writable(const EngineStore &s) const {
    for (int i = 0; i < s.size; i++)
      if (!(prm(s.addr + i) & 2)) return false;
    return true;
  }
  uint32_t probe(int, uint32_t, bool) override { return 0; }
  void mmu_flush() override {}
};

static uisa_m68k_state state(uint32_t pc, uint16_t sr) {
  uisa_m68k_state s;
  memset(&s, 0, sizeof s);
  s.pc = pc;
  s.sr = sr;
  return s;
}

// true if the host would see a point-of-no-return violation running op
static bool violation(Host &h, uint16_t op, uisa_m68k_state s, const char *name, int *status = nullptr) {
  EngineResult r;
  int st = engine_step(op, s, h, r);
  if (status) *status = st;
  if (st == E_FAULT) return r.pnr_violation;
  if (st != E_DONE) { printf("FAIL: %s: engine status %d (%s)\n", name, st, r.why.c_str()); fails++; return false; }
  for (const EngineStore &w : r.stores)
    if (!h.writable(w)) return r.store_after_pnr(w);
  return false;
}

static void expect(bool got, bool want, const char *name) {
  if (got != want) { printf("FAIL: %s: violation %d, expected %d\n", name, got, want); fails++; }
}

// ---- the millicode predicate u ---------------------------------------------
// Memory that logs every access and probe (a device: any access is a side
// effect) and answers probes with a per-page MMUSR.
struct LogMem : Mem {
  struct Acc { char kind; int fc; uint32_t addr; int n; uint32_t v; };
  std::vector<Acc> log;
  std::map<uint32_t, uint32_t> mmusr;           // page -> MMUSR (default: resident, R set)
  bool ifetch(uint32_t, int, uint32_t &v) override { v = 0; return true; }
  bool load(int fc, uint32_t addr, int n, uint32_t &v) override {
    v = 0x80000000u | (addr & 0xffff);            // recognisable, sign bit set
    if (n < 4) v &= (1u << (8 * n)) - 1;
    log.push_back({'r', fc, addr, n, v});
    return true;
  }
  void store(int fc, uint32_t addr, int n, uint32_t v) override { log.push_back({'w', fc, addr, n, v}); }
  uint32_t probe(int fc, uint32_t addr, bool write) override {
    log.push_back({'p', fc, addr, write ? 1 : 0, 0});
    auto m = mmusr.find(addr >> 12);
    return m == mmusr.end() ? 0x1u : m->second;
  }
  int accesses() const { int k = 0; for (const Acc &a : log) k += a.kind != 'p'; return k; }
};

static void check(bool ok, const char *what) {
  if (!ok) { printf("FAIL: %s\n", what); fails++; }
}

static void fresh(Machine &m, LogMem &mem, bool supervisor = true) {
  m.mem = &mem;
  memset(m.R, 0, sizeof m.R);
  memset(m.csr, 0, sizeof m.csr);
  m.N = m.Z = m.V = m.C = m.X = false;
  m.U = false;
  m.locked = false;
  m.fail = R_MATCH;
  m.R[G_SR] = supervisor ? 0x2700 : 0x0000;
  m.cur_pc = 0x100;
  m.next_pc = 0x104;
}
static void run(Machine &m, uint16_t op, uint8_t rd, uint8_t rs1, int32_t imm) {
  uisa_fuop u = {op, rd, rs1, 0, 0, 0, imm};
  m.next_pc = m.cur_pc + 4;
  m.step(u);
}

enum { PG_OK = 0x1, PG_UNMAPPED = 0x2, PG_BUSERR = 0x3, PG_WP = 0x4, PG_SUPER = 0x5 };
static const uint32_t MMUSR_OF[] = {0, 0x00001001u, 0x00002000u, 0x00003803u, 0x00004005u, 0x00005081u};

static void test_probes() {
  struct Case { const char *name; uint16_t op; int page; bool supervisor; uint32_t dfc; bool uin, want; };
  const Case cases[] = {
    {"probe.r.us ok",                 UISA_PROBE_R_US_GD, PG_OK,       true,  0, false, true},
    {"probe.r.us unmapped",           UISA_PROBE_R_US_GD, PG_UNMAPPED, true,  0, true,  false},
    {"probe.r.us bus error (B)",      UISA_PROBE_R_US_GD, PG_BUSERR,   true,  0, true,  false},
    {"probe.r.us write-protected",    UISA_PROBE_R_US_GD, PG_WP,       true,  0, false, true},
    {"probe.w.us write-protected",    UISA_PROBE_W_US_GD, PG_WP,       true,  0, true,  false},
    {"probe.w.us ok",                 UISA_PROBE_W_US_GD, PG_OK,       true,  0, false, true},
    {"probe.r.us S page, supervisor", UISA_PROBE_R_US_GD, PG_SUPER,    true,  0, false, true},
    {"probe.r.us S page, user",       UISA_PROBE_R_US_GD, PG_SUPER,    false, 0, true,  false},
    {"probe.r.us.gp S page, user",    UISA_PROBE_R_US_GP, PG_SUPER,    false, 0, true,  false},
    {"probe.r.us.ga S page, DFC 1",   UISA_PROBE_R_US_GA, PG_SUPER,    true,  1, true,  false},
    {"probe.r.us.ga S page, DFC 5",   UISA_PROBE_R_US_GA, PG_SUPER,    false, 5, false, true},
    {"probe.r.ua ok, u set",          UISA_PROBE_R_UA_GD, PG_OK,       true,  0, true,  true},
    {"probe.r.ua ok, u clear",        UISA_PROBE_R_UA_GD, PG_OK,       true,  0, false, false},
    {"probe.r.ua unmapped, u set",    UISA_PROBE_R_UA_GD, PG_UNMAPPED, true,  0, true,  false},
    {"probe.w.ua write-protected",    UISA_PROBE_W_UA_GD, PG_WP,       true,  0, true,  false},
    {"probe.w.ua ok, u set",          UISA_PROBE_W_UA_GD, PG_OK,       true,  0, true,  true},
    {"probe.r.ua S page, user",       UISA_PROBE_R_UA_GD, PG_SUPER,    false, 0, true,  false},
    {"probe.r (no u) unmapped",       UISA_PROBE_R_GD,    PG_UNMAPPED, true,  0, true,  true},
    {"probe.w (no u) ok",             UISA_PROBE_W_GD,    PG_OK,       true,  0, false, false},
  };
  for (const Case &c : cases) {
    LogMem mem;
    for (int p = 1; p <= 5; p++) mem.mmusr[p] = MMUSR_OF[p];
    Machine m;
    fresh(m, mem, c.supervisor);
    m.csr[UISA_CTL_DFC] = c.dfc;
    m.csr[UISA_CTL_TC] = 0x8000;               // translation on: the MMUSRs are table walks
    m.U = c.uin;
    m.R[10] = (uint32_t)c.page << 12;
    run(m, c.op, 5, 10, 0x10);
    char b[128];
    snprintf(b, sizeof b, "%s: u = %d, expected %d", c.name, m.U, c.want);
    check(m.U == c.want && m.fail == R_MATCH, b);
    snprintf(b, sizeof b, "%s: rd = MMUSR", c.name);
    check(m.R[5] == MMUSR_OF[c.page], b);
    snprintf(b, sizeof b, "%s: one probe, no access", c.name);
    check(mem.log.size() == 1 && mem.log[0].kind == 'p' && mem.log[0].addr == ((uint32_t)c.page << 12) + 0x10 &&
          mem.log[0].n == (c.op == UISA_PROBE_W_US_GD || c.op == UISA_PROBE_W_UA_GD || c.op == UISA_PROBE_W_GD), b);
  }
  check(OPS[UISA_PROBE_R_GD].reads_u == 0 && OPS[UISA_PROBE_R_GD].writes_u == 0, "probe.r: no u");
  check(OPS[UISA_PROBE_R_US_GD].reads_u == 0 && OPS[UISA_PROBE_R_US_GD].writes_u == 1, "probe.r.us: writes u");
  check(OPS[UISA_PROBE_W_UA_GA].reads_u == 1 && OPS[UISA_PROBE_W_UA_GA].writes_u == 1, "probe.w.ua: reads and writes u");
  // translation off (TC.E = 0): the host's MMUSR is 0 when no TTR matches
  // (the access is untranslated and succeeds: u = 1), and T | R | W from a
  // write-protecting TTR (probe.w fails, probe.r succeeds)
  {
    struct TC0 { const char *name; uint16_t op; uint32_t mmusr; bool supervisor, uin, want; } tc0[] = {
      {"TC.E = 0, no TTR: probe.r.us",        UISA_PROBE_R_US_GD, 0,           true,  false, true},
      {"TC.E = 0, no TTR: probe.w.us, user",  UISA_PROBE_W_US_GD, 0,           false, false, true},
      {"TC.E = 0, no TTR: probe.w.ua, u set", UISA_PROBE_W_UA_GD, 0,           true,  true,  true},
      {"TC.E = 0, no TTR: probe.r.ua, u clr", UISA_PROBE_R_UA_GD, 0,           true,  false, false},
      {"TC.E = 0, WP TTR: probe.w.us",        UISA_PROBE_W_US_GD, 0x00006007u, true,  false, false},
      {"TC.E = 0, WP TTR: probe.r.us",        UISA_PROBE_R_US_GD, 0x00006007u, true,  false, true},
      {"TC.E = 0, TTR: probe.w.us, user",     UISA_PROBE_W_US_GD, 0x00006003u, false, false, true},
      {"TC.E = 1, MMUSR 0: probe.r.us",       UISA_PROBE_R_US_GD, 0,           true,  true,  false},
    };
    for (const TC0 &c : tc0) {
      LogMem mem;
      mem.mmusr[6] = c.mmusr;
      Machine m;
      fresh(m, mem, c.supervisor);
      m.csr[UISA_CTL_TC] = strstr(c.name, "TC.E = 1") ? 0x8000 : 0;
      m.U = c.uin;
      m.R[10] = 0x6000;
      run(m, c.op, 5, 10, 0);
      char b[128];
      snprintf(b, sizeof b, "%s: u = %d, expected %d", c.name, m.U, c.want);
      check(m.U == c.want && m.fail == R_MATCH, b);
      snprintf(b, sizeof b, "%s: rd = MMUSR %08x", c.name, c.mmusr);
      check(m.R[5] == c.mmusr && mem.accesses() == 0, b);
    }
  }
  // the motivating sequence: two probes, then the guarded accesses
  for (int bad = 0; bad < 3; bad++) {
    LogMem mem;
    mem.mmusr[0x2] = bad == 1 ? MMUSR_OF[PG_UNMAPPED] : 0x1;
    mem.mmusr[0x3] = bad == 2 ? MMUSR_OF[PG_WP] : 0x1;
    Machine m;
    fresh(m, mem);
    m.R[10] = 0x2000; m.R[11] = 0x3000; m.R[13] = 0xdeadbeef;
    run(m, UISA_PROBE_R_US_GD, 5, 10, 0);
    run(m, UISA_PROBE_W_UA_GD, 6, 11, 0);
    run(m, UISA_LD_C_32_GD, 12, 10, 4);
    run(m, UISA_ST_C_32_GD, 13, 11, 8);
    char b[96];
    snprintf(b, sizeof b, "probe chain (failing probe %d): u", bad);
    check(m.U == (bad == 0), b);
    snprintf(b, sizeof b, "probe chain (failing probe %d): accesses", bad);
    check(mem.accesses() == (bad == 0 ? 2 : 0), b);
    snprintf(b, sizeof b, "probe chain (failing probe %d): loaded value", bad);
    check(m.R[12] == (bad == 0 ? 0x80002004u : 0u), b);
  }
}

static void test_cond_loads() {
  struct L { uint16_t op; int n; bool sx; } ls[] = {
    {UISA_LD_C_8U_GD, 1, false}, {UISA_LD_C_8S_GD, 1, true}, {UISA_LD_C_16U_GD, 2, false},
    {UISA_LD_C_16S_GD, 2, true}, {UISA_LD_C_32_GD, 4, false}};
  for (const L &l : ls) {
    for (int u = 0; u < 2; u++) {
      LogMem mem;
      Machine m;
      fresh(m, mem);
      m.U = u;
      m.R[10] = 0x4000;
      m.R[12] = 0x12345678;                    // overwritten either way
      run(m, l.op, 12, 10, -2);
      uint32_t raw = (0x80000000u | 0x3ffe) & (l.n == 4 ? 0xffffffffu : (1u << (8 * l.n)) - 1);
      uint32_t want = u ? (l.sx ? (uint32_t)SX(raw, 8 * l.n) : raw) : 0;
      char b[96];
      snprintf(b, sizeof b, "%s u=%d: rd %08x, expected %08x", OPS[l.op].mnem, u, m.R[12], want);
      check(m.R[12] == want && m.fail == R_MATCH, b);
      snprintf(b, sizeof b, "%s u=%d: memory accesses", OPS[l.op].mnem, u);
      check(mem.log.size() == (size_t)u && (!u || (mem.log[0].kind == 'r' && mem.log[0].addr == 0x3ffe &&
                                                   mem.log[0].n == l.n && mem.log[0].fc == 5)), b);
      check(m.U == (bool)u, "ld.c leaves u");
    }
    check(OPS[l.op].reads_u && !OPS[l.op].writes_u && OPS[l.op].is_load, "ld.c reads u");
  }
  {   // a false ld.c of an address the memory cannot supply: no fault
    struct NoMem : LogMem {
      bool load(int, uint32_t, int, uint32_t &) override { log.push_back({'r', 0, 0, 0, 0}); return false; }
    } mem;
    Machine m;
    fresh(m, mem);
    m.pnr_seq = 1;                             // after the point of no return
    run(m, UISA_LD_C_32_GD, 12, 10, 0);
    check(m.fail == R_MATCH && !m.pnr_violation && mem.log.empty(), "false ld.c: no fault, no PNR violation");
    m.U = true;
    run(m, UISA_LD_C_32_GD, 12, 10, 0);
    check(m.fail == R_LOAD_NOT_IN_LOG && m.pnr_violation, "true ld.c faults like ld.32");
  }
}

static void test_cond_stores() {
  struct S { uint16_t op; int n; bool unlock; } ss[] = {
    {UISA_ST_C_8_GD, 1, false}, {UISA_ST_C_16_GD, 2, false}, {UISA_ST_C_32_GD, 4, false},
    {UISA_STU_C_8_GD, 1, true}, {UISA_STU_C_16_GD, 2, true}, {UISA_STU_C_32_GD, 4, true}};
  for (const S &st : ss) {
    for (int u = 0; u < 2; u++) {
      LogMem mem;
      Machine m;
      fresh(m, mem);
      m.R[10] = 0x5000;
      m.R[13] = 0xcafef00d;                     // data in rd, as st.N
      if (st.unlock) {
        run(m, UISA_LDL_32_GD, 14, 10, 0);
        check(m.locked, "ldl.32 locks");
        mem.log.clear();
      }
      m.U = u;
      uisa_fuop x = {st.op, 13, 10, 0, 0, 0, 12};
      m.step(x);
      char b[96];
      snprintf(b, sizeof b, "%s u=%d: stores", OPS[st.op].mnem, u);
      check(mem.log.size() == (size_t)u && (!u || (mem.log[0].kind == 'w' && mem.log[0].addr == 0x500c &&
                                                   mem.log[0].n == st.n && mem.log[0].v == 0xcafef00du)), b);
      snprintf(b, sizeof b, "%s u=%d: guest-visible write", OPS[st.op].mnem, u);
      check(m.guest_write(x) == (bool)u, b);
      check(m.R[13] == 0xcafef00d && m.U == (bool)u, "st.c leaves rd and u");
      if (st.unlock) {
        snprintf(b, sizeof b, "%s u=%d: releases the lock", OPS[st.op].mnem, u);
        check(!m.locked, b);
      }
    }
    check(OPS[st.op].reads_u && OPS[st.op].is_store && !OPS[st.op].writes_rd && OPS[st.op].reads_rd,
          "st.c reads u and rd (data)");
    check(OPS[st.op].lock_end == st.unlock, "stu.c ends the locked sequence");
  }
  {   // a false st.c in the hidden space is not a guest write either way; the PNR
      // bookkeeping only counts a conditional store that stored
    LogMem mem;
    Machine m;
    fresh(m, mem);
    uisa_fuop x = {UISA_ST_C_32_H, 13, 10, 0, 0, 0, 0};
    m.U = true;
    check(!m.guest_write(x), "st.c.h is not a guest write");
  }
}

static void test_u_branches() {
  for (int u = 0; u < 2; u++) {
    LogMem mem;
    Machine m;
    fresh(m, mem);
    m.U = u;
    run(m, UISA_B_U, 0, 0, 0x40);
    check(m.next_pc == (u ? 0x140u : 0x104u), u ? "b.u taken with u set" : "b.u not taken with u clear");
    run(m, UISA_B_NU, 0, 0, 0x40);
    check(m.next_pc == (u ? 0x104u : 0x140u), u ? "b.nu not taken with u set" : "b.nu taken with u clear");
    // the register compares ignore u
    m.R[1] = 1; m.R[2] = 2;
    uisa_fuop beq = {UISA_B_EQ, 0, 1, 2, 0, 0, 0x40};
    m.next_pc = 0x104;
    m.step(beq);
    check(m.next_pc == 0x104, "b.eq ignores u");
    uisa_fuop bne = {UISA_B_NE, 0, 1, 2, 0, 0, 0x40};
    m.step(bne);
    check(m.next_pc == 0x140, "b.ne ignores u");
  }
  check(OPS[UISA_B_U].reads_u && OPS[UISA_B_NU].reads_u && !OPS[UISA_B_U].reads_rs1 && !OPS[UISA_B_U].reads_rs2,
        "b.u/b.nu read u, not registers");
  for (unsigned op = 1; op < UISA_NUM_OPS; op++)
    if (!strncmp(OPS[op].mnem, "b.", 2) && op != UISA_B_U && op != UISA_B_NU)
      check(!OPS[op].reads_u && OPS[op].reads_rs1 && OPS[op].reads_rs2, "compare branches do not read u");
  // rdccr / wrccr do not touch u
  LogMem mem;
  Machine m;
  fresh(m, mem);
  m.U = true;
  m.R[1] = 0;
  uisa_fuop w = {UISA_WRCCR, 0, 1, 0, 0, 0, 0};
  m.step(w);
  check(m.U, "wrccr leaves u");
  uisa_fuop r = {UISA_RDCCR, 3, 0, 0, 0, 0, 0};
  m.step(r);
  check(m.R[3] == 0, "rdccr does not see u");
}

int main() {
  test_probes();
  test_cond_loads();
  test_cond_stores();
  test_u_branches();
  {  // TAS (A0) on a read-only page: the unlocking store faults before the
     // flags change (it used to set them first: a point-of-no-return bug)
    Host h;
    h.mem[0x1000] = 0x01;
    h.perm[0x1] = 1;
    uisa_m68k_state s = state(0x100, 0x2700);
    s.r[8] = 0x1000;
    expect(violation(h, 0x4ad0, s, "tas (a0) read-only"), false, "tas (a0) read-only");
  }
  {  // TAS (A0) on a writable page: completes
    Host h;
    h.mem[0x1000] = 0x01;
    uisa_m68k_state s = state(0x100, 0x2700);
    s.r[8] = 0x1000;
    expect(violation(h, 0x4ad0, s, "tas (a0)"), false, "tas (a0)");
  }
  {  // TAS (A0) on an unreadable page: faults on the locked load, before any write
    Host h;
    h.perm[0x1] = 0;
    uisa_m68k_state s = state(0x100, 0x2700);
    s.r[8] = 0x1000;
    int st;
    expect(violation(h, 0x4ad0, s, "tas (a0) unreadable", &st), false, "tas (a0) unreadable");
    if (st != E_FAULT) { printf("FAIL: tas (a0) unreadable: status %d, expected a fault\n", st); fails++; }
  }
  {  // CAS2.L whose second address is read-only: the old-value write-back
     // to (Rn2) is the first write and faults itself, before anything changes
    Host h;
    h.perm[0x3] = 1;
    uisa_m68k_state s = state(0x100, 0x2700);
    s.r[8] = 0x2000;  // A0: address 1
    s.r[9] = 0x3000;  // A1: address 2
    h.mem[0x200] = 0; // extension words at 0x102: (A0) Dc1=D0 Du1=D2, (A1) Dc2=D1 Du2=D3
    uint16_t e1 = 8 << 12 | 2 << 6 | 0, e2 = 9 << 12 | 3 << 6 | 1;
    h.mem[0x102] = e1 >> 8; h.mem[0x103] = e1 & 0xff; h.mem[0x104] = e2 >> 8; h.mem[0x105] = e2 & 0xff;
    expect(violation(h, 0x0efc, s, "cas2 (rn2) read-only"), false, "cas2 (rn2) read-only");
  }
  printf("test_engine: %d failures\n", fails);
  return fails != 0;
}
