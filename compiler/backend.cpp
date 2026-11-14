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
#include "errors.h"

namespace spec {
#include "uisa_intrinsics.inc"
}
using namespace spec;

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
        case Op::Ret:
          for (size_t k = 0; k < a.size(); k++) needReg(i, k);
          break;
        case Op::Call:
          for (size_t k = 0; k < a.size(); k++) needReg(i, k);
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
  // phi constants are materialized at the end of the incoming block
  for (BasicBlock *b : f->blocks)
    for (Instr *i : b->ins) {
      if (i->op != Op::Phi) continue;
      for (size_t k = 0; k < i->a.size(); k++) {
        if (!i->a[k].isImm()) continue;
        BasicBlock *p = i->preds[k];
        Instr *c = new Instr(Op::Copy);
        c->dst = f->newReg("k");
        c->a = {i->a[k]};
        c->line = i->line;
        p->ins.insert(p->ins.end() - 1, c);
        i->a[k] = Operand::reg(c->dst);
      }
    }
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

static std::string names(const std::set<Reg *> &s) {
  std::string r;
  for (Reg *v : s) r += (r.empty() ? "" : ", ") + v->name;
  return r;
}

void buildAllocProblem(Program *p, AllocProblem &ap) {
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
          ap.calls.push_back(cs);
          for (size_t k = 0; k < i->a.size() && k < i->callee->params.size(); k++)
            if (i->a[k].isReg()) ap.prefs.push_back({i->a[k].r, i->callee->params[k], w});
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
          if (i->a[k].isReg()) ap.prefs.push_back({i->dst, i->a[k].r, weightOf(i->preds[k])});
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
    if (maxLive > numColors())   // reported once, where the pressure peaks
      error(worstLine, "%d values live at once in '%s' but only %d registers: %s", maxLive, f->name.c_str(),
            numColors(), names(worst).c_str());
    ap.maxLive[f] = maxLive;
  }
}

// A necessary condition for whole-program allocation: along every call chain,
// the values live across each call plus what the callee needs must fit.
void checkPressure(Program *p, AllocProblem &ap) {
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
    if (n > numColors())
      error(f->line, "'%s' needs at least %d registers but only %d exist: %s", f->name.c_str(), n, numColors(),
            why[f].c_str());
  }
}

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
struct AsmLine {
  enum Kind { Text, Label, Branch, Jump } kind = Text;
  std::string text;              // Text: the instruction; Label: the name
  Cond cond = Cond::EQ;          // Branch
  std::string rs1, rs2, target;  // Branch / Jump
  int line = 0;
};

class Emitter {
 public:
  std::vector<AsmLine> out;
  int curLine = 0;
  void text(const std::string &s) {
    AsmLine l;
    l.text = s;
    l.line = curLine;
    out.push_back(l);
  }
  void label(const std::string &s) {
    AsmLine l;
    l.kind = AsmLine::Label;
    l.text = s;
    out.push_back(l);
  }
  void branch(Cond c, const std::string &a, const std::string &b, const std::string &t) {
    // uisa has eq ne lt ge ltu geu; the others swap their operands
    std::string x = a, y = b;
    if (c == Cond::GT || c == Cond::LE || c == Cond::GTU || c == Cond::LEU) {
      std::swap(x, y);
      c = swapCond(c);
    }
    AsmLine l;
    l.kind = AsmLine::Branch;
    l.cond = c;
    l.rs1 = x;
    l.rs2 = y;
    l.target = t;
    l.line = curLine;
    out.push_back(l);
  }
  void jump(const std::string &t) {
    AsmLine l;
    l.kind = AsmLine::Jump;
    l.target = t;
    l.line = curLine;
    out.push_back(l);
  }
  void mov(const std::string &d, const std::string &s) {
    if (d != s) text("add " + d + ", " + s + ", x0");
  }
  void li(const std::string &d, int32_t v) {
    if (fits(v, ADDI_LO, ADDI_HI)) {
      text("addi " + d + ", x0, " + std::to_string(v));
      return;
    }
    uint32_t u = (uint32_t)v;
    uint32_t lo = u & ((1u << LUI_SHIFT) - 1);
    text("lui " + d + ", " + std::to_string(u >> LUI_SHIFT));
    if (lo) text("ori " + d + ", " + d + ", " + std::to_string(lo));
  }
  // Sequentialize a parallel copy {dst <- src}. Cycles are broken with
  // xor swaps, so no scratch register is needed.
  void parallelCopy(std::vector<std::pair<std::string, std::string>> moves) {
    moves.erase(std::remove_if(moves.begin(), moves.end(), [](auto &m) { return m.first == m.second; }),
                moves.end());
    while (!moves.empty()) {
      bool progress = false;
      for (size_t i = 0; i < moves.size(); i++) {
        const std::string &d = moves[i].first;
        bool isSrc = false;
        for (size_t j = 0; j < moves.size(); j++)
          if (j != i && moves[j].second == d) isSrc = true;
        if (isSrc) continue;
        mov(d, moves[i].second);
        moves.erase(moves.begin() + i);
        progress = true;
        break;
      }
      if (progress) continue;
      // every remaining destination is also a source: a cycle
      std::string d = moves[0].first, s = moves[0].second;
      text("xor " + d + ", " + d + ", " + s);
      text("xor " + s + ", " + s + ", " + d);
      text("xor " + d + ", " + d + ", " + s);
      moves.erase(moves.begin());
      for (auto &m : moves)          // old d now lives in s
        if (m.second == d) m.second = s;
      moves.erase(std::remove_if(moves.begin(), moves.end(), [](auto &m) { return m.first == m.second; }),
                  moves.end());
    }
  }
};

static std::string R(const Operand &o) {
  switch (o.k) {
    case Operand::R: return "x" + std::to_string(o.r->color);
    case Operand::Phys: return regName(o.phys);
    case Operand::Imm:
      if (o.imm == 0) return "x0";
      fatal(0, "internal: constant %d where a register is needed", o.imm);
    default: fatal(0, "internal: missing operand");
  }
}
static std::string R(Reg *r) { return "x" + std::to_string(r->color); }

static std::string blockLabel(BasicBlock *b) { return b->fn->name + "__" + std::to_string(b->id); }

static void emitInstr(Emitter &E, Instr *i, Instr *next) {
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

static void emitFunction(Function *f, FILE *out) {
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
      for (Instr *i : s->ins) {
        if (i->op != Op::Phi) break;
        for (size_t k = 0; k < i->preds.size(); k++)
          if (i->preds[k] == b) moves.push_back({R(i->dst), R(i->a[k])});
      }
      B.parallelCopy(moves);
    }
    line = B.curLine;
    body[b] = B.out;
  }
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

  Emitter E;
  E.label(f->name);
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
      case Op::Ret:
      {
        std::vector<std::pair<std::string, std::string>> moves;
        for (size_t k = 0; k < t->a.size(); k++) moves.push_back({R(f->retRegs[k]), R(t->a[k])});
        E.parallelCopy(moves);
        E.text("ret");
      }
        break;
      case Op::Stop:
        break;
      default:
        fatal(t->line, "internal: bad terminator");
    }
  }

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
  for (Function *f : p->funcs) emitFunction(f, out);
}
