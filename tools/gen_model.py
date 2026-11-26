#!/usr/bin/env python3
"""Generate build/uisa_flows.inc: the PLA flow of every 68k opword, as data
for the C++ functional model (model/shadow.cc).

Each uop is {op, rd, rs1, rs2, ik, ioff, imm}. `op` is a uisa op id, or
UISA_PSEUDO_IDX for a brief-format index computation (rd = rs1 + Xn*scale,
from the extension word at ioff), which the expander writes as the pair
`sext16 T, {xn@k}` + `shNadd D, B, T` with a representative Xn.W*4.
`ik` says how to form the immediate at run time from the instruction's
extension words (IK_* below); bit 7 negates it, bit 6 adds the bitfield byte
offset (bits 10:9 of the extension word at offset 2), and `imm` is added to
any run-time value. Register number 60 (G_XREG) is the register named by the
extension word at offset 2, which the front end substitutes: bits 14:12 (Dn)
for a bitfield, MULx.L Dl or DIVx.L Dq, bits 15:12 (Dn/An) for MOVEC and MOVES (and MOVE16 Ay), bits 8:6
for CAS Du; register 61 (G_XREG2) is bits 2:0 of it (MULx.L Dh, DIVx.L Dr, CAS Dc).

Second-level decode (XSEL): bitfield, MOVEC, MOVE to/from SR, MULx.L, DIVx.L, MOVES, CAS and MOVE16 (Ax)+,(Ay)+ opwords have up to XSEL_NV
flows each, XSEL_START/LEN[XSEL_INDEX[op]*XSEL_NV + v], chosen from the
extension word and the S and M bits (flows.yaml `bitfield:`, `movec:`, `sr:`, `mull:`, `divl:`, `moves:`, `cas:`);
length 0 marks a variant
that enters millicode.
"""

import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import flows  # noqa: E402
import m68kdec  # noqa: E402
import uisa  # noqa: E402

BUILD = os.path.join(HERE, "..", "build")
IK = ["CONST", "d16", "d8", "abs.W", "abs.L", "imm.B", "imm.W", "imm.L", "br.W", "br.L", "pc16", "pc8",
      "bfb", "bfm1", "bfm2", "bfm3", "bfm4", "bfr", "bfi"]
G_XREG = 60
G_XREG2 = 61
XSEL_NV = 16
KINDS = ["illegal", "pla", "sequencer", "millicode", "unmapped"]
REF = re.compile(r"^(-?)\{([A-Za-z0-9.]+)@(\d+)(\|bf)?\}(?:\+(\d+))?$")


class Gen:
    def __init__(self):
        self.spec = uisa.load()
        self.ex = flows.Expander()

    def reg(self, tok):
        tok = tok.strip()
        if tok in ("{dx@2}", "{rn@2}"):
            return G_XREG
        if tok == "{dh@2}":
            return G_XREG2
        return self.spec.regs[tok]

    def imm(self, tok):
        tok = tok.strip()
        m = REF.match(tok)
        if m:
            neg, kind, off, bf, add = m.groups()
            return IK.index(kind) | (0x80 if neg else 0) | (0x40 if bf else 0), int(off), int(add or 0)
        return 0, 0, int(tok, 0)

    def uop(self, line):
        mnem, _, rest = line.partition(" ")
        v = self.spec.by_mnemonic[mnem]
        toks = [t.strip() for t in rest.split(",")] if rest.strip() else []
        f = {"rd": 0, "rs1": 0, "rs2": 0, "ik": 0, "ioff": 0, "imm": 0}
        for kind, tok in zip(v.operands, toks):
            if kind in ("rd", "rs1", "rs2"):
                f[kind] = self.reg(tok)
            elif kind == "mem":
                m = re.match(r"^(.*)\((.+)\)$", tok)
                f["rs1"] = self.reg(m.group(2))
                f["ik"], f["ioff"], f["imm"] = self.imm(m.group(1) or "0")
            elif kind == "csr" and tok in self.spec.csr_by_name:
                f["imm"] = self.spec.csr_by_name[tok]
            elif kind in ("imm", "csr", "off"):
                f["ik"], f["ioff"], f["imm"] = self.imm(tok)
            elif kind == "wid":
                if not tok.startswith("{bfw@"):
                    raise ValueError("constant BFI width in %s" % line)   # the descriptor IK carries it
            else:
                raise ValueError("operand %s in %s" % (kind, line))
        if v.attrs.get("writeback"):
            f["rd"] = f["rs1"]
        elif v.tied:
            f["rs1"] = f["rd"]
        return "uisa::%s" % v.ident, f

    def flow(self, op):
        fl = self.ex.expand(op)
        if fl.kind != "pla":
            return fl.kind, []
        return "pla", self.lower(fl)

    def lower(self, fl):
        out, uops = [], fl.uops
        k = 0
        while k < len(uops):
            m = re.match(r"^sext16 (\S+), \{xn@(\d+)\}$", uops[k])
            if m and k + 1 < len(uops) and re.match(r"^sh[123]add ", uops[k + 1]):
                d, b, _ = [x.strip() for x in uops[k + 1].split(" ", 1)[1].split(",")]
                out.append(("UISA_PSEUDO_IDX", {"rd": self.reg(d), "rs1": self.reg(b), "rs2": 0,
                                                "ik": 0, "ioff": int(m.group(2)), "imm": 0}))
                k += 2
                continue
            out.append(self.uop(uops[k]))
            k += 1
        return out


def main():
    g = Gen()
    kinds, starts, lens, ilens, pool = [], [], [], [], []
    for op in range(65536):
        kind, us = g.flow(op)
        kinds.append(KINDS.index(kind))
        starts.append(len(pool))
        lens.append(len(us))
        ilens.append(m68kdec.length(op)[0])
        pool.extend(us)
    xsel_index, xsel_start, xsel_len, nx = [-1] * 65536, [], [], collections.Counter()
    for op in range(65536):
        vs = g.ex.xsel_variants(op)
        if not vs:
            continue
        nx["movec" if op in (0x4E7A, 0x4E7B) else "bitfield" if op & 0xF8C0 == 0xE8C0 else "mull" if op & 0xFFC0 == 0x4C00 else "divl" if op & 0xFFC0 == 0x4C40 else "cas" if op & 0xF9C0 == 0x08C0 else "move16" if op & 0xFFF8 == 0xF620 else "moves" if op & 0xFF00 == 0x0E00 else "sr"] += 1
        xsel_index[op] = len(xsel_start) // XSEL_NV
        for v in range(XSEL_NV):
            us = g.lower(vs[v]) if v < len(vs) and vs[v] else []
            xsel_start.append(len(pool))
            xsel_len.append(len(us))
            pool.extend(us)
    o = ["// Generated by tools/gen_model.py from spec/flows.yaml and spec/uisa.yaml. Do not edit.",
         "// kinds: " + ", ".join("%d=%s" % (i, k) for i, k in enumerate(KINDS)),
         "// ik: " + ", ".join("%d=%s" % (i, k) for i, k in enumerate(IK)) + "; bit 7 negates", ""]
    o.append("static const uisa_fuop FLOW_UOPS[%d] = {" % max(1, len(pool)))
    for name, f in pool:
        o.append("  {%s, %d, %d, %d, %d, %d, %d}," % (name, f["rd"], f["rs1"], f["rs2"], f["ik"], f["ioff"], f["imm"]))
    if not pool:
        o.append("  {0, 0, 0, 0, 0, 0, 0},")
    o.append("};")

    def arr(ctype, name, vals):
        o.append("static const %s %s[65536] = {" % (ctype, name))
        for i in range(0, 65536, 32):
            o.append("  " + ",".join(str(x) for x in vals[i:i + 32]) + ",")
        o.append("};")
    arr("uint8_t", "FLOW_KIND", kinds)
    arr("uint32_t", "FLOW_START", starts)
    arr("uint8_t", "FLOW_LEN", lens)
    arr("uint8_t", "FLOW_ILEN", ilens)
    arr("int16_t", "XSEL_INDEX", xsel_index)
    o.append("enum { XSEL_NV = %d };" % XSEL_NV)
    o.append("static const uint32_t XSEL_START[%d] = {%s};" % (len(xsel_start), ",".join(map(str, xsel_start))))
    o.append("static const uint8_t XSEL_LEN[%d] = {%s};" % (len(xsel_len), ",".join(map(str, xsel_len))))
    os.makedirs(BUILD, exist_ok=True)
    with open(os.path.join(BUILD, "uisa_flows.inc"), "w") as f:
        f.write("\n".join(o) + "\n")
    print("uisa_flows.inc: %d uops, XSEL opwords: %s; %s" % (len(pool), ", ".join("%s %d" % kv for kv in sorted(nx.items())), ", ".join(
        "%s %d" % (k, kinds.count(i)) for i, k in enumerate(KINDS))))


if __name__ == "__main__":
    main()
