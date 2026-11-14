/* ast.cpp: semantic analysis and TAC generation for the millicode C subset.
 *
 * As in Decaf, each AST node emits its own TAC; unlike Decaf the TAC goes
 * straight into basic blocks (no labels to split on later). C's integer
 * promotions and the usual arithmetic conversions are applied here, so the
 * TAC only ever sees canonical 32-bit values.
 */
#include "ast.h"

#include <cstring>
#include <functional>

#include "errors.h"

std::string Type::str() const {
  if (isVoid) return "void";
  return std::string(sign ? "s" : "u") + std::to_string(bits);
}

// ---------------------------------------------------------------------------
// code generation context
// ---------------------------------------------------------------------------
struct Sym {
  Reg *var = nullptr;
  Type ty;
  bool isConst = false;
  int32_t cval = 0;
};

struct SwitchCtx {
  std::map<CaseLabel *, BasicBlock *> blocks;
};

class CG {
 public:
  Program *prog = nullptr;
  FnDecl *fd = nullptr;
  Function *fn = nullptr;
  BasicBlock *cur = nullptr;
  std::vector<std::map<std::string, Sym>> scopes;
  std::map<std::string, FnDecl *> fns;
  std::vector<BasicBlock *> breaks, conts;
  std::vector<SwitchCtx *> switches;
  int line = 0;

  Instr *emit(Instr *i) {
    i->line = line;
    if (!fn) {
      error(line, "not a constant expression");
      return i;
    }
    if (cur->terminated()) cur = fn->newBlock("dead");
    cur->ins.push_back(i);
    return i;
  }
  BasicBlock *block(const char *n) { return fn->newBlock(n); }
  void jmp(BasicBlock *t) {
    if (!fn) return;
    if (cur->terminated()) return;
    Instr *i = new Instr(Op::Jmp);
    i->t = t;
    emit(i);
  }
  void br(Cond c, Operand a, Operand b, BasicBlock *t, BasicBlock *f) {
    if (a.isImm() && b.isImm()) {
      jmp(evalCond(c, a.imm, b.imm) ? t : f);
      return;
    }
    Instr *i = new Instr(Op::Br);
    i->cond = c;
    i->a = {a, b};
    i->t = t;
    i->f = f;
    emit(i);
  }
  Reg *temp() { return fn->newReg("t"); }
  Reg *var(const std::string &n) {
    Reg *r = fn->newReg(n);
    r->isVar = true;
    return r;
  }
  Operand bin(Op op, Operand a, Operand b) {
    Instr *i = new Instr(op);
    i->dst = temp();
    i->a = {a, b};
    emit(i);
    return Operand::reg(i->dst);
  }
  void copy(Reg *dst, Operand src) {
    Instr *i = new Instr(Op::Copy);
    i->dst = dst;
    i->a = {src};
    emit(i);
  }
  Sym *lookup(const std::string &n) {
    for (int i = (int)scopes.size() - 1; i >= 0; i--) {
      auto it = scopes[i].find(n);
      if (it != scopes[i].end()) return &it->second;
    }
    return nullptr;
  }
  void declare(const std::string &n, const Sym &s, int ln) {
    if (scopes.back().count(n)) error(ln, "'%s' redeclared in the same scope", n.c_str());
    scopes.back()[n] = s;
  }
  void endWithDeadBlock() {
    if (fn) cur = fn->newBlock("dead");
  }
};

static Type promote(Type t) { return t.bits < 32 ? Type::s32() : t; }
static Type common(Type a, Type b) {
  a = promote(a);
  b = promote(b);
  return (!a.sign || !b.sign) ? Type::u32() : Type::s32();
}

static int32_t canon(uint32_t v, Type t) {
  if (t.bits == 32) return (int32_t)v;
  uint32_t m = (1u << t.bits) - 1;
  v &= m;
  if (t.sign && (v >> (t.bits - 1))) v |= ~m;
  return (int32_t)v;
}

static void needValue(const EV &v, int line) {
  if (v.ty.isVoid) error(line, "void value used in an expression");
}

// Convert a value to type `to`, keeping it canonical.
static Operand convert(CG &cg, EV v, Type to) {
  needValue(v, cg.line);
  if (to.isVoid || to.bits == 32) return v.op;
  if (v.isConst()) return Operand::cst(canon(v.op.imm, to));
  const Type &s = v.ty;
  if ((s.bits < to.bits && (!s.sign || to.sign)) || (s.bits == to.bits && s.sign == to.sign)) return v.op;
  Op op = to.sign ? (to.bits == 8 ? Op::Sext8 : Op::Sext16) : (to.bits == 8 ? Op::Zext8 : Op::Zext16);
  Instr *i = new Instr(op);
  i->dst = cg.temp();
  i->a = {v.op};
  cg.emit(i);
  return Operand::reg(i->dst);
}

static bool foldBin(Op op, uint32_t a, uint32_t b, uint32_t *r, int line) {
  int32_t sa = (int32_t)a, sb = (int32_t)b;
  switch (op) {
    case Op::Add: *r = a + b; return true;
    case Op::Sub: *r = a - b; return true;
    case Op::Mul: *r = a * b; return true;
    case Op::And: *r = a & b; return true;
    case Op::Or: *r = a | b; return true;
    case Op::Xor: *r = a ^ b; return true;
    case Op::Shl: *r = a << (b & 31); return true;
    case Op::Shr: *r = a >> (b & 31); return true;
    case Op::Sar: *r = (uint32_t)(sa >> (b & 31)); return true;
    case Op::Div: case Op::DivU: case Op::Rem: case Op::RemU:
      if (b == 0) {
        error(line, "division by zero in a constant expression");
        *r = 0;
        return true;
      }
      if (op == Op::DivU) *r = a / b;
      else if (op == Op::RemU) *r = a % b;
      else if (sa == INT32_MIN && sb == -1) *r = op == Op::Div ? a : 0;
      else *r = op == Op::Div ? (uint32_t)(sa / sb) : (uint32_t)(sa % sb);
      return true;
    default: return false;
  }
}

// Arithmetic shared by BinaryExpr and compound assignment.
static EV arith(CG &cg, int op, EV a, EV b) {
  needValue(a, cg.line);
  needValue(b, cg.line);
  Type ct = (op == 'L' || op == 'R') ? promote(a.ty) : common(a.ty, b.ty);
  Op o;
  switch (op) {
    case '+': o = Op::Add; break;
    case '-': o = Op::Sub; break;
    case '*': o = Op::Mul; break;
    case '/': o = ct.sign ? Op::Div : Op::DivU; break;
    case '%': o = ct.sign ? Op::Rem : Op::RemU; break;
    case '&': o = Op::And; break;
    case '|': o = Op::Or; break;
    case '^': o = Op::Xor; break;
    case 'L': o = Op::Shl; break;
    case 'R': o = ct.sign ? Op::Sar : Op::Shr; break;
    default: fatal(cg.line, "internal: operator %c", op);
  }
  if (a.isConst() && b.isConst()) {
    uint32_t r;
    foldBin(o, a.op.imm, b.op.imm, &r, cg.line);
    return EV{Operand::cst((int32_t)r), ct};
  }
  if ((o == Op::Div || o == Op::DivU || o == Op::Rem || o == Op::RemU) && b.isConst() && b.op.imm == 0)
    error(cg.line, "division by zero");
  return EV{cg.bin(o, a.op, b.op), ct};
}

static Cond unsignedCond(Cond c) {
  switch (c) {
    case Cond::LT: return Cond::LTU;
    case Cond::GE: return Cond::GEU;
    case Cond::GT: return Cond::GTU;
    case Cond::LE: return Cond::LEU;
    default: return c;
  }
}

// ---------------------------------------------------------------------------
// expressions
// ---------------------------------------------------------------------------
void Expr::genCond(CG &cg, BasicBlock *t, BasicBlock *f) {
  EV v = gen(cg);
  needValue(v, line);
  cg.line = line;
  cg.br(Cond::NE, v.op, Operand::cst(0), t, f);
}

EV IntLit::gen(CG &cg) {
  (void)cg;
  return EV{Operand::cst((int32_t)value), isUnsigned ? Type::u32() : Type::s32()};
}

EV VarRef::gen(CG &cg) {
  cg.line = line;
  Sym *s = cg.lookup(name);
  if (!s) {
    error(line, "undeclared identifier '%s'", name.c_str());
    return EV{Operand::cst(0), Type::s32()};
  }
  if (s->isConst) return EV{Operand::cst(s->cval), s->ty};
  return EV{Operand::reg(s->var), s->ty};
}

EV UnaryExprNode::gen(CG &cg) {
  EV v = e->gen(cg);
  cg.line = line;
  needValue(v, line);
  Type pt = promote(v.ty);
  switch (op) {
    case '+': return EV{v.op, pt};
    case '-': return arith(cg, '-', EV{Operand::cst(0), pt}, EV{v.op, pt});
    case '~': return arith(cg, '^', EV{v.op, pt}, EV{Operand::cst(-1), pt});
    case '!': {
      if (v.isConst()) return EV{Operand::cst(v.op.imm == 0), Type::s32()};
      Instr *i = new Instr(Op::Set);
      i->cond = Cond::EQ;
      i->dst = cg.temp();
      i->a = {v.op, Operand::cst(0)};
      cg.emit(i);
      return EV{Operand::reg(i->dst), Type::s32()};
    }
  }
  fatal(line, "internal: unary %c", op);
}

void UnaryExprNode::genCond(CG &cg, BasicBlock *t, BasicBlock *f) {
  if (op == '!') e->genCond(cg, f, t);
  else Expr::genCond(cg, t, f);
}

EV BinaryExpr::gen(CG &cg) {
  EV a = l->gen(cg);
  EV b = r->gen(cg);
  cg.line = line;
  return arith(cg, op, a, b);
}

EV CompareExpr::gen(CG &cg) {
  EV a = l->gen(cg);
  EV b = r->gen(cg);
  cg.line = line;
  needValue(a, line);
  needValue(b, line);
  Type ct = common(a.ty, b.ty);
  Cond cc = ct.sign ? c : unsignedCond(c);
  if (a.isConst() && b.isConst()) return EV{Operand::cst(evalCond(cc, a.op.imm, b.op.imm)), Type::s32()};
  Instr *i = new Instr(Op::Set);
  i->cond = cc;
  i->dst = cg.temp();
  i->a = {a.op, b.op};
  cg.emit(i);
  return EV{Operand::reg(i->dst), Type::s32()};
}

void CompareExpr::genCond(CG &cg, BasicBlock *t, BasicBlock *f) {
  EV a = l->gen(cg);
  EV b = r->gen(cg);
  cg.line = line;
  needValue(a, line);
  needValue(b, line);
  Type ct = common(a.ty, b.ty);
  cg.br(ct.sign ? c : unsignedCond(c), a.op, b.op, t, f);
}

void LogicalExpr::genCond(CG &cg, BasicBlock *t, BasicBlock *f) {
  BasicBlock *mid = cg.block(op == '&' ? "and" : "or");
  if (op == '&') l->genCond(cg, mid, f);
  else l->genCond(cg, t, mid);
  cg.cur = mid;
  r->genCond(cg, t, f);
}

EV LogicalExpr::gen(CG &cg) {
  cg.line = line;
  if (!cg.fn) {   // constant expression
    EV a = l->gen(cg), b = r->gen(cg);
    if (a.isConst() && b.isConst())
      return EV{Operand::cst(op == '&' ? (a.op.imm && b.op.imm) : (a.op.imm || b.op.imm)), Type::s32()};
    error(line, "not a constant expression");
    return EV{Operand::cst(0), Type::s32()};
  }
  // the value is computed on two paths through a source variable;
  // SSA construction turns it into a phi
  Reg *v = cg.var("lv");
  BasicBlock *T = cg.block("lt"), *F = cg.block("lf"), *J = cg.block("lj");
  genCond(cg, T, F);
  cg.cur = T;
  cg.copy(v, Operand::cst(1));
  cg.jmp(J);
  cg.cur = F;
  cg.copy(v, Operand::cst(0));
  cg.jmp(J);
  cg.cur = J;
  return EV{Operand::reg(v), Type::s32()};
}

EV CondExprNode::gen(CG &cg) {
  cg.line = line;
  if (!cg.fn) {
    EV cv = c->gen(cg), av = a->gen(cg), bv = b->gen(cg);
    Type ct = common(av.ty, bv.ty);
    if (cv.isConst()) return EV{cv.op.imm ? av.op : bv.op, ct};
    error(line, "not a constant expression");
    return EV{Operand::cst(0), Type::s32()};
  }
  Reg *v = cg.var("cv");
  BasicBlock *T = cg.block("ct"), *F = cg.block("cf"), *J = cg.block("cj");
  c->genCond(cg, T, F);
  cg.cur = T;
  EV av = a->gen(cg);
  BasicBlock *endT = cg.cur;
  cg.cur = F;
  EV bv = b->gen(cg);
  BasicBlock *endF = cg.cur;
  needValue(av, line);
  needValue(bv, line);
  Type ct = common(av.ty, bv.ty);
  cg.cur = endT;
  cg.copy(v, convert(cg, av, ct));
  cg.jmp(J);
  cg.cur = endF;
  cg.copy(v, convert(cg, bv, ct));
  cg.jmp(J);
  cg.cur = J;
  return EV{Operand::reg(v), ct};
}

static Sym *lvalue(CG &cg, Expr *e, int line) {
  const char *n = e->lvalueName();
  if (!n) {
    error(line, "assignment to something that is not a variable");
    return nullptr;
  }
  Sym *s = cg.lookup(n);
  if (!s) {
    error(line, "undeclared identifier '%s'", n);
    return nullptr;
  }
  if (s->isConst) {
    error(line, "assignment to constant '%s'", n);
    return nullptr;
  }
  return s;
}

EV AssignExpr::gen(CG &cg) {
  cg.line = line;
  Sym *s = lvalue(cg, lhs, line);
  EV v = rhs->gen(cg);
  cg.line = line;
  if (!s) return v;
  if (op) {
    int o = op;
    v = arith(cg, o, EV{Operand::reg(s->var), s->ty}, v);
  }
  Operand o = convert(cg, v, s->ty);
  cg.copy(s->var, o);
  return EV{o, s->ty};
}

EV IncDecExpr::gen(CG &cg) {
  cg.line = line;
  Sym *s = lvalue(cg, e, line);
  if (!s) return EV{Operand::cst(0), Type::s32()};
  Operand old = Operand::reg(s->var);
  if (!prefix) {
    Reg *t = cg.temp();
    cg.copy(t, old);
    old = Operand::reg(t);
  }
  EV nv = arith(cg, op, EV{old, s->ty}, EV{Operand::cst(1), Type::s32()});
  Operand n = convert(cg, nv, s->ty);
  cg.copy(s->var, n);
  return EV{prefix ? n : old, s->ty};
}

EV CastExpr::gen(CG &cg) {
  EV v = e->gen(cg);
  cg.line = line;
  return EV{convert(cg, v, t), t};
}

EV CallExpr::gen(CG &cg) {
  cg.line = line;
  if (name.compare(0, 2, "__") == 0) {
    if (const EntryReg *er = findEntryReg(name)) {
      if (!args.empty()) error(line, "%s takes no arguments", name.c_str());
      if (!cg.fn || !cg.fd->entry) {
        error(line, "%s is only available in an entry function", name.c_str());
        return EV{Operand::cst(0), Type::u32()};
      }
      for (Reg *r : cg.fn->entryRegs)
        if (r->precolor == er->reg) return EV{Operand::reg(r), Type::u32()};
      Reg *r = cg.fn->newReg(name.substr(2));
      r->precolor = er->reg;
      cg.fn->entryRegs.push_back(r);
      return EV{Operand::reg(r), Type::u32()};
    }
    const IntrinsicDesc *d = findIntrinsic(name);
    if (!d) {
      error(line, "unknown intrinsic '%s'", name.c_str());
      return EV{Operand::cst(0), Type::u32()};
    }
    return genIntrinsic(cg, d);
  }
  auto it = cg.fns.find(name);
  if (it == cg.fns.end()) {
    error(line, "call to undeclared function '%s'", name.c_str());
    return EV{Operand::cst(0), Type::s32()};
  }
  FnDecl *callee = it->second;
  if (callee->entry) error(line, "entry function '%s' cannot be called", name.c_str());
  if (callee->params.size() != args.size())
    error(line, "'%s' takes %zu arguments, %zu given", name.c_str(), callee->params.size(), args.size());
  std::vector<Operand> ops;
  for (size_t i = 0; i < args.size(); i++) {
    EV v = args[i]->gen(cg);
    cg.line = line;
    ops.push_back(i < callee->params.size() ? convert(cg, v, callee->params[i].t) : v.op);
  }
  Instr *c = new Instr(Op::Call);
  c->callee = callee->fn;
  c->a = ops;
  if (!callee->ret.isVoid) c->dst = cg.temp();
  cg.emit(c);
  if (cg.fn) cg.fn->callees.insert(callee->fn);
  if (callee->ret.isVoid) return EV{Operand(), Type::voidType()};
  return EV{Operand::reg(c->dst), callee->ret};
}

EV CallExpr::genIntrinsic(CG &cg, const IntrinsicDesc *d) {
  size_t n = strlen(d->args);
  if (args.size() != n) {
    error(line, "%s takes %zu arguments, %zu given", d->name, n, args.size());
    return EV{Operand::cst(0), Type::u32()};
  }
  std::vector<EV> vals;
  for (Expr *e : args) {
    vals.push_back(e->gen(cg));
    needValue(vals.back(), line);
  }
  cg.line = line;
  // constant guest register index -> name the register directly
  if (!strcmp(d->name, "__rdg") && vals[0].isConst()) {
    int idx = vals[0].op.imm;
    if (idx < 0 || idx > 17) error(line, "__rdg: guest register index %d out of range", idx);
    Instr *i = new Instr(Op::Copy);
    i->dst = cg.temp();
    i->a = {Operand::preg(32 + (idx & 31))};
    cg.emit(i);
    return EV{Operand::reg(i->dst), Type::u32()};
  }
  if (!strcmp(d->name, "__wrg") && vals[0].isConst()) {
    int idx = vals[0].op.imm;
    if (idx < 0 || idx > 17) error(line, "__wrg: guest register index %d out of range", idx);
    Instr *i = new Instr(Op::PWrite);
    i->phys = 32 + (idx & 31);
    i->a = {vals[1].op};
    cg.emit(i);
    return EV{Operand(), Type::voidType()};
  }
  Instr *i = new Instr(Op::Intrin);
  i->in = d;
  for (size_t k = 0; k < n; k++) {
    if (d->args[k] == 'i') {
      if (!vals[k].isConst()) {
        error(line, "%s: argument %zu must be a constant", d->name, k + 1);
        i->a.push_back(Operand::cst(0));
        continue;
      }
      int32_t v = vals[k].op.imm;
      if (v < d->range[k].lo || v > d->range[k].hi || v % d->range[k].scale)
        error(line, "%s: argument %zu (%d) out of range [%d, %d]", d->name, k + 1, v, d->range[k].lo, d->range[k].hi);
      i->a.push_back(Operand::cst(v));
    } else {
      i->a.push_back(vals[k].op);
    }
  }
  if (d->has_result) i->dst = cg.temp();
  cg.emit(i);
  if (d->noreturn) {
    Instr *s = new Instr(Op::Stop);
    cg.emit(s);
    cg.endWithDeadBlock();
  }
  if (!d->has_result) return EV{Operand(), Type::voidType()};
  return EV{Operand::reg(i->dst), Type::u32()};
}

// ---------------------------------------------------------------------------
// statements
// ---------------------------------------------------------------------------
void Block::gen(CG &cg) {
  cg.scopes.emplace_back();
  for (Stmt *s : stmts) s->gen(cg);
  cg.scopes.pop_back();
}

void DeclStmt::gen(CG &cg) {
  for (auto &it : items) {
    cg.line = it.line;
    Sym s;
    s.ty = t;
    if (isConst) {
      if (!it.init) error(it.line, "constant '%s' needs a value", it.name);
      EV v = it.init ? it.init->gen(cg) : EV{Operand::cst(0), t};
      if (!v.isConst()) error(it.line, "initializer of constant '%s' is not a constant expression", it.name);
      s.isConst = true;
      s.cval = v.isConst() ? canon(v.op.imm, t) : 0;
      cg.declare(it.name, s, it.line);
      continue;
    }
    s.var = cg.var(it.name);
    if (it.init) {
      EV v = it.init->gen(cg);
      cg.line = it.line;
      cg.copy(s.var, convert(cg, v, t));
    }
    cg.declare(it.name, s, it.line);   // C scope starts after the declarator
  }
}

void ExprStmt::gen(CG &cg) { e->gen(cg); }

void IfStmt::gen(CG &cg) {
  cg.line = line;
  BasicBlock *T = cg.block("then"), *J = cg.block("endif");
  BasicBlock *E = e ? cg.block("else") : J;
  c->genCond(cg, T, E);
  cg.cur = T;
  t->gen(cg);
  cg.jmp(J);
  if (e) {
    cg.cur = E;
    e->gen(cg);
    cg.jmp(J);
  }
  cg.cur = J;
}

void WhileStmt::gen(CG &cg) {
  cg.line = line;
  BasicBlock *H = cg.block("while"), *B = cg.block("body"), *X = cg.block("wend");
  cg.jmp(H);
  cg.cur = H;
  c->genCond(cg, B, X);
  cg.cur = B;
  cg.breaks.push_back(X);
  cg.conts.push_back(H);
  body->gen(cg);
  cg.breaks.pop_back();
  cg.conts.pop_back();
  cg.jmp(H);
  cg.cur = X;
}

void DoWhileStmt::gen(CG &cg) {
  cg.line = line;
  BasicBlock *B = cg.block("do"), *C = cg.block("dcond"), *X = cg.block("dend");
  cg.jmp(B);
  cg.cur = B;
  cg.breaks.push_back(X);
  cg.conts.push_back(C);
  body->gen(cg);
  cg.breaks.pop_back();
  cg.conts.pop_back();
  cg.jmp(C);
  cg.cur = C;
  c->genCond(cg, B, X);
  cg.cur = X;
}

void ForStmt::gen(CG &cg) {
  cg.line = line;
  cg.scopes.emplace_back();
  if (init) init->gen(cg);
  BasicBlock *H = cg.block("for"), *B = cg.block("fbody"), *S = cg.block("fstep"), *X = cg.block("fend");
  cg.jmp(H);
  cg.cur = H;
  if (c) c->genCond(cg, B, X);
  else cg.jmp(B);
  cg.cur = B;
  cg.breaks.push_back(X);
  cg.conts.push_back(S);
  body->gen(cg);
  cg.breaks.pop_back();
  cg.conts.pop_back();
  cg.jmp(S);
  cg.cur = S;
  if (step) step->gen(cg);
  cg.jmp(H);
  cg.cur = X;
  cg.scopes.pop_back();
}

void SwitchStmt::gen(CG &cg) {
  cg.line = line;
  EV v = e->gen(cg);
  needValue(v, line);
  Type st = promote(v.ty);
  SwitchCtx ctx;
  BasicBlock *X = cg.block("swend"), *def = nullptr;
  std::map<int32_t, BasicBlock *> cases;
  std::vector<std::pair<int32_t, BasicBlock *>> order;
  for (Stmt *s : body->stmts) {
    CaseLabel *cl = dynamic_cast<CaseLabel *>(s);
    if (!cl) continue;
    BasicBlock *b = cg.block(cl->value ? "case" : "default");
    ctx.blocks[cl] = b;
    if (!cl->value) {
      if (def) error(cl->line, "duplicate default label");
      def = b;
      continue;
    }
    EV k = cl->value->gen(cg);
    if (!k.isConst()) {
      error(cl->line, "case label is not a constant expression");
      continue;
    }
    int32_t kv = canon(k.op.imm, st);
    if (cases.count(kv)) error(cl->line, "duplicate case value %d", kv);
    cases[kv] = b;
    order.push_back({kv, b});
  }
  cg.line = line;
  for (auto &p : order) {
    BasicBlock *next = cg.block("swnext");
    cg.br(Cond::EQ, v.op, Operand::cst(p.first), p.second, next);
    cg.cur = next;
  }
  cg.jmp(def ? def : X);
  cg.endWithDeadBlock();
  cg.breaks.push_back(X);
  cg.switches.push_back(&ctx);
  body->gen(cg);
  cg.switches.pop_back();
  cg.breaks.pop_back();
  cg.jmp(X);
  cg.cur = X;
}

void CaseLabel::gen(CG &cg) {
  cg.line = line;
  if (cg.switches.empty() || !cg.switches.back()->blocks.count(this)) {
    error(line, "case label not directly inside a switch body");
    return;
  }
  BasicBlock *b = cg.switches.back()->blocks[this];
  cg.jmp(b);
  cg.cur = b;
}

void ReturnStmt::gen(CG &cg) {
  cg.line = line;
  FnDecl *fd = cg.fd;
  if (fd->entry) {
    error(line, "entry function '%s' cannot return; end it with __gexit() or __gfault()", fd->name.c_str());
    return;
  }
  Instr *r = new Instr(Op::Ret);
  if (e) {
    EV v = e->gen(cg);
    cg.line = line;
    if (fd->ret.isVoid) error(line, "void function '%s' returns a value", fd->name.c_str());
    else r->a = {convert(cg, v, fd->ret)};
  } else if (!fd->ret.isVoid) {
    error(line, "function '%s' must return a value", fd->name.c_str());
  }
  cg.emit(r);
  cg.endWithDeadBlock();
}

void BreakStmt::gen(CG &cg) {
  cg.line = line;
  if (cg.breaks.empty()) error(line, "break outside a loop or switch");
  else cg.jmp(cg.breaks.back());
  cg.endWithDeadBlock();
}

void ContinueStmt::gen(CG &cg) {
  cg.line = line;
  if (cg.conts.empty()) error(line, "continue outside a loop");
  else cg.jmp(cg.conts.back());
  cg.endWithDeadBlock();
}

// ---------------------------------------------------------------------------
// translation unit
// ---------------------------------------------------------------------------
Program *generateTAC(TransUnit *tu) {
  CG cg;
  cg.prog = new Program();
  cg.scopes.emplace_back();
  for (const auto &c : specConsts()) {
    Sym s;
    s.ty = Type::s32();
    s.isConst = true;
    s.cval = c.value;
    cg.scopes.back()[c.name] = s;
  }
  for (ConstDecl *c : tu->consts) {
    cg.line = c->line;
    EV v = c->item.init ? c->item.init->gen(cg) : EV{Operand::cst(0), c->t};
    if (!v.isConst()) error(c->item.line, "initializer of constant '%s' is not a constant expression", c->item.name);
    Sym s;
    s.ty = c->t;
    s.isConst = true;
    s.cval = v.isConst() ? canon(v.op.imm, c->t) : 0;
    cg.declare(c->item.name, s, c->item.line);
  }
  for (FnDecl *fd : tu->fns) {
    if (fd->name.compare(0, 2, "__") == 0) error(fd->line, "names starting with __ are reserved for intrinsics");
    if (cg.fns.count(fd->name)) error(fd->line, "function '%s' defined twice", fd->name.c_str());
    if (fd->entry && (!fd->ret.isVoid || !fd->params.empty()))
      error(fd->line, "entry function '%s' must be declared `entry void %s(void)`", fd->name.c_str(), fd->name.c_str());
    fd->fn = new Function(fd->name);
    fd->fn->isEntry = fd->entry;
    fd->fn->returnsValue = !fd->ret.isVoid;
    fd->fn->line = fd->line;
    cg.fns[fd->name] = fd;
    cg.prog->funcs.push_back(fd->fn);
  }
  for (FnDecl *fd : tu->fns) {
    cg.fd = fd;
    cg.fn = fd->fn;
    cg.cur = cg.fn->newBlock("entry");
    cg.scopes.emplace_back();
    for (auto &p : fd->params) {
      Sym s;
      s.ty = p.t;
      s.var = cg.var(p.name);
      cg.fn->paramVars.push_back(s.var);
      cg.declare(p.name, s, fd->line);
    }
    fd->body->gen(cg);
    cg.line = fd->line;
    if (!cg.cur->terminated()) {
      // falling off the end: fine for void helpers, an error otherwise
      // (reported later, and only if the end is reachable)
      Instr *i = new Instr(fd->ret.isVoid && !fd->entry ? Op::Ret : Op::Stop);
      i->falloff = !(fd->ret.isVoid && !fd->entry);
      cg.emit(i);
    }
    cg.scopes.pop_back();
  }
  return cg.prog;
}
