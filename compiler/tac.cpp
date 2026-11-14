/* tac.cpp: TAC printing, edge maintenance and the spec-generated tables. */
#include "tac.h"

#include <cstdio>
#include <cstring>
#include <iterator>

#include "uisa_intrinsics.inc"

const IntrinsicDesc *findIntrinsic(const std::string &name) {
  for (const auto &d : INTRINSICS)
    if (name == d.name) return &d;
  return nullptr;
}

std::vector<NamedConst> specConsts() {
  return std::vector<NamedConst>(std::begin(SPEC_CONSTS), std::end(SPEC_CONSTS));
}

const EntryReg *findEntryReg(const std::string &name) {
  for (const auto &e : ENTRY_REGS)
    if (name == e.name) return &e;
  return nullptr;
}

const char *regName(int idx) { return (idx >= 0 && idx < 64) ? REG_NAMES[idx] : "?"; }

Cond invert(Cond c) {
  switch (c) {
    case Cond::EQ: return Cond::NE;
    case Cond::NE: return Cond::EQ;
    case Cond::LT: return Cond::GE;
    case Cond::GE: return Cond::LT;
    case Cond::LTU: return Cond::GEU;
    case Cond::GEU: return Cond::LTU;
    case Cond::GT: return Cond::LE;
    case Cond::LE: return Cond::GT;
    case Cond::GTU: return Cond::LEU;
    case Cond::LEU: return Cond::GTU;
  }
  return c;
}

Cond swapCond(Cond c) {   // a c b  <=>  b swap(c) a
  switch (c) {
    case Cond::EQ: return Cond::EQ;
    case Cond::NE: return Cond::NE;
    case Cond::LT: return Cond::GT;
    case Cond::GE: return Cond::LE;
    case Cond::LTU: return Cond::GTU;
    case Cond::GEU: return Cond::LEU;
    case Cond::GT: return Cond::LT;
    case Cond::LE: return Cond::GE;
    case Cond::GTU: return Cond::LTU;
    case Cond::LEU: return Cond::GEU;
  }
  return c;
}

const char *condName(Cond c) {
  static const char *n[] = {"eq", "ne", "lt", "ge", "ltu", "geu", "gt", "le", "gtu", "leu"};
  return n[(int)c];
}

bool evalCond(Cond c, uint32_t a, uint32_t b) {
  int32_t sa = (int32_t)a, sb = (int32_t)b;
  switch (c) {
    case Cond::EQ: return a == b;
    case Cond::NE: return a != b;
    case Cond::LT: return sa < sb;
    case Cond::GE: return sa >= sb;
    case Cond::LTU: return a < b;
    case Cond::GEU: return a >= b;
    case Cond::GT: return sa > sb;
    case Cond::LE: return sa <= sb;
    case Cond::GTU: return a > b;
    case Cond::LEU: return a <= b;
  }
  return false;
}

std::string Operand::str() const {
  switch (k) {
    case R: return r->str();
    case Imm: return std::to_string(imm);
    case Phys: return regName(phys);
    default: return "_";
  }
}

bool Instr::hasSideEffects() const {
  switch (op) {
    case Op::Intrin: return in->side_effects;
    case Op::Call: case Op::PWrite: return true;
    default: return isTerm();
  }
}

static const char *opName(Op op) {
  static const char *n[] = {"copy", "add", "sub", "mul", "div", "divu", "rem", "remu", "and", "or", "xor",
                            "shl", "shr", "sar", "set", "sext8", "sext16", "zext8", "zext16", "intrin",
                            "call", "phi", "pwrite", "br", "jmp", "ret", "stop"};
  return n[(int)op];
}

std::string Instr::str() const {
  std::string s;
  if (dst) s += dst->str() + " = ";
  s += opName(op);
  if (op == Op::Set || op == Op::Br) s += std::string(".") + condName(cond);
  if (op == Op::Intrin) s += std::string(" ") + in->name;
  if (op == Op::Call) s += " " + std::string(callee ? "fn" : "?");
  if (op == Op::PWrite) s += std::string(" ") + regName(phys);
  for (size_t i = 0; i < a.size(); i++) {
    s += (i ? ", " : " ") + a[i].str();
    if (op == Op::Phi) s += "[" + preds[i]->name + "]";
  }
  if (t) s += " -> " + t->name;
  if (f) s += " : " + f->name;
  return s;
}

void Function::print(FILE *out) const {
  fprintf(out, "function %s\n", name.c_str());
  for (BasicBlock *b : blocks) {
    fprintf(out, "%s:  ; preds", b->name.c_str());
    for (BasicBlock *p : b->preds) fprintf(out, " %s", p->name.c_str());
    fprintf(out, "\n");
    for (Instr *i : b->ins) fprintf(out, "    %s\n", i->str().c_str());
  }
}

// Recompute preds/succs from the terminators (successor order: t, f).
void Function::rebuildEdges() {
  for (BasicBlock *b : blocks) {
    b->preds.clear();
    b->succs.clear();
  }
  for (BasicBlock *b : blocks) {
    Instr *t = b->term();
    if (!t) continue;
    if (t->t) b->succs.push_back(t->t);
    if (t->f && t->f != t->t) b->succs.push_back(t->f);
    for (BasicBlock *s : b->succs) s->preds.push_back(b);
  }
}
