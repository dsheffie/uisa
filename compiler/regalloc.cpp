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
 *
 * z3 starts from a greedy allocation (greedyAllocate): functions callees
 * first, values in dominance order, each value taking a free register its
 * already-colored preference partners use. Per function that is the classic
 * chordal-graph coloring, so it succeeds whenever the pressure check passed,
 * unless a call-clobber constraint gets in the way. When z3 runs out of its
 * (deterministic) resource budget, the greedy allocation is used as is, so
 * the output never depends on the speed of the machine.
 */
#include <z3++.h>

#include <functional>
#include <map>
#include <set>

#include "backend.h"
#include "errors.h"

static int moveCost(const std::map<std::pair<Reg *, Reg *>, int> &prefs, int *total) {
  int missed = 0;
  if (total) *total = 0;
  for (auto &pw : prefs) {
    if (total) *total += pw.second;
    if (pw.first.first->color != pw.first.second->color) missed += pw.second;
  }
  return missed;
}

// Greedy preference-biased coloring; false if some value finds no register.
static bool greedyAllocate(Program *p, AllocProblem &ap, const std::map<std::pair<Reg *, Reg *>, int> &prefs) {
  const int K = numColors();
  std::map<Reg *, std::vector<Reg *>> adj;
  for (auto &e : ap.edges) {
    adj[e.first].push_back(e.second);
    adj[e.second].push_back(e.first);
  }
  std::map<Reg *, std::vector<std::pair<Reg *, int>>> partners;
  for (auto &pw : prefs) {
    partners[pw.first.first].push_back({pw.first.second, pw.second});
    partners[pw.first.second].push_back({pw.first.first, pw.second});
  }
  std::map<Reg *, std::set<Function *>> across;   // callees a value is live across
  for (auto &cs : ap.calls)
    for (Reg *v : cs.liveAcross) across[v].insert(cs.callee);
  for (Reg *v : ap.vals) v->color = -1;

  std::map<Function *, std::set<int>> used;   // registers f and its callees write
  std::set<Function *> done;
  bool ok = true;
  std::function<void(Function *)> visit = [&](Function *f) {
    if (done.count(f)) return;
    done.insert(f);
    for (Function *g : f->callees) visit(g);
    // dominance order: entry values, then each block's definitions in RPO
    std::vector<Reg *> order(f->entryRegs.begin(), f->entryRegs.end());
    order.insert(order.end(), f->params.begin(), f->params.end());
    std::vector<BasicBlock *> rpo;
    std::set<BasicBlock *> seen;
    std::function<void(BasicBlock *)> dfs = [&](BasicBlock *b) {
      seen.insert(b);
      for (BasicBlock *s : b->succs)
        if (!seen.count(s)) dfs(s);
      rpo.push_back(b);
    };
    dfs(f->blocks[0]);
    for (auto it = rpo.rbegin(); it != rpo.rend(); ++it)
      for (Instr *i : (*it)->ins)
        if (i->dst) order.push_back(i->dst);
    order.insert(order.end(), ap.fvals[f].begin(), ap.fvals[f].end());   // anything left (return registers)
    std::set<int> &u = used[f];
    for (Function *g : f->callees) u.insert(used[g].begin(), used[g].end());
    for (Reg *v : order) {
      if (v->color > 0) continue;
      std::vector<bool> bad(K + 1, false);
      for (Reg *w : adj[v])
        if (w->color > 0) bad[w->color] = true;
      for (Function *g : across[v])
        for (int r : used[g]) bad[r] = true;
      int best = -1;
      if (v->precolor >= 0) {
        best = bad[v->precolor] ? -1 : v->precolor;
      } else {
        // preference partners first; then a register f already writes (keeps
        // the clobber set callers must avoid small); then from the top in
        // helpers, so they stay clear of the entry-ABI registers x1-x4 that
        // callers keep live across calls
        std::vector<int> score(K + 1, 0);
        for (auto &pw : partners[v])
          if (pw.first->color > 0) score[pw.first->color] += 2 * pw.second;
        for (int r : u) score[r] += 1;
        for (int n = 0; n < K; n++) {
          int r = f->isEntry ? n + 1 : K - n;
          if (!bad[r] && (best < 0 || score[r] > score[best])) best = r;
        }
      }
      if (best < 0) {
        ok = false;
        return;
      }
      v->color = best;
      u.insert(best);
    }
  };
  for (Function *f : p->funcs) {
    visit(f);
    if (!ok) return false;
  }
  // local improvement: move single values to a register more of their
  // partners use. A helper's value only moves to a register the helper
  // already writes, so no caller's live-across constraint can break.
  std::map<Reg *, Function *> owner;
  for (auto &fv : ap.fvals)
    for (Reg *v : fv.second) owner[v] = fv.first;
  for (int round = 0; round < 20; round++) {
    bool improved = false;
    for (Reg *v : ap.vals) {
      if (v->precolor >= 0 || !partners.count(v)) continue;
      Function *f = owner[v];
      std::vector<int> score(K + 1, 0);
      for (auto &pw : partners[v])
        if (pw.first->color > 0) score[pw.first->color] += pw.second;
      std::vector<bool> bad(K + 1, false);
      for (Reg *w : adj[v]) bad[w->color] = true;
      for (Function *g : across[v])
        for (int r : used[g]) bad[r] = true;
      int best = v->color;
      for (int r = 1; r <= K; r++) {
        if (bad[r] || (!f->isEntry && !used[f].count(r))) continue;
        if (score[r] > score[best]) best = r;
      }
      if (best != v->color) {
        v->color = best;
        improved = true;
      }
    }
    if (!improved) break;
  }
  return true;
}

void allocateRegisters(Program *p, AllocProblem &ap, unsigned rlimit, bool verbose) {
  const int K = numColors();   // colors 1..K are x1..xK
  z3::context c;
  z3::optimize opt(c);
  z3::params prm(c);
  prm.set("rlimit", rlimit);
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

  int total = 0;
  bool haveGreedy = greedyAllocate(p, ap, prefs);
  int greedyCost = haveGreedy ? moveCost(prefs, &total) : -1;
  std::map<Reg *, int> greedyColor;
  if (haveGreedy)
    for (Reg *v : ap.vals) {
      greedyColor[v] = v->color;
      for (int k = 0; k < K; k++) opt.set_initial_value(B.at(v)[k], v->color == k + 1);
    }

  z3::check_result r = opt.check();
  if (r == z3::unknown && haveGreedy) {
    for (Reg *v : ap.vals) v->color = greedyColor[v];
    if (verbose)
      fprintf(stderr, "ucc: %zu values, %zu interference edges, %zu call sites; z3 budget exhausted, greedy move cost %d of %d\n",
              ap.vals.size(), ap.edges.size(), ap.calls.size(), greedyCost, total);
    return;
  }
  if (r == z3::unsat) {
    error(0, "register allocation is infeasible: the values live across some call do not fit beside the "
             "registers the callee chain needs (millicode cannot spill; reduce live values or inline)");
    return;
  }
  z3::model m(c);
  if (r == z3::sat) {
    m = opt.get_model();
  } else {
    // out of budget and no greedy allocation: settle for any valid assignment
    if (verbose) fprintf(stderr, "ucc: z3 budget exhausted; using a feasible allocation\n");
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
    int missed = moveCost(prefs, &total);
    z3::stats st = opt.statistics();
    double used = 0;
    for (unsigned k = 0; k < st.size(); k++)
      if (st.key(k) == "rlimit count") used = st.is_uint(k) ? st.uint_value(k) : st.double_value(k);
    fprintf(stderr, "ucc: %zu values, %zu interference edges, %zu call sites; move cost %d of %d (%s; greedy %d; rlimit %.0f)\n",
            ap.vals.size(), ap.edges.size(), ap.calls.size(), missed, total, r == z3::sat ? "optimal" : "not proven",
            greedyCost, used);
  }
}
