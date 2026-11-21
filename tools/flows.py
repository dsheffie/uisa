#!/usr/bin/env python3
"""Expand 68k opwords into uisa micro-op flows (spec/flows.yaml).

  flows.py show <opword-hex>...         print the flow for each opword
  flows.py check                        expand every opword, assemble every uop
  flows.py report <ophist> [-o out.md]  coverage and uops/instruction for a histogram

A flow's uops are uisa assembly lines. Values that come from extension words
are left as runtime references, `{kind@offset}`, where offset is the byte
offset of the extension word from the instruction's address:

  {d16@k} {d8@k} {abs.W@k} {abs.L@k}   addressing-mode displacements/addresses
  {imm.B@k} {imm.W@k} {imm.L@k}          immediates
  {br.W@k} {br.L@k}                      branch displacement + 2 (relative to the instruction)
  {pc16@k} {pc8@k}                       PC-relative: k + displacement (16-bit, or the brief format's 8-bit)
  {xn@k}                                 index register named by a brief extension word

Bitfield flows (selected by a second-level decode of the bitfield extension
word, see `bitfield:` in flows.yaml) also use:

  {bfb@k}                                offset / 8 from the bitfield extension word
  {bfm1@k}..{bfm4@k} {bfr@k} {bfi@k}     packed field descriptors (memory n bytes,
                                         register, low bits of a register)
  {bfw@k}                                the width operand of a BFI-format op; the
                                         descriptor before it already carries it
  {dx@k}                                 the data register in bits 14:12
  {rn@k}                                 the general register in bits 15:12 (MOVEC)
  {dh@k}                                 the data register in bits 2:0 (MULx.L Dh)
  {kind@k|bf}                            the reference plus offset / 8
  ...+N                                  any reference plus a constant
"""

import argparse
import collections
import os
import re
import sys

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import m68kdec  # noqa: E402
import uisa  # noqa: E402

FLOWS_PATH = os.path.join(HERE, "..", "spec", "flows.yaml")
CC_NAMES = [c.lower() for c in m68kdec.CC]
SIZE_BITS = {"B": 8, "W": 16, "L": 32}
SAMPLE = {"d16": "8", "d8": "4", "abs.W": "64", "abs.L": "64", "imm.B": "5", "imm.W": "5",
          "imm.L": "5", "br.W": "6", "br.L": "6", "pc16": "6", "pc8": "6", "xn": "d1",
          "bfb": "0", "bfm1": "8", "bfm2": "8", "bfm3": "8", "bfm4": "8", "bfr": "8", "bfi": "8",
          "bfw": "8", "dx": "d1", "rn": "a1", "dh": "d2"}
RUNTIME = re.compile(r"\{([A-Za-z0-9.]+)@(\d+)(?:\|bf)?\}(?:\+\d+)?")
BF_OPS = ["BFTST", "BFEXTU", "BFCHG", "BFEXTS", "BFCLR", "BFFFO", "BFSET", "BFINS"]
BF_MODES = {2: "(An)", 5: "(d16,An)", 6: "(d8,An,Xn)"}
BF_MODE7 = {0: "abs.W", 1: "abs.L", 2: "(d16,PC)", 3: "(d8,PC,Xn)"}


class Flow:
    def __init__(self, kind, uops=(), note=""):
        self.kind, self.uops, self.note = kind, list(uops), note

    @property
    def count(self):
        return len(self.uops)


def load_flows(path=FLOWS_PATH):
    with open(path) as f:
        return yaml.safe_load(f)


def sext8(v):
    return v - 256 if v & 0x80 else v


class Expander:
    def __init__(self, flows=None):
        self.f = flows or load_flows()
        self.t = self.f["temps"]
        self.millicode = set(self.f["millicode"])

    # --- operand roles ------------------------------------------------------
    def roles(self, op, ins):
        """Return (src, dst) as (mode name, reg number) pairs, or None."""
        lo, hi = op & 7, (op >> 9) & 7
        line, m = op >> 12, ins.mnem
        if m in ("ILLEGAL", "LINE-A", "LINE-F", "EXG"):    # a fault uop, or {Rx}/{Ry}
            return None, None

        def lo_ea():
            return (ins.src or ins.dst, lo)
        if line in (1, 2, 3):
            return (ins.src, lo), (ins.dst, hi)
        if line == 7:
            return ("#q", None), ("Dn", hi)
        if line == 0:
            if m in ("BTST", "BCHG", "BCLR", "BSET") and ins.src == "Dn":
                return ("Dn", hi), (ins.dst, lo)
            return ("#imm", None), (ins.dst, lo)
        if line == 4:
            if m in ("LEA", "CHK"):
                return (ins.src, lo), (ins.dst, hi)
            if m in ("TST", "PEA", "JSR", "JMP"):
                return (ins.src, lo), None
            if m in ("LINK", "UNLK"):
                return None, None
            if m in ("EXT", "EXTB", "SWAP"):
                return None, ("Dn", lo)
            return None, (ins.dst, lo) if ins.dst else None
        if line == 5:
            if m.startswith("DB"):
                return None, ("Dn", lo)
            if m in ("ADDQ", "SUBQ"):
                return ("#q", None), (ins.dst, lo)
            return None, (ins.dst, lo)
        if line in (8, 9, 0xB, 0xC, 0xD):
            if m == "CMPM":                     # CMPM (Ay)+,(Ax)+
                return ("(An)+", lo), ("(An)+", hi)
            if m in ("ADDX", "SUBX"):
                return (ins.src, lo), (ins.dst, hi)
            opm = (op >> 6) & 7
            if opm in (4, 5, 6) and m not in ("DIVS", "MULS"):
                return ("Dn", hi), (ins.dst, lo)
            return (ins.src, lo), (ins.dst, hi)
        if line == 0xE:
            if ins.src is None:
                return None, (ins.dst, lo)
            return ((ins.src, hi) if ins.src == "Dn" else ("#q", None)), ("Dn", lo)
        return None, None

    # --- operations -----------------------------------------------------------
    def op_key(self, op, ins):
        m = ins.mnem
        if m in ("ILLEGAL", "LINE-A", "LINE-F"):
            return m
        if op >> 12 == 6:
            return "BRA" if m == "BRA" else "BSR" if m == "BSR" else "Bcc"
        if m.startswith("DB"):
            return "DBF" if m == "DBF" else "DBcc"
        if op >> 12 == 5 and (op >> 6) & 3 == 3 and not m.startswith("TRAP"):
            return "Scc"
        if m.startswith("TRAP") and m not in ("TRAP", "TRAPV"):
            return "TRAPcc"
        if m == "MOVE" and ("SR" in (ins.src, ins.dst) or "CCR" in (ins.src, ins.dst) or "USP" in (ins.src, ins.dst)):
            return "MOVE to/from SR, CCR, USP"
        if m in ("ORI", "ANDI", "EORI") and ins.dst in ("CCR", "SR"):
            return "ORI/ANDI/EORI to CCR, SR"
        return m

    def ext_layout(self, src, dst, size):
        """Byte offsets of each operand's extension words."""
        off, out = 2, {}
        for role, ea in (("src", src), ("dst", dst)):
            if not ea:
                continue
            mode = ea[0]
            out[role] = off
            if mode in ("#imm",):
                off += 4 if size == "L" else 2
            elif mode in ("(d16,An)", "(d16,PC)", "abs.W", "(d8,An,Xn)", "(d8,PC,Xn)"):
                off += 2
            elif mode == "abs.L":
                off += 4
        return out

    def fragment(self, role, ea, size, at, scratch, ext_off):
        """Return (pre, value_or_None, base, disp, post, is_mem) for an operand."""
        mode, r = ea
        spec = self.f["ea_modes"].get(mode)
        if spec is None:
            raise KeyError("no addressing-mode fragment for %s" % mode)
        nbytes = SIZE_BITS.get(size or "L", 32) // 8
        inc = 2 if (nbytes == 1 and r == 7) else nbytes

        def sub(s):
            s = s.replace("{r}", str(r)).replace("{inc}", str(inc)).replace("{at}", at).replace("{s}", scratch)
            s = s.replace("{d16}", "{d16@%d}" % ext_off).replace("{d8}", "{d8@%d}" % ext_off)
            s = s.replace("{abs}", "{%s@%d}" % (mode, ext_off)).replace("{pcd8}", "{pc8@%d}" % ext_off)
            s = s.replace("{pcd}", "{pc16@%d}" % ext_off)
            s = s.replace("d1", "{xn@%d}" % ext_off) if "Xn" in mode else s
            return s
        if "reg" in spec:
            return [], sub(spec["reg"]), None, None, [], False
        if spec.get("imm"):
            return [], None, None, None, [], False
        pre = [sub(x) for x in spec.get("pre", [])]
        post = [sub(x) for x in spec.get("post", [])]
        return pre, None, sub(spec["base"]), sub(str(spec["disp"])), post, True

    def expand(self, op):
        try:
            return self._expand(op)
        except KeyError as e:
            return Flow("unmapped", note=str(e).strip("'\""))

    def _expand(self, op):
        ins = m68kdec.decode(op)
        key = self.op_key(op, ins)
        if key in self.millicode or key.split(".")[0] in self.millicode:
            return Flow("millicode", note=key)
        spec = self.f["ops"].get(key)
        if spec is None:
            return Flow("unmapped", note=key)
        if spec.get("sequencer"):
            n = self.f["params"]["movem_regs"]
            return Flow("sequencer", ["(movem: %d loads/stores + An update)" % n] * (n + 1), spec.get("note", ""))
        tmpl = dict(self.f["templates"].get(spec.get("template"), {}))
        tmpl.update({k: v for k, v in spec.items() if isinstance(v, list)})

        size = ins.size if ins.size in SIZE_BITS else ("L" if ins.size in (None, "S") else ins.size)
        if key in ("BTST", "BCHG", "BCLR", "BSET"):
            size = "L" if ins.dst == "Dn" else "B"
        if key in ("ASL", "ASR", "LSL", "LSR", "ROL", "ROR", "ROXL", "ROXR") and ins.src is None:
            size = "W"
        w = SIZE_BITS[size]
        src, dst = self.roles(op, ins)
        # addressing-mode legality is the decoder's (m68kdec._legal): an
        # opword that reaches here has modes its operation allows
        if (src and src[0] is None) or (dst and dst[0] is None):
            raise AssertionError("%04x: the decoder passed an invalid addressing mode" % op)
        lay = self.ext_layout(src, dst, size)
        uops, post = [], []
        b = {"{w}": str(w), "{g}": spec.get("g", ""), "{T}": self.t["t4"],
             "{len}": str(m68kdec.length(op)[0]), "{A}": "a%d" % (op & 7), "{X}": "d%d" % ((op >> 9) & 7),
             # EXG: opmode 01000 Dx,Dy; 01001 Ax,Ay; 10001 Dx,Ay
             "{Rx}": "%s%d" % ("a" if op & 0xf8 == 0x48 else "d", (op >> 9) & 7),
             "{Ry}": "%s%d" % ("d" if op & 0xf8 == 0x40 else "a", op & 7)}
        q = (op >> 9) & 7 or 8
        if op >> 12 == 7:
            q = sext8(op & 0xFF)
        b["{q}"] = str(q)
        if key in ("Bcc", "DBF") or op >> 12 == 5:
            b["{cc}"] = CC_NAMES[(op >> 8) & 15]
        if op >> 12 == 6:
            d = op & 0xFF
            b["{disp_br}"] = "{br.W@2}" if d == 0 else "{br.L@2}" if d == 0xFF else str(2 + sext8(d))
        if key in ("DBF", "DBcc"):
            b["{disp_br}"] = "{br.W@2}"
        if key == "LINK":
            b["{imm}"] = "{imm.%s@2}" % ("W" if ins.size == "W" else "L")

        imm_src = False
        S = D = None
        dst_mem = None
        if src:
            if src[0] in ("#imm", "#q"):
                imm_src = True
                if src[0] == "#imm":
                    b["{imm}"] = "{imm.%s@%d}" % (size, lay["src"]) if key not in ("BTST", "BCHG", "BCLR", "BSET") else "{imm.W@2}"
                else:
                    b["{imm}"] = str(q)
            else:
                pre, val, base, disp, p2, mem = self.fragment("src", src, size, self.t["t2"], self.t["t3"], lay.get("src", 2))
                uops += pre
                post += p2
                if spec.get("src") == "addr":
                    b["{base}"], b["{disp}"] = base, disp
                    needs_ea = any("{EA}" in line for v in tmpl.values() for line in v)
                    if disp == "0" or not needs_ea:
                        b["{EA}"] = base
                    else:
                        uops.append("addi %s, %s, %s" % (self.t["t2"], base, disp))
                        b["{EA}"] = self.t["t2"]
                elif mem:
                    S = self.t["t0"]
                    uops.append(self.load(size, S, base, disp))
                else:
                    S = val
                # the 68k updates a (An)+ source before computing the
                # destination address, which matters when both use one An
                uops += post
                post = []
        wb_store = None
        if dst:
            pre, val, base, disp, p2, mem = self.fragment("dst", dst, size, self.t["t3"], self.t["t2"], lay.get("dst", 2))
            if dst[0] in ("-(An)", "(An)+") and spec.get("dst") == "write":
                # fold the address update into a writeback store
                inc = (p2 or pre)[0].rsplit(",", 1)[1].strip()
                wb_store = ("pre" if dst[0] == "-(An)" else "post", inc)
                pre, p2 = [], []
            uops += pre
            post = p2 + post
            if mem:
                D = self.t["t1"]
                dst_mem = (base, disp)
                if spec.get("dst") in ("rmw", "read"):
                    uops.append(self.load(size, D, base, disp))
            else:
                D = val
        if S is not None:
            b["{S}"] = S
        if D is not None:
            b["{D}"] = D

        dkind = "none" if not dst else ("mem" if dst_mem else "an" if dst[0] == "An" else "reg")
        cands = []
        for imm in ((True, False) if imm_src else (False,)):
            pfx = "imm_" if imm else ""
            if w == 32:
                cands.append(pfx + dkind + "32")
            cands.append(pfx + dkind)
            cands.append(pfx + "any")
        body = next((tmpl[c] for c in cands if c in tmpl), None)
        if body is None:
            return Flow("unmapped", note="%s: no template for %s" % (key, "/".join(cands)))
        if imm_src and "{S}" not in b and any("{S}" in line for line in body):
            # an immediate source without an immediate template: materialize it
            uops.append("addi %s, x0, %s" % (self.t["t0"], b["{imm}"]))
            b["{S}"] = self.t["t0"]
        for line in body:
            for k, v in b.items():
                if v is not None:
                    line = line.replace(k, v)
            uops.append(line)
        if dst_mem and spec.get("dst") in ("rmw", "write"):
            what = spec.get("store", "D")
            val = {"S": b.get("{S}"), "D": D, "x0": "x0"}[what]
            if wb_store:
                uops.append("st.%d.%s.gd %s, %s(%s)" % (w, wb_store[0], val, wb_store[1], dst_mem[0]))
            else:
                uops.append("st.%d.gd %s, %s(%s)" % (w, val, dst_mem[1], dst_mem[0]))
        uops += post
        uops = self.peephole(uops)
        return Flow("pla", uops)

    # --- bitfields: second-level decode on the extension word ---------------
    def bf_variants(self, op):
        """Flows for a bitfield opword with immediate offset and width, keyed by
        n = bytes the field touches (1-4), plus "long" (a whole long at a byte
        boundary); a register operand has one flow, returned for every n.
        None if the opword always uses millicode."""
        if op & 0xF8C0 != 0xE8C0:
            return None
        name = BF_OPS[(op >> 8) & 7]
        spec = self.f["bitfield"]["ops"][name]
        mode, r = (op >> 3) & 7, op & 7
        write = spec.get("write", False)
        t = self.t
        b = {"{F}": t["t1"], "{T}": t["t4"], "{Dx}": "{dx@2}", "{bfi}": "{bfi@2}, {bfw@2}"}
        if mode == 0:
            b.update({"{V}": "d%d" % r, "{bf}": "{bfr@2}, {bfw@2}", "{bfd}": "{bfr@2}"})
            fl = Flow("pla", self.peephole([self.subst(x, b) for x in spec["body"]]))
            out = {n: fl for n in (1, 2, 3, 4)}
            out["long"] = Flow("pla", [self.subst(x, b) for x in spec["long"]]) if "long" in spec else fl
            return out
        if spec.get("reg_only"):
            return None
        mname = BF_MODES.get(mode) or (BF_MODE7.get(r) if mode == 7 else None)
        if mname is None or (write and "PC" in mname):
            return None
        pre, _, base, disp, _, _ = self.fragment("dst", (mname, r), "L", t["t2"], t["t3"], 4)
        m = RUNTIME.match(disp)
        disp = "{bfb@2}" if disp == "0" else disp[:-1] + "|bf}"
        out = {}
        for n in (1, 2, 3, 4):
            b2 = dict(b, **{"{V}": t["t0"], "{bf}": "{bfm%d@2}, {bfw@2}" % n, "{base}": base,
                            "{disp}": disp, "{disp2}": disp + "+2"})
            lines = list(pre) + self.f["bitfield"]["load"][n] + spec["body"]
            if write:
                lines += self.f["bitfield"]["store"][n]
            out[n] = Flow("pla", self.peephole([self.subst(x, b2) for x in lines]))
            if n == 4:
                body = spec.get("long")
                out["long"] = Flow("pla", self.peephole([self.subst(x, b2) for x in list(pre) + self.f["bitfield"]["load"][4] + body])) if body else out[4]
        return out

    def movec_variants(self, op):
        """Flows for MOVEC (4e7a read, 4e7b write) in supervisor state with M = 0,
        keyed by the 16-way control-register index; None where millicode runs."""
        if op not in (0x4E7A, 0x4E7B):
            return None
        mc = self.f["movec"]
        rw = "write" if op & 1 else "read"
        out = {}
        for v, csr in enumerate(mc["regs"]):
            if csr is None:
                out[v] = None
            elif csr == "isp":
                out[v] = Flow("pla", [self.subst(x, {"{Rn}": "{rn@2}"}) for x in mc["active_" + rw]])
            else:
                out[v] = Flow("pla", [self.subst(x, {"{Rn}": "{rn@2}", "{csr}": csr}) for x in mc[rw]])
        return out

    def sr_variants(self, op):
        """Supervisor-state (M = 0) flows for MOVE SR,Dn (40c0-40c7), MOVE Dn,SR
        (46c0-46c7) and MOVE #imm,SR (46fc: [S stays 1, S goes to 0])."""
        sr = self.f["sr"]
        b = {"{T}": self.t["t4"], "{Dn}": "d%d" % (op & 7)}
        def fl(key):
            return Flow("pla", [self.subst(x, b) for x in sr[key]])
        if op & 0xFFF8 == 0x40C0:
            return [fl("from_sr")]
        if op & 0xFFF8 == 0x46C0:
            return [fl("to_sr")]
        if op == 0x46FC:
            return [fl("imm"), fl("imm_user")]
        return None

    def long_src(self, op):
        """(uops, S) loading the 32-bit source of a MULx.L/DIVx.L opword, whose
        operand extension words follow the extension word at offset 4; None
        for an invalid source."""
        src = (m68kdec.decode(op).src, op & 7)
        if src[0] is None or src[0] == "An":
            return None
        t = self.t
        if src[0] == "#imm":
            return ["addi %s, x0, {imm.L@4}" % t["t0"]], t["t0"]
        fpre, val, base, disp, post, mem = self.fragment("src", src, "L", t["t2"], t["t3"], 4)
        if mem:
            return fpre + [self.load("L", t["t0"], base, disp)] + post, t["t0"]
        return [], val

    def mull_variants(self, op):
        """MULU.L/MULS.L flows indexed by extension bits 11:10 (u32, u64, s32, s64)."""
        if op & 0xFFC0 != 0x4C00:
            return None
        ls = self.long_src(op)
        if not ls:
            return None
        pre, S = ls
        b = {"{Dl}": "{dx@2}", "{Dh}": "{dh@2}", "{S}": S}
        m = self.f["mull"]
        return [Flow("pla", self.peephole(pre + [self.subst(x, b) for x in m[k]])) for k in ("u32", "u64", "s32", "s64")]

    def divl_variants(self, op):
        """DIVU.L/DIVS.L flows indexed by extension bits 11:10 (signed, 64-bit
        dividend) plus 4 when Dr = Dq; None entries (64-bit dividend) use millicode."""
        if op & 0xFFC0 != 0x4C40:
            return None
        ls = self.long_src(op)
        if not ls:
            return None
        pre, S = ls
        b = {"{Dq}": "{dx@2}", "{Dr}": "{dh@2}", "{S}": S}
        d = self.f["divl"]
        def fl(k):
            return Flow("pla", self.peephole(pre + [self.subst(x, b) for x in d[k]]))
        return [fl("u32"), None, fl("s32"), None, fl("u32q"), None, fl("s32q"), None]

    MOVES_MODES = ("(An)", "(An)+", "-(An)", "(d16,An)", "(d8,An,Xn)", "abs.W", "abs.L")

    def moves_variants(self, op):
        """MOVES flows in supervisor state: [load into Dn, load into An, store]."""
        if op & 0xFF00 != 0x0E00 or (op >> 6) & 3 == 3:
            return None
        ins = m68kdec.decode(op)
        if ins.mnem != "MOVES" or ins.dst not in self.MOVES_MODES:
            return None
        size, w = ins.size, {"B": 8, "W": 16, "L": 32}[ins.size]
        t = self.t
        pre, _, base, disp, post, _ = self.fragment("dst", (ins.dst, op & 7), size, t["t2"], t["t3"], 4)
        mem = "%s(%s)" % (disp, base)
        ld = "%s.ga %s, %s" % ({"B": "ld.8u", "W": "ld.16u", "L": "ld.32"}[size], t["t0"], mem)
        m = self.f["moves"]
        out = []
        for key in ("load_dn", "load_an"):
            body = [self.subst(x, {"{Rn}": "{rn@2}", "{V}": t["t0"]}) for x in m[key][size]]
            out.append(Flow("pla", self.peephole(pre + [ld] + post + body)))
        if ins.dst in ("(An)+", "-(An)"):   # Rn may be that An: store its old value
            out.append(Flow("pla", ["add %s, {rn@2}, x0" % t["t1"]] + pre + ["st.%d.ga %s, %s" % (w, t["t1"], mem)] + post))
        else:
            out.append(Flow("pla", pre + ["st.%d.ga {rn@2}, %s" % (w, mem)] + post))
        return out

    def cas_variants(self, op):
        """CAS Dc,Du,<ea>: one flow around the cas uop for a memory-alterable
        <ea>; other modes have none (millicode raises the illegal instruction)."""
        if op & 0xF9C0 != 0x08C0 or (op >> 9) & 3 == 0:
            return None
        ins = m68kdec.decode(op)
        if ins.mnem != "CAS" or ins.dst not in self.MOVES_MODES:
            return None
        size, w = ins.size, {"B": 8, "W": 16, "L": 32}[ins.size]
        t = self.t
        pre, _, base, disp, post, _ = self.fragment("dst", (ins.dst, op & 7), size, t["t2"], t["t3"], 4)
        c = self.f["cas"]
        b = {"{Dc}": "{dh@2}", "{Du}": "{dx@2}", "{V}": t["t0"]}
        swap = [self.subst(x, b) for x in c["swap"]]
        body = [self.subst(x, b) for x in c[size]]
        cas = "cas.%d.gd %s, {dh@2}, %s(%s)" % (w, t["t0"], disp, base)
        return [Flow("pla", self.peephole(swap + pre + [cas] + post + body))]

    def xsel_variants(self, op):
        """Second-level-decode flows of an opword as a list indexed by variant
        (None where millicode runs), or None if the opword has none."""
        bf = self.bf_variants(op)
        if bf:
            return [bf[k] for k in (1, 2, 3, 4, "long")]
        mv = self.movec_variants(op)
        if mv:
            return [mv[k] for k in range(16)]
        return (self.sr_variants(op) or self.mull_variants(op) or self.divl_variants(op) or self.moves_variants(op)
                or self.cas_variants(op))

    @staticmethod
    def subst(line, b):
        for k, v in b.items():
            line = line.replace(k, v)
        return line

    @staticmethod
    def load(size, dst, base, disp):
        return "%s.gd %s, %s(%s)" % ({"B": "ld.8u", "W": "ld.16u", "L": "ld.32"}[size], dst, disp, base)

    PRODUCERS = re.compile(r"(ld\.\S+|gpcaddi|addi|add|sh[123]add|sext16|zext16) ")

    @classmethod
    def peephole(cls, uops):
        """Fold a register copy into the uop that produced its source.

        `addi R, B, 0` is first rewritten as `add R, B, x0`; then when a temp
        T is written by the previous uop and only feeds `add R, T, x0`, the
        producer writes R directly.
        """
        uops = [re.sub(r"^addi (\S+), (\S+), 0$", r"add \1, \2, x0", u) for u in uops]
        out = []
        for k, u in enumerate(uops):
            m = re.match(r"add (\S+), (\S+), x0$", u)
            if m and m.group(2).startswith("x") and m.group(2) != "x0" and out and \
                    cls.PRODUCERS.match(out[-1]) and out[-1].split()[1] == m.group(2) + "," and \
                    not any(re.search(r"\b%s\b" % m.group(2), x) for x in uops[k + 1:]):
                out[-1] = out[-1].replace(" %s," % m.group(2), " %s," % m.group(1), 1)
                continue
            out.append(u)
        return out


def concretize(line):
    return RUNTIME.sub(lambda m: SAMPLE[m.group(1)], line)


def check(spec, flow):
    """Assemble each uop. PLA uops carry full-width immediates, so an
    immediate that only overflows the millicode encoding is not an error."""
    errs = []
    for u in flow.uops:
        try:
            spec.assemble_line(concretize(u))
        except Exception as e:  # noqa: BLE001
            if "out of range" not in str(e):
                errs.append("%s: %s" % (u, e))
    return errs


def labeled_variants(ex, op):
    """The second-level-decode flows of an opword, as (label, flow, compact)
    in display order; compact flows (MOVEC) print on one line."""
    out = []
    vs = ex.bf_variants(op) or {}
    for n, v in vs.items():
        if n in (2, 3, 4) and v is vs[1] or (n == "long" and v is vs[1 if op & 0x38 == 0 else 4]):
            continue
        what = "whole long" if n == "long" else ("register" if op & 0x38 == 0 else "field in %d byte%s" % (n, "s" if n > 1 else ""))
        out.append(("immediate offset/width, %s" % what, v, False))
    for n, v in enumerate(ex.mull_variants(op) or []):
        out.append((("unsigned 32", "unsigned 64", "signed 32", "signed 64")[n], v, False))
    for n, v in enumerate(ex.moves_variants(op) or []):
        out.append(("S = 1, %s" % ("<ea>,Dn", "<ea>,An", "Rn,<ea>")[n], v, False))
    for v in ex.cas_variants(op) or []:
        out.append(("memory-alterable <ea>", v, False))
    for n, v in enumerate(ex.divl_variants(op) or []):
        if v:
            out.append(("%s 32-bit dividend%s" % ("signed" if n & 2 else "unsigned", ", quotient only" if n & 4 else ""), v, False))
    for n, v in enumerate(ex.sr_variants(op) or []):
        what = ("; new S = 1", "; new S = 0")[n] if op == 0x46FC else ""
        out.append(("S = 1, M = 0%s" % what, v, False))
    mv = ex.movec_variants(op) or {}
    for n, v in mv.items():
        if v:
            out.append(("%s (S = 1, M = 0)" % ex.f["movec"]["regs"][n], v, True))
    return out


def cmd_show(a):
    ex, spec = Expander(), uisa.load()
    for h in a.ops:
        op = int(h, 16)
        fl = ex.expand(op)
        print("%04x  %-28s %s (%d uops)%s" % (op, m68kdec.decode(op).form(), fl.kind, fl.count, ("  " + fl.note) if fl.note else ""))
        for u in fl.uops:
            print("        " + u)
        for e in check(spec, fl) if fl.kind == "pla" else []:
            print("   !! " + e)
        for label, v, compact in labeled_variants(ex, op):
            if compact:
                print("      %s: %s" % (label, "; ".join(v.uops)))
            else:
                print("      %s: %d uops" % (label, v.count))
                for u in v.uops:
                    print("          " + u)
            for e in check(spec, v):
                print("   !! " + e)

def cmd_check(a):
    ex, spec = Expander(), uisa.load()
    kinds, bad, errs = collections.Counter(), collections.Counter(), {}
    for op in range(65536):
        fl = ex.expand(op)
        kinds[fl.kind] += 1
        e = check(spec, fl) if fl.kind == "pla" else []
        for v in ex.xsel_variants(op) or []:
            e += check(spec, v) if v else []
        if True:
            if e:
                form = m68kdec.decode(op).form()
                bad[form] += 1
                errs.setdefault(form, (op, e))
    print("opwords by flow kind: " + ", ".join("%s %d" % kv for kv in kinds.most_common()))
    print("forms with uops that do not assemble: %d" % len(bad))
    for form, (op, e) in list(errs.items())[:a.max]:
        print("  %04x %s: %s" % (op, form, e[0]))
    return 1 if bad else 0


def cmd_report(a):
    ex = Expander()
    hist = {}
    with open(a.ophist) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            op, u, s = line.split()
            hist[int(op, 16)] = int(u) + int(s)
    total = sum(hist.values())
    by_kind = collections.Counter()
    uop_weight = 0
    pla_insns = 0
    seq_insns = 0
    by_count = collections.Counter()
    forms = collections.defaultdict(lambda: [0, None, None])
    for op, c in hist.items():
        fl = ex.expand(op)
        by_kind[fl.kind] += c
        if fl.kind in ("pla", "sequencer"):
            uop_weight += c * fl.count
            pla_insns += c
            if fl.kind == "sequencer":
                seq_insns += c
            by_count[min(fl.count, 6)] += c
        f = forms[m68kdec.decode(op).form()]
        f[0] += c
        if f[2] is None or c > hist[f[2]]:
            f[1], f[2] = fl, op       # show the form's most frequent opword
    out = ["# PLA flow coverage", "",
           "Histogram `%s`: %d instructions." % (os.path.basename(a.ophist), total), "",
           "| Flow kind | % of instructions |", "|---|---|"]
    for k in ("pla", "sequencer", "millicode", "unmapped"):
        out.append("| %s | %.2f |" % (k, 100.0 * by_kind[k] / total))
    out += ["", "PLA and sequencer flows average **%.2f uops per 68k instruction** (MOVEM counted as %d registers)." %
            (uop_weight / max(1, pla_insns), ex.f["params"]["movem_regs"]), "",
            "| Uops | % of PLA instructions |", "|---|---|"]
    for n in sorted(by_count):
        out.append("| %s | %.2f |" % ("6+" if n == 6 else n, 100.0 * by_count[n] / max(1, pla_insns)))
    out += ["", "## Top forms", "", "| Form | % | Kind | Uops | Flow |", "|---|---|---|---|---|"]
    for form, (c, fl, op) in sorted(forms.items(), key=lambda kv: -kv[1][0])[:a.top]:
        flow = "<br>".join("`%s`" % u for u in fl.uops) if fl.kind == "pla" else fl.note
        out.append("| `%s` | %.2f | %s | %s | %s |" % (form, 100.0 * c / total, fl.kind,
                                                      fl.count if fl.kind != "millicode" else "", flow))
    unm = collections.Counter()
    for form, (c, fl, op) in forms.items():
        if fl.kind == "unmapped":
            unm[(form, fl.note)] += c
    if unm:
        out += ["", "## Unmapped (worklist)", "", "| Form | % | Why |", "|---|---|---|"]
        for (form, note), c in unm.most_common(40):
            out.append("| `%s` | %.3f | %s |" % (form, 100.0 * c / total, note))
    text = "\n".join(out) + "\n"
    if a.out:
        with open(a.out, "w") as f:
            f.write(text)
    else:
        print(text)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("ops", nargs="+")
    c = sub.add_parser("check")
    c.add_argument("--max", type=int, default=30)
    r = sub.add_parser("report")
    r.add_argument("ophist")
    r.add_argument("-o", "--out")
    r.add_argument("--top", type=int, default=60)
    a = ap.parse_args()
    sys.exit({"show": cmd_show, "check": cmd_check, "report": cmd_report}[a.cmd](a) or 0)


if __name__ == "__main__":
    main()
