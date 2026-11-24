/* codegen.cpp: CFG cleanup, SSA construction and SSA cleanups.
 *
 * Dominators by Lengauer-Tarjan, dominance frontiers, liveness-pruned phi
 * placement, and renaming along the dominator tree with per-variable stacks;
 * then simplification, copy propagation, dead-code removal, global value
 * numbering and loop-invariant code motion.
 */
#include "codegen.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#include "errors.h"

// ---------------------------------------------------------------------------
// CFG utilities
// ---------------------------------------------------------------------------
static void retarget(Instr *t, BasicBlock *from, BasicBlock *to) {
  if (t->t == from) t->t = to;
  if (t->f == from) t->f = to;
  for (BasicBlock *&b : t->targets)
    if (b == from) b = to;
  if (t->op == Op::Switch &&
      std::all_of(t->targets.begin(), t->targets.end(), [&](BasicBlock *b) { return b == t->targets[0]; })) {
    t->op = Op::Jmp;
    t->t = t->targets[0];
    t->targets.clear();
    t->a.clear();
  }
  if (t->op == Op::Br && t->t == t->f) {
    t->op = Op::Jmp;
    t->f = nullptr;
    t->a.clear();
  }
}

// Drop phi operands whose incoming block is no longer a predecessor.
static void syncPhis(Function *f) {
  for (BasicBlock *b : f->blocks)
    for (Instr *i : b->ins) {
      if (i->op != Op::Phi) continue;
      for (size_t k = 0; k < i->preds.size();) {
        if (std::find(b->preds.begin(), b->preds.end(), i->preds[k]) == b->preds.end()) {
          i->preds.erase(i->preds.begin() + k);
          i->a.erase(i->a.begin() + k);
        } else {
          k++;
        }
      }
    }
}

static std::vector<BasicBlock *> reversePostorder(Function *f) {
  std::vector<BasicBlock *> post;
  std::set<BasicBlock *> seen;
  std::function<void(BasicBlock *)> dfs = [&](BasicBlock *b) {
    seen.insert(b);
    for (BasicBlock *s : b->succs)
      if (!seen.count(s)) dfs(s);
    post.push_back(b);
  };
  dfs(f->blocks[0]);
  std::reverse(post.begin(), post.end());
  return post;
}

// Keep only blocks reachable from the entry, in reverse postorder.
static void pruneUnreachable(Function *f) {
  f->rebuildEdges();
  std::vector<BasicBlock *> rpo = reversePostorder(f);
  std::set<BasicBlock *> keep(rpo.begin(), rpo.end());
  for (BasicBlock *b : f->blocks)
    if (!keep.count(b)) delete b;
  f->blocks = rpo;
  for (size_t i = 0; i < rpo.size(); i++) rpo[i]->rpo = (int)i;
  f->rebuildEdges();
  syncPhis(f);
}

void cleanCFG(Function *f) {
  for (BasicBlock *b : f->blocks) {
    Instr *t = b->term();
    if (t && t->op == Op::Br && t->t == t->f) retarget(t, t->t, t->t);
  }
  pruneUnreachable(f);
  for (BasicBlock *b : f->blocks) {
    Instr *t = b->term();
    if (t && t->op == Op::Stop && t->falloff) {
      int line = t->line ? t->line : f->line;
      if (f->isEntry)
        error(line, "entry function '%s' can reach its end; finish every path with __gexit() or __gfault()",
              f->name.c_str());
      else
        error(line, "control can reach the end of non-void function '%s'", f->name.c_str());
    }
  }
  // thread jumps through blocks that only jump
  bool changed = true;
  while (changed) {
    changed = false;
    for (BasicBlock *b : f->blocks) {
      if (b == f->blocks[0] || b->ins.size() != 1 || b->ins[0]->op != Op::Jmp) continue;
      BasicBlock *to = b->ins[0]->t;
      if (to == b) continue;
      for (BasicBlock *p : b->preds) retarget(p->term(), b, to);
      if (!b->preds.empty()) changed = true;
    }
    if (changed) pruneUnreachable(f);
  }
  // merge a block into its only predecessor when that predecessor only jumps to it
  changed = true;
  while (changed) {
    changed = false;
    for (BasicBlock *a : f->blocks) {
      Instr *t = a->term();
      if (!t || t->op != Op::Jmp) continue;
      BasicBlock *b = t->t;
      if (b == a || b == f->blocks[0] || b->preds.size() != 1) continue;
      a->ins.pop_back();
      a->ins.insert(a->ins.end(), b->ins.begin(), b->ins.end());
      b->ins.clear();
      Instr *j = new Instr(Op::Jmp);   // b is now unreachable; keep it well formed
      j->t = b;
      b->ins.push_back(j);
      changed = true;
      break;
    }
    if (changed) pruneUnreachable(f);
  }
  pruneUnreachable(f);
}

// ---------------------------------------------------------------------------
// dominators (Lengauer-Tarjan)
// ---------------------------------------------------------------------------
class LengauerTarjanDominators {
 private:
  std::vector<BasicBlock *> &blocks;
  BasicBlock *root = nullptr;
  long dfsCounter = 1;
  std::map<BasicBlock *, BasicBlock *> Parent, Ancestor, Label;
  std::map<BasicBlock *, std::set<BasicBlock *>> Buckets;
  std::map<long, BasicBlock *> Ndfs;
  std::map<BasicBlock *, long> Sdno;

  void DFS(BasicBlock *bb) {
    Sdno.at(bb) = dfsCounter;
    Ndfs[dfsCounter] = bb;
    Label[bb] = bb;
    Ancestor[bb] = nullptr;
    dfsCounter++;
    for (BasicBlock *nbb : bb->succs) {
      if (Sdno.at(nbb) == 0) {
        Parent[nbb] = bb;
        DFS(nbb);
      }
    }
  }
  void Link(BasicBlock *v, BasicBlock *w) { Ancestor.at(w) = v; }
  void Compress(BasicBlock *bb) {
    if (Ancestor.at(Ancestor.at(bb))) {
      Compress(Ancestor.at(bb));
      if (Sdno.at(Label.at(Ancestor.at(bb))) < Sdno.at(Label.at(bb))) Label.at(bb) = Label.at(Ancestor.at(bb));
      Ancestor.at(bb) = Ancestor.at(Ancestor.at(bb));
    }
  }
  BasicBlock *Eval(BasicBlock *bb) {
    if (!Ancestor.at(bb)) return bb;
    Compress(bb);
    return Label.at(bb);
  }

 public:
  LengauerTarjanDominators(std::vector<BasicBlock *> &blocks) : blocks(blocks) {}
  void operator()() {
    root = blocks[0];
    for (BasicBlock *bb : blocks) {
      Sdno[bb] = 0;
      Parent[bb] = nullptr;
      bb->idom = nullptr;
    }
    DFS(root);
    for (long i = dfsCounter - 1; i > 1; i--) {
      BasicBlock *w = Ndfs.at(i);
      long &sdom_w = Sdno.at(w);
      for (BasicBlock *pbb : w->preds) {
        BasicBlock *u = Eval(pbb);
        long sdom_u = Sdno.at(u);
        if (sdom_u < sdom_w) sdom_w = sdom_u;
      }
      Buckets[Ndfs.at(sdom_w)].insert(w);
      Link(Parent.at(w), w);
      std::set<BasicBlock *> &pBucket = Buckets[Parent.at(w)];
      for (BasicBlock *v : pBucket) {
        BasicBlock *u = Eval(v);
        v->idom = (Sdno.at(u) < Sdno.at(v)) ? u : Parent.at(w);
      }
      pBucket.clear();
    }
    for (long i = 2; i < dfsCounter; i++) {
      BasicBlock *w = Ndfs.at(i);
      if (w->idom != Ndfs.at(Sdno.at(w))) w->idom = w->idom->idom;
    }
    for (BasicBlock *bb : blocks)
      if (bb != root) bb->idom->dtree.push_back(bb);
  }
};

static void computeDominance(Function *f) {
  for (BasicBlock *b : f->blocks) {
    b->dtree.clear();
    b->dfrontier.clear();
  }
  LengauerTarjanDominators LTD(f->blocks);
  LTD();
  f->blocks[0]->idom = nullptr;
  for (BasicBlock *cbb : f->blocks) {
    if (cbb->preds.size() < 2) continue;
    for (BasicBlock *nbb : cbb->preds) {
      while (nbb != cbb->idom) {
        if (std::find(nbb->dfrontier.begin(), nbb->dfrontier.end(), cbb) == nbb->dfrontier.end())
          nbb->dfrontier.push_back(cbb);
        nbb = nbb->idom;
      }
    }
  }
}

static bool dominates(BasicBlock *a, BasicBlock *b) {
  for (; b; b = b->idom)
    if (b == a) return true;
  return false;
}

void computeLoopDepth(Function *f) {
  f->rebuildEdges();
  computeDominance(f);
  for (BasicBlock *b : f->blocks) b->loopDepth = 0;
  for (BasicBlock *t : f->blocks)
    for (BasicBlock *h : t->succs) {
      if (!dominates(h, t)) continue;
      std::set<BasicBlock *> loop{h};
      std::vector<BasicBlock *> work{t};
      while (!work.empty()) {
        BasicBlock *b = work.back();
        work.pop_back();
        if (loop.insert(b).second)
          for (BasicBlock *p : b->preds) work.push_back(p);
      }
      for (BasicBlock *b : loop) b->loopDepth++;
    }
}

// ---------------------------------------------------------------------------
// SSA construction
// ---------------------------------------------------------------------------
static bool renamable(const Operand &o) { return o.isReg() && o.r->precolor < 0; }

void buildSSA(Function *f) {
  f->rebuildEdges();
  computeDominance(f);

  // liveness of source variables (prunes phis where the variable is dead)
  std::map<BasicBlock *, std::set<Reg *>> uses, defs, liveIn, liveOut;
  std::map<Reg *, std::set<BasicBlock *>> defsites;
  std::map<Reg *, int> useLine;
  for (BasicBlock *b : f->blocks) {
    for (Instr *i : b->ins) {
      for (auto &o : i->a)
        if (renamable(o)) {
          if (!defs[b].count(o.r)) uses[b].insert(o.r);
          if (!useLine.count(o.r)) useLine[o.r] = i->line;
        }
      if (i->dst && i->dst->precolor < 0) {
        defs[b].insert(i->dst);
        defsites[i->dst].insert(b);
      }
    }
  }
  for (Reg *p : f->paramVars) {
    defs[f->blocks[0]].insert(p);
    defsites[p].insert(f->blocks[0]);
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto it = f->blocks.rbegin(); it != f->blocks.rend(); ++it) {
      BasicBlock *b = *it;
      std::set<Reg *> out;
      for (BasicBlock *s : b->succs) out.insert(liveIn[s].begin(), liveIn[s].end());
      std::set<Reg *> in = uses[b];
      for (Reg *r : out)
        if (!defs[b].count(r)) in.insert(r);
      if (out != liveOut[b] || in != liveIn[b]) {
        liveOut[b] = out;
        liveIn[b] = in;
        changed = true;
      }
    }
  }
  for (Reg *r : liveIn[f->blocks[0]]) {
    if (std::find(f->paramVars.begin(), f->paramVars.end(), r) != f->paramVars.end()) continue;
    error(useLine[r] ? useLine[r] : f->line, "'%s' may be used before it is assigned", r->name.c_str());
  }
  if (g_errors) return;

  // phi placement on the iterated dominance frontier, pruned by liveness
  std::map<Instr *, Reg *> phiVar;
  // variables in creation order, not pointer order: the phis' order in a
  // block (and everything after) must not depend on the heap
  std::vector<std::pair<Reg *, std::set<BasicBlock *> *>> byId;
  for (auto &ds : defsites) byId.push_back({ds.first, &ds.second});
  std::sort(byId.begin(), byId.end(), [](auto &a, auto &b) { return a.first->id < b.first->id; });
  for (auto &ds : byId) {
    Reg *v = ds.first;
    std::vector<BasicBlock *> work(ds.second->begin(), ds.second->end());
    std::set<BasicBlock *> has, inWork(ds.second->begin(), ds.second->end());
    while (!work.empty()) {
      BasicBlock *x = work.back();
      work.pop_back();
      for (BasicBlock *y : x->dfrontier) {
        if (has.count(y) || !liveIn[y].count(v)) continue;
        has.insert(y);
        Instr *phi = new Instr(Op::Phi);
        phi->preds = y->preds;
        phi->a.assign(y->preds.size(), Operand());
        y->ins.insert(y->ins.begin(), phi);
        phiVar[phi] = v;
        if (inWork.insert(y).second) work.push_back(y);
      }
    }
  }

  // renaming along the dominator tree
  std::map<Reg *, std::vector<Reg *>> stacks;
  auto fresh = [&](Reg *v) {
    Reg *r = f->newReg(v->name);
    r->var = v;
    stacks[v].push_back(r);
    return r;
  };
  std::function<void(BasicBlock *)> rename = [&](BasicBlock *b) {
    std::vector<Reg *> pushed;
    if (b == f->blocks[0])
      for (Reg *p : f->paramVars) {
        f->params.push_back(fresh(p));
        pushed.push_back(p);
      }
    for (Instr *i : b->ins) {
      if (i->op != Op::Phi)
        for (auto &o : i->a)
          if (renamable(o)) {
            auto &st = stacks[o.r];
            if (st.empty()) fatal(i->line, "internal: no reaching definition of %s", o.r->name.c_str());
            o.r = st.back();
          }
      Reg *v = i->op == Op::Phi ? phiVar[i] : i->dst;
      if (v && v->precolor < 0) {
        i->dst = fresh(v);
        i->dst->def = i;
        pushed.push_back(v);
      }
    }
    for (BasicBlock *s : b->succs)
      for (Instr *i : s->ins) {
        if (i->op != Op::Phi) break;
        for (size_t k = 0; k < i->preds.size(); k++) {
          if (i->preds[k] != b) continue;
          auto &st = stacks[phiVar[i]];
          if (st.empty()) fatal(i->line, "internal: phi without a definition of %s", phiVar[i]->name.c_str());
          i->a[k] = Operand::reg(st.back());
        }
      }
    for (BasicBlock *c : b->dtree) rename(c);
    for (Reg *v : pushed) stacks[v].pop_back();
  };
  rename(f->blocks[0]);
  recomputeDefs(f);
}

void recomputeDefs(Function *f) {
  for (Reg *r : f->regs) r->def = nullptr;
  for (BasicBlock *b : f->blocks)
    for (Instr *i : b->ins)
      if (i->dst) i->dst->def = i;
}

// ---------------------------------------------------------------------------
// SSA cleanups
// ---------------------------------------------------------------------------
static int32_t foldExt(Op op, uint32_t v) {
  switch (op) {
    case Op::Sext8: return (int8_t)v;
    case Op::Sext16: return (int16_t)v;
    case Op::Zext8: return v & 0xff;
    case Op::Zext16: return v & 0xffff;
    default: return v;
  }
}

static bool foldBinary(Instr *i, int32_t *out) {
  uint32_t a = i->a[0].imm, b = i->a[1].imm;
  int32_t sa = a, sb = b;
  switch (i->op) {
    case Op::Add: *out = a + b; return true;
    case Op::Sub: *out = a - b; return true;
    case Op::Mul: *out = a * b; return true;
    case Op::And: *out = a & b; return true;
    case Op::Or: *out = a | b; return true;
    case Op::Xor: *out = a ^ b; return true;
    case Op::Shl: *out = a << (b & 31); return true;
    case Op::Shr: *out = a >> (b & 31); return true;
    case Op::Sar: *out = sa >> (b & 31); return true;
    case Op::DivU: if (!b) return false; *out = a / b; return true;
    case Op::RemU: if (!b) return false; *out = a % b; return true;
    case Op::Div: if (!b || (sa == INT32_MIN && sb == -1)) return false; *out = sa / sb; return true;
    case Op::Rem: if (!b || (sa == INT32_MIN && sb == -1)) return false; *out = sa % sb; return true;
    default: return false;
  }
}

static void toCopy(Instr *i, Operand src) {
  i->op = Op::Copy;
  i->a = {src};
}

// One round of local simplification. Returns true if anything changed.
static bool simplify(Function *f) {
  bool changed = false;
  for (BasicBlock *b : f->blocks)
    for (Instr *i : b->ins) {
      auto &a = i->a;
      switch (i->op) {
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::DivU: case Op::Rem: case Op::RemU:
        case Op::And: case Op::Or: case Op::Xor: case Op::Shl: case Op::Shr: case Op::Sar: {
          int32_t v;
          if (a[0].isImm() && a[1].isImm() && foldBinary(i, &v)) { toCopy(i, Operand::cst(v)); changed = true; break; }
          bool comm = i->op == Op::Add || i->op == Op::Mul || i->op == Op::And || i->op == Op::Or || i->op == Op::Xor;
          if (comm && a[0].isImm() && !a[1].isImm()) { std::swap(a[0], a[1]); changed = true; }
          if (!a[1].isImm()) break;
          int32_t k = a[1].imm;
          if (k == 0 && (i->op == Op::Add || i->op == Op::Sub || i->op == Op::Or || i->op == Op::Xor ||
                         i->op == Op::Shl || i->op == Op::Shr || i->op == Op::Sar)) { toCopy(i, a[0]); changed = true; }
          else if (k == 0 && (i->op == Op::Mul || i->op == Op::And)) { toCopy(i, Operand::cst(0)); changed = true; }
          else if (k == 1 && (i->op == Op::Mul || i->op == Op::Div || i->op == Op::DivU)) { toCopy(i, a[0]); changed = true; }
          else if (k == -1 && i->op == Op::And) { toCopy(i, a[0]); changed = true; }
          else if (k > 1 && (k & (k - 1)) == 0 && (i->op == Op::Mul || i->op == Op::DivU || i->op == Op::RemU)) {
            int sh = __builtin_ctz((uint32_t)k);   // power of two: shift or mask
            if (i->op == Op::RemU) { i->op = Op::And; a[1].imm = k - 1; }
            else { i->op = i->op == Op::Mul ? Op::Shl : Op::Shr; a[1].imm = sh; }
            changed = true;
          } else if (i->op == Op::And && (k == 0xffff || k == 0xff)) {
            i->op = k == 0xff ? Op::Zext8 : Op::Zext16;
            a.pop_back();
            changed = true;
          }
          break;
        }
        case Op::Set:
          if (a[0].isImm() && a[1].isImm()) { toCopy(i, Operand::cst(evalCond(i->cond, a[0].imm, a[1].imm))); changed = true; }
          break;
        case Op::Sext8: case Op::Sext16: case Op::Zext8: case Op::Zext16:
          if (a[0].isImm()) { toCopy(i, Operand::cst(foldExt(i->op, a[0].imm))); changed = true; }
          break;
        case Op::Phi: {
          Operand same;
          bool trivial = true;
          for (auto &o : a) {
            if (o.isReg() && o.r == i->dst) continue;
            if (same.k == Operand::None) same = o;
            else if (!(same.k == o.k && same.r == o.r && same.imm == o.imm)) trivial = false;
          }
          if (trivial && same.k != Operand::None) {
            toCopy(i, same);
            i->preds.clear();
            changed = true;
          }
          break;
        }
        case Op::Switch:
          if (a[0].isImm()) {   // a constant index: one way
            BasicBlock *to = i->targets[((uint32_t)a[0].imm >> i->swLsb) & ((1u << i->swWidth) - 1)];
            i->op = Op::Jmp;
            i->t = to;
            i->targets.clear();
            a.clear();
            changed = true;
          }
          break;
        case Op::Br:
          if (a[0].isImm() && a[1].isImm()) {
            BasicBlock *to = evalCond(i->cond, a[0].imm, a[1].imm) ? i->t : i->f;
            i->op = Op::Jmp;
            i->t = to;
            i->f = nullptr;
            a.clear();
            changed = true;
          }
          break;
        default:
          break;
      }
    }
  return changed;
}

// Phis must stay at the top of their block: a phi turned copy moves below them.
static void sinkCopiesBelowPhis(Function *f) {
  for (BasicBlock *b : f->blocks) {
    std::stable_partition(b->ins.begin(), b->ins.end(), [](Instr *i) { return i->op == Op::Phi; });
  }
}

static bool copyPropagate(Function *f) {
  std::map<Reg *, Operand> repl;
  for (BasicBlock *b : f->blocks)
    for (Instr *i : b->ins)
      if (i->op == Op::Copy && i->dst && (i->a[0].isReg() || i->a[0].isImm())) {
        repl[i->dst] = i->a[0];
        // a temporary that now stands for a variable takes its name (diagnostics, listings)
        Reg *src = i->a[0].isReg() ? i->a[0].r : nullptr;
        if (src && i->dst->var && i->dst->var->isVar && !(src->var && src->var->isVar) && src->precolor < 0) {
          src->name = i->dst->name;
        }
      }
  if (repl.empty()) return false;
  auto resolve = [&](Operand o) {
    for (int n = 0; o.isReg() && repl.count(o.r) && n < 1000; n++) o = repl[o.r];
    return o;
  };
  for (BasicBlock *b : f->blocks) {
    std::vector<Instr *> keep;
    for (Instr *i : b->ins) {
      if (i->op == Op::Copy && i->dst && repl.count(i->dst)) continue;
      for (auto &o : i->a) o = resolve(o);
      keep.push_back(i);
    }
    b->ins = keep;
  }
  return true;
}

bool eliminateDeadCode(Function *f) {
  bool any = false, changed = true;
  while (changed) {
    changed = false;
    std::map<Reg *, int> uses;
    for (BasicBlock *b : f->blocks)
      for (Instr *i : b->ins)
        for (auto &o : i->a)
          if (o.isReg()) uses[o.r]++;
    for (BasicBlock *b : f->blocks) {
      std::vector<Instr *> keep;
      for (Instr *i : b->ins) {
        if (i->dst && !uses[i->dst] && !i->hasSideEffects()) {
          changed = any = true;
          continue;
        }
        keep.push_back(i);
      }
      b->ins = keep;
    }
  }
  return any;
}

// (x >> k) & (2^w - 1) -> one bfextui (the assembler's `extract rd, x, k, w`),
// and a mask too wide for andi -> bfextui from bit 0. The shift dies if
// nothing else uses it.
static bool selectExtract(Function *f) {
  static const IntrinsicDesc *bfx = findIntrinsic("__bfextui");
  if (!bfx) return false;
  recomputeDefs(f);
  bool changed = false;
  for (BasicBlock *b : f->blocks)
    for (Instr *i : b->ins) {
      if (i->op != Op::And || !i->a[0].isReg() || !i->a[1].isImm()) continue;
      uint32_t m = (uint32_t)i->a[1].imm;
      if (m == 0 || (m & (m + 1)) != 0 || m == 0xff || m == 0xffff || m == 0xffffffffu) continue;
      int w = __builtin_popcount(m), k = 0;
      Operand x = i->a[0];
      Instr *d = x.r->def;
      if (d && d->op == Op::Shr && d->a[1].isImm() && d->a[1].imm > 0 && d->a[1].imm + w <= 32) {
        k = d->a[1].imm;
        x = d->a[0];
      } else if (m <= 0x7ff) {
        continue;                            // andi does it
      }
      i->op = Op::Intrin;
      i->in = bfx;
      i->a = {x, Operand::cst(32 - k - w), Operand::cst(w)};
      changed = true;
    }
  return changed;
}

// ---------------------------------------------------------------------------
// global value numbering
// ---------------------------------------------------------------------------
// Intrinsics that compute a pure function of their operands (no state read
// or written, no flags): two with the same operands give the same value.
static bool pureIntrinsic(const IntrinsicDesc *d) {
  static const std::set<std::string> pure = {
      "__add", "__sub", "__and", "__or", "__xor", "__andn", "__orn", "__sll", "__srl", "__sra", "__rol",
      "__ror", "__slt", "__sltu", "__min", "__max", "__minu", "__maxu", "__mul", "__mulh", "__mulhu",
      "__div", "__divu", "__rem", "__remu", "__clz", "__ctz", "__cpop", "__rev8", "__sext8", "__sext16",
      "__zext8", "__zext16", "__czeqz", "__cznez", "__mrg8", "__mrg16", "__sh1add", "__sh2add", "__sh3add",
      "__bfmask", "__addi", "__andi", "__ori", "__xori", "__slti", "__sltiu", "__slli", "__srli", "__srai",
      "__rori", "__bfextu", "__bfexts", "__bfffo", "__bfextui", "__bfextsi", "__bfinsi"};
  return d && d->has_result && !d->side_effects && !d->flags && !d->noreturn && pure.count(d->name);
}

// The value an instruction computes, as a comparable key: opcode, the
// attributes that change its result, and its operands (sorted when the
// operation commutes).
struct VNKey {
  int op, cond, fobj, disp, bytes, sign;
  const void *in;
  std::vector<std::pair<int, int64_t>> ops;   // (kind, reg id / immediate / phys)
  bool operator<(const VNKey &o) const {
    return std::tie(op, cond, fobj, disp, bytes, sign, in, ops) <
           std::tie(o.op, o.cond, o.fobj, o.disp, o.bytes, o.sign, o.in, o.ops);
  }
};

// The key of an instruction GVN may merge, or false. Reading a guest or
// other physical register (anything but x0) is reading state, so not pure.
static bool vnKey(Instr *i, VNKey &k, bool consts) {
  switch (i->op) {
    case Op::Copy:   // after legalization: a materialized constant
      if (!consts || !i->a[0].isImm()) return false;
      break;
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::DivU: case Op::Rem: case Op::RemU:
    case Op::And: case Op::Or: case Op::Xor: case Op::Shl: case Op::Shr: case Op::Sar:
    case Op::Set: case Op::Sext8: case Op::Sext16: case Op::Zext8: case Op::Zext16:
    case Op::FrameAddr: case Op::Load:
      break;
    case Op::Intrin:
      if (!pureIntrinsic(i->in)) return false;
      break;
    default:
      return false;
  }
  if (!i->dst || i->dst->precolor >= 0) return false;   // a fixed register is not interchangeable
  k.op = (int)i->op;
  k.cond = i->op == Op::Set ? (int)i->cond : 0;
  k.fobj = i->fobj;
  k.disp = i->disp;
  k.bytes = i->op == Op::Load ? i->memBytes : 0;
  k.sign = i->op == Op::Load ? i->memSigned : 0;
  k.in = i->op == Op::Intrin ? (const void *)i->in : nullptr;
  k.ops.clear();
  for (const Operand &o : i->a) {
    if (o.isReg()) k.ops.push_back({1, o.r->id});
    else if (o.isImm()) k.ops.push_back({2, o.imm});
    else if (o.k == Operand::Phys && o.phys == 0) k.ops.push_back({3, 0});
    else return false;
  }
  bool commutes = i->op == Op::Add || i->op == Op::Mul || i->op == Op::And || i->op == Op::Or || i->op == Op::Xor ||
                  (i->op == Op::Set && (i->cond == Cond::EQ || i->cond == Cond::NE));
  if (commutes && k.ops.size() == 2 && k.ops[1] < k.ops[0]) std::swap(k.ops[0], k.ops[1]);
  return true;
}

// Dominator-tree value numbering over SSA: an instruction computing a value
// that a dominating instruction already computed becomes a copy of it
// (copy propagation and dead-code elimination then remove it). Pure
// operations match anywhere below their first occurrence. Loads (program
// mode) only match within a block, up to the next store, call or
// side-effecting intrinsic.
//
// After legalization (`consts`), constants materialized into registers are
// merged too, and a redundant instruction is removed outright: its uses are
// renamed to the earlier value (no copies, which legalized code must not
// gain). `scope` limits how far a value is reused: the whole dominator
// subtree, or (scope 0) its own block only.
static bool valueNumber(Function *f, bool consts = false, bool global = true) {
  computeDominance(f);
  std::map<VNKey, Reg *> avail;
  std::map<Reg *, Reg *> rename;
  std::set<Instr *> dead;
  bool changed = false;
  std::function<void(BasicBlock *)> walk = [&](BasicBlock *b) {
    std::vector<std::map<VNKey, Reg *>::iterator> added;
    std::map<VNKey, Reg *> loads;
    VNKey k;
    for (Instr *i : b->ins) {
      if (i->op == Op::Store || i->op == Op::Call || (i->op == Op::Intrin && i->in->side_effects)) {
        loads.clear();
        continue;
      }
      if (consts)
        for (Operand &o : i->a)
          if (o.isReg() && rename.count(o.r)) o.r = rename[o.r];
      if (!vnKey(i, k, consts)) continue;
      auto &table = i->op == Op::Load ? loads : avail;
      auto it = table.find(k);
      if (it != table.end()) {
        if (consts) {
          rename[i->dst] = it->second;
          dead.insert(i);
        } else {
          toCopy(i, Operand::reg(it->second));
        }
        changed = true;
      } else {
        auto ins = table.emplace(k, i->dst).first;
        if (&table == &avail) added.push_back(ins);
      }
    }
    if (!global)
      for (auto &it : added) avail.erase(it);
    for (BasicBlock *c : b->dtree) walk(c);
    if (global)
      for (auto &it : added) avail.erase(it);
  };
  walk(f->blocks[0]);
  if (consts && changed) {   // phis (and any use the walk reached first) name the earlier values
    for (BasicBlock *b : f->blocks) {
      for (Instr *i : b->ins)
        for (Operand &o : i->a)
          if (o.isReg() && rename.count(o.r)) o.r = rename[o.r];
      b->ins.erase(std::remove_if(b->ins.begin(), b->ins.end(), [&](Instr *i) { return dead.count(i) > 0; }),
                   b->ins.end());
    }
    recomputeDefs(f);
  }
  return changed;
}

void valueNumberLegal(Function *f, bool global) {
  f->rebuildEdges();
  valueNumber(f, true, global);
}

// ---------------------------------------------------------------------------
// loop-invariant code motion
// ---------------------------------------------------------------------------
// Natural loops (one per header: the union of its back edges' bodies),
// innermost (smallest) first.
struct Loop {
  BasicBlock *header;
  std::set<BasicBlock *> blocks;
};
static std::vector<Loop> findLoops(Function *f) {
  std::map<BasicBlock *, std::set<BasicBlock *>> byHeader;
  for (BasicBlock *t : f->blocks)
    for (BasicBlock *h : t->succs) {
      if (!dominates(h, t)) continue;
      std::set<BasicBlock *> &loop = byHeader[h];
      loop.insert(h);
      std::vector<BasicBlock *> work{t};
      while (!work.empty()) {
        BasicBlock *b = work.back();
        work.pop_back();
        if (loop.insert(b).second)
          for (BasicBlock *p : b->preds) work.push_back(p);
      }
    }
  std::vector<Loop> loops;
  for (BasicBlock *b : f->blocks)   // block order: deterministic
    if (byHeader.count(b)) loops.push_back({b, byHeader[b]});
  std::stable_sort(loops.begin(), loops.end(),
                   [](const Loop &a, const Loop &b) { return a.blocks.size() < b.blocks.size(); });
  return loops;
}

// The block hoisted code goes to: the loop's only outside predecessor when
// that block always continues into the header; else a new block on that
// edge. A header entered from several outside blocks is left alone.
static BasicBlock *preheader(Function *f, const Loop &l, std::vector<Loop> &loops) {
  BasicBlock *h = l.header, *p = nullptr;
  for (BasicBlock *q : h->preds)
    if (!l.blocks.count(q)) {
      if (p) return nullptr;
      p = q;
    }
  if (!p || !p->terminated()) return nullptr;
  if (p->succs.size() == 1) return p;
  BasicBlock *n = new BasicBlock(f->nextBlock++, "pre", f);
  Instr *j = new Instr(Op::Jmp);
  j->t = h;
  n->ins.push_back(j);
  f->blocks.insert(std::find(f->blocks.begin(), f->blocks.end(), h), n);
  for (Loop &o : loops)   // the new block is inside every loop around that edge
    if (o.blocks.count(p) && o.blocks.count(h)) o.blocks.insert(n);
  retarget(p->term(), h, n);
  for (Instr *i : h->ins)
    if (i->op == Op::Phi)
      for (BasicBlock *&b : i->preds)
        if (b == p) b = n;
  f->rebuildEdges();
  return n;
}

// Register pressure inside a loop, from SSA liveness: the most values live
// at one point, and the most live across one call (excluding its results).
struct LoopPressure { int live = 0, acrossCall = -1; };
static LoopPressure loopPressure(Function *f, const std::set<BasicBlock *> &blocks) {
  std::map<BasicBlock *, std::set<Reg *>> in, out;
  auto phiUses = [&](BasicBlock *p, BasicBlock *s, std::set<Reg *> &live) {
    for (Instr *i : s->ins) {
      if (i->op != Op::Phi) break;
      for (size_t k = 0; k < i->preds.size(); k++)
        if (i->preds[k] == p && i->a[k].isReg()) live.insert(i->a[k].r);
    }
  };
  auto scan = [&](BasicBlock *b, std::set<Reg *> live, LoopPressure *lp) {
    if (lp) lp->live = std::max(lp->live, (int)live.size());
    for (auto it = b->ins.rbegin(); it != b->ins.rend(); ++it) {
      Instr *i = *it;
      if (i->op == Op::Phi) {
        live.erase(i->dst);
        continue;
      }
      bool dead = i->dst && !live.count(i->dst);
      if (i->dst) live.erase(i->dst);
      if (lp && i->op == Op::Call) lp->acrossCall = std::max(lp->acrossCall, (int)live.size());
      for (const Operand &o : i->a)
        if (o.isReg()) live.insert(o.r);
      if (lp) lp->live = std::max(lp->live, (int)live.size() + (dead ? 1 : 0));
    }
    return live;
  };
  for (bool changed = true; changed;) {
    changed = false;
    for (auto it = f->blocks.rbegin(); it != f->blocks.rend(); ++it) {
      BasicBlock *b = *it;
      std::set<Reg *> o;
      for (BasicBlock *s : b->succs) {
        o.insert(in[s].begin(), in[s].end());
        phiUses(b, s, o);
      }
      std::set<Reg *> i = scan(b, o, nullptr);
      if (i != in[b] || o != out[b]) {
        in[b] = i;
        out[b] = o;
        changed = true;
      }
    }
  }
  LoopPressure lp;
  for (BasicBlock *b : blocks) scan(b, out[b], &lp);
  return lp;
}

// Registers loop hoisting may fill (a hoisted value is live through the
// whole loop). Program mode leaves loops that call alone: a value live
// across the call takes a callee-saved register, whose save and restore
// cost more code than the hoist saves (measured: +1% size for -1% steps).
// Millicode has no saves; calls are checked afterwards (undoHoists).
static int hoistBudget() { return g_program ? 26 : 30; }

// Move pure instructions whose operands are all defined outside a loop to
// its preheader, innermost loops first (a value hoisted out of an inner loop
// can then leave the outer one too). uisa ALU operations never fault, so
// executing one that the loop might have skipped is safe. Loads and anything
// reading state stay. After legalization (`consts`) materialized constants
// move too. Each move is logged in Function::hoisted with the header of the
// loop it left first, so that millicode can undo it (undoHoists).
static bool hoistInvariants(Function *f, bool consts) {
  f->rebuildEdges();
  computeDominance(f);
  bool changed = false;
  std::vector<Loop> loops = findLoops(f);
  for (size_t li = 0; li < loops.size(); li++) {
    const Loop &l = loops[li];
    std::map<Reg *, BasicBlock *> defBlock;
    for (BasicBlock *b : f->blocks)
      for (Instr *i : b->ins)
        if (i->dst) defBlock[i->dst] = b;
    auto invariant = [&](Instr *i) {
      VNKey k;
      if (i->op == Op::Load || !vnKey(i, k, consts)) return false;
      for (const Operand &o : i->a)
        if (o.isReg() && defBlock.count(o.r) && l.blocks.count(defBlock[o.r])) return false;   // (parameters: no def)
      return true;
    };
    LoopPressure lp = loopPressure(f, l.blocks);
    int room = hoistBudget() - lp.live;
    if (g_program && lp.acrossCall >= 0) room = 0;
    BasicBlock *pre = nullptr;
    bool more = true;
    while (more) {
      more = false;
      for (BasicBlock *b : f->blocks) {
        if (!l.blocks.count(b)) continue;
        for (size_t n = 0; n < b->ins.size();) {
          Instr *i = b->ins[n];
          if (room <= 0) goto nextLoop;
          if (!invariant(i)) {
            n++;
            continue;
          }
          if (!pre && !(pre = preheader(f, l, loops))) goto nextLoop;
          b->ins.erase(b->ins.begin() + n);
          pre->ins.insert(pre->ins.end() - 1, i);
          defBlock[i->dst] = pre;
          bool logged = false;
          for (auto &h : f->hoisted)
            if (h.first == i) logged = true;
          if (!logged) f->hoisted.push_back({i, l.header});
          room--;
          changed = more = true;
        }
      }
    }
  nextLoop:;
  }
  return changed;
}

void hoistLoopInvariants(Function *f) {
  if (hoistInvariants(f, true)) recomputeDefs(f);
}

// Put every logged hoist back at the top of the loop header it first left
// (after the phis): the header dominates every block of that loop and every
// use the value had, or gained through value numbering after the move.
bool undoHoists(Function *f) {
  std::set<BasicBlock *> live(f->blocks.begin(), f->blocks.end());
  bool any = false;
  for (auto &h : f->hoisted) {
    Instr *i = h.first;
    BasicBlock *home = h.second;
    BasicBlock *at = nullptr;
    for (BasicBlock *b : f->blocks)
      if (std::find(b->ins.begin(), b->ins.end(), i) != b->ins.end()) at = b;
    if (!at || !live.count(home) || at == home) continue;
    at->ins.erase(std::find(at->ins.begin(), at->ins.end(), i));
    auto pos = std::find_if(home->ins.begin(), home->ins.end(), [](Instr *j) { return j->op != Op::Phi; });
    home->ins.insert(pos, i);
    any = true;
  }
  f->hoisted.clear();
  return any;
}

// ---------------------------------------------------------------------------
// code hoisting
// ---------------------------------------------------------------------------
// When every successor of a branch is reached only from it and all of them
// compute the same pure value from operands available at the branch,
// compute it once before the branch: one instruction instead of one per
// successor. Repeats, so a chain of such values moves up together. After
// legalization (`consts`) materialized constants count too.
static bool hoistCommon(Function *f, bool consts) {
  f->rebuildEdges();
  std::map<Reg *, Reg *> rename;
  std::set<Instr *> dead;
  bool changed = false;
  for (BasicBlock *b : f->blocks) {
    Instr *t = b->term();
    if (!t || (t->op != Op::Br && t->op != Op::Switch) || b->succs.size() < 2) continue;
    bool ok = true;
    for (BasicBlock *s : b->succs)
      if (s->preds.size() != 1 || s == b) ok = false;
    if (!ok) continue;
    for (bool more = true; more;) {
      more = false;
      BasicBlock *s0 = b->succs[0];
      for (Instr *i : s0->ins) {
        VNKey k;
        if (dead.count(i) || i->op == Op::Load || !vnKey(i, k, consts)) continue;
        bool avail = true;   // operands defined outside every successor
        for (const Operand &o : i->a)
          if (o.isReg())
            for (BasicBlock *s : b->succs)
              for (Instr *j : s->ins)
                if (j->dst == o.r && !dead.count(j)) avail = false;
        if (!avail) continue;
        std::vector<Instr *> same;
        for (size_t n = 1; n < b->succs.size() && ok; n++) {
          Instr *m = nullptr;
          for (Instr *j : b->succs[n]->ins) {
            VNKey kj;
            if (!dead.count(j) && j->op != Op::Load && vnKey(j, kj, consts) && !(kj < k) && !(k < kj)) {
              m = j;
              break;
            }
          }
          if (!m) break;
          same.push_back(m);
        }
        if (same.size() != b->succs.size() - 1) continue;
        s0->ins.erase(std::find(s0->ins.begin(), s0->ins.end(), i));
        b->ins.insert(b->ins.end() - 1, i);
        for (Instr *j : same) {
          rename[j->dst] = i->dst;
          dead.insert(j);
        }
        changed = more = true;
        break;   // s0->ins changed
      }
    }
  }
  if (!changed) return false;
  for (BasicBlock *b : f->blocks) {
    for (Instr *i : b->ins)
      for (Operand &o : i->a)
        if (o.isReg() && rename.count(o.r)) o.r = rename[o.r];
    b->ins.erase(std::remove_if(b->ins.begin(), b->ins.end(), [&](Instr *i) { return dead.count(i) > 0; }),
                 b->ins.end());
  }
  recomputeDefs(f);
  return true;
}

void hoistCommonLegal(Function *f) { hoistCommon(f, true); }

void optimizeSSA(Function *f) {
  for (int round = 0; round < 50; round++) {
    bool c = simplify(f);
    sinkCopiesBelowPhis(f);
    c |= copyPropagate(f);
    c |= eliminateDeadCode(f);
    size_t nb = f->blocks.size();
    pruneUnreachable(f);
    c |= nb != f->blocks.size();
    c |= valueNumber(f);
    c |= hoistCommon(f, false);
    if (!c) c = hoistInvariants(f, false);   // once the rest has settled
    if (!c) break;
  }
  if (selectExtract(f)) eliminateDeadCode(f);
  recomputeDefs(f);
}
