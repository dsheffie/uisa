/* backend.cpp: legalization, liveness/interference and uisa emission.
 *
 * Every immediate width and register index comes from uisa_intrinsics.inc,
 * which compiler/gen_intrinsics.py generates from spec/uisa.yaml.
 */
#include "backend.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "codegen.h"
#include "emit.h"
#include "errors.h"

using namespace spec;   // the spec tables (emit.h)

int numColors() { return NUM_XREGS - 1; }

static bool fits(int64_t v, int64_t lo, int64_t hi) { return v >= lo && v <= hi; }
static bool isGuestPhys(const Operand &o) { return o.k == Operand::Phys && o.phys >= GREG_BASE; }

// ---------------------------------------------------------------------------
// legalization
// ---------------------------------------------------------------------------
// Which operand slots may name any register (x, guest, x0) directly.
static bool regSlot(Instr *i, size_t k) {
  switch (i->op) {
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::DivU: case Op::Rem: case Op::RemU:
    case Op::And: case Op::Or: case Op::Xor: case Op::Shl: case Op::Shr: case Op::Sar:
    case Op::Set: case Op::Br: case Op::Sext8: case Op::Sext16: case Op::Zext8: case Op::Zext16:
    case Op::PWrite:
      return true;
    case Op::Intrin:
      return i->in->args[k] == 'v' || i->in->args[k] == 'm';
    default:
      return false;
  }
}

static bool writesGuestRegs(Instr *i) {
  if (i->op == Op::PWrite || i->op == Op::Call) return true;
  if (i->op == Op::Intrin && !strcmp(i->in->name, "__wrg")) return true;
  return false;
}

// v = copy d3 feeding uses in the same block: name d3 in the uses instead.
static void forwardGuestReads(Function *f) {
  std::map<Reg *, std::vector<std::pair<Instr *, size_t>>> uses;
  std::map<Reg *, BasicBlock *> useBlock;
  std::set<Reg *> multiBlock;
  for (BasicBlock *b : f->blocks)
    for (Instr *i : b->ins)
      for (size_t k = 0; k < i->a.size(); k++)
        if (i->a[k].isReg()) {
          Reg *r = i->a[k].r;
          uses[r].push_back({i, k});
          if (useBlock.count(r) && useBlock[r] != b) multiBlock.insert(r);
          useBlock[r] = b;
        }
  for (BasicBlock *b : f->blocks) {
    std::vector<Instr *> keep;
    for (size_t n = 0; n < b->ins.size(); n++) {
      Instr *i = b->ins[n];
      if (i->op != Op::Copy || !isGuestPhys(i->a[0]) || multiBlock.count(i->dst) ||
          (uses.count(i->dst) && useBlock[i->dst] != b)) {
        keep.push_back(i);
        continue;
      }
      int p = i->a[0].phys;
      auto &us = uses[i->dst];
      bool ok = true;
      size_t remaining = us.size();
      for (size_t m = n + 1; m < b->ins.size() && remaining; m++) {
        Instr *j = b->ins[m];
        for (auto &u : us)
          if (u.first == j) {
            if (!regSlot(j, u.second)) ok = false;
            remaining--;
          }
        if (remaining && writesGuestRegs(j) && (j->op != Op::PWrite || j->phys == p)) ok = false;
        if (!ok) break;
      }
      if (!ok || remaining) {
        keep.push_back(i);
        continue;
      }
      for (auto &u : us) u.first->a[u.second] = Operand::preg(p);
    }
    b->ins = keep;
  }
}

void legalize(Function *f) {
  forwardGuestReads(f);
  for (BasicBlock *b : f->blocks) {
    std::vector<Instr *> out;
    auto mat = [&](int32_t v, int line) {
      if (v == 0) return Operand::preg(0);
      Instr *c = new Instr(Op::Copy);
      c->dst = f->newReg("k");
      c->a = {Operand::cst(v)};
      c->line = line;
      out.push_back(c);
      return Operand::reg(c->dst);
    };
    auto needReg = [&](Instr *i, size_t k) {
      if (i->a[k].isImm()) i->a[k] = mat(i->a[k].imm, i->line);
    };
    // an address argument: base register + 12-bit displacement (constant
    // addresses split into a materialized high part, base + constant folded)
    auto addrArg = [&](Instr *i, size_t k) {
      auto &a = i->a;
      if (a[k].isImm()) {
        int64_t v = (int64_t)a[k].imm + i->disp;
        if (fits(v, MEM_LO, MEM_HI)) {
          i->disp = (int32_t)v;
          a[k] = Operand::preg(0);
        } else {
          int32_t lo = ((int32_t)v << 20) >> 20;   // sign-extended low 12 bits
          a[k] = mat((int32_t)v - lo, i->line);
          i->disp = lo;
        }
      } else if (a[k].isReg() && a[k].r->def && a[k].r->def->op == Op::Add) {
        Instr *d = a[k].r->def;
        if (d->a[1].isImm() && fits((int64_t)d->a[1].imm + i->disp, MEM_LO, MEM_HI) &&
            (d->a[0].isReg() || (d->a[0].k == Operand::Phys && d->a[0].phys == 0))) {
          i->disp += d->a[1].imm;
          a[k] = d->a[0];
        }
      }
    };
    for (Instr *i : b->ins) {
      auto &a = i->a;
      switch (i->op) {
        case Op::Phi: case Op::Copy: case Op::PWrite: case Op::Jmp: case Op::Stop: case Op::CallRes:
          break;
        case Op::Add:
          needReg(i, 0);
          if (a[1].isImm() && !fits(a[1].imm, ADDI_LO, ADDI_HI)) needReg(i, 1);
          break;
        case Op::Sub:
          needReg(i, 0);
          if (a[1].isImm()) {
            if (a[1].imm != INT32_MIN && fits(-(int64_t)a[1].imm, ADDI_LO, ADDI_HI)) {
              i->op = Op::Add;
              a[1].imm = -a[1].imm;
            } else {
              needReg(i, 1);
            }
          }
          break;
        case Op::And: case Op::Or: case Op::Xor:
          needReg(i, 0);
          if (a[1].isImm() && !fits(a[1].imm, 0, LOGI_HI) && !(i->op == Op::Xor && a[1].imm == -1)) needReg(i, 1);
          break;
        case Op::Shl: case Op::Shr: case Op::Sar:
          needReg(i, 0);
          if (a[1].isImm()) a[1].imm &= SHIFT_MAX;
          break;
        case Op::Mul: case Op::Div: case Op::DivU: case Op::Rem: case Op::RemU:
        case Op::Sext8: case Op::Sext16: case Op::Zext8: case Op::Zext16:
          for (size_t k = 0; k < a.size(); k++) needReg(i, k);
          break;
        case Op::Set: {
          if (a[0].isImm() && !a[1].isImm()) {
            std::swap(a[0], a[1]);
            i->cond = swapCond(i->cond);
          }
          needReg(i, 0);
          if (!a[1].isImm()) break;
          int64_t k = a[1].imm;
          Cond c = i->cond;
          if (c == Cond::GT && k != INT32_MAX) { c = Cond::GE; k++; }
          else if (c == Cond::LE && k != INT32_MAX) { c = Cond::LT; k++; }
          else if (c == Cond::GTU && (uint32_t)k != UINT32_MAX) { c = Cond::GEU; k = (int32_t)((uint32_t)k + 1); }
          else if (c == Cond::LEU && (uint32_t)k != UINT32_MAX) { c = Cond::LTU; k = (int32_t)((uint32_t)k + 1); }
          bool ok;
          if (c == Cond::EQ || c == Cond::NE) ok = fits(k, 0, LOGI_HI);
          else if (c == Cond::LT || c == Cond::GE || c == Cond::LTU || c == Cond::GEU) ok = fits(k, ADDI_LO, ADDI_HI);
          else ok = false;
          if (ok) {
            i->cond = c;
            a[1].imm = (int32_t)k;
          } else {
            needReg(i, 1);
          }
          break;
        }
        case Op::Br:
          needReg(i, 0);
          needReg(i, 1);
          break;
        case Op::Switch:
          needReg(i, 0);
          break;
        case Op::Ret:
          for (size_t k = 0; k < a.size(); k++) needReg(i, k);
          break;
        case Op::Call:
          for (size_t k = 0; k < a.size(); k++) needReg(i, k);
          break;
        case Op::Load:
          if (i->fobj < 0) addrArg(i, 0);
          break;
        case Op::Store:
          if (i->fobj < 0) addrArg(i, 0);
          needReg(i, 1);
          break;
        case Op::FrameAddr:
          break;
        case Op::Intrin:
          for (size_t k = 0; k < a.size(); k++) {
            char kind = i->in->args[k];
            if (kind == 'v' || kind == 't') {
              needReg(i, k);
              if (kind == 't' && a[k].k == Operand::Phys) {   // the tied register is rewritten
                Instr *c = new Instr(Op::Copy);
                c->dst = f->newReg("tie");
                c->a = {a[k]};
                c->line = i->line;
                out.push_back(c);
                a[k] = Operand::reg(c->dst);
              }
            } else if (kind == 'm') {
              i->disp = 0;
              if (a[k].isImm()) {
                if (fits(a[k].imm, MEM_LO, MEM_HI)) {
                  i->disp = a[k].imm;
                  a[k] = Operand::preg(0);
                } else {
                  int32_t lo = (a[k].imm << 20) >> 20;   // sign-extended low 12 bits
                  a[k] = mat(a[k].imm - lo, i->line);
                  i->disp = lo;
                }
              } else if (a[k].isReg() && a[k].r->def && a[k].r->def->op == Op::Add) {
                Instr *d = a[k].r->def;
                if (d->a[1].isImm() && fits(d->a[1].imm, MEM_LO, MEM_HI) &&
                    (d->a[0].isReg() || (d->a[0].k == Operand::Phys && d->a[0].phys == 0))) {
                  i->disp = d->a[1].imm;
                  a[k] = d->a[0];
                }
              }
            }
          }
          break;
      }
      out.push_back(i);
    }
    b->ins = out;
  }
  // phi constants stay immediates: out of SSA writes them on the edge
  recomputeDefs(f);
  eliminateDeadCode(f);
  recomputeDefs(f);
}

// ---------------------------------------------------------------------------
// liveness and interference
// ---------------------------------------------------------------------------
static int weightOf(BasicBlock *b) {
  int w = 1;
  for (int d = 0; d < std::min(b->loopDepth, 4); d++) w *= 10;
  return w;
}
// The edge p -> s: phi copies on it run once per traversal. With several
// successors, p's copies go in a block of their own on the edge (split at
// emission), which is inside only the loops that hold both ends.
static int edgeWeight(BasicBlock *p, BasicBlock *s) {
  if (p->succs.size() < 2) return weightOf(p);
  return weightOf(p->loopDepth < s->loopDepth ? p : s);
}

struct FnLive {
  std::map<BasicBlock *, std::set<Reg *>> liveIn, liveOut;
};

static void computeLiveness(Function *f, FnLive &L) {
  std::map<BasicBlock *, std::set<Reg *>> uses, defs, phiDefs;
  for (BasicBlock *b : f->blocks)
    for (Instr *i : b->ins) {
      if (i->op == Op::Phi) {
        phiDefs[b].insert(i->dst);
        continue;
      }
      for (auto &o : i->a)
        if (o.isReg() && !defs[b].count(o.r)) uses[b].insert(o.r);
      if (i->dst) defs[b].insert(i->dst);
    }
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto it = f->blocks.rbegin(); it != f->blocks.rend(); ++it) {
      BasicBlock *b = *it;
      std::set<Reg *> out;
      for (BasicBlock *s : b->succs) {
        for (Reg *r : L.liveIn[s])
          if (!phiDefs[s].count(r)) out.insert(r);
        for (Instr *i : s->ins) {
          if (i->op != Op::Phi) break;
          for (size_t k = 0; k < i->preds.size(); k++)
            if (i->preds[k] == b && i->a[k].isReg()) out.insert(i->a[k].r);
        }
      }
      std::set<Reg *> in = uses[b];
      in.insert(phiDefs[b].begin(), phiDefs[b].end());
      for (Reg *r : out)
        if (!defs[b].count(r)) in.insert(r);
      if (out != L.liveOut[b] || in != L.liveIn[b]) {
        L.liveOut[b] = out;
        L.liveIn[b] = in;
        changed = true;
      }
    }
  }
}

static std::string names(const std::set<Reg *> &s) {   // in creation order, not pointer order
  std::vector<Reg *> v(s.begin(), s.end());
  std::sort(v.begin(), v.end(), [](Reg *a, Reg *b) { return a->id < b->id; });
  std::string r;
  for (Reg *x : v) r += (r.empty() ? "" : ", ") + x->name;
  return r;
}

void buildAllocProblem(Program *p, AllocProblem &ap, bool report) {
  std::map<Reg *, int> order;
  auto addVal = [&](Function *f, Reg *r) {
    if (order.count(r)) return;
    order[r] = (int)ap.vals.size();
    ap.vals.push_back(r);
    ap.fvals[f].push_back(r);
  };
  auto edge = [&](Reg *a, Reg *b) {
    if (a == b) return;
    if (order[a] > order[b]) std::swap(a, b);
    ap.edges.insert({a, b});
  };
  for (Function *f : p->funcs) {
    if (f->isAsm) {   // fixed registers, all written by the call; the implicit reads are not
      for (int k = 0; k < f->asmArgs; k++) addVal(f, f->params[k]);
      for (Reg *r : f->retRegs) addVal(f, r);
      for (Reg *r : f->clobberRegs) addVal(f, r);
      continue;
    }
    while ((int)f->retRegs.size() < f->retWords) f->retRegs.push_back(f->newReg("ret"));
    for (Reg *r : f->entryRegs) addVal(f, r);
    for (Reg *r : f->params) addVal(f, r);
    for (Reg *r : f->retRegs) addVal(f, r);
    for (BasicBlock *b : f->blocks)
      for (Instr *i : b->ins) {
        if (i->dst) addVal(f, i->dst);
        for (auto &o : i->a)
          if (o.isReg()) addVal(f, o.r);
      }
  }
  for (Function *f : p->funcs) {
    if (f->isAsm) {
      ap.maxLive[f] = (int)ap.fvals[f].size();
      continue;
    }
    for (size_t k = 1; k < f->retRegs.size(); k++) edge(f->retRegs[0], f->retRegs[k]);
    FnLive L;
    computeLiveness(f, L);
    int maxLive = 0, worstLine = 0;
    std::set<Reg *> worst;
    auto note = [&](const std::set<Reg *> &live, int line) {
      if ((int)live.size() <= maxLive) return;
      maxLive = (int)live.size();
      worst = live;
      worstLine = line ? line : f->line;
    };
    for (BasicBlock *b : f->blocks) {
      int w = weightOf(b);
      std::set<Reg *> live = L.liveOut[b];
      for (auto it = b->ins.rbegin(); it != b->ins.rend(); ++it) {
        Instr *i = *it;
        if (i->op == Op::Phi) break;
        if (i->op == Op::Call) {
          AllocProblem::CallSite cs{f, i->callee, {}, i->line};
          for (Reg *r : live)
            if (r != i->dst) cs.liveAcross.push_back(r);   // a CallRes result is defined after the call
          std::sort(cs.liveAcross.begin(), cs.liveAcross.end(), [&](Reg *x, Reg *y) { return order[x] < order[y]; });
          ap.calls.push_back(cs);
          size_t np = i->callee->isAsm ? (size_t)i->callee->asmArgs : i->callee->params.size();
          for (size_t k = 0; k < i->a.size() && k < np; k++)
            if (i->a[k].isReg()) ap.prefs.push_back({i->a[k].r, i->callee->params[k], w});
          if (i->callee->isAsm) {   // a fixed register live across a routine that writes it
            std::set<int> wr;
            for (Reg *r : ap.fvals[i->callee]) wr.insert(r->precolor);
            for (Reg *r : cs.liveAcross)
              if (r->precolor >= 0 && wr.count(r->precolor))
                error(i->line, "%s (x%d) is still needed after the call to %s, which overwrites it",
                      r->name.c_str(), r->precolor, i->callee->name.c_str());
          }
          if (i->dst && !i->callee->retRegs.empty()) ap.prefs.push_back({i->dst, i->callee->retRegs[0], w});
        }
        if (i->op == Op::CallRes) {
          // both result words are copied out of the return registers together
          ap.prefs.push_back({i->dst, i->callee->retRegs[1], w});
          auto it2 = std::find(b->ins.begin(), b->ins.end(), i);
          if (it2 == b->ins.begin() || (*(it2 - 1))->op != Op::Call) fatal(i->line, "internal: CallRes not after its call");
          if (Reg *d1 = (*(it2 - 1))->dst) edge(d1, i->dst);
        }
        if (i->dst) {
          std::set<Reg *> after = live;
          after.insert(i->dst);
          note(after, i->line);
          for (Reg *v : live) {
            if (i->op == Op::Copy && i->a[0].isReg() && i->a[0].r == v) continue;
            edge(i->dst, v);
          }
          if (i->op == Op::Intrin && i->in->tied >= 0) {
            for (size_t k = 0; k < i->a.size(); k++)
              if ((int)k != i->in->tied && i->a[k].isReg()) edge(i->dst, i->a[k].r);
            if (i->a[i->in->tied].isReg()) ap.prefs.push_back({i->dst, i->a[i->in->tied].r, w});
          }
          if (i->op == Op::Copy && i->a[0].isReg()) ap.prefs.push_back({i->dst, i->a[0].r, w});
          live.erase(i->dst);
        }
        if (i->op == Op::Ret)
          for (size_t k = 0; k < i->a.size(); k++)
            if (i->a[k].isReg()) ap.prefs.push_back({i->a[k].r, f->retRegs[k], w});
        for (auto &o : i->a)
          if (o.isReg()) live.insert(o.r);
        note(live, i->line);
      }
      // phis are defined together at the top of the block
      std::set<Reg *> top = live;
      for (Instr *i : b->ins) {
        if (i->op != Op::Phi) break;
        top.insert(i->dst);
        for (size_t k = 0; k < i->a.size(); k++)
          if (i->a[k].isReg()) ap.prefs.push_back({i->dst, i->a[k].r, edgeWeight(i->preds[k], b)});
      }
      for (Instr *i : b->ins) {
        if (i->op != Op::Phi) break;
        for (Reg *v : top) edge(i->dst, v);
      }
      note(top, b->ins.empty() ? 0 : b->ins[0]->line);
      if (b == f->blocks[0]) {   // parameters and entry-ABI values arrive together
        std::set<Reg *> entry = top;
        entry.insert(f->params.begin(), f->params.end());
        entry.insert(f->entryRegs.begin(), f->entryRegs.end());
        for (Reg *r : f->params)
          for (Reg *v : entry) edge(r, v);
        for (Reg *r : f->entryRegs)
          for (Reg *v : entry) edge(r, v);
        note(entry, f->line);
      }
    }
    if (report && maxLive > numColors())   // reported once, where the pressure peaks
      error(worstLine, "%d values live at once in '%s' but only %d registers: %s", maxLive, f->name.c_str(),
            numColors(), names(worst).c_str());
    ap.maxLive[f] = maxLive;
  }
}

// A necessary condition for whole-program allocation: along every call chain,
// the values live across each call plus what the callee needs must fit.
static std::vector<Function *> pressureCheck(Program *p, AllocProblem &ap, bool report) {
  std::vector<Function *> over;
  std::map<Function *, int> need;
  std::map<Function *, std::string> why;
  std::function<int(Function *)> needOf = [&](Function *f) -> int {
    if (need.count(f)) return need[f];
    int n = ap.maxLive[f];
    n = std::max(n, (int)f->retRegs.size());
    std::string w = f->name + " alone";
    for (auto &cs : ap.calls) {
      if (cs.caller != f) continue;
      int m = (int)cs.liveAcross.size() + needOf(cs.callee);
      if (m > n) {
        n = m;
        w = std::to_string(cs.liveAcross.size()) + " values live across the call to " + cs.callee->name +
            " (line " + std::to_string(cs.line) + "), which needs " + std::to_string(needOf(cs.callee)) +
            " (" + why[cs.callee] + ")";
      }
    }
    why[f] = w;
    return need[f] = n;
  };
  for (Function *f : p->funcs) {
    int n = needOf(f);
    if (n <= numColors()) continue;
    over.push_back(f);
    if (report)
      error(f->line, "'%s' needs at least %d registers but only %d exist: %s", f->name.c_str(), n, numColors(),
            why[f].c_str());
  }
  return over;
}
void checkPressure(Program *p, AllocProblem &ap) { pressureCheck(p, ap, true); }
std::vector<Function *> overPressure(Program *p, AllocProblem &ap) { return pressureCheck(p, ap, false); }

void verifyAllocation(Program *p, AllocProblem &ap) {
  (void)p;
  for (Reg *v : ap.vals) {
    if (v->color < 1 || v->color > numColors()) fatal(0, "internal: %s has no register", v->str().c_str());
    if (v->precolor >= 0 && v->color != v->precolor) fatal(0, "internal: %s lost its precolor", v->str().c_str());
  }
  for (auto &e : ap.edges)
    if (e.first->color == e.second->color)
      fatal(0, "internal: interfering %s and %s share x%d", e.first->str().c_str(), e.second->str().c_str(),
            e.first->color);
  // registers each function may write, including everything its callees write
  std::map<Function *, std::set<int>> used;
  std::function<const std::set<int> &(Function *)> usedOf = [&](Function *f) -> const std::set<int> & {
    if (used.count(f)) return used[f];
    std::set<int> s;
    for (Reg *v : ap.fvals[f]) s.insert(v->color);
    for (Function *c : f->callees) {
      const std::set<int> &cu = usedOf(c);
      s.insert(cu.begin(), cu.end());
    }
    return used[f] = s;
  };
  for (auto &cs : ap.calls)
    for (Reg *v : cs.liveAcross)
      if (usedOf(cs.callee).count(v->color))
        fatal(cs.line, "internal: %s (x%d) is live across the call to %s, which clobbers it", v->str().c_str(),
              v->color, cs.callee->name.c_str());
}

// ---------------------------------------------------------------------------
// emission
// ---------------------------------------------------------------------------
std::string R(const Operand &o) {
  switch (o.k) {
    case Operand::R: return "x" + std::to_string(o.r->color);
    case Operand::Phys: return regName(o.phys);
    case Operand::Imm:
      if (o.imm == 0) return "x0";
      fatal(0, "internal: constant %d where a register is needed", o.imm);
    default: fatal(0, "internal: missing operand");
  }
}
std::string R(Reg *r) { return "x" + std::to_string(r->color); }

std::string blockLabel(BasicBlock *b) { return b->fn->name + "__" + std::to_string(b->id); }

void emitInstr(Emitter &E, Instr *i, Instr *next) {
  auto &a = i->a;
  std::string d = i->dst ? R(i->dst) : "";
  auto ri = [&](const char *rr, const char *ii) {   // reg-reg or reg-imm form
    if (a[1].isImm()) E.text(std::string(ii) + " " + d + ", " + R(a[0]) + ", " + std::to_string(a[1].imm));
    else E.text(std::string(rr) + " " + d + ", " + R(a[0]) + ", " + R(a[1]));
  };
  switch (i->op) {
    case Op::Copy:
      if (a[0].isImm()) E.li(d, a[0].imm);
      else E.mov(d, R(a[0]));
      break;
    case Op::Add: ri("add", "addi"); break;
    case Op::Sub: E.text("sub " + d + ", " + R(a[0]) + ", " + R(a[1])); break;
    case Op::And: ri("and", "andi"); break;
    case Op::Or: ri("or", "ori"); break;
    case Op::Xor:
      if (a[1].isImm() && a[1].imm == -1) E.text("orn " + d + ", x0, " + R(a[0]));
      else ri("xor", "xori");
      break;
    case Op::Shl: ri("sll", "slli"); break;
    case Op::Shr: ri("srl", "srli"); break;
    case Op::Sar: ri("sra", "srai"); break;
    case Op::Mul: E.text("mul " + d + ", " + R(a[0]) + ", " + R(a[1])); break;
    case Op::Div: E.text("div " + d + ", " + R(a[0]) + ", " + R(a[1])); break;
    case Op::DivU: E.text("divu " + d + ", " + R(a[0]) + ", " + R(a[1])); break;
    case Op::Rem: E.text("rem " + d + ", " + R(a[0]) + ", " + R(a[1])); break;
    case Op::RemU: E.text("remu " + d + ", " + R(a[0]) + ", " + R(a[1])); break;
    case Op::Sext8: E.text("sext8 " + d + ", " + R(a[0])); break;
    case Op::Sext16: E.text("sext16 " + d + ", " + R(a[0])); break;
    case Op::Zext8: E.text("zext8 " + d + ", " + R(a[0])); break;
    case Op::Zext16: E.text("zext16 " + d + ", " + R(a[0])); break;
    case Op::Set: {
      std::string x = R(a[0]);
      bool imm = a[1].isImm();
      std::string y = imm ? std::to_string(a[1].imm) : R(a[1]);
      switch (i->cond) {
        case Cond::EQ: case Cond::NE:
          if (imm && a[1].imm == 0) {
            if (i->cond == Cond::EQ) E.text("sltiu " + d + ", " + x + ", 1");
            else E.text("sltu " + d + ", x0, " + x);
            break;
          }
          E.text((imm ? "xori " : "xor ") + d + ", " + x + ", " + y);
          if (i->cond == Cond::EQ) E.text("sltiu " + d + ", " + d + ", 1");
          else E.text("sltu " + d + ", x0, " + d);
          break;
        case Cond::LT: case Cond::GE:
          E.text((imm ? "slti " : "slt ") + d + ", " + x + ", " + y);
          if (i->cond == Cond::GE) E.text("xori " + d + ", " + d + ", 1");
          break;
        case Cond::LTU: case Cond::GEU:
          E.text((imm ? "sltiu " : "sltu ") + d + ", " + x + ", " + y);
          if (i->cond == Cond::GEU) E.text("xori " + d + ", " + d + ", 1");
          break;
        case Cond::GT: case Cond::LE:
          E.text("slt " + d + ", " + y + ", " + x);
          if (i->cond == Cond::LE) E.text("xori " + d + ", " + d + ", 1");
          break;
        case Cond::GTU: case Cond::LEU:
          E.text("sltu " + d + ", " + y + ", " + x);
          if (i->cond == Cond::LEU) E.text("xori " + d + ", " + d + ", 1");
          break;
      }
      break;
    }
    case Op::Intrin: {
      const IntrinsicDesc *in = i->in;
      if (in->tied >= 0) E.mov(d, R(a[in->tied]));
      std::string s;
      for (const char *p = in->asmfmt; *p; p++) {
        if (*p != '%') {
          s += *p;
          continue;
        }
        p++;
        if (*p == 'd') {
          s += d;
        } else if (*p == 'm') {
          p++;
          int k = *p - '0';
          s += std::to_string(i->disp) + "(" + R(a[k]) + ")";
        } else {
          int k = *p - '0';
          s += in->args[k] == 'i' ? std::to_string(a[k].imm) : R(a[k]);
        }
      }
      E.text(s);
      break;
    }
    case Op::Call: {
      std::vector<std::pair<std::string, std::string>> moves;
      for (size_t k = 0; k < a.size(); k++) moves.push_back({R(i->callee->params[k]), R(a[k])});
      E.parallelCopy(moves);
      E.text("call " + i->callee->name);
      moves.clear();
      if (i->dst) moves.push_back({d, R(i->callee->retRegs[0])});
      if (next && next->op == Op::CallRes) moves.push_back({R(next->dst), R(i->callee->retRegs[1])});
      E.parallelCopy(moves);
      break;
    }
    case Op::CallRes:   // emitted with its Call
      break;
    case Op::PWrite:
      if (a[0].isImm()) E.li(regName(i->phys), a[0].imm);
      else E.mov(regName(i->phys), R(a[0]));
      break;
    case Op::Phi:
      break;
    default:
      fatal(i->line, "internal: cannot emit %s", i->str().c_str());
  }
}

// Out of SSA, step 1: a phi's copies run on the edge from each predecessor,
// emitted at the end of that predecessor. A predecessor with several
// successors (a critical edge into a block with phis) gets a block of its own
// on that edge, placed just before the phi block so it falls through.
void splitCriticalEdges(Function *f) {
  for (BasicBlock *p : std::vector<BasicBlock *>(f->blocks)) {
    if (p->succs.size() < 2) continue;
    for (BasicBlock *s : std::vector<BasicBlock *>(p->succs)) {
      if (s->preds.size() < 2 || s->ins.empty() || s->ins[0]->op != Op::Phi) continue;
      BasicBlock *e = f->newBlock(p->name + "_" + s->name + "_crit_edge");
      f->blocks.pop_back();
      f->blocks.insert(std::find(f->blocks.begin(), f->blocks.end(), s), e);
      Instr *j = new Instr(Op::Jmp);
      j->t = s;
      j->line = p->term()->line;
      e->ins.push_back(j);
      Instr *t = p->term();
      if (t->t == s) t->t = e;
      if (t->f == s) t->f = e;
      for (BasicBlock *&b : t->targets)
        if (b == s) b = e;
      for (Instr *i : s->ins) {
        if (i->op != Op::Phi) break;
        for (BasicBlock *&q : i->preds)
          if (q == p) q = e;
      }
    }
  }
  f->rebuildEdges();
}

// The registers f or anything it calls may write: its values' registers.
// Callers already keep values live across a call to f out of them, so f can
// use one as scratch wherever it holds nothing live.
static const std::set<int> &writtenRegs(Function *f) {
  static std::map<Function *, std::set<int>> memo;
  auto it = memo.find(f);
  if (it != memo.end()) return it->second;
  std::set<int> &w = memo[f];
  auto add = [&](Reg *r) {
    if (r && r->color > 0) w.insert(r->color);
  };
  for (Reg *r : f->params) add(r);
  for (Reg *r : f->entryRegs) add(r);
  for (Reg *r : f->retRegs) add(r);
  for (Reg *r : f->clobberRegs) add(r);
  for (BasicBlock *b : f->blocks)
    for (Instr *i : b->ins) {
      add(i->dst);
      for (auto &o : i->a)
        if (o.isReg()) add(o.r);
    }
  for (Function *g : f->callees)
    if (g != f) {
      const std::set<int> &gw = writtenRegs(g);
      w.insert(gw.begin(), gw.end());
    }
  return w;
}

static void emitFunction(Function *f, FILE *out) {
  splitCriticalEdges(f);
  FnLive live;
  computeLiveness(f, live);
  // each block's body, including the phi copies on its (unique, after edge
  // splitting) outgoing edge
  std::map<BasicBlock *, std::vector<AsmLine>> body;
  int line = f->line;
  for (BasicBlock *b : f->blocks) {
    Emitter B;
    B.curLine = line;
    for (size_t k = 0; k < b->ins.size(); k++) {
      Instr *i = b->ins[k];
      if (i->line) B.curLine = i->line;
      if (i->isTerm()) break;
      emitInstr(B, i, k + 1 < b->ins.size() ? b->ins[k + 1] : nullptr);
    }
    if (b->succs.size() == 1) {
      BasicBlock *s = b->succs[0];
      std::vector<std::pair<std::string, std::string>> moves;
      std::vector<std::pair<std::string, int32_t>> consts;   // after the moves: they read no register
      for (Instr *i : s->ins) {
        if (i->op != Op::Phi) break;
        for (size_t k = 0; k < i->preds.size(); k++)
          if (i->preds[k] == b) {
            if (i->a[k].isImm()) consts.push_back({R(i->dst), i->a[k].imm});
            else moves.push_back({R(i->dst), R(i->a[k])});
          }
      }
      // scratch for copy cycles: a register f may write that holds nothing
      // live across the edge, is no copy's source or destination, and is not
      // read by b's terminator (registers of constant phis are fine: they are
      // written after the copies)
      std::set<int> busy;
      for (Reg *r : live.liveIn[s]) busy.insert(r->color);
      for (Instr *i : s->ins) {
        if (i->op != Op::Phi) break;
        for (size_t k = 0; k < i->preds.size(); k++)
          if (i->preds[k] == b && i->a[k].isImm()) busy.erase(i->dst->color);
      }
      for (auto &o : b->term()->a)
        if (o.isReg()) busy.insert(o.r->color);
      for (auto &m : moves) {
        busy.insert(atoi(m.first.c_str() + 1));
        busy.insert(atoi(m.second.c_str() + 1));
      }
      std::vector<std::string> temps;
      for (int r : writtenRegs(f))
        if (!busy.count(r)) temps.push_back("x" + std::to_string(r));
      B.parallelCopy(moves, temps);
      for (auto &c : consts) B.li(c.first, c.second);
    }
    line = B.curLine;
    body[b] = B.out;
  }
  Emitter E;
  E.label(f->name);
  emitLayout(f, body, E, out, [&](Emitter &E, Instr *t) {
    std::vector<std::pair<std::string, std::string>> moves;
    for (size_t k = 0; k < t->a.size(); k++) moves.push_back({R(f->retRegs[k]), R(t->a[k])});
    E.parallelCopy(moves);
    E.text("ret");
  });
}

// Lay out the blocks (empty jump-only blocks are skipped), emit the
// terminators, relax out-of-range branches and print. E holds the function's
// label (and any prologue); `ret` emits a Ret terminator.
// ---------------------------------------------------------------------------
// tail merging (cross-jumping), on the final assembly
// ---------------------------------------------------------------------------
// An instruction after which control never falls through: a return, or a
// no-return intrinsic (gexit, gfault, halt, ...).
static bool endsControl(const std::string &text) {
  static std::set<std::string> stops;
  if (stops.empty()) {
    stops.insert("ret");
    for (const IntrinsicDesc &d : spec::INTRINSICS)
      if (d.noreturn) stops.insert(std::string(d.asmfmt).substr(0, std::string(d.asmfmt).find(' ')));
  }
  if (text.compare(0, 7, "jalr x0") == 0) return true;
  return stops.count(text.substr(0, text.find(' '))) > 0;
}
static bool sameLine(const AsmLine &a, const AsmLine &b) {
  return a.kind == b.kind && a.text == b.text && a.cond == b.cond && a.rs1 == b.rs1 && a.rs2 == b.rs2 &&
         a.target == b.target;
}

// Registers are assigned, so identical instruction text is identical work.
// Two blocks that end in the same instructions and then go to the same place
// (the same jump target, the label they fall into, or the same return or
// no-return instruction) keep one copy of those instructions: the other
// block jumps into the middle of the first. Repeats until nothing merges.
static void tailMerge(std::vector<AsmLine> &L, const std::string &fname) {
  int fresh = 0;
  for (bool changed = true; changed;) {
    changed = false;
    // segments: [begin, end) of the instructions after a label or a transfer,
    // the transfer included; exit = where control goes next
    struct Seg { size_t begin, end, body; std::string exit; bool fall; };
    std::vector<Seg> segs;
    size_t start = 0;
    bool table = false;   // bx and its jump table are never touched
    auto close = [&](size_t end, bool fall) {
      if (end <= start) return;
      Seg g{start, end, end, "", fall};
      const AsmLine &last = L[end - 1];
      if (fall) {
        g.exit = end < L.size() && L[end].kind == AsmLine::Label ? "L:" + L[end].text : "";
      } else if (last.kind == AsmLine::Jump) {
        g.body = end - 1;
        g.exit = "J:" + last.target;
      } else {
        g.body = end - 1;
        g.exit = "T:" + last.text;
      }
      if (!table && !g.exit.empty()) segs.push_back(g);
    };
    for (size_t k = 0; k < L.size(); k++) {
      const AsmLine &l = L[k];
      if (l.kind == AsmLine::Label) {
        close(k, true);
        start = k + 1;
        table = false;
      } else if (l.kind == AsmLine::Text && l.text.compare(0, 3, "bx ") == 0) {
        table = true;
      } else if (l.kind == AsmLine::Jump || (l.kind == AsmLine::Text && endsControl(l.text))) {
        close(k + 1, false);
        start = k + 1;
      }
    }
    close(L.size(), true);
    // a fall-through segment into label X is the same exit as a jump to X
    for (Seg &g : segs)
      if (g.exit.compare(0, 2, "L:") == 0) g.exit = "J:" + g.exit.substr(2);
    std::map<std::string, std::vector<size_t>> byExit;
    for (size_t n = 0; n < segs.size(); n++) byExit[segs[n].exit].push_back(n);
    for (auto &e : byExit) {
      auto &v = e.second;
      for (size_t x = 0; x < v.size() && !changed; x++)
        for (size_t y = 0; y < v.size() && !changed; y++) {
          if (x == y) continue;
          Seg &keep = segs[v[x]], &other = segs[v[y]];
          size_t n = 0;   // common suffix of the bodies
          while (n < keep.body - keep.begin && n < other.body - other.begin &&
                 sameLine(L[keep.body - 1 - n], L[other.body - 1 - n]))
            n++;
          // the other block loses n body lines (and its jump or terminal) and
          // gains one jump; a falling-through one had no jump to lose
          int saved = (int)n + (other.fall ? 0 : 1) - 1;
          if (saved <= 0 || (keep.fall == false && other.fall && n < 2)) continue;
          std::string lab = fname + ".tm" + std::to_string(fresh++);
          AsmLine j;
          j.kind = AsmLine::Jump;
          j.target = lab;
          j.line = L[other.body - n].line;
          AsmLine lb;
          lb.kind = AsmLine::Label;
          lb.text = lab;
          size_t at = keep.body - n;
          // edit the later position first so the earlier indices stay valid
          if (other.begin > at) {
            L.erase(L.begin() + (other.body - n), L.begin() + other.end);
            L.insert(L.begin() + (other.body - n), j);
            L.insert(L.begin() + at, lb);
          } else {
            L.insert(L.begin() + at, lb);
            L.erase(L.begin() + (other.body - n), L.begin() + other.end);
            L.insert(L.begin() + (other.body - n), j);
          }
          changed = true;
        }
      if (changed) break;
    }
  }
}

// After tail merging: a branch or jump to a block that only jumps goes
// straight to where it leads; a jump to the label right after it is
// dropped; a block that is neither jumped to nor fallen into is deleted.
static void threadJumps(std::vector<AsmLine> &L) {
  for (bool changed = true; changed;) {
    changed = false;
    std::map<std::string, std::string> alias;   // label -> target of the jump it stands on
    for (size_t k = 0; k < L.size(); k++) {
      if (L[k].kind != AsmLine::Label) continue;
      size_t m = k;
      while (m < L.size() && L[m].kind == AsmLine::Label) m++;
      if (m < L.size() && L[m].kind == AsmLine::Jump) alias[L[k].text] = L[m].target;
    }
    auto final = [&](std::string t) {
      for (int n = 0; n < 100 && alias.count(t) && alias[t] != t; n++) t = alias[t];
      return t;
    };
    std::set<std::string> used;
    for (AsmLine &l : L)
      if ((l.kind == AsmLine::Branch || l.kind == AsmLine::Jump) && !l.target.empty()) {
        std::string t = final(l.target);
        if (t != l.target) {
          l.target = t;
          changed = true;
        }
        used.insert(l.target);
      }
    std::vector<AsmLine> out;
    bool reachable = true;   // can control fall into the next line?
    bool inTable = false;
    for (size_t k = 0; k < L.size(); k++) {
      AsmLine &l = L[k];
      if (l.kind == AsmLine::Label) {
        if (used.count(l.text)) reachable = true;
        inTable = false;
        if (reachable) out.push_back(l);
        else changed = true;   // the label of a dead block
        continue;
      }
      if (!reachable && !inTable) {
        changed = true;
        continue;
      }
      if (l.kind == AsmLine::Jump && !inTable) {   // a jump to the very next label falls through
        size_t m = k + 1;
        bool next = false;
        for (; m < L.size() && L[m].kind == AsmLine::Label; m++)
          if (L[m].text == l.target) next = true;
        if (next) {
          changed = true;
          continue;
        }
      }
      out.push_back(l);
      if (l.kind == AsmLine::Text && l.text.compare(0, 3, "bx ") == 0) inTable = true;
      else if (!inTable && (l.kind == AsmLine::Jump || (l.kind == AsmLine::Text && endsControl(l.text))))
        reachable = false;
    }
    L.swap(out);
  }
}

void emitLayout(Function *f, std::map<BasicBlock *, std::vector<AsmLine>> &body, Emitter &E, FILE *out,
                const std::function<void(Emitter &, Instr *)> &ret) {
  // blocks that end up empty and only jump are not emitted; branches to
  // them go straight to where they lead
  auto skippable = [&](BasicBlock *b) {
    return b != f->blocks[0] && body[b].empty() && b->term()->op == Op::Jmp;
  };
  auto final = [&](BasicBlock *b) {
    for (int n = 0; n < 1000 && skippable(b); n++) b = b->term()->t;
    return b;
  };
  std::vector<BasicBlock *> layout;
  for (BasicBlock *b : f->blocks)
    if (!skippable(b)) layout.push_back(b);

  for (size_t n = 0; n < layout.size(); n++) {
    BasicBlock *b = layout[n];
    BasicBlock *next = n + 1 < layout.size() ? layout[n + 1] : nullptr;
    if (n) E.label(blockLabel(b));
    E.out.insert(E.out.end(), body[b].begin(), body[b].end());
    Instr *t = b->term();
    E.curLine = t->line ? t->line : (body[b].empty() ? E.curLine : body[b].back().line);
    switch (t->op) {
      case Op::Jmp:
        if (final(t->t) != next) E.jump(blockLabel(final(t->t)));
        break;
      case Op::Br: {
        BasicBlock *tt = final(t->t), *ff = final(t->f);
        if (tt == next) {
          E.branch(invert(t->cond), R(t->a[0]), R(t->a[1]), blockLabel(ff));
        } else {
          E.branch(t->cond, R(t->a[0]), R(t->a[1]), blockLabel(tt));
          if (ff != next) E.jump(blockLabel(ff));
        }
        break;
      }
      case Op::Switch:   // bx, then one jal slot per table entry
        E.text("bx " + R(t->a[0]) + ", " + std::to_string(t->swLsb) + ", " + std::to_string(t->swWidth));
        for (BasicBlock *s : t->targets) E.jump(blockLabel(final(s)));
        break;
      case Op::Ret:
        ret(E, t);
        break;
      case Op::Stop:
        break;
      default:
        fatal(t->line, "internal: bad terminator");
    }
  }

  tailMerge(E.out, f->name);
  threadJumps(E.out);

  // branch relaxation: a compare-and-branch reaches BR_LO..BR_HI bytes;
  // beyond that, branch around a jal on the inverted condition
  std::vector<AsmLine> &L = E.out;
  for (bool changed = true; changed;) {
    changed = false;
    std::map<std::string, int> addr;
    int pc = 0;
    for (auto &l : L) {
      if (l.kind == AsmLine::Label) addr[l.text] = pc;
      else pc += 4;
    }
    pc = 0;
    for (size_t k = 0; k < L.size(); k++) {
      AsmLine &l = L[k];
      if (l.kind == AsmLine::Label) continue;
      if (l.kind == AsmLine::Branch && !l.target.empty()) {
        int disp = addr.at(l.target) - pc;
        if (!fits(disp, BR_LO, BR_HI)) {
          AsmLine j;
          j.kind = AsmLine::Jump;
          j.target = l.target;
          j.line = l.line;
          l.cond = invert(l.cond);
          l.target = "";    // skips the jal: displacement 8
          L.insert(L.begin() + k + 1, j);
          changed = true;
          break;
        }
      }
      if (l.kind == AsmLine::Jump && !fits(addr.at(l.target) - pc, JAL_LO, JAL_HI))
        fatal(l.line, "jump in '%s' out of jal range", f->name.c_str());
      pc += 4;
    }
  }

  int last = -1;
  for (auto &l : L) {
    if (l.kind == AsmLine::Label) {
      fprintf(out, "%s:\n", l.text.c_str());
      continue;
    }
    std::string s;
    switch (l.kind) {
      case AsmLine::Text: s = l.text; break;
      case AsmLine::Branch:
        s = std::string("b.") + condName(l.cond) + " " + l.rs1 + ", " + l.rs2 + ", " + (l.target.empty() ? "8" : l.target);
        break;
      case AsmLine::Jump: s = "jal x0, " + l.target; break;
      default: break;
    }
    if (l.line && l.line != last) {
      fprintf(out, "        %-36s ; line %d\n", s.c_str(), l.line);
      last = l.line;
    } else {
      fprintf(out, "        %s\n", s.c_str());
    }
  }
  fprintf(out, "\n");
}

void emitProgram(Program *p, FILE *out, const char *srcName) {
  fprintf(out, "; Generated by ucc from %s. Do not edit.\n\n", srcName);
  for (Function *f : p->funcs)
    if (!f->isAsm) emitFunction(f, out);   // hand-written routines live in millicode/*.s
}
