#!/usr/bin/env python3
"""Generate build/uisa_gop.sv: the guest-op semantics as SystemVerilog.

Every guest op's `sem` in spec/uisa.yaml is the small C subset that
tools/gen_smt.py parses (its tokenizer and grammar are reused here). This
translates it into a combinational SystemVerilog function per family and
operand width (n is a constant in each, so shifts and the `% n` / `% (n + 1)`
rotates by n are by constants), with C's integer semantics made explicit:
every value carries its C type (bool, int, unsigned, int64_t, uint64_t, as
model/uisa_sym.h), the integer promotions and usual arithmetic conversions
pick each operator's width and signedness, and every result is cast to that
width so SystemVerilog's context-dependent widths never widen a C
expression. `if`/`else` stays procedural.

The package (uisa_gop_pkg) provides:
  uisa_gop_<family>_<n>(a, b, xin, nin, zin, vin, cin)   one family at width n
  uisa_gop_eval(fn, sz, ...)    every family (sz: the spec's size code)
  uisa_gop_alu(fn, sz, ...)     the families that run in the ALU (unit alu);
                                the multiply / divide families (unit mul /
                                div) are left to those units
  uisa_gop_reads_nzvc / uisa_gop_reads_x (fn, sz, flags_on)
                                which renamed flag resources a variant reads,
                                derived from the semantics: a flag input is
                                read if the result (or, with flags_on, a flag
                                the variant writes) depends on it, and a flag
                                output that some path leaves unassigned keeps
                                (so reads) its input
  uisa_guest_cond(cond, n, z, v, c)   the guest conditions (spec guest_conds)
Each returns uisa_gop_out_t {r, N, Z, V, C, X, T} as gop_eval in uisa.h does:
flags a family does not assign keep their inputs; T != 0 is a trap vector.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import uisa  # noqa: E402
from gen_smt import BINARY, TYPES, SemError, tokenize  # noqa: E402

BUILD = os.path.join(HERE, "..", "build")

WIDTH = {"B": 1, "I32": 32, "U32": 32, "I64": 64, "U64": 64}
SIGNED = {"I32", "I64"}
FLAG_IN = ("xin", "nin", "zin", "vin", "cin")
FLAG_OUT = ("N", "Z", "V", "C", "X")
HELPERS = {"MSK": (["U32"], "U32"), "MSB": (["U64", "U32"], "B"), "SX": (["U64", "U32"], "I64")}


# ---- parsing: gen_smt's grammar, building a tree instead of C++ text -------
class Parser:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else None

    def take(self, want=None):
        tok = self.peek()
        if tok is None or (want is not None and tok != want):
            raise SemError("expected %r, got %r" % (want, tok))
        self.i += 1
        return tok

    def expr(self):
        c = self.binary(0)
        if self.peek() == "?":
            self.take("?")
            x = self.expr()
            self.take(":")
            return ("tern", c, x, self.expr())
        return c

    def binary(self, level):
        if level == len(BINARY):
            return self.unary()
        left = self.binary(level + 1)
        while self.peek() in BINARY[level]:
            op = self.take()
            left = ("bin", op, left, self.binary(level + 1))
        return left

    def unary(self):
        tok = self.peek()
        if tok in ("!", "~", "-", "+"):
            self.take()
            return ("un", tok, self.unary())
        if tok == "(" and self.peek(1) in TYPES and self.peek(2) == ")":
            self.take("(")
            ty = TYPES[self.take()]
            self.take(")")
            return ("cast", ty, self.unary())
        return self.postfix()

    def postfix(self):
        tok = self.take()
        if tok == "(":
            e = self.expr()
            self.take(")")
            return e
        if re.match(r"^\d|^0[xX]", tok):
            return ("num", tok)
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
            if tok not in HELPERS:
                raise SemError("unknown function %s" % tok)
            return ("call", tok, args)
        return ("id", tok)

    def stmts(self):
        out = []
        while self.peek() is not None and self.peek() != "}":
            out.append(self.stmt())
        return out

    def stmt(self):
        tok = self.peek()
        if tok == "{":
            self.take("{")
            body = self.stmts()
            self.take("}")
            return ("block", body)
        if tok == "if":
            self.take("if")
            self.take("(")
            c = self.expr()
            self.take(")")
            then = self.stmt()
            els = None
            if self.peek() == "else":
                self.take("else")
                els = self.stmt()
            return ("if", c, then, els)
        if tok in TYPES:
            ty = TYPES[self.take()]
            decls = []
            while True:
                name = self.take()
                self.take("=")
                decls.append((name, self.expr()))
                if self.peek() == ",":
                    self.take(",")
                    continue
                self.take(";")
                return ("decl", ty, decls)
        if re.match(r"^[A-Za-z_]\w*$", tok or "") and self.peek(1) in ("=", "+=", "-=", "|=", "&=", "^=", "*=",
                                                                        "<<=", ">>="):
            name = self.take()
            op = self.take()
            e = self.expr()
            self.take(";")
            if op != "=":
                e = ("bin", op[:-1], ("id", name), e)
            return ("assign", name, e)
        raise SemError("unsupported statement at %r" % tok)


def parse(sem):
    p = Parser(tokenize(sem))
    out = p.stmts()
    if p.peek() is not None:
        raise SemError("trailing %r" % p.peek())
    return out


# ---- C types ----------------------------------------------------------------
def promote(t):
    return "I32" if t == "B" else t


def common(a, b):
    a, b = promote(a), promote(b)
    if WIDTH[a] == WIDTH[b]:
        if a in SIGNED and b in SIGNED:
            return a
        return "U64" if WIDTH[a] == 64 else "U32"
    return a if WIDTH[a] == 64 else b


def literal_type(tok):
    m = re.match(r"^(0[xX][0-9a-fA-F]+|\d+)([uUlL]*)$", tok)
    text, suf = m.group(1), m.group(2).lower()
    v = int(text, 0)
    hexa = text.lower().startswith("0x")
    unsigned, long_ = "u" in suf, "l" in suf
    if unsigned:
        ty = "U64" if (long_ or v > 0xffffffff) else "U32"
    elif long_:
        ty = "I64" if v <= 0x7fffffffffffffff else "U64"
    elif v <= 0x7fffffff:
        ty = "I32"
    elif hexa and v <= 0xffffffff:
        ty = "U32"
    else:
        ty = "I64" if v <= 0x7fffffffffffffff else "U64"
    return v, ty


class E:
    """A translated expression: SystemVerilog text of exactly WIDTH[ty] bits
    (an identifier, a sized literal, a size cast or a parenthesized
    compare / logic op), and its C type."""

    def __init__(self, t, ty):
        self.t, self.ty = t, ty


def lit(v, ty):
    w = WIDTH[ty]
    return E("%d'h%x" % (w, v & ((1 << w) - 1)), ty)


def conv(e, to):
    if e.ty == to:
        return e
    wf, wt = WIDTH[e.ty], WIDTH[to]
    if to == "B":
        return E("(%s != %d'd0)" % (e.t, wf), "B")
    if e.ty == "B":
        return E("{%d'd0, %s}" % (wt - 1, e.t), to)
    if wt > wf:
        return E("%d'(%s(%s))" % (wt, "$signed" if e.ty in SIGNED else "$unsigned", e.t), to)
    if wt < wf:
        return E("%d'(%s)" % (wt, e.t), to)
    return E(e.t, to)


def sgn(e, signed):
    return "%s(%s)" % ("$signed" if signed else "$unsigned", e.t)


# ---- translation of one function --------------------------------------------
class Func:
    def __init__(self, env):
        self.env = dict(env)        # name -> C type
        self.locals = {}            # declared name -> C type
        self.lines = []

    def name(self, n):
        return ("l_" + n) if n in self.locals else n

    def expr(self, x):
        k = x[0]
        if k == "num":
            v, ty = literal_type(x[1])
            return lit(v, ty)
        if k == "id":
            if x[1] not in self.env:
                raise SemError("unknown identifier %s" % x[1])
            return E(self.name(x[1]), self.env[x[1]])
        if k == "cast":
            return conv(self.expr(x[2]), x[1])
        if k == "call":
            argt, rty = HELPERS[x[1]]
            if len(x[2]) != len(argt):
                raise SemError("%s takes %d arguments" % (x[1], len(argt)))
            args = [conv(self.expr(a), t).t for a, t in zip(x[2], argt)]
            return E("uisa_%s(%s)" % (x[1].lower(), ", ".join(args)), rty)
        if k == "un":
            op, e = x[1], self.expr(x[2])
            if op == "!":
                return E("(%s == %d'd0)" % (e.t, WIDTH[e.ty]), "B")
            ty = promote(e.ty)
            e = conv(e, ty)
            w = WIDTH[ty]
            if op == "+":
                return e
            if op == "~":
                return E("%d'(~%s)" % (w, e.t), ty)
            return E("%d'(%d'd0 - %s)" % (w, w, e.t), ty)
        if k == "tern":
            c = conv(self.expr(x[1]), "B")
            a, b = self.expr(x[2]), self.expr(x[3])
            ty = "B" if (a.ty == "B" and b.ty == "B") else common(a.ty, b.ty)
            return E("(%s ? %s : %s)" % (c.t, conv(a, ty).t, conv(b, ty).t), ty)
        if k == "bin":
            return self.binop(x[1], self.expr(x[2]), self.expr(x[3]))
        raise SemError("bad expression %r" % (x,))

    def binop(self, op, l, r):
        if op in ("&&", "||"):
            return E("(%s %s %s)" % (conv(l, "B").t, op, conv(r, "B").t), "B")
        if op in ("<<", ">>"):
            ty = promote(l.ty)
            w = WIDTH[ty]
            a = conv(l, ty)
            k = conv(r, "U64" if w == 64 else "U32")
            if op == "<<":
                return E("%d'(%s << %s)" % (w, a.t, k.t), ty)
            if ty in SIGNED:
                return E("%d'(%s >>> %s)" % (w, sgn(a, True), k.t), ty)
            return E("%d'(%s >> %s)" % (w, sgn(a, False), k.t), ty)
        ty = common(l.ty, r.ty)
        w = WIDTH[ty]
        a, b = conv(l, ty), conv(r, ty)
        s = ty in SIGNED
        if op in ("==", "!="):
            return E("(%s %s %s)" % (a.t, op, b.t), "B")
        if op in ("<", "<=", ">", ">="):
            return E("(%s %s %s)" % (sgn(a, s), op, sgn(b, s)), "B")
        if op in ("/", "%"):
            return E("%d'(%s %s %s)" % (w, sgn(a, s), op, sgn(b, s)), ty)
        if op in ("+", "-", "*", "&", "|", "^"):
            return E("%d'(%s %s %s)" % (w, a.t, op, b.t), ty)
        raise SemError("operator %s" % op)

    def stmts(self, body, ind):
        for s in body:
            self.stmt(s, ind)

    def stmt(self, s, ind):
        pad = "   " * ind
        k = s[0]
        if k == "block":
            self.lines.append(pad + "begin")
            self.stmts(s[1], ind + 1)
            self.lines.append(pad + "end")
        elif k == "if":
            c = conv(self.expr(s[1]), "B")
            self.lines.append(pad + "if(%s)" % c.t)
            self.lines.append(pad + "  begin")
            self.stmt(s[2], ind + 1)
            self.lines.append(pad + "  end")
            if s[3] is not None:
                self.lines.append(pad + "else")
                self.lines.append(pad + "  begin")
                self.stmt(s[3], ind + 1)
                self.lines.append(pad + "  end")
        elif k == "decl":
            for name, e in s[2]:
                v = conv(self.expr(e), s[1])   # the initializer cannot see the new name
                if name in self.locals and self.locals[name] != s[1]:
                    raise SemError("%s redeclared with another type" % name)
                if name in self.env and name not in self.locals:
                    raise SemError("%s shadows an input" % name)
                self.locals[name] = s[1]
                self.env[name] = s[1]
                self.lines.append(pad + "%s = %s;" % (self.name(name), v.t))
        elif k == "assign":
            if s[1] not in self.env or s[1] in FLAG_IN + ("a", "b", "n", "m"):
                raise SemError("cannot assign %s" % s[1])
            v = conv(self.expr(s[2]), self.env[s[1]])
            self.lines.append(pad + "%s = %s;" % (self.name(s[1]), v.t))
        else:
            raise SemError("bad statement %r" % (s,))


# ---- which flag inputs a variant reads ---------------------------------------
def ids(x, out):
    if x[0] == "id":
        out.add(x[1])
    elif x[0] in ("un", "cast"):
        ids(x[2], out)
    elif x[0] == "bin":
        ids(x[2], out)
        ids(x[3], out)
    elif x[0] == "tern":
        for y in x[1:]:
            ids(y, out)
    elif x[0] == "call":
        for y in x[2]:
            ids(y, out)
    return out


def taint(body, env, ctl=frozenset()):
    """env: name -> the flag inputs its value may depend on (data and control
    dependence). Returns the env after body."""
    for s in body:
        k = s[0]
        if k == "block":
            env = taint(s[1], env, ctl)
        elif k == "if":
            c = set(ctl)
            for n in ids(s[1], set()):
                c |= env.get(n, set())
            c = frozenset(c)
            t = taint([s[2]], dict(env), c)
            e = taint([s[3]], dict(env), c) if s[3] is not None else dict(env)
            env = {n: t.get(n, set()) | e.get(n, set()) for n in set(t) | set(e)}
        elif k in ("decl", "assign"):
            pairs = s[2] if k == "decl" else [(s[1], s[2])]
            for name, e in pairs:
                d = set(ctl)
                for n in ids(e, set()):
                    d |= env.get(n, set())
                env = dict(env)
                env[name] = d
    return env


def flag_reads(body, writes_x):
    env = {f: {f} for f in FLAG_IN}
    for f, i in zip(FLAG_OUT, ("nin", "zin", "vin", "cin", "xin")):
        env[f] = {i}
    env["r"], env["T"] = set(), set()
    env = taint(body, env)
    nf = env["r"] | env["T"]
    on = nf | env["N"] | env["Z"] | env["V"] | env["C"] | (env["X"] if writes_x else set())
    def res(s):
        return int(bool(s & {"nin", "zin", "vin", "cin"})), int("xin" in s)
    return res(nf), res(on)


# ---- output -------------------------------------------------------------------
def gen_family(op, n):
    body = parse(op["sem"])
    env = {"a": "U32", "b": "U32", "n": "U32", "m": "U32", "r": "U32", "T": "U32"}
    env.update({f: "B" for f in FLAG_IN + FLAG_OUT})
    f = Func(env)
    f.stmts(body, 3)
    fname = "uisa_gop_%s_%d" % (op["name"], n)
    o = ["   /* %s, n = %d */" % (op["name"], n),
         "   function automatic uisa_gop_out_t %s(input logic [31:0] a, input logic [31:0] b," % fname,
         "      input logic xin, input logic nin, input logic zin, input logic vin, input logic cin);",
         "      logic [31:0] n, m, r, T;",
         "      logic N, Z, V, C, X;"]
    for name, ty in f.locals.items():
        o.append("      logic %s%s;" % ("" if ty == "B" else "[%d:0] " % (WIDTH[ty] - 1), "l_" + name))
    o += ["      begin",
          "         n = 32'd%d;" % n,
          "         m = 32'h%x;" % ((1 << n) - 1),
          "         r = 32'd0; N = nin; Z = zin; V = vin; C = cin; X = xin; T = 32'd0;"]
    o += f.lines
    o += ["         %s = '{r: r, N: N, Z: Z, V: V, C: C, X: X, T: T};" % fname,
          "      end",
          "   endfunction", ""]
    return fname, o


def cond_expr(expr):
    body = parse("r = %s;" % expr)
    f = Func({"r": "B", "N": "B", "Z": "B", "V": "B", "C": "B"})
    f.stmts(body, 0)
    rhs = f.lines[0][len("r = "):-1]
    for fl in "NZVC":
        rhs = re.sub(r"\b%s\b" % fl, fl.lower() + "in", rhs)
    return rhs


def main():
    spec = uisa.load()
    units, sizes = {}, {}
    for v in spec.variants:
        if v.kind == "guest":
            units[v.family] = v.unit
            sizes.setdefault(v.family, set()).add(v.gsize)
    o = ["// Generated by tools/gen_sv.py from spec/uisa.yaml. Do not edit.",
         "// The guest-op semantics (spec guest_ops `sem`) as SystemVerilog; see the",
         "// generator for the conventions.",
         "package uisa_gop_pkg;",
         "",
         "   typedef struct packed {",
         "      logic [31:0] r;",
         "      logic        N, Z, V, C, X;",
         "      logic [31:0] T;   /* a trap vector, 0 for none */",
         "   } uisa_gop_out_t;",
         "",
         "   /* uisa.h's helpers, with their C argument types */",
         "   function automatic logic [31:0] uisa_msk(input logic [31:0] n);",
         "      uisa_msk = (n >= 32'd32) ? 32'hffffffff : 32'((32'd1 << n) - 32'd1);",
         "   endfunction",
         "   function automatic logic uisa_msb(input logic [63:0] v, input logic [31:0] n);",
         "      uisa_msb = 1'(v >> (n - 32'd1));",
         "   endfunction",
         "   function automatic logic [63:0] uisa_sx(input logic [63:0] v, input logic [31:0] n);",
         "      uisa_sx = 64'($signed(v << (32'd64 - n)) >>> (32'd64 - n));",
         "   endfunction",
         ""]
    cases_all, cases_alu, reads_nzvc, reads_x = [], [], [], []
    nfam = 0
    for op in spec.d["guest_ops"]:
        nfam += 1
        writes_x = "X" in op.get("writes", [])
        for n in sorted(sizes[op["name"]]):
            try:
                fname, lines = gen_family(op, n)
                (nf_nzvc, nf_x), (on_nzvc, on_x) = flag_reads(parse(op["sem"]), writes_x)
            except SemError as e:
                raise SystemExit("gen_sv: guest %s: %s" % (op["name"], e))
            o += lines
            key = "{6'd%d, 2'd%d}" % (op["fn"], spec.size_code[n])
            call = "%s(a, b, xin, nin, zin, vin, cin)" % fname
            cases_all.append("        %s: o = %s;" % (key, call))
            if units[op["name"]] == "alu":
                cases_alu.append("        %s: o = %s;" % (key, call))
            reads_nzvc.append("        %s: y = flags_on ? 1'b%d : 1'b%d;" % (key, on_nzvc, nf_nzvc))
            reads_x.append("        %s: y = flags_on ? 1'b%d : 1'b%d;" % (key, on_x, nf_x))

    def dispatch(name, cases, what):
        return (["   /* %s */" % what,
                 "   function automatic uisa_gop_out_t %s(input logic [5:0] fn, input logic [1:0] sz," % name,
                 "      input logic [31:0] a, input logic [31:0] b,",
                 "      input logic xin, input logic nin, input logic zin, input logic vin, input logic cin);",
                 "      uisa_gop_out_t o;",
                 "      begin",
                 "         o = '0;",
                 "         case({fn, sz})"] + cases +
                ["           default: o = '0;",
                 "         endcase",
                 "         %s = o;" % name,
                 "      end",
                 "   endfunction", ""])

    def table(name, cases, what):
        return (["   /* %s */" % what,
                 "   function automatic logic %s(input logic [5:0] fn, input logic [1:0] sz, input logic flags_on);" % name,
                 "      logic y;",
                 "      begin",
                 "         case({fn, sz})"] + cases +
                ["           default: y = 1'b0;",
                 "         endcase",
                 "         %s = y;" % name,
                 "      end",
                 "   endfunction", ""])

    o += dispatch("uisa_gop_eval", cases_all, "every guest-op family, by fn and size code")
    o += dispatch("uisa_gop_alu", cases_alu, "the families that execute in the ALU (unit alu)")
    o += table("uisa_gop_reads_nzvc", reads_nzvc, "the variant reads the renamed NZVC")
    o += table("uisa_gop_reads_x", reads_x, "the variant reads the renamed X")
    o += ["   /* the guest conditions on N, Z, V, C (spec guest_conds) */",
          "   function automatic logic uisa_guest_cond(input logic [3:0] cond, input logic nin, input logic zin,",
          "      input logic vin, input logic cin);",
          "      logic y;",
          "      begin",
          "         case(cond)"]
    for c in spec.guest_conds:
        o.append("           4'd%d: y = %s;   /* %s */" % (c["code"], cond_expr(c["expr"]), c["name"]))
    o += ["           default: y = 1'b0;",
          "         endcase",
          "         uisa_guest_cond = y;",
          "      end",
          "   endfunction", "",
          "endpackage // uisa_gop_pkg"]
    os.makedirs(BUILD, exist_ok=True)
    with open(os.path.join(BUILD, "uisa_gop.sv"), "w") as f:
        f.write("\n".join(o) + "\n")
    print("uisa_gop.sv: %d guest op families, %d family/size functions (%d in the ALU)" %
          (nfam, len(cases_all), len(cases_alu)))


if __name__ == "__main__":
    main()
