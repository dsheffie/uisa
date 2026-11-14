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

#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "backend.h"
#include "errors.h"

using PairCmp = std::function<bool(const std::pair<Reg *, Reg *> &, const std::pair<Reg *, Reg *> &)>;
using PrefMap = std::map<std::pair<Reg *, Reg *>, int, PairCmp>;

static int moveCost(const PrefMap &prefs, int *total) {
  int missed = 0;
  if (total) *total = 0;
  for (auto &pw : prefs) {
    if (total) *total += pw.second;
    if (pw.first.first->color != pw.first.second->color) missed += pw.second;
  }
  return missed;
}

// Greedy preference-biased coloring; false if some value finds no register.
static bool greedyAllocate(Program *p, AllocProblem &ap, const PrefMap &prefs) {
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

// The pieces of phase 2: loop discovery, neighbourhood growth, and a small,
// exact z3 problem over one neighbourhood with every other value fixed.
struct Neighbourhoods {
  Program *p;
  AllocProblem &ap;
  const PrefMap &prefs;
  const std::map<Reg *, int> &idx;
  int K = numColors();
  std::map<Reg *, std::vector<Reg *>> adj, partners;
  std::map<Reg *, Function *> owner;
  // call-clobber pairs: a value live across a call never shares a register
  // with any value of the callee or its callees
  std::map<Reg *, std::vector<Reg *>> clobber;
  std::vector<std::vector<Reg *>> loops;   // values of each natural loop, innermost first

  Neighbourhoods(Program *p, AllocProblem &ap, const PrefMap &prefs, const std::map<Reg *, int> &idx)
      : p(p), ap(ap), prefs(prefs), idx(idx) {
    auto byIdx = [&](Reg *a, Reg *b) { return idx.at(a) < idx.at(b); };
    for (auto &e : ap.edges) {
      adj[e.first].push_back(e.second);
      adj[e.second].push_back(e.first);
    }
    for (auto &a : adj) std::sort(a.second.begin(), a.second.end(), byIdx);
    for (auto &pw : prefs) {
      partners[pw.first.first].push_back(pw.first.second);
      partners[pw.first.second].push_back(pw.first.first);
    }
    for (auto &fv : ap.fvals)
      for (Reg *v : fv.second) owner[v] = fv.first;
    std::map<Function *, std::vector<Reg *>> closureVals;
    std::function<void(Function *, Function *, std::set<Function *> &)> close =
        [&](Function *root, Function *f, std::set<Function *> &seen) {
          if (!seen.insert(f).second) return;
          for (Reg *v : ap.fvals[f]) closureVals[root].push_back(v);
          for (Function *g : f->callees) close(root, g, seen);
        };
    for (Function *f : p->funcs) {
      std::set<Function *> seen;
      close(f, f, seen);
    }
    std::map<Reg *, std::set<Reg *, decltype(byIdx)>> cl;
    for (auto &cs : ap.calls)
      for (Reg *v : cs.liveAcross)
        for (Reg *w : closureVals[cs.callee]) {
          cl.emplace(v, std::set<Reg *, decltype(byIdx)>(byIdx)).first->second.insert(w);
          cl.emplace(w, std::set<Reg *, decltype(byIdx)>(byIdx)).first->second.insert(v);
        }
    for (auto &c : cl) clobber[c.first].assign(c.second.begin(), c.second.end());
    findLoops();
  }

  // Natural loops from DFS back edges (back edges to one header make one
  // loop). A loop's values are those defined or used in its own blocks: its
  // body minus the bodies of loops nested in it, which are settled first.
  // Deepest first, then program order.
  void findLoops() {
    std::vector<std::pair<int, std::vector<Reg *>>> found;
    for (Function *f : p->funcs) {
      std::map<BasicBlock *, std::set<BasicBlock *>> bodies;   // by header
      std::map<BasicBlock *, int> state;   // 1 on the DFS stack, 2 done
      std::vector<std::pair<BasicBlock *, BasicBlock *>> back;
      std::function<void(BasicBlock *)> dfs = [&](BasicBlock *b) {
        state[b] = 1;
        for (BasicBlock *s : b->succs) {
          if (state[s] == 1) back.push_back({b, s});
          else if (!state[s]) dfs(s);
        }
        state[b] = 2;
      };
      dfs(f->blocks[0]);
      for (auto &be : back) {
        std::set<BasicBlock *> &body = bodies[be.second];
        body.insert(be.second);
        std::vector<BasicBlock *> work = {be.first};
        while (!work.empty()) {
          BasicBlock *b = work.back();
          work.pop_back();
          if (!body.insert(b).second) continue;
          for (BasicBlock *q : b->preds) work.push_back(q);
        }
      }
      for (BasicBlock *h : f->blocks) {   // program order of headers
        if (!bodies.count(h)) continue;
        const std::set<BasicBlock *> &body = bodies[h];
        std::set<BasicBlock *> own = body;
        for (auto &ob : bodies)
          if (ob.first != h && body.count(ob.first) && ob.second.size() < body.size())
            for (BasicBlock *b : ob.second) own.erase(b);
        std::set<Reg *> seen;
        std::vector<Reg *> vals;
        auto add = [&](Reg *v) {
          if (idx.count(v) && seen.insert(v).second) vals.push_back(v);
        };
        for (BasicBlock *b : f->blocks) {   // program order
          if (!own.count(b)) continue;
          for (Instr *i : b->ins) {
            if (i->dst) add(i->dst);
            for (auto &o : i->a)
              if (o.isReg()) add(o.r);
          }
        }
        found.push_back({h->loopDepth, vals});
      }
    }
    std::stable_sort(found.begin(), found.end(), [](auto &a, auto &b) { return a.first > b.first; });
    for (auto &l : found) loops.push_back(l.second);
  }

  // seeds, then their preference webs, then one hop of interference
  std::vector<Reg *> around(const std::vector<Reg *> &seeds, size_t cap, bool grow = true) {
    std::vector<Reg *> N;
    std::set<Reg *> in;
    auto add = [&](Reg *v) {
      if (N.size() < cap && v->precolor < 0 && in.insert(v).second) N.push_back(v);
    };
    for (Reg *v : seeds) add(v);
    if (!grow) return N;
    for (size_t i = 0; i < N.size() && N.size() < cap * 2 / 3; i++)
      for (Reg *w : partners[N[i]]) add(w);
    for (size_t i = 0, n = N.size(); i < n; i++)
      for (Reg *w : adj[N[i]]) add(w);
    return N;
  }

  // Optimal colors for N with everything else fixed at cur; false if z3 ran
  // out of budget.
  bool solve(const std::vector<Reg *> &N, const std::map<Reg *, int> &cur, double budget,
             std::map<Reg *, int> *out, double *used) {
    std::set<Reg *> in(N.begin(), N.end());
    z3::context c;
    z3::optimize o(c);
    z3::params q(c);
    q.set("rlimit", (unsigned)std::max(1.0, budget));
    o.set(q);
    std::map<Reg *, std::vector<z3::expr>> B;
    for (Reg *v : N) {
      std::vector<bool> ok(K + 1, true);
      for (Reg *w : adj[v])
        if (!in.count(w)) ok[cur.at(w)] = false;
      for (Reg *w : clobber[v])
        if (!in.count(w)) ok[cur.at(w)] = false;
      z3::expr_vector vs(c);
      std::vector<z3::expr> bits;
      for (int k = 1; k <= K; k++) {
        z3::expr e = c.bool_const(("n" + std::to_string(idx.at(v)) + "_" + std::to_string(k)).c_str());
        bits.push_back(e);
        if (ok[k]) vs.push_back(e);
        else o.add(!e);
      }
      if (vs.empty()) return false;   // cannot happen: cur[v] is allowed
      o.add(z3::mk_or(vs));
      o.add(z3::atmost(vs, 1));
      B.emplace(v, bits);
    }
    auto differ = [&](Reg *a, Reg *b) {
      for (int k = 0; k < K; k++) o.add(!B.at(a)[k] || !B.at(b)[k]);
    };
    for (Reg *v : N) {
      for (Reg *w : adj[v])
        if (in.count(w) && idx.at(v) < idx.at(w)) differ(v, w);
      for (Reg *w : clobber[v])
        if (in.count(w) && idx.at(v) < idx.at(w)) differ(v, w);
    }
    int n = 0;
    for (auto &pw : prefs) {
      Reg *a = pw.first.first, *b = pw.first.second;
      bool ia = in.count(a), ib = in.count(b);
      if (!ia && !ib) continue;
      if (ia && ib) {
        z3::expr sv = c.bool_const(("s" + std::to_string(n++)).c_str());
        for (int k = 0; k < K; k++) o.add(z3::implies(sv, B.at(a)[k] == B.at(b)[k]));
        o.add_soft(sv, (unsigned)pw.second);
      } else {
        Reg *v = ia ? a : b, *w = ia ? b : a;
        o.add_soft(B.at(v)[cur.at(w) - 1], (unsigned)pw.second);
      }
    }
    for (Reg *v : N)
      for (int k = 0; k < K; k++) o.set_initial_value(B.at(v)[k], cur.at(v) == k + 1);
    z3::check_result r = o.check();
    z3::stats st = o.statistics();
    *used = 0;
    for (unsigned k = 0; k < st.size(); k++)
      if (st.key(k) == "rlimit count") *used = st.is_uint(k) ? st.uint_value(k) : st.double_value(k);
    if (r != z3::sat) return false;
    z3::model m = o.get_model();
    for (Reg *v : N)
      for (int k = 0; k < K; k++)
        if (m.eval(B.at(v)[k], true).is_true()) (*out)[v] = k + 1;
    return true;
  }
};

void allocateRegisters(Program *p, AllocProblem &ap, unsigned rlimit, bool verbose) {
  const int K = numColors();   // colors 1..K are x1..xK
  z3::context c;
  z3::optimize opt(c);
  z3::params prm(c);
  prm.set("rlimit", rlimit);
  opt.set(prm);

  // everything below is keyed and ordered by a value's index in ap.vals,
  // never by pointer, so z3 sees the same problem on every run
  std::map<Reg *, int> idx;
  for (Reg *v : ap.vals) idx[v] = (int)idx.size();
  auto byIdx = [&](Reg *a, Reg *b) { return idx.at(a) < idx.at(b); };
  std::map<Reg *, std::vector<z3::expr>> B;
  for (Reg *v : ap.vals) {
    z3::expr_vector vs(c);
    std::vector<z3::expr> bits;
    for (int k = 1; k <= K; k++) {
      z3::expr e = c.bool_const(("b" + std::to_string(idx.at(v)) + "_" + std::to_string(k)).c_str());
      bits.push_back(e);
      vs.push_back(e);
    }
    opt.add(z3::mk_or(vs));
    opt.add(z3::atmost(vs, 1));
    if (v->precolor >= 0) opt.add(bits[v->precolor - 1]);
    B.emplace(v, bits);
  }
  std::vector<std::pair<Reg *, Reg *>> edges(ap.edges.begin(), ap.edges.end());
  for (auto &e : edges)
    if (byIdx(e.second, e.first)) std::swap(e.first, e.second);
  std::sort(edges.begin(), edges.end(), [&](const std::pair<Reg *, Reg *> &x, const std::pair<Reg *, Reg *> &y) {
    return idx.at(x.first) != idx.at(y.first) ? idx.at(x.first) < idx.at(y.first) : idx.at(x.second) < idx.at(y.second);
  });
  for (auto &e : edges)
    for (int k = 0; k < K; k++) opt.add(!B.at(e.first)[k] || !B.at(e.second)[k]);

  // used[g][r]: g, or a function it calls, writes x(r+1)
  std::map<Function *, std::vector<z3::expr>> U;
  for (Function *f : p->funcs) {
    if (f->isEntry) continue;
    std::vector<z3::expr> u;
    for (int k = 1; k <= K; k++) u.push_back(c.bool_const(("u_" + f->name + "_" + std::to_string(k)).c_str()));
    U.emplace(f, u);
  }
  for (Function *f : p->funcs) {   // program order, not pointer order
    if (f->isEntry) continue;
    for (Reg *v : ap.fvals[f])
      for (int k = 0; k < K; k++) opt.add(z3::implies(B.at(v)[k], U.at(f)[k]));
    std::vector<Function *> callees(f->callees.begin(), f->callees.end());
    std::sort(callees.begin(), callees.end(), [](Function *a, Function *b) { return a->name < b->name; });
    for (Function *g : callees)
      for (int k = 0; k < K; k++) opt.add(z3::implies(U.at(g)[k], U.at(f)[k]));
  }
  for (auto &cs : ap.calls)
    for (Reg *v : cs.liveAcross)
      for (int k = 0; k < K; k++) opt.add(!U.at(cs.callee)[k] || !B.at(v)[k]);

  // soft: preferred pairs share a register
  PrefMap prefs([&](const std::pair<Reg *, Reg *> &x, const std::pair<Reg *, Reg *> &y) {
    int a = idx.at(x.first), b = idx.at(y.first);
    return a != b ? a < b : idx.at(x.second) < idx.at(y.second);
  });
  for (auto &pr : ap.prefs) {
    if (pr.a == pr.b || !B.count(pr.a) || !B.count(pr.b)) continue;
    auto key = byIdx(pr.a, pr.b) ? std::make_pair(pr.a, pr.b) : std::make_pair(pr.b, pr.a);
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
  std::map<Reg *, int> cur;   // the best allocation so far
  if (haveGreedy)
    for (Reg *v : ap.vals) {
      cur[v] = v->color;
      for (int k = 0; k < K; k++) opt.set_initial_value(B.at(v)[k], v->color == k + 1);
    }

  // z3's resource counter, read before and after each check: deterministic
  auto spentSoFar = [&]() -> double {
    z3::stats st = opt.statistics();
    for (unsigned k = 0; k < st.size(); k++)
      if (st.key(k) == "rlimit count") return st.is_uint(k) ? st.uint_value(k) : st.double_value(k);
    return 0;
  };
  auto check = [&](double budget) {
    z3::params q(c);
    q.set("rlimit", (unsigned)std::max(1.0, budget));
    opt.set(q);
    return opt.check();
  };
  auto take = [&](const z3::model &m, const std::vector<Reg *> &vs) {
    for (Reg *v : vs) {
      v->color = -1;
      for (int k = 0; k < K; k++)
        if (m.eval(B.at(v)[k], true).is_true()) v->color = k + 1;
    }
  };

  // Phase 1: the whole problem, optimally, if a third of the budget suffices
  // (it does for programs up to a few hundred values; larger ones go
  // straight to the neighbourhood search).
  double base = spentSoFar();
  z3::check_result r = ap.vals.size() <= 700 ? check(rlimit / 3.0) : z3::unknown;
  if (r == z3::unsat) {
    error(0, "register allocation is infeasible: the values live across some call do not fit beside the "
             "registers the callee chain needs (millicode cannot spill; reduce live values or inline)");
    return;
  }
  if (r == z3::sat) {
    take(opt.get_model(), ap.vals);
    if (verbose)
      fprintf(stderr, "ucc: %zu values, %zu interference edges, %zu call sites; move cost %d of %d (optimal; greedy %d)\n",
              ap.vals.size(), ap.edges.size(), ap.calls.size(), moveCost(prefs, &total), total, greedyCost);
    return;
  }
  if (!haveGreedy) {
    // no starting point: any valid assignment
    z3::solver sv(c);
    for (const z3::expr &a : opt.assertions()) sv.add(a);
    if (sv.check() != z3::sat) {
      error(0, "register allocation failed: z3 found no assignment within its budget");
      return;
    }
    take(sv.get_model(), ap.vals);
    for (Reg *v : ap.vals) cur[v] = v->color;
  }

  // Phase 2: large-neighbourhood search (solveNeighbourhood). Loops first,
  // innermost first, so the moves that execute most are settled optimally
  // given their surroundings; then the values around the heaviest remaining
  // moves, until nothing improves or the budget is spent.
  Neighbourhoods nb(p, ap, prefs, idx);
  double spent = spentSoFar() - base;
  for (Reg *v : ap.vals) v->color = cur[v];
  int curCost = moveCost(prefs, nullptr);
  int steps = 0, better = 0, loopsOpt = 0;
  auto attempt = [&](const std::vector<Reg *> &N, bool *optimal) {
    double left = rlimit - spent;
    *optimal = false;
    if (left <= 0) return false;
    double used = 0;
    std::map<Reg *, int> got;
    bool ok = nb.solve(N, cur, std::min(left, std::max(2e6, rlimit / 40.0)), &got, &used);
    spent += used;
    steps++;
    if (!ok) return true;
    *optimal = true;
    std::map<Reg *, int> save = cur;
    for (auto &g : got) cur[g.first] = g.second;
    for (Reg *v : ap.vals) v->color = cur[v];
    int nc = moveCost(prefs, nullptr);
    if (nc < curCost) {
      curCost = nc;
      better++;
    } else {
      cur = save;
    }
    return true;
  };
  size_t nLoops = nb.loops.size();
  for (auto &L : nb.loops) {
    bool optimal;
    if (!attempt(nb.around(L, 400), &optimal)) break;
    // too big to settle with its neighbours: the loop's own values alone
    if (!optimal && !attempt(nb.around(L, 400, false), &optimal)) break;
    if (optimal) loopsOpt++;
  }
  for (bool improved = true; improved && spent < rlimit && curCost > 0;) {
    improved = false;
    int before = curCost;
    for (Reg *v : ap.vals) v->color = cur[v];
    std::vector<std::pair<int, std::pair<Reg *, Reg *>>> bad;   // violated preferences, heaviest first
    for (auto &pw : prefs)
      if (cur[pw.first.first] != cur[pw.first.second]) bad.push_back({pw.second, pw.first});
    std::stable_sort(bad.begin(), bad.end(), [](auto &x, auto &y) { return x.first > y.first; });
    std::set<Reg *> tried;
    for (auto &bp : bad) {
      Reg *a = bp.second.first, *b = bp.second.second;
      if (cur[a] == cur[b] || (tried.count(a) && tried.count(b))) continue;
      std::vector<Reg *> N = nb.around({a, b}, 160);
      tried.insert(N.begin(), N.end());
      bool optimal;
      if (!attempt(N, &optimal)) break;
    }
    improved = curCost < before;
  }
  for (Reg *v : ap.vals) v->color = cur[v];
  if (verbose) {
    // remaining cost by loop depth (weights are 10^depth)
    std::map<int, std::pair<int, int>> byDepth;   // depth -> (missed, total)
    for (auto &pw : prefs) {
      int d = 0;
      for (int w = pw.second; w >= 10; w /= 10) d++;
      byDepth[d].second += pw.second;
      if (pw.first.first->color != pw.first.second->color) byDepth[d].first += pw.second;
    }
    std::string bd;
    for (auto &x : byDepth)
      bd += " depth " + std::to_string(x.first) + ": " + std::to_string(x.second.first) + "/" +
            std::to_string(x.second.second) + ";";
    fprintf(stderr, "ucc: %zu values, %zu interference edges, %zu call sites; move cost %d of %d "
            "(greedy %d; %zu of %zu loops optimal given their surroundings; %d of %d neighbourhood steps improved; "
            "rlimit %.0f)\nucc: remaining move cost by loop depth:%s\n",
            ap.vals.size(), ap.edges.size(), ap.calls.size(), moveCost(prefs, &total), total, greedyCost,
            (size_t)loopsOpt, nLoops, better, steps, spent, bd.c_str());
  }
}
