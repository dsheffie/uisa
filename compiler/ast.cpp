/* ast.cpp: semantic analysis and TAC generation for the millicode C subset.
 *
 * As in Decaf, each AST node emits its own TAC; unlike Decaf the TAC goes
 * straight into basic blocks (no labels to split on later). C's integer
 * promotions and the usual arithmetic conversions are applied here, so the
 * TAC only ever sees canonical 32-bit values.
 *
 * u64/s64 values are lowered here to pairs of 32-bit words (EV::op low,
 * EV::hi high; a variable is two source variables), so SSA construction,
 * allocation and emission need nothing new: add/sub propagate the carry with
 * sltu, mul uses mulhu, shifts by a variable count are branch-free, division
 * is an inline shift-subtract loop, and comparisons test the high words
 * first. A 64-bit argument takes two parameter registers and a 64-bit result
 * comes back in two (Call + CallRes).
 */
#include "ast.h"

#include <cstring>
#include <functional>
#include <set>

#include "errors.h"

int Type::words() const {
  if (isVoid) return 0;
  if (sd) {
    int n = 0;
    for (auto &f : sd->fields) n += f.second.words();
    return n;
  }
  return bits == 64 ? 2 : 1;
}

std::string Type::str() const {
  if (isVoid) return "void";
  if (sd) return "struct " + sd->name;
  return std::string(sign ? "s" : "u") + std::to_string(bits);
}

// ---------------------------------------------------------------------------
// code generation context
// ---------------------------------------------------------------------------
struct Sym {
  Reg *var = nullptr;
  Reg *varHi = nullptr;     // 64-bit variables: the high word
  Type ty;
  bool isConst = false;
  uint64_t cval = 0;        // constants: the value, extended per its type
  std::vector<Sym> fields;  // struct variables: one per field
  Sym *alias = nullptr;     // reference parameters: the caller's variable
};

// One inline expansion in progress.
struct InlineFrame {
  FnDecl *fd;
  Sym ret;                  // where `return` puts the value
  BasicBlock *join;         // where `return` goes
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
  std::vector<InlineFrame *> inl;   // inline expansions, innermost last
  FnDecl *rootFd = nullptr;         // the function being compiled (inline bodies included)
  size_t floor = 1;                 // innermost scope a body may see (below it: only globals)
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
  // An inlined body sees its own scopes and the globals, not its caller's.
  Sym *lookup(const std::string &n) {
    for (int i = (int)scopes.size() - 1; i >= 0; i--) {
      if (i > 0 && (size_t)i < floor) i = 0;
      auto it = scopes[i].find(n);
      if (it != scopes[i].end()) {
        Sym *s = &it->second;
        while (s->alias) s = s->alias;
        return s;
      }
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
// C's usual arithmetic conversions; s64 holds every u32, so s64 op u32 is s64
static Type common(Type a, Type b) {
  a = promote(a);
  b = promote(b);
  if (a.is64() || b.is64()) return Type(64, !((a.is64() && !a.sign) || (b.is64() && !b.sign)));
  return (!a.sign || !b.sign) ? Type::u32() : Type::s32();
}

static int32_t canon(uint32_t v, Type t) {
  if (t.bits >= 32) return (int32_t)v;
  uint32_t m = (1u << t.bits) - 1;
  v &= m;
  if (t.sign && (v >> (t.bits - 1))) v |= ~m;
  return (int32_t)v;
}

static void needValue(const EV &v, int line) {
  if (v.ty.isVoid) error(line, "void value used in an expression");
  else if (v.ty.isStruct()) error(line, "%s value used as a number", v.ty.str().c_str());
}

// The value of a constant, extended to 64 bits according to its type.
static uint64_t cvalOf(const EV &v) {
  if (v.ty.is64()) return ((uint64_t)(uint32_t)v.hi.imm << 32) | (uint32_t)v.op.imm;
  return v.ty.sign ? (uint64_t)(int64_t)v.op.imm : (uint64_t)(uint32_t)v.op.imm;
}
// A constant of type t (truncated and kept canonical).
static EV constOf(uint64_t v, Type t) {
  if (t.is64()) return EV{Operand::cst((int32_t)(uint32_t)v), t, Operand::cst((int32_t)(uint32_t)(v >> 32))};
  return EV{Operand::cst(canon((uint32_t)v, t)), t};
}

// Convert a value to type `to`, keeping it canonical.
static EV constOf(uint64_t v, Type t);
static EV zeroOf(Type t);
static EV convert(CG &cg, EV v, Type to) {
  if (to.isStruct() || v.ty.isStruct()) {   // structs convert only to themselves
    if (!(to == v.ty)) {
      error(cg.line, "cannot convert %s to %s", v.ty.str().c_str(), to.str().c_str());
      return zeroOf(to);
    }
    return v;
  }
  needValue(v, cg.line);
  if (to.isVoid) return v;
  if (v.ty.isVoid) return constOf(0, to);
  if (v.isConst()) return constOf(cvalOf(v), to);
  if (to.is64()) {
    if (v.ty.is64()) return EV{v.op, to, v.hi};
    // narrower values are canonical 32-bit words: the high word is the sign
    Operand hi = v.ty.sign ? cg.bin(Op::Sar, v.op, Operand::cst(31)) : Operand::cst(0);
    return EV{v.op, to, hi};
  }
  Type s = v.ty.is64() ? Type::u32() : v.ty;   // 64 -> narrower: the low word, then narrow it
  if (to.bits == 32) return EV{v.op, to};
  if ((s.bits < to.bits && (!s.sign || to.sign)) || (s.bits == to.bits && s.sign == to.sign)) return EV{v.op, to};
  Op op = to.sign ? (to.bits == 8 ? Op::Sext8 : Op::Sext16) : (to.bits == 8 ? Op::Zext8 : Op::Zext16);
  Instr *i = new Instr(op);
  i->dst = cg.temp();
  i->a = {v.op};
  cg.emit(i);
  return EV{Operand::reg(i->dst), to};
}

// A 32-bit word that is nonzero exactly when the value is.
static Operand truth(CG &cg, const EV &v) {
  if (!v.ty.is64()) return v.op;
  if (v.isConst()) return Operand::cst(cvalOf(v) != 0);
  if (v.hi.isImm() && v.hi.imm == 0) return v.op;
  return cg.bin(Op::Or, v.op, v.hi);
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

// ---------------------------------------------------------------------------
// 64-bit arithmetic on word pairs
// ---------------------------------------------------------------------------
// One 32-bit operation, folded when both operands are constants.
static Operand W(CG &cg, Op op, Operand a, Operand b) {
  if (a.isImm() && b.isImm()) {
    uint32_t r;
    if (foldBin(op, a.imm, b.imm, &r, cg.line)) return Operand::cst((int32_t)r);
  }
  bool comm = op == Op::Add || op == Op::Mul || op == Op::And || op == Op::Or || op == Op::Xor;
  if (comm && a.isImm()) std::swap(a, b);
  if (b.isImm()) {
    int32_t k = b.imm;
    if (k == 0 && (op == Op::Add || op == Op::Sub || op == Op::Or || op == Op::Xor || op == Op::Shl ||
                   op == Op::Shr || op == Op::Sar))
      return a;
    if (k == 0 && (op == Op::Mul || op == Op::And)) return Operand::cst(0);
    if (k == -1 && op == Op::And) return a;
  }
  return cg.bin(op, a, b);
}
// dst = (a cond b) as 0/1, folded when both are constants.
static Operand S(CG &cg, Cond c, Operand a, Operand b) {
  if (a.isImm() && b.isImm()) return Operand::cst(evalCond(c, a.imm, b.imm));
  Instr *i = new Instr(Op::Set);
  i->cond = c;
  i->dst = cg.temp();
  i->a = {a, b};
  cg.emit(i);
  return Operand::reg(i->dst);
}
static Operand mulhu(CG &cg, Operand a, Operand b) {
  if (a.isImm() && b.isImm()) return Operand::cst((int32_t)(((uint64_t)(uint32_t)a.imm * (uint32_t)b.imm) >> 32));
  if ((a.isImm() && (a.imm == 0 || a.imm == 1)) || (b.isImm() && (b.imm == 0 || b.imm == 1))) return Operand::cst(0);
  static const IntrinsicDesc *d = findIntrinsic("__mulhu");
  if (!d) fatal(cg.line, "internal: the ISA has no mulhu");
  Instr *i = new Instr(Op::Intrin);
  i->in = d;
  i->dst = cg.temp();
  i->a = {a, b};
  cg.emit(i);
  return Operand::reg(i->dst);
}

static EV pair(Operand lo, Operand hi, Type t) { return EV{lo, t, hi}; }

static EV add64(CG &cg, EV a, EV b) {
  Operand lo = W(cg, Op::Add, a.op, b.op);
  Operand c = (b.op.isImm() && b.op.imm == 0) ? Operand::cst(0) : S(cg, Cond::LTU, lo, b.op);
  return pair(lo, W(cg, Op::Add, W(cg, Op::Add, a.hi, b.hi), c), a.ty);
}
static EV sub64(CG &cg, EV a, EV b) {
  Operand bw = (b.op.isImm() && b.op.imm == 0) ? Operand::cst(0) : S(cg, Cond::LTU, a.op, b.op);
  Operand lo = W(cg, Op::Sub, a.op, b.op);
  return pair(lo, W(cg, Op::Sub, W(cg, Op::Sub, a.hi, b.hi), bw), a.ty);
}
static EV xor64(CG &cg, EV a, Operand m) { return pair(W(cg, Op::Xor, a.op, m), W(cg, Op::Xor, a.hi, m), a.ty); }

// Shift a word pair by n (mod 64). Op is Shl, Shr or Sar.
static EV shift64(CG &cg, Op op, EV a, Operand n) {
  Operand lo = a.op, hi = a.hi;
  if (n.isImm()) {
    int k = n.imm & 63;
    if (k == 0) return a;
    if (op == Op::Shl) {
      if (k >= 32) return pair(Operand::cst(0), W(cg, Op::Shl, lo, Operand::cst(k - 32)), a.ty);
      return pair(W(cg, Op::Shl, lo, Operand::cst(k)),
                  W(cg, Op::Or, W(cg, Op::Shl, hi, Operand::cst(k)), W(cg, Op::Shr, lo, Operand::cst(32 - k))), a.ty);
    }
    Operand fill = op == Op::Sar ? W(cg, Op::Sar, hi, Operand::cst(31)) : Operand::cst(0);
    if (k >= 32) return pair(W(cg, op, hi, Operand::cst(k - 32)), fill, a.ty);
    return pair(W(cg, Op::Or, W(cg, Op::Shr, lo, Operand::cst(k)), W(cg, Op::Shl, hi, Operand::cst(32 - k))),
                W(cg, op, hi, Operand::cst(k)), a.ty);
  }
  // Branch-free. The word shifts use n mod 32 (uisa shifts mask the count);
  // (x >> 1) >> (n ^ 31) is x >> (32 - n) without the n = 0 problem; then
  // bit 5 of n selects the "moved a whole word" result.
  Operand inv = W(cg, Op::Xor, n, Operand::cst(31));
  Operand bit5 = W(cg, Op::And, W(cg, Op::Shr, n, Operand::cst(5)), Operand::cst(1));
  Operand mask = W(cg, Op::Sub, Operand::cst(0), bit5);   // all ones when n >= 32
  Operand keep = W(cg, Op::Add, bit5, Operand::cst(-1));  // all ones when n < 32
  auto sel = [&](Operand x, Operand y) {                  // n < 32 ? x : y
    return W(cg, Op::Xor, x, W(cg, Op::And, W(cg, Op::Xor, x, y), mask));
  };
  if (op == Op::Shl) {
    Operand l = W(cg, Op::Shl, lo, n);
    Operand h = W(cg, Op::Or, W(cg, Op::Shl, hi, n), W(cg, Op::Shr, W(cg, Op::Shr, lo, Operand::cst(1)), inv));
    return pair(W(cg, Op::And, l, keep), sel(h, l), a.ty);
  }
  Operand l = W(cg, Op::Or, W(cg, Op::Shr, lo, n), W(cg, Op::Shl, W(cg, Op::Shl, hi, Operand::cst(1)), inv));
  Operand h = W(cg, op, hi, n);
  if (op == Op::Shr) return pair(sel(l, h), W(cg, Op::And, h, keep), a.ty);
  return pair(sel(l, h), sel(h, W(cg, Op::Sar, hi, Operand::cst(31))), a.ty);
}

static void br64(CG &cg, Cond c, EV a, EV b, BasicBlock *t, BasicBlock *f);
static Cond unsignedCond(Cond c);

static Operand intrin(CG &cg, const char *name, std::vector<Operand> args) {
  const IntrinsicDesc *d = findIntrinsic(name);
  if (!d) fatal(cg.line, "internal: the ISA has no %s", name);
  Instr *i = new Instr(Op::Intrin);
  i->in = d;
  i->dst = cg.temp();
  i->a = std::move(args);
  cg.emit(i);
  return Operand::reg(i->dst);
}

// (u1:u0) / v for u1 < v: a 32-bit quotient and remainder (Hacker's Delight
// divlu, Knuth's algorithm D with 16-bit digits). v is normalized so its top
// bit is set; each quotient digit is estimated with one divu by the top half
// of v and corrected at most twice.
static void divlu(CG &cg, Operand u1, Operand u0, Operand v, Operand *q, Operand *r) {
  const Operand B = Operand::cst(0x10000), M = Operand::cst(0xffff), K16 = Operand::cst(16);
  Operand s = intrin(cg, "__clz", {v});
  Operand vn = W(cg, Op::Shl, v, s);
  Operand vn1 = W(cg, Op::Shr, vn, K16), vn0 = W(cg, Op::And, vn, M);
  // u >> (32 - s) without the s = 0 problem, as in shift64
  Operand un32 = W(cg, Op::Or, W(cg, Op::Shl, u1, s),
                   W(cg, Op::Shr, W(cg, Op::Shr, u0, Operand::cst(1)), W(cg, Op::Xor, s, Operand::cst(31))));
  Operand un10 = W(cg, Op::Shl, u0, s);
  Operand un1 = W(cg, Op::Shr, un10, K16), un0 = W(cg, Op::And, un10, M);
  // one quotient digit of (top / vn), with un the next dividend digit
  auto digit = [&](Operand top, Operand un, const char *nm) {
    Reg *qd = cg.var(nm), *rhat = cg.var("rhat");
    cg.copy(qd, W(cg, Op::DivU, top, vn1));
    cg.copy(rhat, W(cg, Op::Sub, top, W(cg, Op::Mul, Operand::reg(qd), vn1)));
    BasicBlock *L = cg.block("qtest"), *C = cg.block("qtest2"), *F = cg.block("qfix"), *D = cg.block("qok");
    cg.jmp(L);
    cg.cur = L;
    cg.br(Cond::GEU, Operand::reg(qd), B, F, C);
    cg.cur = C;
    cg.br(Cond::GTU, W(cg, Op::Mul, Operand::reg(qd), vn0),
          W(cg, Op::Add, W(cg, Op::Shl, Operand::reg(rhat), K16), un), F, D);
    cg.cur = F;
    cg.copy(qd, W(cg, Op::Add, Operand::reg(qd), Operand::cst(-1)));
    cg.copy(rhat, W(cg, Op::Add, Operand::reg(rhat), vn1));
    cg.br(Cond::LTU, Operand::reg(rhat), B, L, D);
    cg.cur = D;
    // the partial remainder: top * b + un - digit * vn
    return std::make_pair(Operand::reg(qd),
                          W(cg, Op::Sub, W(cg, Op::Add, W(cg, Op::Shl, top, K16), un), W(cg, Op::Mul, Operand::reg(qd), vn)));
  };
  auto d1 = digit(un32, un1, "q1");
  auto d0 = digit(d1.second, un0, "q0");
  *q = W(cg, Op::Or, W(cg, Op::Shl, d1.first, K16), d0.first);
  *r = W(cg, Op::Shr, d0.second, s);
}

// Unsigned 64-bit division: quotient and remainder, without a bit loop
// (Hacker's Delight divdu). A divisor that fits 32 bits takes one divu for
// the high word and divlu for the rest; a wider divisor is normalized with
// clz, divlu on the shifted dividend estimates the (32-bit) quotient, and one
// multiply-subtract corrects it. Both paths share one copy of divlu.
static void udivmod64(CG &cg, EV n, EV d, EV *q, EV *r) {
  Type u = Type(64, false);
  bool dSmall = d.hi.isImm() && d.hi.imm == 0;    // known at compile time
  bool dWide = d.hi.isImm() && d.hi.imm != 0;
  if (dSmall && n.hi.isImm() && n.hi.imm == 0) {
    *q = pair(W(cg, Op::DivU, n.op, d.op), Operand::cst(0), u);
    *r = pair(W(cg, Op::RemU, n.op, d.op), Operand::cst(0), u);
    return;
  }
  Reg *ql = cg.var("q"), *qh = cg.var("q.hi"), *rl = cg.var("r"), *rh = cg.var("r.hi");
  // divlu's inputs and what each path needs after it
  Reg *x1 = cg.var("x1"), *x0 = cg.var("x0"), *xv = cg.var("xv"), *wide = cg.var("wide");
  Reg *qtop = cg.var("qtop"), *sh = cg.var("sh");
  BasicBlock *J = cg.block("divend"), *D = cg.block("divlu");
  BasicBlock *narrow = dWide ? nullptr : cg.block("div64by32");
  BasicBlock *big = dSmall ? nullptr : cg.block("div64by64");
  if (!dSmall && !dWide) cg.br(Cond::NE, d.hi, Operand::cst(0), big, narrow);
  else cg.jmp(dSmall ? narrow : big);
  if (narrow) {
    cg.cur = narrow;
    BasicBlock *fast = cg.block("div32"), *two = cg.block("div2step");
    cg.br(Cond::EQ, n.hi, Operand::cst(0), fast, two);
    cg.cur = fast;
    cg.copy(ql, W(cg, Op::DivU, n.op, d.op));
    cg.copy(qh, Operand::cst(0));
    cg.copy(rl, W(cg, Op::RemU, n.op, d.op));
    cg.copy(rh, Operand::cst(0));
    cg.jmp(J);
    cg.cur = two;
    cg.copy(qtop, W(cg, Op::DivU, n.hi, d.op));
    cg.copy(x1, W(cg, Op::RemU, n.hi, d.op));
    cg.copy(x0, n.op);
    cg.copy(xv, d.op);
    cg.copy(wide, Operand::cst(0));
    cg.copy(sh, Operand::cst(0));
    cg.jmp(D);
  }
  if (big) {
    cg.cur = big;
    Operand s = intrin(cg, "__clz", {d.hi});
    EV dn = shift64(cg, Op::Shl, d, s);            // top bit of the high word set
    EV n1 = shift64(cg, Op::Shr, n, Operand::cst(1));
    cg.copy(sh, s);
    cg.copy(x1, n1.hi);
    cg.copy(x0, n1.op);
    cg.copy(xv, dn.hi);
    cg.copy(wide, Operand::cst(1));
    cg.copy(qtop, Operand::cst(0));
    cg.jmp(D);
  }
  cg.cur = D;
  Operand dq, dr;
  divlu(cg, Operand::reg(x1), Operand::reg(x0), Operand::reg(xv), &dq, &dr);
  BasicBlock *post32 = narrow ? cg.block("div2done") : nullptr, *post64 = big ? cg.block("div64done") : nullptr;
  if (narrow && big) cg.br(Cond::EQ, Operand::reg(wide), Operand::cst(0), post32, post64);
  else cg.jmp(narrow ? post32 : post64);
  if (post32) {
    cg.cur = post32;
    cg.copy(ql, dq);
    cg.copy(qh, Operand::reg(qtop));
    cg.copy(rl, dr);
    cg.copy(rh, Operand::cst(0));
    cg.jmp(J);
  }
  if (post64) {
    cg.cur = post64;
    // q0 = (dq << sh) >> 31, at most one too big: back off by one, then the
    // remainder tells whether that was one too small
    Operand q0 = W(cg, Op::Shr, dq, W(cg, Op::Xor, Operand::reg(sh), Operand::cst(31)));
    q0 = W(cg, Op::Sub, q0, S(cg, Cond::NE, q0, Operand::cst(0)));
    EV prod = pair(W(cg, Op::Mul, q0, d.op), W(cg, Op::Add, mulhu(cg, q0, d.op), W(cg, Op::Mul, q0, d.hi)), u);
    EV rem = sub64(cg, n, prod);
    Reg *qq = cg.var("q0"), *r0 = cg.var("r0"), *r1 = cg.var("r0.hi");
    cg.copy(qq, q0);
    cg.copy(r0, rem.op);
    cg.copy(r1, rem.hi);
    BasicBlock *fix = cg.block("divfix"), *done = cg.block("divfixed");
    br64(cg, Cond::GEU, rem, d, fix, done);
    cg.cur = fix;
    cg.copy(qq, W(cg, Op::Add, Operand::reg(qq), Operand::cst(1)));
    EV r2 = sub64(cg, rem, d);
    cg.copy(r0, r2.op);
    cg.copy(r1, r2.hi);
    cg.jmp(done);
    cg.cur = done;
    cg.copy(ql, Operand::reg(qq));
    cg.copy(qh, Operand::cst(0));
    cg.copy(rl, Operand::reg(r0));
    cg.copy(rh, Operand::reg(r1));
    cg.jmp(J);
  }
  cg.cur = J;
  *q = pair(Operand::reg(ql), Operand::reg(qh), u);
  *r = pair(Operand::reg(rl), Operand::reg(rh), u);
}

static bool foldBin64(int op, bool sign, uint64_t a, uint64_t b, uint64_t *r, int line) {
  int64_t sa = (int64_t)a, sb = (int64_t)b;
  switch (op) {
    case '+': *r = a + b; return true;
    case '-': *r = a - b; return true;
    case '*': *r = a * b; return true;
    case '&': *r = a & b; return true;
    case '|': *r = a | b; return true;
    case '^': *r = a ^ b; return true;
    case 'L': *r = a << (b & 63); return true;
    case 'R': *r = sign ? (uint64_t)(sa >> (b & 63)) : a >> (b & 63); return true;
    case '/': case '%':
      if (b == 0) {
        error(line, "division by zero in a constant expression");
        *r = 0;
      } else if (!sign) {
        *r = op == '/' ? a / b : a % b;
      } else if (sa == INT64_MIN && sb == -1) {
        *r = op == '/' ? a : 0;
      } else {
        *r = op == '/' ? (uint64_t)(sa / sb) : (uint64_t)(sa % sb);
      }
      return true;
  }
  return false;
}

static EV arith64(CG &cg, int op, EV a, EV b, Type ct) {
  bool shift = op == 'L' || op == 'R';
  a = convert(cg, a, ct);
  if (!shift) b = convert(cg, b, ct);
  if (a.isConst() && b.isConst()) {
    uint64_t r = 0;
    foldBin64(op, ct.sign, cvalOf(a), shift ? (uint32_t)b.op.imm : cvalOf(b), &r, cg.line);
    return constOf(r, ct);
  }
  switch (op) {
    case '&': return pair(W(cg, Op::And, a.op, b.op), W(cg, Op::And, a.hi, b.hi), ct);
    case '|': return pair(W(cg, Op::Or, a.op, b.op), W(cg, Op::Or, a.hi, b.hi), ct);
    case '^': return pair(W(cg, Op::Xor, a.op, b.op), W(cg, Op::Xor, a.hi, b.hi), ct);
    case '+': return add64(cg, a, b);
    case '-': return sub64(cg, a, b);
    case '*': {
      Operand hi = W(cg, Op::Add, W(cg, Op::Add, mulhu(cg, a.op, b.op), W(cg, Op::Mul, a.op, b.hi)),
                     W(cg, Op::Mul, a.hi, b.op));
      return pair(W(cg, Op::Mul, a.op, b.op), hi, ct);
    }
    case 'L': return shift64(cg, Op::Shl, a, b.op);   // the count's low word (mod 64)
    case 'R': return shift64(cg, ct.sign ? Op::Sar : Op::Shr, a, b.op);
    case '/': case '%': {
      if (b.isConst() && cvalOf(b) == 0) error(cg.line, "division by zero");
      if (!ct.sign && b.isConst() && cvalOf(b) && !(cvalOf(b) & (cvalOf(b) - 1))) {   // power of two
        int k = __builtin_ctzll(cvalOf(b));
        if (op == '/') return shift64(cg, Op::Shr, a, Operand::cst(k));
        return arith64(cg, '&', a, constOf(cvalOf(b) - 1, ct), ct);
      }
      EV q, r;
      if (!ct.sign) {
        udivmod64(cg, a, b, &q, &r);
        return op == '/' ? q : r;
      }
      // signed: divide the magnitudes; the quotient is negative when the
      // signs differ, the remainder takes the dividend's sign (C truncation)
      Operand sa = W(cg, Op::Sar, a.hi, Operand::cst(31)), sb = W(cg, Op::Sar, b.hi, Operand::cst(31));
      EV ua = sub64(cg, xor64(cg, a, sa), pair(sa, sa, ct));
      EV ub = sub64(cg, xor64(cg, b, sb), pair(sb, sb, ct));
      udivmod64(cg, ua, ub, &q, &r);
      if (op == '/') {
        Operand sq = W(cg, Op::Xor, sa, sb);
        return sub64(cg, xor64(cg, pair(q.op, q.hi, ct), sq), pair(sq, sq, ct));
      }
      return sub64(cg, xor64(cg, pair(r.op, r.hi, ct), sa), pair(sa, sa, ct));
    }
  }
  fatal(cg.line, "internal: operator %c", op);
}

// a < b on word pairs, as 0/1
static Operand lt64(CG &cg, EV a, EV b, bool sign) {
  Operand hl = S(cg, sign ? Cond::LT : Cond::LTU, a.hi, b.hi);
  Operand he = S(cg, Cond::EQ, a.hi, b.hi);
  return W(cg, Op::Or, hl, W(cg, Op::And, he, S(cg, Cond::LTU, a.op, b.op)));
}
static Operand cmp64(CG &cg, Cond c, EV a, EV b) {
  switch (c) {
    case Cond::EQ: case Cond::NE:
      return S(cg, c, W(cg, Op::Or, W(cg, Op::Xor, a.op, b.op), W(cg, Op::Xor, a.hi, b.hi)), Operand::cst(0));
    case Cond::LT: case Cond::LTU: return lt64(cg, a, b, c == Cond::LT);
    case Cond::GT: case Cond::GTU: return lt64(cg, b, a, c == Cond::GT);
    case Cond::GE: case Cond::GEU: return W(cg, Op::Xor, lt64(cg, a, b, c == Cond::GE), Operand::cst(1));
    case Cond::LE: case Cond::LEU: return W(cg, Op::Xor, lt64(cg, b, a, c == Cond::LE), Operand::cst(1));
  }
  return Operand::cst(0);
}
// Branch on a comparison of word pairs: the high words decide unless equal.
static void br64(CG &cg, Cond c, EV a, EV b, BasicBlock *t, BasicBlock *f) {
  BasicBlock *m = cg.block("hieq");
  if (c == Cond::EQ) {
    cg.br(Cond::NE, a.hi, b.hi, f, m);
    cg.cur = m;
    cg.br(Cond::EQ, a.op, b.op, t, f);
    return;
  }
  if (c == Cond::NE) {
    cg.br(Cond::NE, a.hi, b.hi, t, m);
    cg.cur = m;
    cg.br(Cond::NE, a.op, b.op, t, f);
    return;
  }
  bool sign = c == Cond::LT || c == Cond::LE || c == Cond::GT || c == Cond::GE;
  bool less = c == Cond::LT || c == Cond::LE || c == Cond::LTU || c == Cond::LEU;
  Cond strict = less ? (sign ? Cond::LT : Cond::LTU) : (sign ? Cond::GT : Cond::GTU);
  BasicBlock *m2 = cg.block("hine");
  cg.br(strict, a.hi, b.hi, t, m2);
  cg.cur = m2;
  cg.br(Cond::NE, a.hi, b.hi, f, m);
  cg.cur = m;
  cg.br(unsignedCond(c), a.op, b.op, t, f);
}

// Arithmetic shared by BinaryExpr and compound assignment.
static EV arith(CG &cg, int op, EV a, EV b) {
  needValue(a, cg.line);
  needValue(b, cg.line);
  Type ct = (op == 'L' || op == 'R') ? promote(a.ty) : common(a.ty, b.ty);
  if (ct.is64()) return arith64(cg, op, a, b, ct);
  a = convert(cg, a, ct);       // only the low word of a 64-bit operand matters
  if (op != 'L' && op != 'R') b = convert(cg, b, ct);
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
  cg.br(Cond::NE, truth(cg, v), Operand::cst(0), t, f);
}

EV IntLit::gen(CG &cg) {
  (void)cg;
  return constOf(value, ty);
}

static EV symValue(Sym *s) {
  if (s->isConst) return constOf(s->cval, s->ty);
  if (s->ty.isStruct()) {
    EV v{Operand(), s->ty};
    for (Sym &f : s->fields) v.fields.push_back(symValue(&f));
    return v;
  }
  if (s->ty.is64()) return EV{Operand::reg(s->var), s->ty, Operand::reg(s->varHi)};
  return EV{Operand::reg(s->var), s->ty};
}

// The 32-bit words of a value / variable, in order (64-bit: low, high;
// structs: their fields in declaration order). Calls pass and return these.
static void evWords(const EV &v, std::vector<Operand> &out) {
  if (v.ty.isStruct()) {
    for (const EV &f : v.fields) evWords(f, out);
  } else {
    out.push_back(v.op);
    if (v.ty.is64()) out.push_back(v.hi);
  }
}
static void symWords(Sym &s, std::vector<Reg *> &out) {
  if (s.ty.isStruct()) {
    for (Sym &f : s.fields) symWords(f, out);
  } else {
    out.push_back(s.var);
    if (s.ty.is64()) out.push_back(s.varHi);
  }
}
static EV evFromWords(Type t, const std::vector<Operand> &w, size_t &k) {
  if (t.isStruct()) {
    EV v{Operand(), t};
    for (auto &f : t.sd->fields) v.fields.push_back(evFromWords(f.second, w, k));
    return v;
  }
  EV v{w[k++], t};
  if (t.is64()) v.hi = w[k++];
  return v;
}
// A zero of any type (error recovery).
static EV zeroOf(Type t) {
  if (t.isStruct()) {
    EV v{Operand(), t};
    for (auto &f : t.sd->fields) v.fields.push_back(zeroOf(f.second));
    return v;
  }
  return t.isVoid ? EV{Operand(), t} : constOf(0, t);
}

// Assign a value already converted to the variable's type. The words are
// copied one after the other, so when the value still names one of the
// target's own variables (d = d << 32 makes the new high word the old low
// variable) it is read into temporaries first.
static void assignSym(CG &cg, Sym *s, EV v) {
  std::vector<Reg *> dst;
  std::vector<Operand> src;
  symWords(*s, dst);
  evWords(v, src);
  if (dst.size() != src.size()) return;   // a type error, already reported
  std::set<Reg *> mine(dst.begin(), dst.end());
  bool overlap = false;
  for (size_t k = 1; k < src.size(); k++)
    if (src[k].isReg() && mine.count(src[k].r)) overlap = true;
  if (overlap)
    for (auto &o : src)
      if (o.isReg()) {
        Reg *t = cg.temp();
        cg.copy(t, o);
        o = Operand::reg(t);
      }
  for (size_t k = 0; k < dst.size(); k++) cg.copy(dst[k], src[k]);
}
static Sym newVar(CG &cg, Type t, const std::string &name) {
  Sym s;
  s.ty = t;
  if (t.isStruct()) {
    for (auto &f : t.sd->fields) s.fields.push_back(newVar(cg, f.second, name + "." + f.first));
    return s;
  }
  s.var = cg.var(name);
  if (t.is64()) s.varHi = cg.var(name + ".hi");
  return s;
}

EV VarRef::gen(CG &cg) {
  cg.line = line;
  Sym *s = cg.lookup(name);
  if (!s) {
    error(line, "undeclared identifier '%s'", name.c_str());
    return EV{Operand::cst(0), Type::s32()};
  }
  return symValue(s);
}

Sym *VarRef::lvalueSym(CG &cg) {
  Sym *s = cg.lookup(name);
  if (!s) error(line, "undeclared identifier '%s'", name.c_str());
  else if (s->isConst) error(line, "assignment to constant '%s'", name.c_str());
  else return s;
  return nullptr;
}

static int fieldIndex(const Type &t, const std::string &f, int line) {
  if (!t.isStruct()) {
    error(line, "'.%s' applied to %s, not a struct", f.c_str(), t.str().c_str());
    return -1;
  }
  int k = t.sd->find(f);
  if (k < 0) error(line, "%s has no field '%s'", t.str().c_str(), f.c_str());
  return k;
}

EV MemberExpr::gen(CG &cg) {
  EV b = base->gen(cg);
  cg.line = line;
  int k = fieldIndex(b.ty, field, line);
  if (k < 0) return EV{Operand::cst(0), Type::s32()};
  return b.fields[k];
}

Sym *MemberExpr::lvalueSym(CG &cg) {
  Sym *b = base->lvalueSym(cg);
  if (!b) return nullptr;
  int k = fieldIndex(b->ty, field, line);
  return k < 0 ? nullptr : &b->fields[k];
}

EV UnaryExprNode::gen(CG &cg) {
  EV v = e->gen(cg);
  cg.line = line;
  needValue(v, line);
  Type pt = promote(v.ty);
  switch (op) {
    case '+': return convert(cg, v, pt);
    case '-': return arith(cg, '-', constOf(0, pt), convert(cg, v, pt));
    case '~': return arith(cg, '^', convert(cg, v, pt), constOf(~0ull, pt));
    case '!': {
      if (v.isConst()) return EV{Operand::cst(cvalOf(v) == 0), Type::s32()};
      return EV{S(cg, Cond::EQ, truth(cg, v), Operand::cst(0)), Type::s32()};
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
  a = convert(cg, a, ct);
  b = convert(cg, b, ct);
  if (ct.is64()) {
    if (a.isConst() && b.isConst()) {
      uint64_t x = cvalOf(a), y = cvalOf(b);
      bool r;
      switch (cc) {
        case Cond::EQ: r = x == y; break;
        case Cond::NE: r = x != y; break;
        case Cond::LT: r = (int64_t)x < (int64_t)y; break;
        case Cond::GE: r = (int64_t)x >= (int64_t)y; break;
        case Cond::GT: r = (int64_t)x > (int64_t)y; break;
        case Cond::LE: r = (int64_t)x <= (int64_t)y; break;
        case Cond::LTU: r = x < y; break;
        case Cond::GEU: r = x >= y; break;
        case Cond::GTU: r = x > y; break;
        default: r = x <= y; break;
      }
      return EV{Operand::cst(r), Type::s32()};
    }
    return EV{cmp64(cg, cc, a, b), Type::s32()};
  }
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
  if (ct.is64()) {
    br64(cg, ct.sign ? c : unsignedCond(c), convert(cg, a, ct), convert(cg, b, ct), t, f);
    return;
  }
  a = convert(cg, a, ct);
  b = convert(cg, b, ct);
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
      return EV{Operand::cst(op == '&' ? (cvalOf(a) && cvalOf(b)) : (cvalOf(a) || cvalOf(b))), Type::s32()};
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
    if (cv.isConst() && av.isConst() && bv.isConst()) return constOf(cvalOf(cvalOf(cv) ? av : bv), ct);
    error(line, "not a constant expression");
    return EV{Operand::cst(0), Type::s32()};
  }
  BasicBlock *T = cg.block("ct"), *F = cg.block("cf"), *J = cg.block("cj");
  c->genCond(cg, T, F);
  cg.cur = T;
  EV av = a->gen(cg);
  BasicBlock *endT = cg.cur;
  cg.cur = F;
  EV bv = b->gen(cg);
  BasicBlock *endF = cg.cur;
  Type ct;
  if (av.ty.isStruct() || bv.ty.isStruct()) {   // both must be the same struct
    ct = av.ty.isStruct() ? av.ty : bv.ty;
  } else {
    needValue(av, line);
    needValue(bv, line);
    ct = common(av.ty, bv.ty);
  }
  Sym v = newVar(cg, ct, "cv");
  cg.cur = endT;
  assignSym(cg, &v, convert(cg, av, ct));
  cg.jmp(J);
  cg.cur = endF;
  assignSym(cg, &v, convert(cg, bv, ct));
  cg.jmp(J);
  cg.cur = J;
  return symValue(&v);
}

static Sym *lvalue(CG &cg, Expr *e, int line) {
  if (!dynamic_cast<VarRef *>(e) && !dynamic_cast<MemberExpr *>(e)) {
    error(line, "assignment to something that is not a variable");
    return nullptr;
  }
  return e->lvalueSym(cg);
}

EV AssignExpr::gen(CG &cg) {
  cg.line = line;
  Sym *s = lvalue(cg, lhs, line);
  EV v = rhs->gen(cg);
  cg.line = line;
  if (!s) return v;
  if (op) v = arith(cg, op, symValue(s), v);
  EV o = convert(cg, v, s->ty);
  assignSym(cg, s, o);
  return o;
}

EV IncDecExpr::gen(CG &cg) {
  cg.line = line;
  Sym *s = lvalue(cg, e, line);
  if (!s) return EV{Operand::cst(0), Type::s32()};
  if (s->ty.isStruct()) {
    error(line, "++/-- on a struct");
    return EV{Operand::cst(0), Type::s32()};
  }
  EV old = symValue(s);
  if (!prefix) {   // the old value outlives the assignment
    Reg *t = cg.temp();
    cg.copy(t, old.op);
    old.op = Operand::reg(t);
    if (s->ty.is64()) {
      Reg *th = cg.temp();
      cg.copy(th, old.hi);
      old.hi = Operand::reg(th);
    }
  }
  EV nv = arith(cg, op, old, EV{Operand::cst(1), Type::s32()});
  EV n = convert(cg, nv, s->ty);
  assignSym(cg, s, n);
  return prefix ? n : old;
}

EV CastExpr::gen(CG &cg) {
  EV v = e->gen(cg);
  cg.line = line;
  EV r = convert(cg, v, t);
  r.ty = t;
  return r;
}

EV CallExpr::gen(CG &cg) {
  cg.line = line;
  if (name.compare(0, 2, "__") == 0) {
    if (const EntryReg *er = findEntryReg(name)) {
      if (!args.empty()) error(line, "%s takes no arguments", name.c_str());
      if (!cg.fn || !cg.rootFd->entry) {
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
  if (callee->params.size() != args.size()) {
    error(line, "'%s' takes %zu arguments, %zu given", name.c_str(), callee->params.size(), args.size());
    return zeroOf(callee->ret);
  }
  if (!cg.fn) {
    error(line, "not a constant expression");
    return zeroOf(callee->ret);
  }
  if (callee->isInline) return genInline(cg, callee);
  std::vector<Operand> ops;   // every argument's words: 64-bit low first, structs field by field
  for (size_t i = 0; i < args.size(); i++) {
    EV v = args[i]->gen(cg);
    cg.line = line;
    evWords(convert(cg, v, callee->params[i].t), ops);
  }
  Instr *c = new Instr(Op::Call);
  c->callee = callee->fn;
  c->a = ops;
  int rw = callee->ret.words();
  if (rw) c->dst = cg.temp();
  cg.emit(c);
  cg.fn->callees.insert(callee->fn);
  if (!rw) return EV{Operand(), Type::voidType()};
  std::vector<Operand> res = {Operand::reg(c->dst)};
  if (rw == 2) {
    Instr *h = new Instr(Op::CallRes);
    h->callee = callee->fn;
    h->dst = cg.temp();
    cg.emit(h);
    res.push_back(Operand::reg(h->dst));
  }
  size_t k = 0;
  return evFromWords(callee->ret, res, k);
}

// An inline call: the callee's body is generated here, with its parameters
// bound to new variables (or, for reference parameters, to the caller's
// variables) and `return` assigning the result and jumping to the end.
EV CallExpr::genInline(CG &cg, FnDecl *callee) {
  for (InlineFrame *f : cg.inl)
    if (f->fd == callee) {
      error(line, "inline function '%s' calls itself (recursion is not allowed in millicode)", name.c_str());
      return zeroOf(callee->ret);
    }
  // arguments are evaluated in the caller's scope
  std::vector<EV> vals(args.size());
  std::vector<Sym *> refs(args.size(), nullptr);
  for (size_t i = 0; i < args.size(); i++) {
    const Param &p = callee->params[i];
    if (p.ref) {
      refs[i] = lvalue(cg, args[i], line);
      if (refs[i] && !(refs[i]->ty == p.t))
        error(line, "argument %zu of '%s' is a reference to %s, but %s was passed", i + 1, name.c_str(),
              p.t.str().c_str(), refs[i]->ty.str().c_str());
    } else {
      vals[i] = args[i]->gen(cg);
    }
  }
  cg.line = line;
  InlineFrame fr;
  fr.fd = callee;
  fr.join = cg.block("inl");
  if (!callee->ret.isVoid) fr.ret = newVar(cg, callee->ret, callee->name + ".ret");
  size_t savedFloor = cg.floor;
  FnDecl *savedFd = cg.fd;
  std::vector<BasicBlock *> savedBreaks, savedConts;
  std::vector<SwitchCtx *> savedSwitches;
  std::swap(savedBreaks, cg.breaks);
  std::swap(savedConts, cg.conts);
  std::swap(savedSwitches, cg.switches);
  cg.scopes.emplace_back();
  cg.floor = cg.scopes.size() - 1;
  for (size_t i = 0; i < args.size(); i++) {
    const Param &p = callee->params[i];
    Sym s;
    if (p.ref) {
      if (!refs[i] || !(refs[i]->ty == p.t)) s = newVar(cg, p.t, p.name);   // error recovery
      else {
        s.ty = p.t;
        s.alias = refs[i];
      }
    } else {
      s = newVar(cg, p.t, p.name);
      assignSym(cg, &s, convert(cg, vals[i], p.t));
    }
    cg.declare(p.name, s, callee->line);
  }
  cg.fd = callee;
  cg.inl.push_back(&fr);
  callee->body->gen(cg);
  cg.line = callee->line;
  if (!cg.cur->terminated()) {
    if (callee->ret.isVoid) {
      cg.jmp(fr.join);
    } else {   // reported later, and only if reachable
      Instr *i = new Instr(Op::Stop);
      i->falloff = true;
      cg.emit(i);
    }
  }
  cg.inl.pop_back();
  cg.fd = savedFd;
  cg.scopes.pop_back();
  cg.floor = savedFloor;
  std::swap(savedBreaks, cg.breaks);
  std::swap(savedConts, cg.conts);
  std::swap(savedSwitches, cg.switches);
  cg.cur = fr.join;
  cg.line = line;
  if (callee->ret.isVoid) return EV{Operand(), Type::voidType()};
  return symValue(&fr.ret);
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
      EV v = it.init ? it.init->gen(cg) : constOf(0, t);
      if (!v.isConst()) error(it.line, "initializer of constant '%s' is not a constant expression", it.name);
      s.isConst = true;
      s.cval = v.isConst() ? cvalOf(constOf(cvalOf(v), t)) : 0;
      cg.declare(it.name, s, it.line);
      continue;
    }
    s = newVar(cg, t, it.name);
    if (it.init) {
      EV v = it.init->gen(cg);
      cg.line = it.line;
      assignSym(cg, &s, convert(cg, v, t));
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
  std::map<uint64_t, BasicBlock *> cases;
  std::vector<std::pair<uint64_t, BasicBlock *>> order;
  EV sv = convert(cg, v, st);
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
    uint64_t kv = cvalOf(constOf(cvalOf(k), st));
    if (cases.count(kv)) error(cl->line, "duplicate case value %lld", (long long)kv);
    cases[kv] = b;
    order.push_back({kv, b});
  }
  cg.line = line;
  for (auto &p : order) {
    BasicBlock *next = cg.block("swnext");
    if (st.is64()) br64(cg, Cond::EQ, sv, constOf(p.first, st), p.second, next);
    else cg.br(Cond::EQ, sv.op, constOf(p.first, st).op, p.second, next);
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
  EV rv;
  if (e) {
    EV v = e->gen(cg);
    cg.line = line;
    if (fd->ret.isVoid) error(line, "void function '%s' returns a value", fd->name.c_str());
    else rv = convert(cg, v, fd->ret);
  } else if (!fd->ret.isVoid) {
    error(line, "function '%s' must return a value", fd->name.c_str());
    rv = zeroOf(fd->ret);
  }
  if (!cg.inl.empty()) {   // inside an inline expansion: assign the result, go to the end
    InlineFrame *fr = cg.inl.back();
    if (!fd->ret.isVoid) assignSym(cg, &fr->ret, rv);
    cg.jmp(fr->join);
    cg.endWithDeadBlock();
    return;
  }
  Instr *r = new Instr(Op::Ret);
  if (!fd->ret.isVoid) evWords(rv, r->a);
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
    s.cval = (uint64_t)(int64_t)c.value;
    cg.scopes.back()[c.name] = s;
  }
  for (ConstDecl *c : tu->consts) {
    cg.line = c->line;
    EV v = c->item.init ? c->item.init->gen(cg) : constOf(0, c->t);
    if (!v.isConst()) error(c->item.line, "initializer of constant '%s' is not a constant expression", c->item.name);
    Sym s;
    s.ty = c->t;
    s.isConst = true;
    s.cval = v.isConst() ? cvalOf(constOf(cvalOf(v), c->t)) : 0;
    cg.declare(c->item.name, s, c->item.line);
  }
  for (FnDecl *fd : tu->fns) {
    if (fd->name.compare(0, 2, "__") == 0) error(fd->line, "names starting with __ are reserved for intrinsics");
    if (cg.fns.count(fd->name)) error(fd->line, "function '%s' defined twice", fd->name.c_str());
    if (fd->entry && (!fd->ret.isVoid || !fd->params.empty()))
      error(fd->line, "entry function '%s' must be declared `entry void %s(void)`", fd->name.c_str(), fd->name.c_str());
    cg.fns[fd->name] = fd;
    if (fd->isInline) continue;   // expanded where it is called; no code of its own
    for (auto &p : fd->params)
      if (p.ref) error(fd->line, "reference parameter '%s': only inline functions take references", p.name);
    if (fd->ret.words() > 2)
      error(fd->line, "'%s' returns %s (%d words); only inline functions return more than two words",
            fd->name.c_str(), fd->ret.str().c_str(), fd->ret.words());
    fd->fn = new Function(fd->name);
    fd->fn->isEntry = fd->entry;
    fd->fn->returnsValue = !fd->ret.isVoid;
    fd->fn->retWords = fd->ret.words();
    fd->fn->line = fd->line;
    cg.prog->funcs.push_back(fd->fn);
  }
  for (FnDecl *fd : tu->fns) {
    if (fd->isInline) continue;
    cg.fd = fd;
    cg.rootFd = fd;
    cg.fn = fd->fn;
    cg.cur = cg.fn->newBlock("entry");
    cg.scopes.emplace_back();
    cg.floor = 1;
    for (auto &p : fd->params) {
      Sym s = newVar(cg, p.t, p.name);
      symWords(s, cg.fn->paramVars);
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
