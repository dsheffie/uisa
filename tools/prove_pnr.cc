// prove_pnr: symbolic execution of the millicode image, proving the point of
// no return for every routine the dispatch table reaches: on no path does
// an access that can fault follow a guest-visible write (or the 68k
// instruction could not be restarted).
//
// Each routine starts from a symbolic state: the opword (constrained to the
// opwords that dispatch to it), the extension words, the guest registers,
// flags, CSRs, millicode x registers and guest memory are all symbolic. The
// uop semantics are build/uisa_smt.inc (tools/gen_smt.py, from the spec's
// `sem`), so the proof runs on the same semantics as the ISS. Branches fork
// where z3 finds both directions feasible; states that reach the same
// millicode address with the same return stack and commit status merge
// (registers as if-then-else), which keeps loops and diamonds linear.
//
// A guest-visible write is one that may change state: a guest register,
// gpc/gsr, a flag, an architectural CSR, or a guest-space store that is not
// provably silent (the value already there). After the first one on a path,
// a guest-space access is allowed only on a page that path has already
// accessed (stores: already stored to); anything else is a violation,
// reported with a concrete counterexample from z3's model. Exception entry
// (gfault) ends a path: a fault while stacking is a double fault.
//
// The millicode predicate u is a symbolic 1-bit flag (not guest-visible, so
// writing it is never a commit). A probe's .us/.ua form sets it to (u and)
// the probe's success, spec probe_ok over the probe's symbolic MMUSR. A
// conditional access (ld.c, st.c, stu.c) happens only where u holds: its
// fault check runs under the path condition and u, and it is safe on a page
// the path accessed or on one a probe whose success u implies has probed;
// with u clear a load gives 0 and a store changes nothing.
//
//   prove_pnr [-v] [routine...]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "uisa_symsem.h"

using namespace uisa;
using usym::Sym;
namespace z = z3;

#include "uisa_millicode.inc"

static z3::context &C() { return usym::ctx(); }
static bool verbose = false;

// address space of each memory op, by mnemonic suffix (as the model)
static int space_of(unsigned op) {
  const char *m = OPS[op].mnem;
  size_t n = strlen(m);
  if (n > 3 && !strcmp(m + n - 3, ".gp")) return 2;
  if (n > 3 && !strcmp(m + n - 3, ".ga")) return 3;
  if (n > 2 && !strcmp(m + n - 2, ".h")) return 0;
  return 1;
}

struct State {
  uint32_t mpc = 0;
  std::vector<uint32_t> cstack;
  z3::expr pc = C().bool_val(true);           // path condition
  Sym R[64];
  Sym X, N, Z, V, Cf;
  Sym U;                                      // the millicode predicate u
  std::map<int, Sym> csr;
  z3::expr mem = C().bool_val(true);          // guest memory: an array BV32 -> BV8
  bool committed = false;
  uint32_t commit_mpc = 0;
  std::vector<z3::expr> rd_pages, wr_pages;   // pages accessed on this path (after or before the commit)
  struct Probe { z3::expr page, ok; bool write; };
  std::vector<Probe> probes;                  // .us/.ua probes: ok means an access to page would not fault
  int steps = 0;
};

static int n_sym = 0;
static Sym fresh(const char *base, usym::Ty t) {
  char b[64];
  snprintf(b, sizeof b, "%s_%d", base, n_sym++);
  return Sym::var(b, t);
}

struct Result {
  int paths_ok = 0, paths_exc = 0, paths_brk = 0, paths_bad = 0;
  bool inconclusive = false;
  std::string why;
  std::vector<std::string> violations;
};

class Prover {
public:
  z3::solver solver{C()};
  z3::expr op_var = C().bv_const("opword", 32);
  z3::expr ext1 = C().bv_const("ext1", 32), ext2 = C().bv_const("ext2", 32);
  std::map<int, Sym> csr0;   // initial CSR symbols
  Result res;
  std::string routine;

  bool feasible(const z3::expr &e) {
    solver.push();
    solver.add(e);
    bool r = solver.check() == z3::sat;
    solver.pop();
    return r;
  }
  bool valid(const z3::expr &pc, const z3::expr &e) { return !feasible(pc && !e); }

  // may this write change the value? (a write of the same value is not a commit)
  bool may_differ(const State &s, const Sym &o, const Sym &n) {
    z3::expr a = usym::conv(o, n.t).e, b = n.e;
    if (z3::eq(a, b)) return false;
    return feasible(s.pc && a != b);
  }
  void commit(State &s, bool changes) {
    if (changes && !s.committed) { s.committed = true; s.commit_mpc = s.mpc; }
  }

  Sym csr_get(State &s, int i) {
    auto it = s.csr.find(i);
    if (it != s.csr.end()) return it->second;
    auto c0 = csr0.find(i);
    if (c0 == csr0.end()) {
      char b[32];
      snprintf(b, sizeof b, "csr%02x", i);
      c0 = csr0.emplace(i, Sym::var(b, usym::U32)).first;
    }
    return c0->second;
  }

  // ---- memory ----
  z3::expr page(const z3::expr &a) { return z3::lshr(a, 12); }
  // may a guest access at addr (n bytes) fault on this path? Only an
  // access to a page the path already touched (for a write: wrote) is safe.
  // cond: the access happens only under it (a conditional access: u)
  bool can_fault(const State &s, const z3::expr &addr, int n, bool write, const z3::expr &cond) {
    z3::expr last = addr + C().bv_val(n - 1, 32);
    z3::expr pc = s.pc && cond;
    for (const z3::expr &a : {addr, last}) {
      bool ok = false;
      for (const z3::expr &p : (write ? s.wr_pages : s.rd_pages))
        if (valid(pc, page(a) == p)) { ok = true; break; }
      if (!ok && !s.probes.empty()) {               // a successful probe of the page covers it
        z3::expr covered = C().bool_val(false);
        for (const State::Probe &pr : s.probes)
          if (pr.write || !write) covered = covered || (pr.ok && page(a) == pr.page);
        ok = valid(pc, covered);
      }
      if (!ok) return true;
    }
    return false;
  }
  void touched(State &s, const z3::expr &addr, int n, bool write) {
    z3::expr last = addr + C().bv_val(n - 1, 32);
    for (const z3::expr &a : {addr, last}) {
      s.rd_pages.push_back(page(a));
      if (write) s.wr_pages.push_back(page(a));
    }
  }
  void access(State &s, const z3::expr &addr, int n, bool write, const char *what) {
    if (s.committed && can_fault(s, addr, n, write, C().bool_val(true))) violation(s, what);
    touched(s, addr, n, write);
  }
  // a conditional access (ld.c, st.c, stu.c): only where u holds; the page
  // counts as accessed only if it certainly happened
  void cond_access(State &s, const z3::expr &addr, int n, bool write, const z3::expr &u, const char *what) {
    if (s.committed && can_fault(s, addr, n, write, u)) violation(s, what);
    if (valid(s.pc, u)) touched(s, addr, n, write);
  }
  Sym load(State &s, const z3::expr &addr, int n) {
    z3::expr v = z3::select(s.mem, addr);
    for (int i = 1; i < n; i++) v = z3::concat(v, z3::select(s.mem, addr + C().bv_val(i, 32)));
    return Sym(n == 4 ? v : z3::zext(v, 32 - 8 * n), usym::U32);
  }
  // cond: a conditional store (st.c, stu.c) stores only under it (u)
  void store(State &s, const z3::expr &addr, int n, const Sym &val, const z3::expr *cond = nullptr) {
    z3::expr v = usym::conv(val, usym::U32).e;
    z3::expr old = load(s, addr, n).e;
    z3::expr nv = n == 4 ? v : z3::zext(v.extract(8 * n - 1, 0), 32 - 8 * n);
    z3::expr c = cond ? *cond : C().bool_val(true);
    if (cond) cond_access(s, addr, n, true, c, "conditional store");
    else access(s, addr, n, true, "store");
    commit(s, feasible(s.pc && c && old != nv));   // a provably silent (or skipped) store changes nothing
    z3::expr m = s.mem;
    for (int i = 0; i < n; i++)
      m = z3::store(m, addr + C().bv_val(i, 32), v.extract(8 * (n - 1 - i) + 7, 8 * (n - 1 - i)));
    s.mem = cond ? z3::ite(c, m, s.mem) : m;
  }

  void violation(State &s, const char *what) {
    res.paths_bad++;
    if (res.violations.size() >= 3) return;
    solver.push();
    solver.add(s.pc);
    std::string ce;
    if (solver.check() == z3::sat) {
      z3::model m = solver.get_model();
      char b[160];
      snprintf(b, sizeof b, "opword %04x ext %08x %08x",
               (unsigned)m.eval(op_var, true).get_numeral_uint64() & 0xffff,
               (unsigned)m.eval(ext1, true).get_numeral_uint64(), (unsigned)m.eval(ext2, true).get_numeral_uint64());
      ce = b;
    }
    solver.pop();
    char b[256];
    snprintf(b, sizeof b, "%s that can fault at millicode %05x after a guest-visible write at %05x (%s)", what,
             s.mpc, s.commit_mpc, ce.c_str());
    res.violations.push_back(b);
  }

  // ---- the symbolic semantics' hooks ----
  struct Sem : SymSem {
    Prover *p;
    State *s;
    Sym CSR_READ(const Sym &i) override { return p->csr_get(*s, idx(i)); }
    void CSR_WRITE(const Sym &i, const Sym &v) override {
      int k = idx(i);
      Sym nv = p->csr_get(*s, k);
      nv = v;   // guarded merge
      if (k < 0x40) p->commit(*s, p->may_differ(*s, p->csr_get(*s, k), nv));
      s->csr[k] = nv;
    }
    Sym GREG_READ(const Sym &i) override {
      Sym r = s->R[32];
      for (int k = 1; k < 32; k++) r.e = usym::ITE((usym::conv(i, usym::U32) & 31u) == (unsigned)k, s->R[32 + k], r).e;
      return r;
    }
    void GREG_WRITE(const Sym &i, const Sym &v) override {
      for (int k = 0; k < 32; k++) {
        Sym nv = s->R[32 + k];
        nv.e = usym::ITE((usym::conv(i, usym::U32) & 31u) == (unsigned)k, usym::conv(v, usym::U32), nv).e;
        Sym g = s->R[32 + k];
        g = nv;   // guarded
        if (k <= 17) p->commit(*s, p->may_differ(*s, s->R[32 + k], g));   // D0-A7, gpc, gsr
        s->R[32 + k] = g;
      }
    }
    void CSTACK_PUSH(const Sym &v) override {
      z3::expr e = v.e.simplify();
      if (!e.is_numeral()) { p->res.inconclusive = true; p->res.why = "symbolic return address"; return; }
      s->cstack.push_back((uint32_t)e.get_numeral_uint64());
    }
    Sym CSTACK_POP() override {
      if (s->cstack.empty()) { p->res.inconclusive = true; p->res.why = "return stack underflow"; return Sym(0u); }
      uint32_t v = s->cstack.back();
      s->cstack.pop_back();
      return Sym(v);
    }
    void GNPC_SET(const Sym &) override {}
    void GSTORE(int space, const Sym &addr, int n, const Sym &v) override {
      (void)space;
      p->store(*s, usym::conv(addr, usym::U32).e, n, v);
    }
    int idx(const Sym &i) {
      z3::expr e = usym::conv(i, usym::U32).e.simplify();
      if (!e.is_numeral()) { p->res.inconclusive = true; p->res.why = "symbolic CSR number"; return 0; }
      return (int)(e.get_numeral_uint64() & 0xff);
    }
  };

  void set_reg(State &s, int rd, const Sym &v) {
    if (rd == 0) return;
    Sym nv = usym::conv(v, usym::U32);
    if (rd >= 32 && rd <= 49) commit(s, may_differ(s, s.R[rd], nv));
    s.R[rd] = nv;
  }
  void set_flag(State &s, Sym &f, const Sym &v) {
    Sym nv = usym::conv(v, usym::B);
    commit(s, may_differ(s, f, nv));
    f = nv;
  }

  // run one millicode word; returns the possible next states
  void step(State s, std::vector<State> &out) {
    uint32_t i = (s.mpc - MC_BASE) / 4;
    if (i >= sizeof MC_IMAGE / sizeof MC_IMAGE[0]) { res.inconclusive = true; res.why = "ran off the image"; return; }
    dec_t d = decode(MC_IMAGE[i]);
    const opinfo &o = OPS[d.op];
    if (d.op == UISA_ILLEGAL) { res.inconclusive = true; res.why = "illegal millicode word"; return; }
    if (d.op == UISA_GEXIT) { res.paths_ok++; return; }
    if (d.op == UISA_GFAULT || d.op == UISA_GFAULTR) { res.paths_exc++; return; }
    if (d.op == UISA_BRK || d.op == UISA_HALT) { res.paths_brk++; return; }
    Sym a = s.R[d.rs1], immv((uint32_t)d.imm);
    Sym b = o.reads_rs2 ? s.R[d.rs2] : immv;
    uint32_t npc = s.mpc + 4;
    if (o.is_load || o.is_store || o.is_probe) {
      int sp = space_of(d.op);
      if (sp == 0) { res.inconclusive = true; res.why = "hidden-memory access"; return; }
      z3::expr addr = (usym::conv(a, usym::U32) + immv).e;
      int n = o.mem_bytes;
      if (o.is_probe) {
        Sym mmusr = fresh("mmusr", usym::U32);
        set_reg(s, d.rd, mmusr);
        if (o.writes_u) {                                  // .us: u = ok; .ua: u = u && ok
          bool w = !strncmp(o.mnem, "probe.w", 7);
          // a user-mode access: gd/gp in user state, ga with a user function code (DFC < 4)
          Sym user = sp == 3 ? !(usym::conv(csr_get(s, 2), usym::U32) & 4u)
                             : !(usym::conv(s.R[49], usym::U32) & 0x2000u);
          // tce: TC.E (with translation off, MMUSR 0 = untranslated = ok)
          Sym tce = (usym::conv(csr_get(s, 4), usym::U32) & 0x8000u) != Sym(0u);
          Sym ok = sym_probe_ok(mmusr, Sym(w), user, tce);
          s.U = o.reads_u ? usym::conv(s.U && ok, usym::B) : ok;
          s.probes.push_back({page(addr), ok.cond(), w});
        }
      } else if (o.reads_u) {                              // ld.c / st.c / stu.c: only if u
        z3::expr u = s.U.cond();
        if (!feasible(s.pc && u)) {                        // never happens on this path
          if (o.is_load) set_reg(s, d.rd, Sym(0u));
        } else if (o.is_load) {
          cond_access(s, addr, n, false, u, "conditional load");
          Sym v = load(s, addr, n);
          if (o.load_signed) v = usym::conv(usym::SX(v, Sym((unsigned)(8 * n))), usym::U32);
          set_reg(s, d.rd, usym::ITE(s.U, v, Sym(0u)));
        } else {
          store(s, addr, n, s.R[d.rd], &u);
        }
      } else if (o.is_load && o.is_store) {               // cas
        access(s, addr, n, false, "cas");
        Sym old = load(s, addr, n);
        uint32_t m = n == 4 ? 0xffffffffu : (1u << (8 * n)) - 1;
        z3::expr eq = old.e == (usym::conv(s.R[d.rs2], usym::U32) & m).e;
        z3::expr cur = load(s, addr, n).e;
        Sym val(z3::ite(eq, (usym::conv(s.R[d.rd], usym::U32) & m).e, cur), usym::U32);
        store(s, addr, n, val);
        set_reg(s, d.rd, old);
      } else if (o.is_load) {
        access(s, addr, n, false, "load");
        Sym v = load(s, addr, n);
        if (o.load_signed) v = usym::conv(usym::SX(v, Sym((unsigned)(8 * n))), usym::U32);
        set_reg(s, d.rd, v);
      } else if (d.op == UISA_GPUSHR) {
        Sym base = usym::conv(a, usym::U32) - 4u;
        store(s, base.e, 4, s.R[48] + immv);
        set_reg(s, d.rd, base);
      } else if (o.wb == 1) {
        store(s, addr, n, s.R[d.rs2]);
        set_reg(s, d.rd, Sym(addr, usym::U32));
      } else if (o.wb == 2) {
        store(s, usym::conv(a, usym::U32).e, n, s.R[d.rs2]);
        set_reg(s, d.rd, Sym(addr, usym::U32));
      } else {
        store(s, addr, n, s.R[d.rd]);
      }
    } else if (o.gform != GF_NONE) {
      SymGop g;
      sym_gop_eval(o.gfn, o.gsize_bits, a, b, s.X, s.N, s.Z, s.V, s.Cf, g);
      if (o.writes_rd) {
        Sym old = usym::conv(s.R[d.rd], usym::U32), r = usym::conv(g.r, usym::U32);
        Sym m = usym::MSK(Sym((unsigned)o.gsize_bits));
        Sym w = o.gform == GF_MERGE ? (old & ~m) | (r & m) : o.gform == GF_ZFILL ? r & m : r;
        set_reg(s, d.rd, w);
      }
      if (o.flags_on) {
        if (o.writes_nzvc) { set_flag(s, s.N, g.N); set_flag(s, s.Z, g.Z); set_flag(s, s.V, g.V); set_flag(s, s.Cf, g.C); }
        if (o.writes_x) set_flag(s, s.X, g.X);
      }
    } else {
      Sem sem;
      sem.p = this;
      sem.s = &s;
      sem.a = a; sem.b = b; sem.IMM = immv; sem.OLD = s.R[d.rd]; sem.r = Sym(0u);
      sem.pc = Sym(s.mpc); sem.npc = Sym(npc); sem.GPC_INSN = s.R[48];
      sem.xin = sem.X = s.X; sem.nin = sem.N = s.N; sem.zin = sem.Z = s.Z; sem.vin = sem.V = s.V;
      sem.cin = sem.C = s.Cf;
      sem.uin = sem.U = s.U;
      if (!sem.base(d.op)) { res.inconclusive = true; res.why = std::string("no semantics for ") + o.mnem; return; }
      if (o.writes_rd) set_reg(s, d.rd, sem.r);
      set_flag(s, s.X, sem.X); set_flag(s, s.N, sem.N); set_flag(s, s.Z, sem.Z);
      set_flag(s, s.V, sem.V); set_flag(s, s.Cf, sem.C);
      s.U = usym::conv(sem.U, usym::B);                  // millicode-private: never a commit
      // the next millicode address: fork over its feasible values
      z3::expr target = usym::conv(sem.npc, usym::U32).e.simplify();
      if (!target.is_numeral()) {
        int forks = 0;
        z3::expr seen = C().bool_val(false);
        solver.push();
        solver.add(s.pc);
        while (solver.check() == z3::sat && forks < 64) {
          z3::model m = solver.get_model();
          z3::expr v = m.eval(target, true);
          State t = s;
          t.pc = s.pc && target == v;
          t.mpc = (uint32_t)v.get_numeral_uint64();
          t.steps++;
          out.push_back(t);
          solver.add(target != v);
          forks++;
        }
        solver.pop();
        if (forks >= 64) { res.inconclusive = true; res.why = "too many branch targets"; }
        return;
      }
      npc = (uint32_t)target.get_numeral_uint64();
    }
    s.mpc = npc;
    s.steps++;
    out.push_back(s);
  }

  // merge b into a (same mpc, return stack and commit status)
  void merge(State &a, const State &b) {
    z3::expr c = a.pc;
    auto m = [&](Sym &x, const Sym &y) {
      Sym yy = usym::conv(y, x.t);
      if (!z3::eq(x.e, yy.e)) x.e = z3::ite(c, x.e, yy.e);
    };
    for (int i = 0; i < 64; i++) m(a.R[i], b.R[i]);
    m(a.X, b.X); m(a.N, b.N); m(a.Z, b.Z); m(a.V, b.V); m(a.Cf, b.Cf); m(a.U, b.U);
    for (auto &kv : b.csr) {
      Sym x = a.csr.count(kv.first) ? a.csr[kv.first] : csr_get(a, kv.first);
      m(x, kv.second);
      a.csr[kv.first] = x;
    }
    for (auto &kv : a.csr)
      if (!b.csr.count(kv.first)) { Sym y = csr0.at(kv.first); m(kv.second, y); }
    if (!z3::eq(a.mem, b.mem)) a.mem = z3::ite(c, a.mem, b.mem);
    auto keep = [](std::vector<z3::expr> &x, const std::vector<z3::expr> &y) {
      std::vector<z3::expr> out;
      for (auto &e : x)
        for (auto &f : y)
          if (z3::eq(e, f)) { out.push_back(e); break; }
      x = out;
    };
    keep(a.rd_pages, b.rd_pages);
    keep(a.wr_pages, b.wr_pages);
    {   // probes of both paths: each one's success counts only on its own path
      std::vector<State::Probe> pr;
      for (const State::Probe &x : a.probes) pr.push_back({x.page, c && x.ok, x.write});
      for (const State::Probe &x : b.probes) pr.push_back({x.page, !c && x.ok, x.write});
      a.probes = pr;
    }
    a.pc = a.pc || b.pc;
    a.steps = std::max(a.steps, b.steps);
  }

  void run(uint32_t entry, const std::vector<uint16_t> &ops) {
    res = Result();
    solver.reset();
    csr0.clear();
    State s0;
    s0.mpc = MC_BASE + entry;
    for (int i = 0; i < 64; i++) {
      char b[16];
      snprintf(b, sizeof b, "r%d", i);
      s0.R[i] = Sym::var(b, usym::U32);
    }
    s0.R[0] = Sym(0u);
    s0.R[1] = Sym(op_var, usym::U32);
    s0.R[2] = Sym(ext1, usym::U32);
    s0.R[3] = Sym(ext2, usym::U32);
    z3::expr opc = C().bool_val(false);
    for (uint16_t op : ops) opc = opc || op_var == C().bv_val(op, 32);
    z3::expr gsr = s0.R[49].e;
    s0.pc = opc && ((gsr & C().bv_val(0xffff00ffu, 32)) == C().bv_val(0, 32));
    s0.X = Sym::var("X0", usym::B); s0.N = Sym::var("N0", usym::B); s0.Z = Sym::var("Z0", usym::B);
    s0.V = Sym::var("V0", usym::B); s0.Cf = Sym::var("C0", usym::B);
    s0.U = Sym::var("U0", usym::B);
    s0.mem = C().constant("mem", C().array_sort(C().bv_sort(32), C().bv_sort(8)));

    // worklist ordered by millicode address; merge states that meet
    std::multimap<uint32_t, State> work;
    work.emplace(s0.mpc, s0);
    std::map<uint32_t, int> visits;
    long total = 0;
    while (!work.empty() && !res.inconclusive) {
      auto it = work.begin();
      State s = it->second;
      work.erase(it);
      if (++visits[s.mpc] > 400 || ++total > 200000) { res.inconclusive = true; res.why = "loop bound"; break; }
      std::vector<State> next;
      step(s, next);
      for (State &t : next) {
        if (!feasible(t.pc)) continue;
        bool merged = false;
        auto r = work.equal_range(t.mpc);
        for (auto j = r.first; j != r.second; ++j)
          if (j->second.cstack == t.cstack && j->second.committed == t.committed) {
            merge(j->second, t);
            merged = true;
            break;
          }
        if (!merged) work.emplace(t.mpc, t);
      }
    }
  }
};

int main(int argc, char **argv) {
  std::set<std::string> only;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-v")) verbose = true;
    else only.insert(argv[i]);
  }
  // routine names from the millicode map
  std::map<uint32_t, std::string> names;
  {
    std::ifstream f(std::string(UISA_BUILD) + "/uisa_millicode.map");
    std::string line;
    while (std::getline(f, line)) {
      std::istringstream ss(line);
      std::string addr, name;
      if (ss >> addr >> name) names.emplace((uint32_t)strtoul(addr.c_str(), nullptr, 16), name);
    }
  }
  std::map<int32_t, std::vector<uint16_t>> routines;
  for (int op = 0; op < 65536; op++)
    if (MC_ENTRY[op] >= 0) routines[MC_ENTRY[op]].push_back((uint16_t)op);
  Prover p;
  int proved = 0, bad = 0, unknown = 0;
  for (auto &kv : routines) {
    std::string name = names.count(kv.first) ? names[kv.first] : "?";
    if (!only.empty() && !only.count(name)) continue;
    p.routine = name;
    p.run((uint32_t)kv.first, kv.second);
    const Result &r = p.res;
    const char *verdict = r.inconclusive ? "INCONCLUSIVE" : r.paths_bad ? "VIOLATION" : "proved";
    if (r.inconclusive) unknown++;
    else if (r.paths_bad) bad++;
    else proved++;
    printf("%-16s %5zu opwords  %-12s  paths: %d exit, %d exception, %d unhandled%s%s\n", name.c_str(),
           kv.second.size(), verdict, r.paths_ok, r.paths_exc, r.paths_brk,
           r.inconclusive ? "  -- " : "", r.inconclusive ? r.why.c_str() : "");
    for (const std::string &v : r.violations) printf("    %s\n", v.c_str());
  }
  printf("prove_pnr: %d proved, %d with violations, %d inconclusive\n", proved, bad, unknown);
  return bad || unknown ? 1 : 0;
}
