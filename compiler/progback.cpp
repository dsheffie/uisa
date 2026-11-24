/* progback.cpp: the program-mode back end (ucc --program).
 *
 * Millicode has no stack and allocates registers for the whole program at
 * once (backend.cpp, regalloc.cpp). Program mode compiles ordinary C programs
 * instead, so it uses the conventional pipeline:
 *
 *   1. Out of SSA, before allocation: critical edges are split and each phi
 *      becomes a parallel copy on its incoming edge, sequentialized into
 *      plain copies (a cycle goes through a new temporary).
 *   2. ABI lowering: parameters, arguments and results move through the
 *      fixed argument registers (precolored nodes); words past the eighth go
 *      through the stack; a call writes every caller-saved register.
 *   3. Iterated register coalescing (George & Appel, "Iterated Register
 *      Coalescing", TOPLAS 1996): Chaitin-Briggs graph colouring with
 *      conservative coalescing, freezing and optimistic spilling. Spilled
 *      values get a stack slot, every use and definition goes through a new
 *      short-lived temporary, and allocation repeats until nothing spills.
 *   4. Frame layout and emission: a fixed-size frame below sp (outgoing
 *      stack arguments, locals in memory, spill slots, saved registers);
 *      calls are `jal x1, f`, returns `jalr x0, x1, 0`, so recursion and
 *      call depth are limited only by the stack.
 *
 * Register convention (README "Program mode"): x0 zero, x1 ra, x2 sp, x31
 * reserved for the emitter (large frame offsets); x10-x17 arguments and
 * x10-x11 results; caller-saved x3-x7, x10-x17, x28-x30; callee-saved x8, x9,
 * x18-x27.
 */
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "backend.h"
#include "codegen.h"
#include "emit.h"
#include "errors.h"

using namespace spec;

namespace {

const int RA = 1, SP = 2, AT = 31;
const int ARG0 = 10, NARGREGS = 8;
const int CALLER_SAVED[] = {3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15, 16, 17, 28, 29, 30};
const int CALLEE_SAVED[] = {8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
// colouring preference: temporaries, then argument registers, then callee-saved
const int ALLOC_ORDER[] = {5, 6, 7, 28, 29, 30, 3, 4, 17, 16, 15, 14, 13, 12, 11, 10,
                           8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
const int K = sizeof ALLOC_ORDER / sizeof ALLOC_ORDER[0];

bool isCalleeSaved(int r) {
  return std::find(std::begin(CALLEE_SAVED), std::end(CALLEE_SAVED), r) != std::end(CALLEE_SAVED);
}

// ---------------------------------------------------------------------------
// out of SSA
// ---------------------------------------------------------------------------
// The parallel copy {dst <- src} as a sequence of copies: a move whose
// destination no other pending move reads goes first; when only cycles are
// left, one destination's old value is saved in a new temporary.
void sequentialize(Function *f, std::vector<std::pair<Reg *, Operand>> moves, int line, std::vector<Instr *> &out) {
  auto copy = [&](Reg *d, Operand s) {
    Instr *c = new Instr(Op::Copy);
    c->dst = d;
    c->a = {s};
    c->line = line;
    out.push_back(c);
  };
  std::vector<std::pair<Reg *, Operand>> consts;   // they read no register: last
  std::vector<std::pair<Reg *, Reg *>> pend;
  for (auto &m : moves) {
    if (!m.second.isReg()) consts.push_back(m);
    else if (m.second.r != m.first) pend.push_back({m.first, m.second.r});
  }
  while (!pend.empty()) {
    bool progress = false;
    for (size_t i = 0; i < pend.size(); i++) {
      Reg *d = pend[i].first;
      bool read = false;
      for (size_t j = 0; j < pend.size(); j++)
        if (j != i && pend[j].second == d) read = true;
      if (read) continue;
      copy(d, Operand::reg(pend[i].second));
      pend.erase(pend.begin() + i);
      progress = true;
      break;
    }
    if (progress) continue;
    Reg *d = pend[0].first;
    Reg *t = f->newReg(d->name + ".cyc");
    copy(t, Operand::reg(d));
    for (auto &m : pend)
      if (m.second == d) m.second = t;
  }
  for (auto &c : consts) copy(c.first, c.second);
}

void outOfSSA(Function *f) {
  splitCriticalEdges(f);
  for (BasicBlock *b : f->blocks) {
    if (b->succs.size() != 1) continue;
    BasicBlock *s = b->succs[0];
    std::vector<std::pair<Reg *, Operand>> moves;
    for (Instr *i : s->ins) {
      if (i->op != Op::Phi) break;
      for (size_t k = 0; k < i->preds.size(); k++)
        if (i->preds[k] == b) moves.push_back({i->dst, i->a[k]});
    }
    if (moves.empty()) continue;
    std::vector<Instr *> seq;
    sequentialize(f, moves, b->term()->line, seq);
    b->ins.insert(b->ins.end() - 1, seq.begin(), seq.end());
  }
  for (BasicBlock *b : f->blocks)
    b->ins.erase(std::remove_if(b->ins.begin(), b->ins.end(), [](Instr *i) { return i->op == Op::Phi; }),
                 b->ins.end());
  f->rebuildEdges();
}

// ---------------------------------------------------------------------------
// ABI lowering
// ---------------------------------------------------------------------------
struct PhysRegs {
  Function *f;
  std::map<int, Reg *> r;
  Reg *get(int n) {
    Reg *&p = r[n];
    if (!p) {
      p = f->newReg("x" + std::to_string(n));
      p->precolor = p->color = n;
    }
    return p;
  }
};

Instr *mkCopy(Reg *d, Operand s, int line) {
  Instr *c = new Instr(Op::Copy);
  c->dst = d;
  c->a = {s};
  c->line = line;
  return c;
}

void lowerABI(Function *f, PhysRegs &P, int &outWords) {
  // parameters: the first eight words arrive in x10-x17, the rest on the stack
  std::vector<Instr *> pro;
  for (size_t k = 0; k < f->params.size(); k++) {
    if (k < (size_t)NARGREGS) {
      pro.push_back(mkCopy(f->params[k], Operand::reg(P.get(ARG0 + (int)k)), f->line));
    } else {
      Instr *l = new Instr(Op::Load);
      l->dst = f->params[k];
      l->a = {Operand::preg(0)};
      l->fobj = f->addFrameObj(Function::FrameObj::InArg, 4, 4, (int)k - NARGREGS);
      l->line = f->line;
      pro.push_back(l);
    }
  }
  BasicBlock *entry = f->blocks[0];
  entry->ins.insert(entry->ins.begin(), pro.begin(), pro.end());

  for (BasicBlock *b : f->blocks) {
    std::vector<Instr *> out;
    for (size_t n = 0; n < b->ins.size(); n++) {
      Instr *i = b->ins[n];
      if (i->op == Op::Call) {
        std::vector<Operand> used;
        for (size_t k = 0; k < i->a.size(); k++) {
          Operand v = i->a[k];
          if (k < (size_t)NARGREGS) {
            Reg *p = P.get(ARG0 + (int)k);
            out.push_back(mkCopy(p, v, i->line));
            used.push_back(Operand::reg(p));
          } else {
            if (v.isImm() && v.imm != 0) {   // stores take a register
              Reg *t = f->newReg("arg");
              out.push_back(mkCopy(t, v, i->line));
              v = Operand::reg(t);
            } else if (v.isImm()) {
              v = Operand::preg(0);
            }
            Instr *st = new Instr(Op::Store);
            st->a = {Operand::preg(0), v};
            st->fobj = f->addFrameObj(Function::FrameObj::OutArg, 4, 4, (int)k - NARGREGS);
            st->line = i->line;
            out.push_back(st);
            outWords = std::max(outWords, (int)k - NARGREGS + 1);
          }
        }
        Reg *d = i->dst;
        i->dst = nullptr;
        i->a = used;
        for (int r : CALLER_SAVED) i->clobbers.push_back(P.get(r));
        out.push_back(i);
        if (d) out.push_back(mkCopy(d, Operand::reg(P.get(ARG0)), i->line));
        if (n + 1 < b->ins.size() && b->ins[n + 1]->op == Op::CallRes) {
          out.push_back(mkCopy(b->ins[n + 1]->dst, Operand::reg(P.get(ARG0 + 1)), i->line));
          n++;
        }
        continue;
      }
      if (i->op == Op::Ret) {
        std::vector<Operand> used;
        for (size_t k = 0; k < i->a.size(); k++) {
          Reg *p = P.get(ARG0 + (int)k);
          out.push_back(mkCopy(p, i->a[k], i->line));
          used.push_back(Operand::reg(p));
        }
        i->a = used;
      }
      out.push_back(i);
    }
    b->ins = out;
  }
}

// ---------------------------------------------------------------------------
// iterated register coalescing
// ---------------------------------------------------------------------------
bool isMove(Instr *i) { return i->op == Op::Copy && i->dst && i->a[0].isReg(); }

void usesOf(Instr *i, std::vector<Reg *> &u) {
  u.clear();
  for (auto &o : i->a)
    if (o.isReg()) u.push_back(o.r);
}
void defsOf(Instr *i, std::vector<Reg *> &d) {
  d.clear();
  if (i->dst) d.push_back(i->dst);
  d.insert(d.end(), i->clobbers.begin(), i->clobbers.end());
}

class IRC {
 public:
  Function *f;
  int N;
  std::vector<Reg *> node;               // index -> value
  std::vector<int> degree, alias, color;
  std::vector<std::vector<int>> adjList;
  std::set<std::pair<int, int>> adjSet;
  std::vector<std::vector<int>> moveList;   // node -> move indices
  std::vector<std::pair<int, int>> moves;   // move index -> (dst, src)
  std::vector<double> cost;
  // node state
  enum State { Precolored, Initial, Simplify, Freeze, Spill, Spilled, Coalesced, Colored, Stack };
  std::vector<State> st;
  std::set<int> simplifyWL, freezeWL, spillWL;
  std::vector<int> spilled, selectStack;
  // move state
  enum MState { MCoalesced, MConstrained, MFrozen, MWorklist, MActive };
  std::vector<MState> mst;
  std::set<int> worklistMoves, activeMoves;

  explicit IRC(Function *f) : f(f) {}

  bool precolored(int n) const { return node[n]->precolor >= 0; }

  void addEdge(int u, int v) {
    if (u == v) return;
    if (adjSet.count({u, v})) return;
    adjSet.insert({u, v});
    adjSet.insert({v, u});
    if (!precolored(u)) {
      adjList[u].push_back(v);
      degree[u]++;
    }
    if (!precolored(v)) {
      adjList[v].push_back(u);
      degree[v]++;
    }
  }

  void build() {
    N = (int)f->regs.size();
    node = f->regs;
    degree.assign(N, 0);
    alias.assign(N, -1);
    color.assign(N, -1);
    adjList.assign(N, {});
    adjSet.clear();
    moveList.assign(N, {});
    moves.clear();
    cost.assign(N, 0);
    st.assign(N, Initial);
    for (int n = 0; n < N; n++)
      if (precolored(n)) {
        st[n] = Precolored;
        color[n] = node[n]->precolor;
        degree[n] = 1 << 28;
      }
    // liveness (backward dataflow over blocks)
    std::map<BasicBlock *, std::vector<char>> liveIn, liveOut, useB, defB;
    std::vector<Reg *> u, d;
    for (BasicBlock *b : f->blocks) {
      auto &U = useB[b], &D = defB[b];
      U.assign(N, 0);
      D.assign(N, 0);
      for (Instr *i : b->ins) {
        usesOf(i, u);
        defsOf(i, d);
        for (Reg *r : u)
          if (!D[r->id]) U[r->id] = 1;
        for (Reg *r : d) D[r->id] = 1;
      }
      liveIn[b].assign(N, 0);
      liveOut[b].assign(N, 0);
    }
    for (bool changed = true; changed;) {
      changed = false;
      for (auto it = f->blocks.rbegin(); it != f->blocks.rend(); ++it) {
        BasicBlock *b = *it;
        std::vector<char> out(N, 0);
        for (BasicBlock *s : b->succs)
          for (int n = 0; n < N; n++) out[n] |= liveIn[s][n];
        std::vector<char> in = useB[b];
        for (int n = 0; n < N; n++)
          if (out[n] && !defB[b][n]) in[n] = 1;
        if (out != liveOut[b] || in != liveIn[b]) {
          liveOut[b] = out;
          liveIn[b] = in;
          changed = true;
        }
      }
    }
    // interference, moves and spill costs
    for (BasicBlock *b : f->blocks) {
      double w = 1;
      for (int k = 0; k < std::min(b->loopDepth, 6); k++) w *= 10;
      std::vector<int> live;
      std::vector<char> isLive = liveOut[b];
      for (int n = 0; n < N; n++)
        if (isLive[n]) live.push_back(n);
      auto setLive = [&](int n, bool v) {
        if (isLive[n] == v) return;
        isLive[n] = v;
        if (v) live.push_back(n);
        else live.erase(std::find(live.begin(), live.end(), n));
      };
      for (auto it = b->ins.rbegin(); it != b->ins.rend(); ++it) {
        Instr *i = *it;
        usesOf(i, u);
        defsOf(i, d);
        for (Reg *r : u) cost[r->id] += w;
        for (Reg *r : d) cost[r->id] += w;
        if (isMove(i)) {
          for (Reg *r : u) setLive(r->id, false);
          int m = (int)moves.size();
          moves.push_back({i->dst->id, i->a[0].r->id});
          moveList[i->dst->id].push_back(m);
          moveList[i->a[0].r->id].push_back(m);
          worklistMoves.insert(m);
        }
        for (Reg *r : d) setLive(r->id, true);
        for (Reg *r : d)
          for (int l : live) addEdge(l, r->id);
        for (Reg *r : d) setLive(r->id, false);
        for (Reg *r : u) setLive(r->id, true);
      }
    }
    mst.assign(moves.size(), MWorklist);
  }

  std::vector<int> adjacent(int n) {
    std::vector<int> r;
    for (int m : adjList[n])
      if (st[m] != Stack && st[m] != Coalesced) r.push_back(m);
    return r;
  }
  std::vector<int> nodeMoves(int n) {
    std::vector<int> r;
    for (int m : moveList[n])
      if (mst[m] == MActive || mst[m] == MWorklist) r.push_back(m);
    return r;
  }
  bool moveRelated(int n) { return !nodeMoves(n).empty(); }

  void setState(int n, State s) {
    switch (st[n]) {
      case Simplify: simplifyWL.erase(n); break;
      case Freeze: freezeWL.erase(n); break;
      case Spill: spillWL.erase(n); break;
      default: break;
    }
    st[n] = s;
    switch (s) {
      case Simplify: simplifyWL.insert(n); break;
      case Freeze: freezeWL.insert(n); break;
      case Spill: spillWL.insert(n); break;
      default: break;
    }
  }
  void setMove(int m, MState s) {
    if (mst[m] == MWorklist) worklistMoves.erase(m);
    if (mst[m] == MActive) activeMoves.erase(m);
    mst[m] = s;
    if (s == MWorklist) worklistMoves.insert(m);
    if (s == MActive) activeMoves.insert(m);
  }

  void makeWorklist() {
    for (int n = 0; n < N; n++) {
      if (st[n] != Initial) continue;
      if (degree[n] >= K) setState(n, Spill);
      else if (moveRelated(n)) setState(n, Freeze);
      else setState(n, Simplify);
    }
  }

  void enableMoves(int n) {
    for (int m : nodeMoves(n))
      if (mst[m] == MActive) setMove(m, MWorklist);
  }
  void decrementDegree(int m) {
    if (precolored(m)) return;
    int d = degree[m]--;
    if (d == K) {
      enableMoves(m);
      for (int a : adjacent(m)) enableMoves(a);
      if (moveRelated(m)) setState(m, Freeze);
      else setState(m, Simplify);
    }
  }
  void simplify() {
    int n = *simplifyWL.begin();
    setState(n, Stack);
    selectStack.push_back(n);
    for (int m : adjacent(n)) decrementDegree(m);
  }

  int getAlias(int n) { return st[n] == Coalesced ? getAlias(alias[n]) : n; }
  void addWorkList(int u) {
    if (!precolored(u) && !moveRelated(u) && degree[u] < K) setState(u, Simplify);
  }
  bool ok(int t, int r) { return degree[t] < K || precolored(t) || adjSet.count({t, r}); }
  bool conservative(const std::vector<int> &nodes) {
    int k = 0;
    for (int n : nodes)
      if (degree[n] >= K) k++;
    return k < K;
  }
  void combine(int u, int v) {
    setState(v, Coalesced);
    alias[v] = u;
    moveList[u].insert(moveList[u].end(), moveList[v].begin(), moveList[v].end());
    enableMoves(v);
    for (int t : adjacent(v)) {
      addEdge(t, u);
      decrementDegree(t);
    }
    if (degree[u] >= K && st[u] == Freeze) setState(u, Spill);
  }
  void coalesce() {
    int m = *worklistMoves.begin();
    int x = getAlias(moves[m].first), y = getAlias(moves[m].second);
    int u = x, v = y;
    if (precolored(y)) std::swap(u, v);
    if (u == v) {
      setMove(m, MCoalesced);
      addWorkList(u);
    } else if (precolored(v) || adjSet.count({u, v})) {
      setMove(m, MConstrained);
      addWorkList(u);
      addWorkList(v);
    } else {
      bool can;
      if (precolored(u)) {   // George
        can = true;
        for (int t : adjacent(v))
          if (!ok(t, u)) can = false;
      } else {               // Briggs
        std::vector<int> nb = adjacent(u);
        for (int t : adjacent(v))
          if (std::find(nb.begin(), nb.end(), t) == nb.end()) nb.push_back(t);
        can = conservative(nb);
      }
      if (can) {
        setMove(m, MCoalesced);
        combine(u, v);
        addWorkList(u);
      } else {
        setMove(m, MActive);
      }
    }
  }
  void freezeMoves(int u) {
    for (int m : nodeMoves(u)) {
      int x = moves[m].first, y = moves[m].second;
      int v = getAlias(y) == getAlias(u) ? getAlias(x) : getAlias(y);
      setMove(m, MFrozen);
      if (!precolored(v) && nodeMoves(v).empty() && degree[v] < K && st[v] == Freeze) setState(v, Simplify);
    }
  }
  void freeze() {
    int u = *freezeWL.begin();
    setState(u, Simplify);
    freezeMoves(u);
  }
  void selectSpill() {
    int best = -1;
    double bestScore = 0;
    for (int n : spillWL) {
      double c = node[n]->noSpill ? 1e30 : cost[n] / std::max(1, degree[n]);
      if (best < 0 || c < bestScore) {
        best = n;
        bestScore = c;
      }
    }
    setState(best, Simplify);
    freezeMoves(best);
  }
  void assignColors() {
    while (!selectStack.empty()) {
      int n = selectStack.back();
      selectStack.pop_back();
      std::set<int> okColors(std::begin(ALLOC_ORDER), std::end(ALLOC_ORDER));
      for (int w : adjList[n]) {
        int a = getAlias(w);
        if (st[a] == Colored || st[a] == Precolored) okColors.erase(color[a]);
      }
      int c = -1;
      for (int r : ALLOC_ORDER)
        if (okColors.count(r)) {
          c = r;
          break;
        }
      if (c < 0) {
        st[n] = Spilled;
        spilled.push_back(n);
      } else {
        st[n] = Colored;
        color[n] = c;
      }
    }
    for (int n = 0; n < N; n++)
      if (st[n] == Coalesced) color[n] = color[getAlias(n)];
  }

  // false: some values were spilled (rewrite and try again)
  bool run() {
    build();
    makeWorklist();
    while (!simplifyWL.empty() || !worklistMoves.empty() || !freezeWL.empty() || !spillWL.empty()) {
      if (!simplifyWL.empty()) simplify();
      else if (!worklistMoves.empty()) coalesce();
      else if (!freezeWL.empty()) freeze();
      else selectSpill();
    }
    assignColors();
    return spilled.empty();
  }
};

// Every spilled value lives in a stack slot: each use loads it into a new
// temporary first, each definition stores a new temporary after.
void rewriteSpills(Function *f, const std::vector<Reg *> &spills) {
  std::map<Reg *, int> slot;
  for (Reg *r : spills) slot[r] = f->addFrameObj(Function::FrameObj::Spill, 4, 4);
  for (BasicBlock *b : f->blocks) {
    std::vector<Instr *> out;
    for (Instr *i : b->ins) {
      std::map<Reg *, Reg *> tmp;
      auto temp = [&](Reg *r) {
        Reg *&t = tmp[r];
        if (!t) {
          t = f->newReg(r->name + ".s");
          t->noSpill = true;
        }
        return t;
      };
      for (auto &o : i->a)
        if (o.isReg() && slot.count(o.r)) {
          Reg *r = o.r;
          bool loaded = tmp.count(r);
          Reg *t = temp(r);
          if (!loaded) {
            Instr *l = new Instr(Op::Load);
            l->dst = t;
            l->a = {Operand::preg(0)};
            l->fobj = slot[r];
            l->line = i->line;
            out.push_back(l);
          }
          o = Operand::reg(t);
        }
      Instr *store = nullptr;
      if (i->dst && slot.count(i->dst)) {
        Reg *r = i->dst;
        Reg *t = temp(r);
        i->dst = t;
        store = new Instr(Op::Store);
        store->a = {Operand::preg(0), Operand::reg(t)};
        store->fobj = slot[r];
        store->line = i->line;
      }
      out.push_back(i);
      if (store) out.push_back(store);
    }
    b->ins = out;
  }
}

// ---------------------------------------------------------------------------
// frame and emission
// ---------------------------------------------------------------------------
struct Frame {
  int size = 0;
  int raOff = -1;
  std::vector<std::pair<int, int>> saves;   // (register, offset)
};

int alignUp(int v, int a) { return (v + a - 1) / a * a; }

Frame layoutFrame(Function *f, int outWords, bool hasCalls) {
  Frame fr;
  int off = 4 * outWords;   // outgoing stack arguments at sp + 0
  for (auto &o : f->frame)
    if (o.kind == Function::FrameObj::Local || o.kind == Function::FrameObj::Spill) {
      off = alignUp(off, o.align);
      o.offset = off;
      off += o.size;
    }
  std::set<int> used;
  for (Reg *r : f->regs)
    if (r->color > 0 && isCalleeSaved(r->color)) used.insert(r->color);
  off = alignUp(off, 4);
  for (int r : used) {
    fr.saves.push_back({r, off});
    off += 4;
  }
  if (hasCalls) {
    fr.raOff = off;
    off += 4;
  }
  fr.size = alignUp(off, 8);
  for (auto &o : f->frame) {
    if (o.kind == Function::FrameObj::OutArg) o.offset = 4 * o.index;
    if (o.kind == Function::FrameObj::InArg) o.offset = fr.size + 4 * o.index;
  }
  return fr;
}

std::string X(int r) { return "x" + std::to_string(r); }

// sp-relative (or any base) access with an offset of any size
void memOp(Emitter &E, const std::string &mnem, const std::string &reg, const std::string &base, int64_t off) {
  if (off >= MEM_LO && off <= MEM_HI) {
    E.text(mnem + " " + reg + ", " + std::to_string(off) + "(" + base + ")");
    return;
  }
  E.li(X(AT), (int32_t)off);
  E.text("add " + X(AT) + ", " + X(AT) + ", " + base);
  E.text(mnem + " " + reg + ", 0(" + X(AT) + ")");
}
void addImm(Emitter &E, const std::string &d, const std::string &s, int64_t v) {
  if (v >= ADDI_LO && v <= ADDI_HI) {
    if (v || d != s) E.text("addi " + d + ", " + s + ", " + std::to_string(v));
    return;
  }
  E.li(X(AT), (int32_t)v);
  E.text("add " + d + ", " + s + ", " + X(AT));
}

std::string loadMnem(Instr *i) {
  switch (i->memBytes) {
    case 1: return i->memSigned ? "ld.8s.h" : "ld.8u.h";
    case 2: return i->memSigned ? "ld.16s.h" : "ld.16u.h";
    default: return "ld.32.h";
  }
}
std::string storeMnem(Instr *i) { return "st." + std::to_string(i->memBytes * 8) + ".h"; }

void emitProgInstr(Function *f, Emitter &E, Instr *i) {
  auto addr = [&](std::string &base, int64_t &off) {
    if (i->fobj >= 0) {
      base = X(SP);
      off = (int64_t)f->frame[i->fobj].offset + i->disp;
    } else {
      base = R(i->a[0]);
      off = i->disp;
    }
  };
  std::string base;
  int64_t off;
  switch (i->op) {
    case Op::Load:
      addr(base, off);
      memOp(E, loadMnem(i), R(i->dst), base, off);
      break;
    case Op::Store:
      addr(base, off);
      memOp(E, storeMnem(i), R(i->a[1]), base, off);
      break;
    case Op::FrameAddr:
      addImm(E, R(i->dst), X(SP), (int64_t)f->frame[i->fobj].offset + i->disp);
      break;
    case Op::Call:
      E.text("jal " + X(RA) + ", " + i->callee->name);
      break;
    default:
      emitInstr(E, i, nullptr);
  }
}

void emitFunctionProgram(Function *f, FILE *out) {
  PhysRegs P{f, {}};
  outOfSSA(f);
  int outWords = 0;
  lowerABI(f, P, outWords);
  computeLoopDepth(f);
  bool hasCalls = false;
  for (BasicBlock *b : f->blocks)
    for (Instr *i : b->ins)
      if (i->op == Op::Call) hasCalls = true;
  for (int round = 0;; round++) {
    IRC a(f);
    if (a.run()) {
      for (int n = 0; n < a.N; n++)
        if (!a.precolored(n)) a.node[n]->color = a.color[n];
      break;
    }
    if (round > 50) fatal(f->line, "internal: register allocation of '%s' does not converge", f->name.c_str());
    std::vector<Reg *> sp;
    for (int n : a.spilled) sp.push_back(a.node[n]);
    rewriteSpills(f, sp);
  }
  Frame fr = layoutFrame(f, outWords, hasCalls);

  std::map<BasicBlock *, std::vector<AsmLine>> body;
  int line = f->line;
  for (BasicBlock *b : f->blocks) {
    Emitter B;
    B.curLine = line;
    for (Instr *i : b->ins) {
      if (i->line) B.curLine = i->line;
      if (i->isTerm()) break;
      emitProgInstr(f, B, i);
    }
    line = B.curLine;
    body[b] = B.out;
  }
  Emitter E;
  E.label(f->name);
  E.curLine = f->line;
  if (fr.size) addImm(E, X(SP), X(SP), -fr.size);
  for (auto &s : fr.saves) memOp(E, "st.32.h", X(s.first), X(SP), s.second);
  if (fr.raOff >= 0) memOp(E, "st.32.h", X(RA), X(SP), fr.raOff);
  emitLayout(f, body, E, out, [&](Emitter &E, Instr *) {
    for (auto &s : fr.saves) memOp(E, "ld.32.h", X(s.first), X(SP), s.second);
    if (fr.raOff >= 0) memOp(E, "ld.32.h", X(RA), X(SP), fr.raOff);
    if (fr.size) addImm(E, X(SP), X(SP), fr.size);
    E.text("jalr x0, " + X(RA) + ", 0");
  });
}

}  // namespace

// The program image: `_start` at address 0 sets up sp, calls main and halts
// with main's result in x10; then the functions; then the data section as
// `.data <address>` and `.word` lines for compiler/prog/uimage.py.
void emitProgramMode(Program *p, FILE *out, const char *srcName) {
  const uint32_t stackRoom = 1u << 20;   // at least 1 MB of stack below PROG_STACK_TOP
  if (p->data.size() > PROG_STACK_TOP - PROG_DATA_BASE - stackRoom)
    fatal(0, "%zu bytes of global data do not fit below the stack (at most %u)", p->data.size(),
          PROG_STACK_TOP - PROG_DATA_BASE - stackRoom);
  fprintf(out, "; Generated by ucc --program from %s. Do not edit.\n", srcName);
  fprintf(out, "; code at 0x%08x, data at 0x%08x, stack below 0x%08x (hidden space)\n\n", PROG_CODE_BASE,
          PROG_DATA_BASE, PROG_STACK_TOP);
  fprintf(out, "_start:\n");
  Emitter E;
  E.li("x2", (int32_t)PROG_STACK_TOP);
  E.text("jal x1, main");
  E.text("halt 0");
  for (auto &l : E.out) fprintf(out, "        %s\n", l.text.c_str());
  fprintf(out, "\n");
  for (Function *f : p->funcs)
    if (!f->isAsm) emitFunctionProgram(f, out);
  if (!p->data.empty()) {
    fprintf(out, ".data 0x%08x\n", PROG_DATA_BASE);
    std::vector<uint8_t> d = p->data;
    while (d.size() % 4) d.push_back(0);
    for (size_t k = 0; k < d.size(); k += 4)
      fprintf(out, ".word 0x%02x%02x%02x%02x\n", d[k], d[k + 1], d[k + 2], d[k + 3]);
  }
}
