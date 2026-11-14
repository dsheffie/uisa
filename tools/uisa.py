"""uisa: load the micro-ISA spec and expand it into concrete instruction variants.

Every consumer (the generators, the assembler, the disassembler, the tests)
goes through `load()`, so the YAML spec stays the single source of truth.
"""

import os
import re
import yaml

SPEC_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spec", "uisa.yaml")

FLAG_RESOURCE = {"N": "nzvc", "Z": "nzvc", "V": "nzvc", "C": "nzvc", "X": "x"}


class SpecError(Exception):
    pass


class Field:
    def __init__(self, name, hi, lo):
        self.name, self.hi, self.lo = name, hi, lo
        self.width = hi - lo + 1
        self.mask = ((1 << self.width) - 1) << lo

    def put(self, value):
        if value < 0 or value >= (1 << self.width):
            raise SpecError("value %d does not fit field %s (%d bits)" % (value, self.name, self.width))
        return value << self.lo

    def get(self, word):
        return (word >> self.lo) & ((1 << self.width) - 1)


class Format:
    def __init__(self, name, d):
        self.name = name
        self.fields = {f[0]: Field(*f) for f in d["fields"]}
        imm = d.get("imm", {})
        self.imm_signed = imm.get("signed", True)
        self.imm_scale = imm.get("scale", 1)
        covered = 0
        for f in self.fields.values():
            if covered & f.mask:
                raise SpecError("format %s: field %s overlaps" % (name, f.name))
            covered |= f.mask
        if covered != 0xFFFFFFFF:
            raise SpecError("format %s does not cover all 32 bits (%08x)" % (name, covered))


class Variant:
    """One concrete instruction: a mnemonic with fixed encoding bits."""

    def __init__(self, mnemonic, fmt, fixed, operands, **attrs):
        self.mnemonic = mnemonic
        self.fmt = fmt
        self.fixed = fixed            # field name -> value
        self.operands = operands      # assembly operand names, in order
        self.attrs = attrs
        self.mask = 0
        self.match = 0
        for name, value in fixed.items():
            f = fmt.fields[name]
            self.mask |= f.mask
            self.match |= f.put(value)

    @property
    def ident(self):
        return "UISA_" + re.sub(r"[^A-Za-z0-9]", "_", self.mnemonic).upper()

    def __getattr__(self, name):
        try:
            return self.__dict__["attrs"][name]
        except KeyError:
            raise AttributeError(name)


class Spec:
    def __init__(self, d):
        self.d = d
        self.params = d["params"]
        self.formats = {k: Format(k, v) for k, v in d["formats"].items()}
        self.majors = d["majors"]
        if len(set(self.majors.values())) != len(self.majors):
            raise SpecError("duplicate major opcode values")
        for k, v in self.majors.items():
            if not 0 <= v < 64:
                raise SpecError("major %s out of range" % k)
        self.sizes = {int(k): v for k, v in d["sizes"].items()}
        self.size_code = {v: k for k, v in self.sizes.items()}
        self.spaces = d["spaces"]
        self.base_conds = d["base_conds"]
        self.guest_conds = d["guest_conds"]
        self.csrs = d["csrs"]
        self.csr_by_name = {c["name"]: c["num"] for c in self.csrs}
        self.csr_by_num = {c["num"]: c["name"] for c in self.csrs}
        self.regs = self._build_regs(d["reg_classes"])
        self.reg_by_index = {v: k for k, v in self.regs.items()}
        self.variants = []
        self._expand_base(d["base_ops"])
        self._expand_mem(d["mem_ops"])
        self._expand_guest(d["guest_ops"])
        self._check()
        self.by_mnemonic = {v.mnemonic: v for v in self.variants}

    # --- registers -------------------------------------------------------
    @staticmethod
    def _build_regs(classes):
        regs = {}
        for c in classes:
            if "prefix" in c:
                for i in range(c["count"]):
                    regs["%s%d" % (c["prefix"], i)] = c["base"] + i
            else:
                regs[c["name"]] = c["base"]
        if len(set(regs.values())) != len(regs):
            raise SpecError("register namespace overlap")
        return regs

    # --- expansion -------------------------------------------------------
    def _add(self, v):
        self.variants.append(v)

    @staticmethod
    def _flag_resources(flags):
        return sorted({FLAG_RESOURCE.get(f, f) for f in flags})

    def _expand_base(self, ops):
        for op in ops:
            fmt = self.formats[op["fmt"]]
            common = dict(kind="base", unit=op.get("unit", "alu"), sem=op.get("sem"),
                          desc=op.get("desc", ""), note=op.get("note", ""),
                          cti=op.get("cti"), serialize=op.get("serialize", False),
                          tied=op.get("tied", False), reads_rd=op.get("reads_rd", False),
                          store_bytes=op.get("store_bytes"),
                          imm_signed=op.get("imm_signed", fmt.imm_signed),
                          imm_max=op.get("imm_max"),
                          reads_flags=self._flag_resources(op.get("reads_flags", [])),
                          writes_flags=self._flag_resources(op.get("writes_flags", [])),
                          major=op["major"], family=op.get("family", op.get("name")))
            if "per_cond" in op:
                conds = self.guest_conds if op["per_cond"] == "guest" else self.base_conds
                for c in conds:
                    fixed = {"op": self.majors[op["major"]]}
                    if "fn_base" in op:
                        fixed["fn"] = op["fn_base"] + c["code"]
                    else:
                        fixed["cond"] = c["code"]
                    expr = c["expr"]
                    if op["per_cond"] == "guest":
                        expr = re.sub(r"\b([NZVC])\b", lambda m: m.group(1).lower() + "in", expr)
                    sem = op["sem"].replace("COND", "(" + expr + ")")
                    attrs = dict(common, sem=sem, cond=c["name"])
                    self._add(Variant("%s.%s" % (op["family"], c["name"]), fmt, fixed, op["asm"], **attrs))
                continue
            fixed = {"op": self.majors[op["major"]]}
            if "fn" in op:
                fixed["fn"] = op["fn"]
            if "cond" in op:
                fixed["cond"] = op["cond"]
            if "rsv" in fmt.fields:
                fixed["rsv"] = 0
            self._add(Variant(op["name"], fmt, fixed, op["asm"], **common))

    def _expand_mem(self, ops):
        for op in ops:
            wb = op.get("writeback")
            fmt = self.formats["MW" if wb else "M"]
            for sp in self.spaces:
                fixed = {"op": self.majors[op["major"]], "sp": sp["code"]}
                if wb:
                    operands = ["rs2", "mem"]    # data in rs2; rd = rs1 gets the new address
                else:
                    operands = ["rd", "mem"]     # loads: rd is the result; stores: rd carries the data
                attrs = dict(kind=op["kind"], unit="mem", bytes=op["bytes"],
                             signed=op.get("signed", False), locked=op.get("locked"),
                             space=sp["name"], desc=op.get("desc", ""), sem=None, writeback=wb,
                             cti=None, serialize=op.get("locked") is not None, tied=bool(wb),
                             imm_signed=True, imm_max=None, reads_flags=[], writes_flags=[],
                             major=op["major"], family=op["name"])
                self._add(Variant("%s.%s" % (op["name"], sp["suffix"]), fmt, fixed, operands, **attrs))

    def _expand_guest(self, ops):
        fns = set()
        for op in ops:
            if op["fn"] in fns:
                raise SpecError("guest op fn %d reused" % op["fn"])
            fns.add(op["fn"])
            kind = op["kind"]
            sizes = op.get("sizes", [8, 16, 32])
            reads = self._flag_resources(op.get("reads", []))
            writes = self._flag_resources(op.get("writes", []))
            forms = []   # (suffix, major, size, form, immediate)
            for n in sizes:
                if kind in ("binary", "unary"):
                    forms.append(("%d" % n, "GOP", n, "merge" if n < 32 else "full32", False))
                    if n < 32:
                        forms.append(("%dz" % n, "GOPZ", n, "zfill", False))
                elif kind == "compare":
                    forms.append(("%d" % n, "GOP", n, "compare", False))
                elif kind == "full":
                    forms.append(("%d" % n, "GOP", n, "full", False))
                elif kind == "tiedfull":
                    forms.append(("%d" % n, "GOP", n, "tiedfull", False))
                else:
                    raise SpecError("guest op %s: unknown kind %s" % (op["name"], kind))
            if op.get("imm"):
                for (suf, major, n, form, _) in list(forms):
                    forms.append((suf, {"GOP": "GOPI", "GOPZ": "GOPIZ"}[major], n, form, True))
            for (suf, major, n, form, immediate) in forms:
                fmt = self.formats["GI" if immediate else "G"]
                base = op["name"] + ("i" if immediate else "")
                src2 = "imm" if immediate else "rs2"
                if form in ("merge", "tiedfull"):
                    operands = ["rd", src2]
                    tied = True
                elif form == "compare":
                    operands = ["rs1", src2]
                    tied = False
                elif kind == "unary":
                    operands = ["rd", src2]
                    tied = False
                else:
                    operands = ["rd", "rs1", src2]
                    tied = False
                for f in (1, 0):
                    if f == 0 and form == "compare":
                        continue            # a compare without flags does nothing
                    fixed = {"op": self.majors[major if f else major + "N"],
                             "fn": op["fn"], "sz": self.size_code[n]}
                    mnem = "%s.%s%s" % (base, suf, "" if f else ".nf")
                    attrs = dict(kind="guest", unit=op.get("unit", "mul" if op["name"].startswith("mul") else
                                                     "div" if op["name"].startswith("div") else "alu"),
                                 sem=op["sem"], desc=op.get("desc", ""), note=op.get("precondition", ""),
                                 cti=None, serialize=False, tied=tied, gfn=op["fn"], gsize=n,
                                 gform=form, gkind=kind, unary=(kind == "unary"),
                                 imm_signed=False, imm_max=None,
                                 reads_flags=reads if f else [], writes_flags=writes if f else [],
                                 major=major, family=op["name"], flags_on=bool(f))
                    self._add(Variant(mnem, fmt, fixed, operands, **attrs))

    # --- checks ----------------------------------------------------------
    def _check(self):
        seen = {}
        for v in self.variants:
            if v.mnemonic in seen:
                raise SpecError("duplicate mnemonic %s" % v.mnemonic)
            seen[v.mnemonic] = v
        vs = self.variants
        for i in range(len(vs)):
            for j in range(i + 1, len(vs)):
                a, b = vs[i], vs[j]
                common = a.mask & b.mask
                if (a.match & common) == (b.match & common):
                    raise SpecError("encoding overlap: %s and %s" % (a.mnemonic, b.mnemonic))

    # --- operand semantics -------------------------------------------------
    def reads(self, v):
        """Register fields this variant reads."""
        r = []
        ops = v.operands
        if "rs1" in ops or "mem" in ops or v.tied:
            r.append("rs1")
        if "rs2" in ops:
            r.append("rs2")
        if (v.kind == "store" and not v.attrs.get("writeback")) or v.attrs.get("reads_rd"):
            r.append("rd")
        return r

    def writes_rd(self, v):
        if v.kind == "store":
            return bool(v.attrs.get("writeback"))
        if v.kind == "guest":
            return v.gform != "compare"
        return "rd" in v.operands

    # --- encode / decode ------------------------------------------------
    def imm_info(self, v):
        f = v.fmt
        return v.imm_signed if v.fmt.name != "GI" else False, f.imm_scale

    def encode(self, v, values):
        """values: dict of field-level operand values (rd, rs1, rs2, imm, off, wid)."""
        word = v.match
        vals = dict(values)
        if v.attrs.get("writeback"):
            vals["rd"] = vals["rs1"]       # the base register is also the result
        elif v.tied:
            vals["rs1"] = vals["rd"]
        for name, value in vals.items():
            if name == "imm":
                signed, scale = self.imm_info(v)
                if value % scale:
                    raise SpecError("%s: immediate %d not a multiple of %d" % (v.mnemonic, value, scale))
                value //= scale
                f = v.fmt.fields["imm"]
                lo, hi = (-(1 << (f.width - 1)), (1 << (f.width - 1)) - 1) if signed else (0, (1 << f.width) - 1)
                if v.imm_max is not None:
                    hi = min(hi, v.imm_max)
                if not lo <= value <= hi:
                    raise SpecError("%s: immediate %d out of range [%d,%d]" % (v.mnemonic, value * scale, lo * scale, hi * scale))
                word |= f.put(value & ((1 << f.width) - 1))
            else:
                word |= v.fmt.fields[name].put(value)
        return word

    def match(self, word):
        hits = [v for v in self.variants if (word & v.mask) == v.match]
        return hits[0] if hits else None

    def decode(self, word):
        v = self.match(word)
        if v is None:
            return None, {}
        vals = {}
        for name, f in v.fmt.fields.items():
            if name in v.fixed:
                continue
            raw = f.get(word)
            if name == "imm":
                signed, scale = self.imm_info(v)
                if signed and raw >> (f.width - 1):
                    raw -= 1 << f.width
                raw *= scale
            vals[name] = raw
        return v, vals

    # --- assembly text ----------------------------------------------------
    def reg(self, tok):
        tok = tok.strip().lower()
        if tok not in self.regs:
            raise SpecError("unknown register %r" % tok)
        return self.regs[tok]

    @staticmethod
    def num(tok):
        tok = tok.strip().lstrip("#")
        return int(tok, 0)

    def assemble_line(self, line, pc=0, labels=None):
        line = line.split(";")[0].strip()
        if not line:
            return None
        mnem, _, rest = line.partition(" ")
        v = self.by_mnemonic.get(mnem.lower())
        if v is None:
            raise SpecError("unknown mnemonic %r" % mnem)
        toks = [t.strip() for t in rest.split(",")] if rest.strip() else []
        if len(toks) != len(v.operands):
            raise SpecError("%s expects operands %s" % (v.mnemonic, ", ".join(v.operands)))
        vals = {}
        for kind, tok in zip(v.operands, toks):
            if kind in ("rd", "rs1", "rs2"):
                vals[kind] = self.reg(tok)
            elif kind == "mem":
                m = re.match(r"^(.*)\((.+)\)$", tok)
                if not m:
                    raise SpecError("bad memory operand %r" % tok)
                vals["imm"] = self.num(m.group(1) or "0")
                vals["rs1"] = self.reg(m.group(2))
            elif kind == "csr":
                vals["imm"] = self.csr_by_name[tok] if tok in self.csr_by_name else self.num(tok)
            elif kind == "imm":
                if v.cti in ("branch", "jump", "call") and labels is not None and tok in labels:
                    vals["imm"] = labels[tok] - pc
                else:
                    vals["imm"] = self.num(tok)
            elif kind in ("off", "wid"):
                vals[kind] = self.num(tok) & 31
            else:
                raise SpecError("operand kind %s" % kind)
        return self.encode(v, vals)

    def assemble(self, text):
        """Two-pass assembler: one instruction per line, `label:` definitions."""
        lines, labels, pc = [], {}, 0
        for raw in text.splitlines():
            line = raw.split(";")[0].strip()
            while ":" in line.split(" ")[0]:
                lab, _, line = line.partition(":")
                labels[lab.strip()] = pc
                line = line.strip()
            if line:
                lines.append((pc, line))
                pc += 4
        return [self.assemble_line(l, p, labels) for p, l in lines]

    def disassemble(self, word):
        v, vals = self.decode(word)
        if v is None:
            return ".word 0x%08x" % word
        out = []
        for kind in v.operands:
            if kind in ("rd", "rs1", "rs2"):
                out.append(self.reg_by_index.get(vals[kind], "r%d" % vals[kind]))
            elif kind == "mem":
                out.append("%d(%s)" % (vals["imm"], self.reg_by_index.get(vals["rs1"], "r%d" % vals["rs1"])))
            elif kind == "csr":
                out.append(self.csr_by_num.get(vals["imm"], str(vals["imm"])))
            elif kind in ("imm", "off", "wid"):
                out.append(str(vals[kind]))
        return (v.mnemonic + " " + ", ".join(out)).strip()


def load(path=SPEC_PATH):
    with open(path) as f:
        return Spec(yaml.safe_load(f))
