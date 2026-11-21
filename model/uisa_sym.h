/* Symbolic C integers over z3 bitvectors, for the uop semantics.
 *
 * tools/gen_smt.py translates each op's `sem` (spec/uisa.yaml, a small C
 * subset) into C++ over Sym, so the symbolic semantics are the same text as
 * the concrete ones the ISS compiles. A Sym carries a z3 bitvector and its C
 * type; the operators follow C's integer promotions and usual arithmetic
 * conversions, so 64-bit intermediates, signed compares, signed division and
 * so on mean what they mean in C.
 *
 * Control flow on symbolic values is guarded execution: the translator turns
 * `if (c) A else B` into both arms run under a guard (SymGuard), and an
 * assignment under a guard merges, x = ite(guard, new, old). `c ? x : y`
 * becomes ITE(c, x, y). A declaration (`uint32_t t = ...`) is a fresh value
 * and is not guarded.
 */
#ifndef UISA_SYM_H
#define UISA_SYM_H

#include <z3++.h>

#include <cstdint>
#include <vector>

namespace usym {

enum Ty { B, I32, U32, I64, U64 };   // bool, int / int32_t, unsigned / uint32_t, int64_t, uint64_t

inline unsigned width(Ty t) { return t == B ? 1 : (t == I64 || t == U64) ? 64 : 32; }
inline bool is_signed(Ty t) { return t == I32 || t == I64; }

inline z3::context &ctx() {
  static z3::context c;
  return c;
}

// guards of the enclosing symbolic ifs (innermost last)
inline std::vector<z3::expr> &guards() {
  static std::vector<z3::expr> g;
  return g;
}

struct Sym;
Sym conv(const Sym &x, Ty to);

struct Sym {
  z3::expr e;
  Ty t;
  Sym() : e(ctx().bv_val(0, 32)), t(U32) {}
  Sym(const z3::expr &e_, Ty t_) : e(e_), t(t_) {}
  Sym(const Sym &o) = default;
  Sym(bool v) : e(ctx().bv_val(v ? 1 : 0, 1)), t(B) {}
  Sym(int v) : e(ctx().bv_val(v, 32)), t(I32) {}
  Sym(unsigned v) : e(ctx().bv_val(v, 32)), t(U32) {}
  Sym(long v) : e(ctx().bv_val((int64_t)v, 64)), t(I64) {}
  Sym(long long v) : e(ctx().bv_val((int64_t)v, 64)), t(I64) {}
  Sym(unsigned long v) : e(ctx().bv_val((uint64_t)v, 64)), t(U64) {}
  Sym(unsigned long long v) : e(ctx().bv_val((uint64_t)v, 64)), t(U64) {}
  static Sym of(Ty t, const Sym &x) { return conv(x, t); }
  static Sym var(const char *name, Ty t) { return Sym(ctx().bv_const(name, width(t)), t); }
  // assignment converts to the target's type and merges under the guards
  Sym &operator=(const Sym &o) {
    Sym v = conv(o, t);
    if (guards().empty()) e = v.e;
    else e = z3::ite(guards().back(), v.e, e);
    return *this;
  }
  z3::expr cond() const { return e != ctx().bv_val(0, width(t)); }   // as a C condition
};

inline Sym conv(const Sym &x, Ty to) {
  if (x.t == to) return x;
  if (to == B) return Sym(z3::ite(x.cond(), ctx().bv_val(1, 1), ctx().bv_val(0, 1)), B);
  unsigned wf = width(x.t), wt = width(to);
  z3::expr e = x.e;
  if (wt > wf) e = is_signed(x.t) ? z3::sext(e, wt - wf) : z3::zext(e, wt - wf);
  else if (wt < wf) e = e.extract(wt - 1, 0);
  return Sym(e, to);
}

// integer promotion (bool -> int) and the usual arithmetic conversions
inline Ty promote(Ty t) { return t == B ? I32 : t; }
inline Ty common(Ty a, Ty b) {
  a = promote(a); b = promote(b);
  if (width(a) == width(b)) return (is_signed(a) && is_signed(b)) ? a : (width(a) == 64 ? U64 : U32);
  Ty wide = width(a) == 64 ? a : b;
  return wide;   // a 64-bit type holds every 32-bit value: it keeps its signedness
}

#define USYM_ARITH(OP, EXPR)                                            \
  inline Sym operator OP(const Sym &x, const Sym &y) {                  \
    Ty t = common(x.t, y.t);                                            \
    z3::expr a = conv(x, t).e, b = conv(y, t).e;                        \
    (void)a; (void)b;                                                   \
    return Sym(EXPR, t);                                                \
  }
USYM_ARITH(+, a + b)
USYM_ARITH(-, a - b)
USYM_ARITH(*, a * b)
USYM_ARITH(&, a & b)
USYM_ARITH(|, a | b)
USYM_ARITH(^, a ^ b)
USYM_ARITH(/, is_signed(t) ? z3::expr(a / b) : z3::udiv(a, b))
USYM_ARITH(%, is_signed(t) ? z3::srem(a, b) : z3::urem(a, b))
#undef USYM_ARITH

#define USYM_CMP(OP, SEXPR, UEXPR)                                      \
  inline Sym operator OP(const Sym &x, const Sym &y) {                  \
    Ty t = common(x.t, y.t);                                            \
    z3::expr a = conv(x, t).e, b = conv(y, t).e;                        \
    z3::expr c = is_signed(t) ? z3::expr(SEXPR) : z3::expr(UEXPR);      \
    return Sym(z3::ite(c, ctx().bv_val(1, 1), ctx().bv_val(0, 1)), B);  \
  }
USYM_CMP(==, a == b, a == b)
USYM_CMP(!=, a != b, a != b)
USYM_CMP(<, a < b, z3::ult(a, b))
USYM_CMP(<=, a <= b, z3::ule(a, b))
USYM_CMP(>, a > b, z3::ugt(a, b))
USYM_CMP(>=, a >= b, z3::uge(a, b))
#undef USYM_CMP

// shifts: the type of the promoted left operand; the count converted to its width
inline Sym operator<<(const Sym &x, const Sym &y) {
  Ty t = promote(x.t);
  return Sym(z3::shl(conv(x, t).e, conv(y, t == I64 || t == U64 ? U64 : U32).e), t);
}
inline Sym operator>>(const Sym &x, const Sym &y) {
  Ty t = promote(x.t);
  z3::expr a = conv(x, t).e, k = conv(y, width(t) == 64 ? U64 : U32).e;
  return Sym(is_signed(t) ? z3::ashr(a, k) : z3::lshr(a, k), t);
}

inline Sym operator!(const Sym &x) { return Sym(z3::ite(x.cond(), ctx().bv_val(0, 1), ctx().bv_val(1, 1)), B); }
inline Sym operator~(const Sym &x) { Ty t = promote(x.t); return Sym(~conv(x, t).e, t); }
inline Sym operator-(const Sym &x) { Ty t = promote(x.t); return Sym(-conv(x, t).e, t); }
inline Sym operator&&(const Sym &x, const Sym &y) {
  return Sym(z3::ite(x.cond() && y.cond(), ctx().bv_val(1, 1), ctx().bv_val(0, 1)), B);
}
inline Sym operator||(const Sym &x, const Sym &y) {
  return Sym(z3::ite(x.cond() || y.cond(), ctx().bv_val(1, 1), ctx().bv_val(0, 1)), B);
}
inline Sym ITE(const Sym &c, const Sym &x, const Sym &y) {
  Ty t = (x.t == B && y.t == B) ? B : common(x.t, y.t);
  return Sym(z3::ite(c.cond(), conv(x, t).e, conv(y, t).e), t);
}

// one symbolic `if`: the then-arm runs under parent && c, flip() switches
// to the else-arm (parent && !c)
struct SymGuard {
  z3::expr c, parent;
  explicit SymGuard(const Sym &cond)
      : c(cond.cond()), parent(guards().empty() ? ctx().bool_val(true) : guards().back()) {
    guards().push_back(parent && c);
  }
  void flip() { guards().back() = parent && !c; }
  ~SymGuard() { guards().pop_back(); }
  SymGuard(const SymGuard &) = delete;
  SymGuard &operator=(const SymGuard &) = delete;
};
inline z3::expr guard() { return guards().empty() ? ctx().bool_val(true) : guards().back(); }

// ---- the helpers the semantics call (uisa.h), symbolically ----------------
inline Sym MSK(const Sym &n) {
  Sym nn = conv(n, U32);
  return ITE(nn >= 32u, Sym(0xffffffffu), (Sym(1u) << nn) - 1u);
}
inline Sym MSB(const Sym &v, const Sym &n) {
  Sym w = conv(v, U64);
  return conv((w >> (conv(n, U64) - 1ull)) & 1ull, B);
}
inline Sym SX(const Sym &v, const Sym &n) {
  Sym w = conv(v, U64), k = 64ull - conv(n, U64);
  return conv(w << k, I64) >> k;
}
inline Sym ROL32(const Sym &a, const Sym &k) {
  Sym x = conv(a, U32), s = conv(k, U32) & 31u;
  return (x << s) | (x >> ((32u - s) & 63u));   // z3: a shift by 32 or more gives 0
}
inline Sym CCR_PACK(const Sym &x, const Sym &n, const Sym &z, const Sym &v, const Sym &c) {
  return (conv(x, U32) << 4u) | (conv(n, U32) << 3u) | (conv(z, U32) << 2u) | (conv(v, U32) << 1u) | conv(c, U32);
}
inline Sym BF_OFF(const Sym &p) { return conv(p, U32) & 31u; }
inline Sym BF_WID(const Sym &p) { Sym w = (conv(p, U32) >> 5u) & 31u; return ITE(w != 0u, w, Sym(32u)); }
inline Sym BF_TOPMASK(const Sym &w) { return ITE(w >= 32u, Sym(0xffffffffu), MSK(w) << (32u - w)); }
inline Sym BF_MASK(const Sym &p) { return ROL32(BF_TOPMASK(BF_WID(p)), 32u - BF_OFF(p)); }
inline Sym BF_EXTU(const Sym &a, const Sym &p) {
  Sym w = BF_WID(p), t = ROL32(a, BF_OFF(p));
  return ITE(w >= 32u, t, t >> (32u - w));
}
inline Sym BF_EXTS(const Sym &a, const Sym &p) {
  Sym w = BF_WID(p), t = ROL32(a, BF_OFF(p));
  return ITE(w >= 32u, t, conv(conv(t, I32) >> conv(32u - w, I32), U32));
}
inline Sym BF_INS(const Sym &old, const Sym &v, const Sym &p) {
  Sym w = BF_WID(p), top = BF_TOPMASK(w);
  Sym field = ITE(w >= 32u, conv(v, U32), (conv(v, U32) << (32u - w)) & top);
  Sym m = ROL32(top, 32u - BF_OFF(p));
  return (conv(old, U32) & ~m) | ROL32(field, 32u - BF_OFF(p));
}
inline Sym CLZ32(const Sym &a) {   // __builtin_clz, defined as 32 for 0 (callers guard 0 anyway)
  Sym x = conv(a, U32), r(32u);
  for (int i = 0; i < 32; i++) r.e = ITE(((x >> (unsigned)i) & 1u) != 0u, Sym((unsigned)(31 - i)), r).e;
  return r;
}
inline Sym BF_FFO(const Sym &a, const Sym &p) {
  Sym w = BF_WID(p), f = BF_EXTU(a, p);
  return ITE(f == 0u, BF_OFF(p) + w, BF_OFF(p) + (CLZ32(f) - (32u - w)));
}
inline Sym CTZ32(const Sym &a) {
  Sym x = conv(a, U32), r(32u);
  for (int i = 31; i >= 0; i--) r.e = ITE(((x >> (unsigned)i) & 1u) != 0u, Sym((unsigned)i), r).e;
  return r;
}
inline Sym POPCOUNT32(const Sym &a) {
  Sym x = conv(a, U32), r(0u);
  for (int i = 0; i < 32; i++) r.e = (r + ((x >> (unsigned)i) & 1u)).e;
  return r;
}
inline Sym BSWAP32(const Sym &a) {
  z3::expr x = conv(a, U32).e;
  return Sym(z3::concat(z3::concat(x.extract(7, 0), x.extract(15, 8)), z3::concat(x.extract(23, 16), x.extract(31, 24))), U32);
}

}  // namespace usym

#endif
