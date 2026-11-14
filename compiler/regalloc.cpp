/* regalloc.cpp: whole-program register allocation with z3.
 *
 * Millicode never spills, so allocation is a pure assignment problem: give
 * every SSA value of every function one of the x registers such that
 *
 *   - interfering values (same function) get different registers,
 *   - entry-ABI values keep the register the hardware puts them in,
 *   - no value live across a call sits in a register the callee, or anything
 *     it calls, writes (used[g][r] collects those registers per function),
 *
 * and, among those assignments, minimize the weighted moves that remain:
 * phi copies, copies, tied operands, call arguments/results and returns.
 * Each value has one boolean per register (exactly one true); z3's MaxSAT
 * engine optimizes the soft "same register" constraints. Interference of
 * SSA values is chordal per function, so the per-function pressure check in
 * backend.cpp already rules out the infeasible cases it can see; z3 settles
 * the whole-program ones and proves optimality of the move cost.
 */
#include <z3++.h>

#include <map>

#include "backend.h"
#include "errors.h"

void allocateRegisters(Program *p, AllocProblem &ap, int timeoutMs, bool verbose) {
  const int K = numColors();   // colors 1..K are x1..xK
  z3::context c;
  z3::optimize opt(c);
  z3::params prm(c);
  prm.set("timeout", (unsigned)timeoutMs);
  opt.set(prm);

  std::map<Reg *, std::vector<z3::expr>> B;
  for (Reg *v : ap.vals) {
    z3::expr_vector vs(c);
    std::vector<z3::expr> bits;
    for (int k = 1; k <= K; k++) {
      z3::expr e = c.bool_const(("b" + std::to_string((size_t)v) + "_" + std::to_string(k)).c_str());
      bits.push_back(e);
      vs.push_back(e);
    }
    opt.add(z3::mk_or(vs));
    opt.add(z3::atmost(vs, 1));
    if (v->precolor >= 0) opt.add(bits[v->precolor - 1]);
    B.emplace(v, bits);
  }
  for (auto &e : ap.edges)
    for (int k = 0; k < K; k++) opt.add(!B.at(e.first)[k] || !B.at(e.second)[k]);

  // used[g][r]: g, or a function it calls, writes x(r+1)
  std::map<Function *, std::vector<z3::expr>> U;
  for (Function *f : p->funcs) {
    if (f->isEntry) continue;
    std::vector<z3::expr> u;
    for (int k = 1; k <= K; k++) u.push_back(c.bool_const(("u_" + f->name + "_" + std::to_string(k)).c_str()));
    U.emplace(f, u);
  }
  for (auto &fu : U) {
    Function *f = fu.first;
    for (Reg *v : ap.fvals[f])
      for (int k = 0; k < K; k++) opt.add(z3::implies(B.at(v)[k], fu.second[k]));
    for (Function *g : f->callees)
      for (int k = 0; k < K; k++) opt.add(z3::implies(U.at(g)[k], fu.second[k]));
  }
  for (auto &cs : ap.calls)
    for (Reg *v : cs.liveAcross)
      for (int k = 0; k < K; k++) opt.add(!U.at(cs.callee)[k] || !B.at(v)[k]);

  // soft: preferred pairs share a register
  std::map<std::pair<Reg *, Reg *>, int> prefs;
  for (auto &pr : ap.prefs) {
    if (pr.a == pr.b || !B.count(pr.a) || !B.count(pr.b)) continue;
    auto key = pr.a < pr.b ? std::make_pair(pr.a, pr.b) : std::make_pair(pr.b, pr.a);
    prefs[key] += pr.weight;
  }
  int n = 0;
  for (auto &pw : prefs) {
    z3::expr s = c.bool_const(("s" + std::to_string(n++)).c_str());
    for (int k = 0; k < K; k++) opt.add(z3::implies(s, B.at(pw.first.first)[k] == B.at(pw.first.second)[k]));
    opt.add_soft(s, (unsigned)pw.second);
  }

  z3::check_result r = opt.check();
  if (r == z3::unsat) {
    error(0, "register allocation is infeasible: the values live across some call do not fit beside the "
             "registers the callee chain needs (millicode cannot spill; reduce live values or inline)");
    return;
  }
  z3::model m(c);
  if (r == z3::sat) {
    m = opt.get_model();
  } else {
    // timed out: settle for any valid assignment
    if (verbose) fprintf(stderr, "ucc: z3 optimization timed out; using a feasible allocation\n");
    z3::solver s(c);
    for (const z3::expr &a : opt.assertions()) s.add(a);
    if (s.check() != z3::sat) {
      error(0, "register allocation failed: z3 found no assignment within the time limit");
      return;
    }
    m = s.get_model();
  }
  for (Reg *v : ap.vals) {
    v->color = -1;
    for (int k = 0; k < K; k++)
      if (m.eval(B.at(v)[k], true).is_true()) v->color = k + 1;
  }
  if (verbose) {
    int missed = 0, total = 0;
    for (auto &pw : prefs) {
      total += pw.second;
      if (pw.first.first->color != pw.first.second->color) missed += pw.second;
    }
    fprintf(stderr, "ucc: %zu values, %zu interference edges, %zu call sites; move cost %d of %d (%s)\n",
            ap.vals.size(), ap.edges.size(), ap.calls.size(), missed, total, r == z3::sat ? "optimal" : "not proven");
  }
}
