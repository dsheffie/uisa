#!/usr/bin/env python3
"""Random C programs for differential testing of `ucc --program` (csmith-style).

usage: gen_prog.py seed > prog.c

Every program is free of undefined behaviour under the test's host flags
(-fwrapv): division and remainder go through guarded helpers, shift counts
are masked and signed values are never shifted left, array indices are masked
to the array's (power-of-two) length, pointers only ever point at live
objects, loops have constant trip counts and the one recursive function has a
depth argument. Like csmith, main ends by folding every global (scalars,
arrays, struct fields) into a CRC-32 and returns it, so the host build and the
uisa build can be compared by their result alone.

Language used: the 8/16/32/64-bit integer types, structs (scalar, pointer
and array fields; whole-struct copies), sizeof of integer types and arrays, global and local arrays (1-2 dimensions, of integers and structs),
pointers and pointers to pointers, &, *, [], ->, the comma operator,
compound assignment, ++/--, ?:, && || !, casts, if/for/while/do/switch,
break/continue, calls with up to 10 argument words, recursion.
"""

import random
import sys

INT_TYPES = ["u8", "s8", "u16", "s16", "u32", "s32", "u64", "s64"]
BITS = {"u8": 8, "s8": 8, "u16": 16, "s16": 16, "u32": 32, "s32": 32, "u64": 64, "s64": 64}


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.out = []
        self.structs = []        # (name, [(field, type)]) type: int type or "struct S *" / "u32 *"
        self.globals = []        # (name, type, dims) type: int type or "struct Sx"
        self.gptrs = []          # (name, pointee type, level)
        self.funcs = []          # (name, ret, [(ptype, pname)])
        self.depth = 0

    # -------------------------------------------------------------- helpers
    def c(self, p):
        return self.r.random() < p

    def pick(self, xs):
        return self.r.choice(xs)

    def const(self, t):
        b = BITS[t]
        style = self.r.randrange(4)
        if style == 0:
            v = self.r.choice([0, 1, 2, 3, 7, 8, 15, 16, 31, 127, 128, 255])
        elif style == 1:
            v = self.r.getrandbits(min(b, 32))
        else:
            v = self.r.getrandbits(b)
        v &= (1 << b) - 1
        if t in ("u64", "s64"):
            return "0x%xull" % v
        if v < 0x80000000 and v < 1000:
            return "%du" % v if t[0] == "u" else "%d" % v
        return "0x%xu" % (v & 0xffffffff)

    def emit(self, s, ind):
        self.out.append("  " * ind + s)

    # ------------------------------------------------------------ the world
    def make_types(self):
        for k in range(self.r.randint(1, 3)):
            fields = []
            for j in range(self.r.randint(2, 5)):
                fields.append(("f%d" % j, self.pick(INT_TYPES)))
            if self.c(0.5):
                fields.insert(self.r.randrange(len(fields) + 1),
                              ("fa", "%s[%d]" % (self.pick(INT_TYPES), self.pick([1, 2, 3, 4, 8]))))
            if self.c(0.4):
                fields.append(("fp", "u32 *"))
            self.structs.append(("S%d" % k, fields))

    @staticmethod
    def arr_field(ft):
        """(element type, length) of an array field type "T[n]", else None."""
        if not ft.endswith("]"):
            return None
        t, n = ft[:-1].split("[")
        return t, int(n)

    def field_obj(self, scope, f, ft):
        """(".f" or ".f[i]", integer type) for an integer field or an array field's element."""
        a = self.arr_field(ft)
        if a:
            return ".%s[%s]" % (f, self.index(scope, a[1]) if a[1] & (a[1] - 1) == 0 else self.r.randrange(a[1])), a[0]
        return "." + f, ft

    def make_globals(self):
        # the u32 * struct fields point here; declared first
        self.globals.append(("g_u32", "u32", []))
        n = 0
        for k in range(self.r.randint(4, 10)):
            t = self.pick(INT_TYPES)
            self.globals.append(("g_%d" % n, t, []))
            n += 1
        for k in range(self.r.randint(1, 4)):
            t = self.pick(INT_TYPES + ["struct %s" % s[0] for s in self.structs])
            dims = [self.pick([2, 4, 8])] + ([self.pick([2, 4])] if self.c(0.4) else [])
            self.globals.append(("g_%d" % n, t, dims))
            n += 1
        for s in self.structs:
            if self.c(0.7):
                self.globals.append(("g_%d" % n, "struct %s" % s[0], []))
                n += 1
        # pointers to scalar globals (and one pointer to pointer)
        for k in range(self.r.randint(1, 4)):
            tgt = self.pick([g for g in self.globals if not g[1].startswith("struct")])
            self.gptrs.append(("gp_%d" % k, tgt[1], 1))
        if self.gptrs and self.c(0.6):
            self.gptrs.append(("gpp_0", self.gptrs[0][1], 2))

    def struct_fields(self, t):
        name = t.split()[1]
        return [s for s in self.structs if s[0] == name][0][1]

    def init_for(self, t, dims):
        if dims:
            inner = [self.init_for(t, dims[1:]) for _ in range(dims[0] if self.c(0.7) else self.r.randint(1, dims[0]))]
            return "{" + ", ".join(inner) + "}"
        if t.startswith("struct"):
            vals = []
            for f, ft in self.struct_fields(t):
                a = self.arr_field(ft)
                if a:
                    vals.append("{" + ", ".join(self.const(a[0]) for _ in range(self.r.randint(1, a[1]))) + "}")
                else:
                    vals.append("&g_u32" if ft == "u32 *" else self.const(ft))
            return "{" + ", ".join(vals) + "}"
        return self.const(t)

    def addr_of_global(self, t, level):
        """An address expression of type t * (level 1) or t ** (level 2), always valid."""
        if level == 2:
            cands = [p for p in self.gptrs if p[2] == 1 and p[1] == t]
            return "&" + self.pick(cands)[0] if cands else None
        cands = []
        for name, gt, dims in self.globals:
            if gt != t:
                continue
            if not dims:
                cands.append("&" + name)
            else:
                idx = "".join("[%d]" % self.r.randrange(d) for d in dims)
                cands.append("&%s%s" % (name, idx))
        return self.pick(cands) if cands else None

    # ---------------------------------------------------------- expressions
    def lvalue(self, scope, writable=False):
        """(expression, type) of a random integer object; candidates are
        listed cheaply and only the chosen one builds its index expressions."""
        c = []
        for name, t in scope["vars"]:
            if not (writable and name in scope["loopvars"]):
                c.append(lambda name=name, t=t: (name, t))
        for name, t, dims in self.globals:
            if t.startswith("struct"):
                fields = [(f, ft) for f, ft in self.struct_fields(t) if ft != "u32 *"]
                if fields:
                    def g(name=name, t=t, dims=dims, fields=fields):
                        f, ft = self.pick(fields)
                        acc = name + "".join("[%s]" % self.index(scope, d) for d in dims)
                        sel, et = self.field_obj(scope, f, ft)
                        return (acc + sel, et)
                    c.append(g)
            else:
                c.append(lambda name=name, t=t, dims=dims:
                         (name + "".join("[%s]" % self.index(scope, d) for d in dims), t))
        for name, t, level in self.gptrs:
            c.append(lambda name=name, t=t, level=level: ("*" * level + name, t))
        for name, t, n in scope["arrays"]:
            c.append(lambda name=name, t=t, n=n: ("%s[%s]" % (name, self.index(scope, n)), t))
        for name, st in scope["sptrs"]:
            fields = [(f, ft) for f, ft in self.struct_fields(st) if ft != "u32 *"]
            if fields:
                def sp(name=name, fields=fields):
                    sel, et = self.field_obj(scope, *self.pick(fields))
                    return (name + "->" + sel[1:], et)
                c.append(sp)
        for name, t in scope["iptrs"]:
            c.append(lambda name=name, t=t: ("*" + name, t))
        return self.pick(c)()

    def index(self, scope, n):
        if self.c(0.4) or self.depth > 3:
            return str(self.r.randrange(n))
        self.depth += 1
        e = "(%s) & %d" % (self.expr(scope, 1), n - 1)
        self.depth -= 1
        return e

    def expr(self, scope, d):
        if d >= 4 or self.c(0.25):
            if self.c(0.3):
                return self.const(self.pick(INT_TYPES))
            return self.lvalue(scope)[0]
        k = self.r.randrange(15)
        a = lambda: self.expr(scope, d + 1)
        if k <= 3:
            op = self.pick(["+", "-", "*", "&", "|", "^"])
            return "(%s %s %s)" % (a(), op, a())
        if k == 4:
            op = self.pick(["==", "!=", "<", "<=", ">", ">="])
            return "(%s %s %s)" % (a(), op, a())
        if k == 5:
            return "(%s %s %s)" % (a(), self.pick(["&&", "||"]), a())
        if k == 6:
            return "(%s%s)" % (self.pick(["!", "~", "-"]), a())
        if k == 7:
            t = self.pick(INT_TYPES)
            return "((%s)%s)" % (t, a())
        if k == 8:
            fn = self.pick(["sdiv", "udiv", "smod", "umod", "sdiv64", "umod64"])
            return "%s(%s, %s)" % (fn, a(), a())
        if k == 9:
            if self.c(0.5):
                return "(((u32)%s) %s ((%s) & 31))" % (a(), self.pick(["<<", ">>"]), a())
            if self.c(0.5):
                return "(((s32)%s) >> ((%s) & 31))" % (a(), a())
            return "(((u64)%s) %s ((%s) & 63))" % (a(), self.pick(["<<", ">>"]), a())
        if k == 10:
            return "(%s ? %s : %s)" % (a(), a(), a())
        if k == 11 and self.c(0.3):   # layouts agree with the host for these
            arrs = [g for g in self.globals if g[2] and not g[1].startswith("struct")]
            if arrs and self.c(0.5):
                g = self.pick(arrs)
                return "((u32)sizeof(%s%s))" % (g[0], "[0]" * self.r.randint(0, len(g[2])))
            return "((u32)sizeof(%s))" % self.pick(INT_TYPES)
        if k == 12:
            return "(%s, %s)" % (a(), a())
        if k == 13 and self.gptrs:
            p = self.pick([g for g in self.gptrs if g[2] == 1])
            q = self.addr_of_global(p[1], 1)
            return "(%s == %s)" % (p[0], q)
        return self.lvalue(scope)[0]

    def call(self, scope, d):
        self.calls_left -= 1
        name, ret, params = self.pick(scope["callable"])
        args = []
        for pt, pn in params:
            if pt.endswith("*"):
                args.append(self.ptr_arg(scope, pt))
            else:
                args.append(self.expr(scope, d + 2))
        return "%s(%s)" % (name, ", ".join(args))

    def ptr_arg(self, scope, pt):
        base = pt[:-2].strip()
        if base.startswith("struct"):
            cands = [n for n, st in scope["sptrs"] if st == base]
            for name, gt, dims in self.globals:
                if gt == base:
                    cands.append("&" + name + "".join("[%d]" % self.r.randrange(x) for x in dims))
            return self.pick(cands)
        cands = [n for n, t in scope["iptrs"] if t == base]
        cands += ["&" + n for n, t in scope["addr_vars"] if t == base]
        a = self.addr_of_global(base, 1)
        if a:
            cands.append(a)
        for name, t, n in scope["arrays"]:
            if t == base:
                cands.append("&%s[%d]" % (name, self.r.randrange(n)))
        return self.pick(cands)

    # ----------------------------------------------------------- statements
    def stmt(self, scope, ind, d):
        k = self.r.randrange(14 if d < 3 else 8)
        if k <= 3:
            target, t = self.lvalue(scope, True)
            op = self.pick(["=", "=", "+=", "-=", "^=", "|=", "&="])
            self.emit("%s %s %s;" % (target, op, self.expr(scope, 0)), ind)
        elif k == 4:
            target, t = self.lvalue(scope, True)
            self.emit("(%s)%s;" % (target, self.pick(["++", "--"])) if self.c(0.5) else
                      "%s%s;" % (self.pick(["++", "--"]), target), ind)
        elif k == 5 and scope["callable"] and self.calls_left > 0 and not scope.get("inloop"):
            # the result goes to a fresh local: no other operand can observe the call's side effects
            self.emit("{ u64 r = (u64)%s;" % self.call(scope, 1), ind)
            self.emit("crc_byte((u8)r); }", ind + 1)
        elif k == 6 and self.gptrs:
            p = self.pick(self.gptrs)
            a = self.addr_of_global(p[1], p[2])
            if a:
                self.emit("%s = %s;" % (p[0], a), ind)
        elif k == 7 and scope["sptrs"]:
            name, st = self.pick(scope["sptrs"])
            f, ft = self.pick([(f, ft) for f, ft in self.struct_fields(st) if ft != "u32 *"])
            sel = self.field_obj(scope, f, ft)[0]
            src = [g[0] + "".join("[%d]" % self.r.randrange(x) for x in g[2]) for g in self.globals if g[1] == st]
            if src and self.c(0.3):   # a whole-struct copy (array fields included)
                self.emit("*%s = %s;" % (name, self.pick(src)), ind)
            else:
                self.emit("%s->%s %s %s;" % (name, sel[1:], self.pick(["=", "+=", "^="]), self.expr(scope, 1)), ind)
        elif k == 8:
            self.emit("if (%s) {" % self.expr(scope, 0), ind)
            self.block(scope, ind + 1, d + 1)
            if self.c(0.5):
                self.emit("} else {", ind)
                self.block(scope, ind + 1, d + 1)
            self.emit("}", ind)
        elif k == 9:
            v = "i%d" % d
            n = self.r.randint(1, 6)
            self.emit("for (u32 %s = 0; %s < %d; %s++) {" % (v, v, n, v), ind)
            inner = dict(scope)
            inner["vars"] = scope["vars"] + [(v, "u32")]
            inner["loopvars"] = scope["loopvars"] + [v]
            self.block(inner, ind + 1, d + 1, loop=True)
            self.emit("}", ind)
        elif k == 10:
            v = "w%d" % d
            n = self.r.randint(1, 5)
            self.emit("{ u32 %s = %d;" % (v, n), ind)
            self.emit("do {", ind + 1)
            inner = dict(scope)
            inner["loopvars"] = scope["loopvars"] + [v]
            self.block(inner, ind + 2, d + 1, loop=True)
            self.emit("} while (--%s);" % v, ind + 1)
            self.emit("}", ind)
        elif k == 11:
            self.emit("switch ((%s) & 7) {" % self.expr(scope, 1), ind)
            for c in sorted(self.r.sample(range(8), self.r.randint(1, 4))):
                self.emit("case %d:" % c, ind + 1)
                self.block(scope, ind + 2, d + 1, braces=False)
                if self.c(0.7):
                    self.emit("break;", ind + 2)
            if self.c(0.5):
                self.emit("default:", ind + 1)
                self.block(scope, ind + 2, d + 1, braces=False)
            self.emit("}", ind)
        elif k == 12 and scope.get("inloop"):
            self.emit("if (%s) %s;" % (self.expr(scope, 1), self.pick(["break", "continue"])), ind)
        elif k == 13 and scope["arrays"]:
            name, t, n = self.pick(scope["arrays"])
            p = "lp%d" % self.r.randrange(1000)
            self.emit("{ %s *%s = &%s[%d];" % (t, p, name, self.r.randrange(n)), ind)
            self.emit("*%s += %s;" % (p, self.expr(scope, 1)), ind + 1)
            self.emit("}", ind)
        else:
            target, t = self.lvalue(scope, True)
            self.emit("%s = %s;" % (target, self.expr(scope, 0)), ind)

    def block(self, scope, ind, d, loop=False, braces=True):
        inner = dict(scope)
        if loop:
            inner["inloop"] = True
        for _ in range(self.r.randint(1, 4 if d < 2 else 2)):
            self.stmt(inner, ind, d)

    # ------------------------------------------------------------ functions
    def make_function(self, idx, nfuncs):
        name = "func_%d" % idx
        ret = self.pick(INT_TYPES)
        params = []
        # pointer parameters only to types with a global object to point at
        stypes = sorted({g[1] for g in self.globals if g[1].startswith("struct")})
        itypes = sorted({g[1] for g in self.globals if not g[1].startswith("struct")})
        for k in range(self.r.randint(0, 5)):
            kind = self.r.randrange(6)
            if kind == 0 and stypes:
                params.append((self.pick(stypes) + " *", "p_%d" % k))
            elif kind == 1:
                params.append((self.pick(itypes) + " *", "p_%d" % k))
            else:
                params.append((self.pick(INT_TYPES), "p_%d" % k))
        self.funcs.append((name, ret, params))
        return name, ret, params

    def body(self, fn, callable_):
        name, ret, params = fn
        self.calls_left = 2
        self.emit("%s %s(%s) {" % (ret, name, ", ".join("%s %s" % p for p in params) or "void"), 0)
        scope = {"vars": [], "arrays": [], "sptrs": [], "iptrs": [], "addr_vars": [], "callable": callable_,
                 "loopvars": []}
        for pt, pn in params:
            if pt.startswith("struct"):
                scope["sptrs"].append((pn, pt[:-2].strip()))
            elif pt.endswith("*"):
                scope["iptrs"].append((pn, pt[:-2].strip()))
            else:
                scope["vars"].append((pn, pt))
        for k in range(self.r.randint(1, 4)):
            t = self.pick(INT_TYPES)
            self.emit("%s l_%d = %s;" % (t, k, self.const(t)), 1)
            scope["vars"].append(("l_%d" % k, t))
            if self.c(0.3):
                scope["addr_vars"].append(("l_%d" % k, t))
        if self.c(0.6):
            t = self.pick(INT_TYPES)
            n = self.pick([2, 4, 8])
            vals = ", ".join(self.const(t) for _ in range(self.r.randint(1, n)))
            self.emit("%s la[%d] = {%s};" % (t, n, vals), 1)
            scope["arrays"].append(("la", t, n))
        if self.structs and self.c(0.5):
            st = "struct " + self.pick(self.structs)[0]
            self.emit("%s ls = %s;" % (st, self.init_for(st, [])), 1)
            self.emit("%s *lsp = &ls;" % st, 1)
            scope["sptrs"].append(("lsp", st))
        for _ in range(self.r.randint(2, 6)):
            self.stmt(scope, 1, 0)
        self.emit("return %s;" % self.expr(scope, 0), 1)
        self.emit("}", 0)
        self.emit("", 0)

    def program(self):
        self.make_types()
        self.make_globals()
        self.emit("// generated by compiler/prog/gen_prog.py", 0)
        for name, fields in self.structs:
            self.emit("struct %s {" % name, 0)
            for f, ft in fields:
                a = self.arr_field(ft)
                self.emit("%s %s[%d];" % (a[0], f, a[1]) if a else
                          "%s %s;" % (ft, f) if not ft.endswith("*") else "u32 *%s;" % f, 1)
            self.emit("};", 0)
        for name, t, dims in self.globals:
            init = self.init_for(t, dims) if self.c(0.85) else None
            decl = "%s %s%s" % (t, name, "".join("[%d]" % d for d in dims))
            self.emit("%s%s;" % (decl, " = " + init if init else ""), 0)
        for name, t, level in self.gptrs:
            a = self.addr_of_global(t, level)
            self.emit("%s %s%s = %s;" % (t, "*" * level, name, a), 0)
        self.emit("""
u32 crc32_context = 0xffffffffu;
void crc_byte(u8 b) {
  crc32_context ^= b;
  for (u32 k = 0; k < 8; k++) crc32_context = (crc32_context >> 1) ^ (0xedb88320u & (0u - (crc32_context & 1)));
}
void transparent_crc(u64 v) {
  for (u32 k = 0; k < 8; k++) crc_byte((u8)(v >> (8 * k)));
}
s32 sdiv(s32 a, s32 b) { return (b == 0 || (a == (s32)0x80000000u && b == -1)) ? a : a / b; }
s32 smod(s32 a, s32 b) { return (b == 0 || (a == (s32)0x80000000u && b == -1)) ? a : a % b; }
u32 udiv(u32 a, u32 b) { return b == 0 ? a : a / b; }
u32 umod(u32 a, u32 b) { return b == 0 ? a : a % b; }
s64 sdiv64(s64 a, s64 b) { return (b == 0 || (a == (s64)0x8000000000000000ull && b == -1)) ? a : a / b; }
u64 umod64(u64 a, u64 b) { return b == 0 ? a : a % b; }
""", 0)
        n = self.r.randint(2, 7)
        fns = [self.make_function(k, n) for k in range(n)]
        # func_k calls only func_j with j > k (no cycles); rec(depth) recurses
        self.emit("u32 rec(u32 depth, u32 acc) {", 0)
        self.emit("u32 loc[3] = {depth, acc, depth ^ acc};", 1)
        self.emit("if (depth == 0) return acc;", 1)
        self.emit("return rec(depth - 1, acc * 31 + loc[depth % 3]) + loc[0];", 1)
        self.emit("}", 0)
        for k in reversed(range(n)):
            self.body(fns[k], [f for f in fns[k + 1:]])
        self.emit("int main(void) {", 0)
        for name, ret, params in fns[:3]:
            scope = {"vars": [], "arrays": [], "sptrs": [], "iptrs": [], "addr_vars": [], "callable": [], "loopvars": []}
            args = [self.ptr_arg(scope, pt) if pt.endswith("*") else self.const(self.pick(INT_TYPES)) for pt, pn in params]
            self.emit("transparent_crc((u64)%s(%s));" % (name, ", ".join(args)), 1)
        self.emit("transparent_crc(rec(%d, g_u32));" % self.r.randint(0, 12), 1)
        for name, t, dims in self.globals:
            loops = ""
            idx = ""
            for k, d in enumerate(dims):
                self.emit("%sfor (u32 c%d = 0; c%d < %d; c%d++)" % ("  " * k, k, k, d, k), 1)
                idx += "[c%d]" % k
            ind = 1 + len(dims)
            if t.startswith("struct"):
                self.emit("{", ind)
                for f, ft in self.struct_fields(t):
                    a = self.arr_field(ft)
                    if a:
                        for e in range(a[1]):
                            self.emit("transparent_crc((u64)%s%s.%s[%d]);" % (name, idx, f, e), ind + 1)
                    elif ft != "u32 *":
                        self.emit("transparent_crc((u64)%s%s.%s);" % (name, idx, f), ind + 1)
                    else:
                        self.emit("transparent_crc((u64)(%s%s.%s == &g_u32));" % (name, idx, f), ind + 1)
                self.emit("}", ind)
            else:
                self.emit("transparent_crc((u64)%s%s);" % (name, idx), ind)
        for name, t, level in self.gptrs:
            self.emit("transparent_crc((u64)%s);" % ("*" * level + name), 1)
        self.emit("return (int)(crc32_context ^ 0xffffffffu);", 1)
        self.emit("}", 0)
        return "\n".join(self.out) + "\n"


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    sys.stdout.write(Gen(seed).program())


if __name__ == "__main__":
    main()
