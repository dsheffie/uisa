/* codegen.cpp: CFG cleanup, SSA construction and SSA cleanups.
 *
 * The dominator computation (LengauerTarjanDominators), the dominance
 * frontier walk and the liveness-pruned phi placement come from Decaf's
 * codegen.cpp; renaming follows Decaf's dominator-tree walk with per-variable
 * stacks.
 */
#include "codegen.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <map>
#include <set>

#include "errors.h"

// ---------------------------------------------------------------------------
// CFG utilities
// ---------------------------------------------------------------------------
static void retarget(Instr *t, BasicBlock *from, BasicBlock *to) {
  if (t->t == from) t->t = to;
  if (t->f == from) t->f = to;
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
  // split critical edges so that phi copies have a home
  std::vector<BasicBlock *> blocks = f->blocks;
  for (BasicBlock *p : blocks) {
    if (p->succs.size() < 2) continue;
    for (BasicBlock *s : std::vector<BasicBlock *>(p->succs)) {
      if (s->preds.size() < 2) continue;
      BasicBlock *e = f->newBlock("edge");
      Instr *j = new Instr(Op::Jmp);
      j->t = s;
      e->ins.push_back(j);
      Instr *t = p->term();
      if (t->t == s) t->t = e;
      if (t->f == s) t->f = e;
    }
  }
  pruneUnreachable(f);
}

// ---------------------------------------------------------------------------
// dominators (Decaf's Lengauer-Tarjan implementation)
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

  // liveness of source variables (Decaf's insertPhis pruning)
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
  for (auto &ds : defsites) {
    Reg *v = ds.first;
    std::vector<BasicBlock *> work(ds.second.begin(), ds.second.end());
    std::set<BasicBlock *> has, inWork(ds.second.begin(), ds.second.end());
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

void optimizeSSA(Function *f) {
  for (int round = 0; round < 50; round++) {
    bool c = simplify(f);
    sinkCopiesBelowPhis(f);
    c |= copyPropagate(f);
    c |= eliminateDeadCode(f);
    size_t nb = f->blocks.size();
    pruneUnreachable(f);
    c |= nb != f->blocks.size();
    if (!c) break;
  }
  if (selectExtract(f)) eliminateDeadCode(f);
  recomputeDefs(f);
}
