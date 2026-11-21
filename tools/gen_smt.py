#!/usr/bin/env python3
"""Generate build/uisa_smt.inc: the uop semantics as symbolic C++ (z3).

Every op's `sem` in spec/uisa.yaml is a small C subset. This translates it
into C++ over usym::Sym (model/uisa_sym.h): C-typed z3 bitvectors whose
operators follow C's conversions. `if`/`else` becomes guarded execution of
both arms (usym::SymGuard), `c ? x : y` becomes ITE(c, x, y), casts and
typed declarations become usym::conv. The output is included by
model/uisa_symsem.h, which supplies the machine context (a, b, r, the flags,
CSR_READ, GREG_WRITE, GSTORE, ...).

Anything outside the subset is an error here, so the semantics stay
provable as the spec grows.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import uisa  # noqa: E402

BUILD = os.path.join(HERE, "..", "build")

TYPES = {"uint32_t": "U32", "unsigned": "U32", "int32_t": "I32", "int": "I32",
         "uint64_t": "U64", "int64_t": "I64", "bool": "B"}
FUNCS = {"CCR_UNPACK": "SYM_CCR_UNPACK", "__builtin_clz": "usym::CLZ32", "__builtin_ctz": "usym::CTZ32",
         "__builtin_popcount": "usym::POPCOUNT32", "__builtin_bswap32": "usym::BSWAP32"}
HELPERS = {"MSK", "MSB", "SX", "ROL32", "CCR_PACK", "BF_OFF", "BF_WID", "BF_MASK", "BF_EXTU", "BF_EXTS",
           "BF_INS", "BF_FFO"}
# provided by the context (model/uisa_symsem.h)
CONTEXT_FUNCS = {"CSR_READ", "CSR_WRITE", "GREG_READ", "GREG_WRITE", "CSTACK_PUSH", "CSTACK_POP",
                 "GNPC_SET", "GSTORE", "MMU_FLUSH", "CACHE_OP"}
TABLES = {"CSR_WMASK"}


class SemError(Exception):
    pass


TOKEN = re.compile(r"\s*(?:(0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*)|([A-Za-z_]\w*)|"
                   r"(<<=|>>=|\+=|-=|\|=|&=|\^=|\*=|==|!=|<=|>=|&&|\|\||<<|>>|[-+*/%&|^~!<>=?:;,(){}\[\]]))")


def tokenize(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    toks, pos = [], 0
    while pos < len(src):
        if src[pos:].strip() == "":
            break
        m = TOKEN.match(src, pos)
        if not m:
            raise SemError("cannot tokenize at %r" % src[pos:pos + 20])
        toks.append(m.group(m.lastindex))
        pos = m.end()
    return toks


BINARY = [("||",), ("&&",), ("|",), ("^",), ("&",), ("==", "!="), ("<", "<=", ">", ">="), ("<<", ">>"),
          ("+", "-"), ("*", "/", "%")]


class Parser:
    def __init__(self, toks):
        self.t, self.i, self.nguard = toks, 0, 0

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else None

    def take(self, want=None):
        tok = self.peek()
        if tok is None or (want is not None and tok != want):
            raise SemError("expected %r, got %r" % (want, tok))
        self.i += 1
        return tok

    # ---- expressions: return C++ text ----
    def expr(self):
        c = self.binary(0)
        if self.peek() == "?":
            self.take("?")
            x = self.expr()
            self.take(":")
            y = self.expr()
            return "usym::ITE(usym::Sym(%s), %s, %s)" % (c, x, y)
        return c

    def binary(self, level):
        if level == len(BINARY):
            return self.unary()
        left = self.binary(level + 1)
        while self.peek() in BINARY[level]:
            op = self.take()
            right = self.binary(level + 1)
            left = "(%s %s %s)" % (left, op, right)
        return left

    def unary(self):
        tok = self.peek()
        if tok in ("!", "~", "-", "+"):
            self.take()
            return "(%s%s)" % (tok, self.unary())
        if tok == "(" and self.peek(1) in TYPES and self.peek(2) == ")":
            self.take("(")
            ty = TYPES[self.take()]
            self.take(")")
            return "usym::conv(usym::Sym(%s), usym::%s)" % (self.unary(), ty)
        return self.postfix()

    def postfix(self):
        tok = self.take()
        if tok == "(":
            e = self.expr()
            self.take(")")
            return "(%s)" % e
        if re.match(r"^\d|^0[xX]", tok):
            return tok
        if not re.match(r"^[A-Za-z_]\w*$", tok):
            raise SemError("unexpected %r" % tok)
        if self.peek() == "(":
            self.take("(")
            args = []
            while self.peek() != ")":
                args.append(self.expr())
                if self.peek() == ",":
                    self.take(",")
            self.take(")")
            if tok in FUNCS:
                name = FUNCS[tok]
            elif tok in HELPERS:
                name = "usym::" + tok
            elif tok in CONTEXT_FUNCS:
                name = tok
            else:
                raise SemError("unknown function %s" % tok)
            return "%s(%s)" % (name, ", ".join(args))
        if self.peek() == "[":
            if tok not in TABLES:
                raise SemError("unknown table %s" % tok)
            self.take("[")
            idx = self.expr()
            self.take("]")
            return "usym::index(uisa::%s, sizeof uisa::%s / sizeof uisa::%s[0], %s)" % (tok, tok, tok, idx)
        return tok

    # ---- statements: return lists of C++ lines ----
    def stmts(self):
        out = []
        while self.peek() is not None and self.peek() != "}":
            out += self.stmt()
        return out

    def stmt(self):
        tok = self.peek()
        if tok == "{":
            self.take("{")
            body = self.stmts()
            self.take("}")
            return ["{"] + body + ["}"]
        if tok == "if":
            self.take("if")
            self.take("(")
            c = self.expr()
            self.take(")")
            then = self.stmt()
            els = []
            if self.peek() == "else":
                self.take("else")
                els = self.stmt()
            g = "_g%d" % self.nguard
            self.nguard += 1
            return (["{", "usym::SymGuard %s{usym::Sym(%s)};" % (g, c)] + then +
                    ["%s.flip();" % g] + els + ["}"])
        if tok in TYPES:
            ty = TYPES[self.take()]
            out = []
            while True:
                name = self.take()
                self.take("=")
                out.append("usym::Sym %s = usym::conv(usym::Sym(%s), usym::%s);" % (name, self.expr(), ty))
                if self.peek() == ",":
                    self.take(",")
                    continue
                self.take(";")
                return out
        if re.match(r"^[A-Za-z_]\w*$", tok or "") and self.peek(1) in ("=", "+=", "-=", "|=", "&=", "^=", "*=",
                                                                        "<<=", ">>="):
            name = self.take()
            op = self.take()
            e = self.expr()
            self.take(";")
            if op == "=":
                return ["%s = %s;" % (name, e)]
            return ["%s = (%s %s (%s));" % (name, name, op[:-1], e)]
        e = self.expr()
        self.take(";")
        return ["%s;" % e]


def translate(sem):
    p = Parser(tokenize(sem))
    out = p.stmts()
    if p.peek() is not None:
        raise SemError("trailing %r" % p.peek())
    return out


def main():
    spec = uisa.load()
    o = ["// Generated by tools/gen_smt.py from spec/uisa.yaml. Do not edit.",
         "// Symbolic uop semantics; included by model/uisa_symsem.h.", ""]
    o.append("inline bool SymSem::base(unsigned op) {")
    o.append("  switch (op) {")
    nbase = 0
    for v in spec.variants:
        if v.kind != "base" or not v.sem:
            continue
        try:
            body = translate(v.sem)
        except SemError as e:
            raise SystemExit("gen_smt: %s: %s" % (v.mnemonic, e))
        o.append("  case uisa::%s: {" % v.ident)
        o += ["    " + ln for ln in body]
        o.append("    return true; }")
        nbase += 1
    o += ["  default: return false;", "  }", "}", ""]

    o.append("inline void sym_gop_eval(unsigned fn, unsigned n, const usym::Sym &a, const usym::Sym &b,")
    o.append("                         const usym::Sym &xin, const usym::Sym &nin, const usym::Sym &zin,")
    o.append("                         const usym::Sym &vin, const usym::Sym &cin, SymGop &o) {")
    o.append("  o.r = usym::Sym(0u); o.N = nin; o.Z = zin; o.V = vin; o.C = cin; o.X = xin; o.T = usym::Sym(0u);")
    o.append("  const uint32_t m = n >= 32 ? 0xffffffffu : ((1u << n) - 1); (void)m;")
    o.append("  usym::Sym &r = o.r, &N = o.N, &Z = o.Z, &V = o.V, &C = o.C, &X = o.X, &T = o.T;")
    o.append("  (void)r; (void)N; (void)Z; (void)V; (void)C; (void)X; (void)T;")
    o.append("  switch (fn) {")
    nguest = 0
    for op in spec.d["guest_ops"]:
        try:
            body = translate(op["sem"])
        except SemError as e:
            raise SystemExit("gen_smt: guest %s: %s" % (op["name"], e))
        o.append("  case %d: { // %s" % (op["fn"], op["name"]))
        o += ["    " + ln for ln in body]
        o.append("    break; }")
        nguest += 1
    o += ["  default: break;", "  }", "}", ""]
    # the probe success rule (spec probe_ok), which the .us/.ua probes write to u
    p = Parser(tokenize(spec.d["probe_ok"]))
    try:
        ok = p.expr()
        if p.peek() is not None:
            raise SemError("trailing %r" % p.peek())
    except SemError as e:
        raise SystemExit("gen_smt: probe_ok: %s" % e)
    o.append("// spec probe_ok: the access a probe translated (MMUSR mmusr) would succeed")
    o.append("inline usym::Sym sym_probe_ok(const usym::Sym &mmusr, const usym::Sym &w, const usym::Sym &user, const usym::Sym &tce) {")
    o.append("  (void)w; (void)user; (void)tce;")
    o.append("  return usym::conv(usym::Sym(%s), usym::B);" % ok)
    o.append("}")
    o.append("")
    os.makedirs(BUILD, exist_ok=True)
    with open(os.path.join(BUILD, "uisa_smt.inc"), "w") as f:
        f.write("\n".join(o) + "\n")
    print("uisa_smt.inc: %d base ops, %d guest op families" % (nbase, nguest))


if __name__ == "__main__":
    main()
