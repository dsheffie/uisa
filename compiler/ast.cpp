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
  Reg *varHi = nullptr;     // 64-bit variables: the high word
  Type ty;
  bool isConst = false;
  uint64_t cval = 0;        // constants: the value, extended per its type
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
static EV convert(CG &cg, EV v, Type to) {
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

// Unsigned 64-bit division: quotient and remainder.
static void udivmod64(CG &cg, EV n, EV d, EV *q, EV *r) {
  Type u = Type(64, false);
  bool small = n.hi.isImm() && n.hi.imm == 0 && d.hi.isImm() && d.hi.imm == 0;
  if (small) {
    *q = pair(W(cg, Op::DivU, n.op, d.op), Operand::cst(0), u);
    *r = pair(W(cg, Op::RemU, n.op, d.op), Operand::cst(0), u);
    return;
  }
  Reg *ql = cg.var("q"), *qh = cg.var("q.hi"), *rl = cg.var("r"), *rh = cg.var("r.hi");
  BasicBlock *fast = cg.block("div32"), *slow = cg.block("div64"), *J = cg.block("divend");
  cg.br(Cond::EQ, W(cg, Op::Or, n.hi, d.hi), Operand::cst(0), fast, slow);
  cg.cur = fast;
  cg.copy(ql, W(cg, Op::DivU, n.op, d.op));
  cg.copy(qh, Operand::cst(0));
  cg.copy(rl, W(cg, Op::RemU, n.op, d.op));
  cg.copy(rh, Operand::cst(0));
  cg.jmp(J);
  // restoring shift-subtract: the dividend shifts left through the
  // remainder and the quotient bits shift in behind it
  cg.cur = slow;
  Reg *i = cg.var("i");
  cg.copy(ql, n.op);
  cg.copy(qh, n.hi);
  cg.copy(rl, Operand::cst(0));
  cg.copy(rh, Operand::cst(0));
  cg.copy(i, Operand::cst(64));
  BasicBlock *L = cg.block("divloop"), *Sub = cg.block("divsub"), *C = cg.block("divnext");
  cg.jmp(L);
  cg.cur = L;
  Operand QL = Operand::reg(ql), QH = Operand::reg(qh), RL = Operand::reg(rl), RH = Operand::reg(rh);
  Operand nrh = W(cg, Op::Or, W(cg, Op::Shl, RH, Operand::cst(1)), W(cg, Op::Shr, RL, Operand::cst(31)));
  Operand nrl = W(cg, Op::Or, W(cg, Op::Shl, RL, Operand::cst(1)), W(cg, Op::Shr, QH, Operand::cst(31)));
  Operand nqh = W(cg, Op::Or, W(cg, Op::Shl, QH, Operand::cst(1)), W(cg, Op::Shr, QL, Operand::cst(31)));
  Operand nql = W(cg, Op::Shl, QL, Operand::cst(1));
  cg.copy(rh, nrh);
  cg.copy(rl, nrl);
  cg.copy(qh, nqh);
  cg.copy(ql, nql);
  br64(cg, Cond::GEU, pair(RL, RH, u), d, Sub, C);
  cg.cur = Sub;
  EV nr = sub64(cg, pair(RL, RH, u), d);
  cg.copy(rl, nr.op);
  cg.copy(rh, nr.hi);
  cg.copy(ql, W(cg, Op::Or, QL, Operand::cst(1)));
  cg.jmp(C);
  cg.cur = C;
  cg.copy(i, W(cg, Op::Add, Operand::reg(i), Operand::cst(-1)));
  cg.br(Cond::NE, Operand::reg(i), Operand::cst(0), L, J);
  cg.cur = J;
  *q = pair(QL, QH, u);
  *r = pair(RL, RH, u);
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
  if (s->ty.is64()) return EV{Operand::reg(s->var), s->ty, Operand::reg(s->varHi)};
  return EV{Operand::reg(s->var), s->ty};
}
// Assign a value already converted to the variable's type.
static void assignSym(CG &cg, Sym *s, const EV &v) {
  cg.copy(s->var, v.op);
  if (s->ty.is64()) cg.copy(s->varHi, v.hi);
}
static Sym newVar(CG &cg, Type t, const std::string &name) {
  Sym s;
  s.ty = t;
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
  needValue(av, line);
  needValue(bv, line);
  Type ct = common(av.ty, bv.ty);
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
  if (op) v = arith(cg, op, symValue(s), v);
  EV o = convert(cg, v, s->ty);
  assignSym(cg, s, o);
  return o;
}

EV IncDecExpr::gen(CG &cg) {
  cg.line = line;
  Sym *s = lvalue(cg, e, line);
  if (!s) return EV{Operand::cst(0), Type::s32()};
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
  std::vector<Operand> ops;   // 64-bit arguments take two words, low first
  for (size_t i = 0; i < args.size(); i++) {
    EV v = args[i]->gen(cg);
    cg.line = line;
    if (i >= callee->params.size()) continue;
    EV cv = convert(cg, v, callee->params[i].t);
    ops.push_back(cv.op);
    if (callee->params[i].t.is64()) ops.push_back(cv.hi);
  }
  Instr *c = new Instr(Op::Call);
  c->callee = callee->fn;
  c->a = ops;
  if (!callee->ret.isVoid) c->dst = cg.temp();
  cg.emit(c);
  if (cg.fn) cg.fn->callees.insert(callee->fn);
  if (callee->ret.isVoid) return EV{Operand(), Type::voidType()};
  if (callee->ret.is64()) {
    Instr *h = new Instr(Op::CallRes);
    h->callee = callee->fn;
    h->dst = cg.temp();
    cg.emit(h);
    return EV{Operand::reg(c->dst), callee->ret, Operand::reg(h->dst)};
  }
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
  Instr *r = new Instr(Op::Ret);
  if (e) {
    EV v = e->gen(cg);
    cg.line = line;
    if (fd->ret.isVoid) error(line, "void function '%s' returns a value", fd->name.c_str());
    else {
      EV rv = convert(cg, v, fd->ret);
      r->a = {rv.op};
      if (fd->ret.is64()) r->a.push_back(rv.hi);
    }
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
    fd->fn = new Function(fd->name);
    fd->fn->isEntry = fd->entry;
    fd->fn->returnsValue = !fd->ret.isVoid;
    fd->fn->retWords = fd->ret.isVoid ? 0 : fd->ret.is64() ? 2 : 1;
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
      Sym s = newVar(cg, p.t, p.name);
      cg.fn->paramVars.push_back(s.var);
      if (s.varHi) cg.fn->paramVars.push_back(s.varHi);
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
