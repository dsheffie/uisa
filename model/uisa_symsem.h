/* The uop semantics, symbolically: the context build/uisa_smt.inc (from
 * tools/gen_smt.py) runs in. A user (the symbolic executor, the tests)
 * derives from SymSem, fills in the operands and implements the hooks for
 * the machine state outside the register file: CSRs, guest registers by
 * number, the millicode return stack, the guest next PC, guest stores.
 */
#ifndef UISA_SYMSEM_H
#define UISA_SYMSEM_H

#include "uisa.h"
#include "uisa_sym.h"

namespace usym {

// t[i] for a symbolic i (constant-folds when i is a numeral)
inline Sym index(const uint32_t *t, size_t n, const Sym &i) {
  Sym ix = conv(i, U32);
  z3::expr s = ix.e.simplify();
  if (s.is_numeral()) {
    uint64_t k = s.get_numeral_uint64();
    return Sym(k < n ? t[k] : 0u);
  }
  Sym r(0u);
  for (size_t k = 0; k < n; k++) r.e = ITE(ix == (unsigned)k, Sym(t[k]), r).e;
  return r;
}

}  // namespace usym

using usym::Sym;

// the result of a guest op (as uisa::gop_out)
struct SymGop {
  Sym r, N, Z, V, C, X, T;
};

struct SymSem {
  virtual ~SymSem() {}
  // operands, as the ISS names them (gen.py's sem.inc header)
  Sym a, b, r, OLD, IMM, pc, npc, GPC_INSN;
  Sym xin, nin, zin, vin, cin;   // flags before
  Sym X, N, Z, V, C;             // flags after (start equal to the before values)
  Sym uin, U;                    // the millicode predicate u (a 1-bit flag), before and after
  static constexpr int GDATA = 1;

  // machine state outside the register file
  virtual Sym CSR_READ(const Sym &i) = 0;
  virtual void CSR_WRITE(const Sym &i, const Sym &v) = 0;
  virtual Sym GREG_READ(const Sym &i) = 0;
  virtual void GREG_WRITE(const Sym &i, const Sym &v) = 0;
  virtual void CSTACK_PUSH(const Sym &v) = 0;
  virtual Sym CSTACK_POP() = 0;
  virtual void GNPC_SET(const Sym &v) = 0;
  virtual void GSTORE(int space, const Sym &addr, int n, const Sym &v) = 0;
  virtual void MMU_FLUSH(const Sym &, const Sym &) {}
  virtual void CACHE_OP(const Sym &, const Sym &, int) {}
  void SYM_CCR_UNPACK(const Sym &v, Sym &X_, Sym &N_, Sym &Z_, Sym &V_, Sym &C_) {   // CCR_UNPACK
    Sym w = usym::conv(v, usym::U32);
    X_ = (w >> 4u) & 1u; N_ = (w >> 3u) & 1u; Z_ = (w >> 2u) & 1u; V_ = (w >> 1u) & 1u; C_ = w & 1u;
  }

  // run a base op's semantics (`op` a uisa::op_t); false if it has none
  bool base(unsigned op);
};

#include "uisa_smt.inc"

#endif
