"""Decode a 68040 opword into (mnemonic, size, source EA, destination EA).

Only the opword is available (the histogram counts opwords), so indexed
modes are reported as `(d8,An,Xn)` / `(d8,PC,Xn)` even when the extension word
turns out to be full-format, and FPU/MMU ops that are selected by an extension
word are reported by their opword group.
"""

EA_NAMES = {0: "Dn", 1: "An", 2: "(An)", 3: "(An)+", 4: "-(An)", 5: "(d16,An)", 6: "(d8,An,Xn)"}
EA7_NAMES = {0: "abs.W", 1: "abs.L", 2: "(d16,PC)", 3: "(d8,PC,Xn)", 4: "#imm"}
SZ = {0: "B", 1: "W", 2: "L"}
CC = ["T", "F", "HI", "LS", "CC", "CS", "NE", "EQ", "VC", "VS", "PL", "MI", "GE", "LT", "GT", "LE"]


class Insn:
    __slots__ = ("mnem", "size", "src", "dst")

    def __init__(self, mnem, size=None, src=None, dst=None):
        self.mnem, self.size, self.src, self.dst = mnem, size, src, dst

    def form(self):
        m = self.mnem + ("." + self.size if self.size else "")
        ops = ",".join(x for x in (self.src, self.dst) if x)
        return (m + " " + ops).strip()

    def __repr__(self):
        return "Insn(%s)" % self.form()


def ea(mode, reg):
    if mode < 7:
        return EA_NAMES[mode]
    return EA7_NAMES.get(reg)


def ea_lo(op):
    return ea((op >> 3) & 7, op & 7)


def valid_ea(op):
    return ea_lo(op) is not None


def is_memory(name):
    return name is not None and name not in ("Dn", "An", "#imm") and not name.startswith("#") and name not in ("list", "CCR", "SR", "USP", "Rc")


def decode(op):
    """Decode an opword. An encoding the 68040 does not define, including a
    defined operation with an addressing mode its category excludes, decodes
    as ILLEGAL (the illegal-instruction exception, vector 4)."""
    line = op >> 12
    fn = _LINES.get(line)
    r = fn(op) if fn else None
    if r is None or not _legal(op, r):
        return Insn("ILLEGAL")
    return r


# --- addressing-mode categories (68040 UM / M68000 PRM, section 2) ---------------
ALL = frozenset(list(EA_NAMES.values()) + list(EA7_NAMES.values()))
DATA = ALL - {"An"}
MEMORY = DATA - {"Dn"}
CONTROL = frozenset({"(An)", "(d16,An)", "(d8,An,Xn)", "abs.W", "abs.L", "(d16,PC)", "(d8,PC,Xn)"})
ALTERABLE = ALL - {"(d16,PC)", "(d8,PC,Xn)", "#imm"}
DATA_ALT = ALTERABLE - {"An"}
MEM_ALT = DATA_ALT - {"Dn"}
CTRL_ALT = CONTROL - {"(d16,PC)", "(d8,PC,Xn)"}

# the category of the opword's EA field (bits 5:0), by mnemonic
_EA_CAT = {
    "ORI": DATA_ALT, "ANDI": DATA_ALT, "SUBI": DATA_ALT, "ADDI": DATA_ALT, "EORI": DATA_ALT,
    "CMPI": DATA - {"#imm"},
    "BCHG": DATA_ALT, "BCLR": DATA_ALT, "BSET": DATA_ALT,
    "CMP2/CHK2": CONTROL, "CAS": MEM_ALT, "MOVES": MEM_ALT,
    "MOVEA": ALL,
    "NEGX": DATA_ALT, "CLR": DATA_ALT, "NEG": DATA_ALT, "NOT": DATA_ALT, "NBCD": DATA_ALT, "TAS": DATA_ALT,
    "TST": ALL, "PEA": CONTROL, "JSR": CONTROL, "JMP": CONTROL, "LEA": CONTROL, "CHK": DATA,
    "MULx": DATA, "DIVx": DATA, "MULU": DATA, "MULS": DATA, "DIVU": DATA, "DIVS": DATA,
    "ADDQ": ALTERABLE, "SUBQ": ALTERABLE,
    "CMP": ALL, "CMPA": ALL, "ADDA": ALL, "SUBA": ALL, "EOR": DATA_ALT,
    "BFTST": CONTROL | {"Dn"}, "BFEXTU": CONTROL | {"Dn"}, "BFEXTS": CONTROL | {"Dn"}, "BFFFO": CONTROL | {"Dn"},
    "BFCHG": CTRL_ALT | {"Dn"}, "BFCLR": CTRL_ALT | {"Dn"}, "BFSET": CTRL_ALT | {"Dn"}, "BFINS": CTRL_ALT | {"Dn"},
    "FSAVE": CTRL_ALT | {"-(An)"}, "FRESTORE": CONTROL | {"(An)+"},
}
_SHIFTS = {"ASL", "ASR", "LSL", "LSR", "ROXL", "ROXR", "ROL", "ROR"}


def _legal(op, i):
    """Whether the decoded instruction's addressing modes are in the 68040's
    categories for it (only the EA field is checked; register-only operands
    are fixed by the encoding)."""
    m, line = i.mnem, op >> 12
    e = ea_lo(op)
    if line in (1, 2, 3):                                   # MOVE / MOVEA
        if e is None or (m == "MOVEA" and i.size == "B"):
            return False
        if i.size == "B" and e == "An":
            return False
        return m == "MOVEA" or i.dst in DATA_ALT
    if m in ("ORI", "ANDI", "EORI") and i.dst in ("CCR", "SR"):
        return True
    if m == "BTST":                                         # static: no #imm; dynamic: any data mode
        return e in (DATA - {"#imm"} if i.src == "#imm" else DATA)
    if m == "MOVE":                                         # line 4: to/from SR, CCR, USP
        if "USP" in (i.src, i.dst):
            return True
        return e in (DATA if i.dst in ("SR", "CCR") else DATA_ALT)
    if m == "MOVEM":
        return e in ((CONTROL | {"(An)+"}) if i.dst == "list" else (CTRL_ALT | {"-(An)"}))
    if line == 5 and m.startswith("S") and m != "SUBQ":    # Scc
        return e in DATA_ALT
    if m in ("ADDQ", "SUBQ", "CMP", "TST") and i.size == "B" and e == "An":
        return False
    if m in ("OR", "AND", "ADD", "SUB"):
        if (op >> 6) & 7 < 3:                               # opmode 0-2: <ea>,Dn
            if m in ("OR", "AND"):
                return e in DATA
            return e in ALL and not (i.size == "B" and e == "An")
        return e in MEM_ALT                                 # Dn,<ea>
    if line == 0xE and m in _SHIFTS and i.src is None:      # memory shifts
        return e in MEM_ALT
    cat = _EA_CAT.get(m)
    return cat is None or e in cat


# --- line 0: immediates, bit ops, MOVEP, CAS, CMP2/CHK2, MOVES -------------
def _line0(op):
    special = {0x003C: ("ORI", "B", "#imm", "CCR"), 0x007C: ("ORI", "W", "#imm", "SR"),
               0x023C: ("ANDI", "B", "#imm", "CCR"), 0x027C: ("ANDI", "W", "#imm", "SR"),
               0x0A3C: ("EORI", "B", "#imm", "CCR"), 0x0A7C: ("EORI", "W", "#imm", "SR"),
               0x0CFC: ("CAS2", "W", None, None), 0x0EFC: ("CAS2", "L", None, None)}
    if op in special:
        return Insn(*special[op])
    if op & 0xF138 == 0x0108:
        opm = (op >> 6) & 3
        return Insn("MOVEP", "W" if opm in (0, 2) else "L",
                    "(d16,An)" if opm < 2 else "Dn", "Dn" if opm < 2 else "(d16,An)")
    if op & 0x0100:   # dynamic bit ops
        t = (op >> 6) & 3
        return Insn(["BTST", "BCHG", "BCLR", "BSET"][t], None, "Dn", ea_lo(op))
    hi = (op >> 8) & 0xF
    sz = (op >> 6) & 3
    if hi == 0x8:     # static bit ops
        return Insn(["BTST", "BCHG", "BCLR", "BSET"][sz], None, "#imm", ea_lo(op))
    if sz == 3:
        if hi in (0x0, 0x2, 0x4):
            return Insn("CMP2/CHK2", SZ[hi >> 1], ea_lo(op), "Rn")
        if hi in (0xA, 0xC, 0xE):
            return Insn("CAS", {0xA: "B", 0xC: "W", 0xE: "L"}[hi], "Dc,Du", ea_lo(op))
        return None
    if hi == 0xE:
        return Insn("MOVES", SZ[sz], None, ea_lo(op))
    name = {0x0: "ORI", 0x2: "ANDI", 0x4: "SUBI", 0x6: "ADDI", 0xA: "EORI", 0xC: "CMPI"}.get(hi)
    if name:
        return Insn(name, SZ[sz], "#imm", ea_lo(op))
    return None


# --- lines 1-3: MOVE / MOVEA --------------------------------------------------
def _move(op):
    size = {1: "B", 2: "L", 3: "W"}[op >> 12]
    dmode, dreg = (op >> 6) & 7, (op >> 9) & 7
    if dmode == 1:
        return Insn("MOVEA", size, ea_lo(op), "An")
    return Insn("MOVE", size, ea_lo(op), ea(dmode, dreg))


# --- line 4: miscellaneous ----------------------------------------------------
def _line4(op):
    fixed = {0x4AFC: "ILLEGAL", 0x4E70: "RESET", 0x4E71: "NOP", 0x4E72: "STOP", 0x4E73: "RTE",
             0x4E74: "RTD", 0x4E75: "RTS", 0x4E76: "TRAPV", 0x4E77: "RTR"}
    if op in fixed:
        return Insn(fixed[op])
    if op & 0xFFFE == 0x4E7A:
        return Insn("MOVEC", "L", "Rc" if op & 1 == 0 else "Rn", "Rn" if op & 1 == 0 else "Rc")
    if op & 0xFFF0 == 0x4E40:
        return Insn("TRAP", None, "#imm")
    if op & 0xFFF8 == 0x4E50:
        return Insn("LINK", "W", "An", "#imm")
    if op & 0xFFF8 == 0x4808:
        return Insn("LINK", "L", "An", "#imm")
    if op & 0xFFF8 == 0x4E58:
        return Insn("UNLK", None, "An")
    if op & 0xFFF0 == 0x4E60:
        return Insn("MOVE", "L", "An", "USP") if op & 8 == 0 else Insn("MOVE", "L", "USP", "An")
    if op & 0xFFC0 == 0x4E80:
        return Insn("JSR", None, ea_lo(op))
    if op & 0xFFC0 == 0x4EC0:
        return Insn("JMP", None, ea_lo(op))
    if op & 0xFFF8 == 0x4840:
        return Insn("SWAP", "W", "Dn")
    if op & 0xFFF8 == 0x4848:
        return Insn("BKPT", None, "#imm")
    if op & 0xFFF8 == 0x4880:
        return Insn("EXT", "W", "Dn")
    if op & 0xFFF8 == 0x48C0:
        return Insn("EXT", "L", "Dn")
    if op & 0xFFF8 == 0x49C0:
        return Insn("EXTB", "L", "Dn")
    if op & 0xFFC0 == 0x4800:
        return Insn("NBCD", "B", None, ea_lo(op))
    if op & 0xFFC0 == 0x4840:
        return Insn("PEA", "L", ea_lo(op))
    if op & 0xFB80 == 0x4880:
        size = "L" if op & 0x40 else "W"
        if op & 0x0400:
            return Insn("MOVEM", size, ea_lo(op), "list")
        return Insn("MOVEM", size, "list", ea_lo(op))
    if op & 0xFFC0 == 0x4AC0:
        return Insn("TAS", "B", None, ea_lo(op))
    if op & 0xFFC0 == 0x4C00:
        return Insn("MULx", "L", ea_lo(op), "Dn")
    if op & 0xFFC0 == 0x4C40:
        return Insn("DIVx", "L", ea_lo(op), "Dn")
    if op & 0xFFC0 == 0x40C0:
        return Insn("MOVE", "W", "SR", ea_lo(op))
    if op & 0xFFC0 == 0x42C0:
        return Insn("MOVE", "W", "CCR", ea_lo(op))
    if op & 0xFFC0 == 0x44C0:
        return Insn("MOVE", "W", ea_lo(op), "CCR")
    if op & 0xFFC0 == 0x46C0:
        return Insn("MOVE", "W", ea_lo(op), "SR")
    if op & 0xF1C0 == 0x41C0:
        return Insn("LEA", "L", ea_lo(op), "An")
    if op & 0xF140 == 0x4100:
        return Insn("CHK", "W" if op & 0x80 else "L", ea_lo(op), "Dn")
    sz = (op >> 6) & 3
    if sz != 3:
        name = {0x40: "NEGX", 0x42: "CLR", 0x44: "NEG", 0x46: "NOT", 0x4A: "TST"}.get(op >> 8)
        if name:
            return Insn(name, SZ[sz], None if name != "TST" else ea_lo(op), ea_lo(op) if name != "TST" else None)
    return None


# --- line 5: ADDQ/SUBQ/Scc/DBcc/TRAPcc ------------------------------------------
def _line5(op):
    sz = (op >> 6) & 3
    if sz == 3:
        cc = CC[(op >> 8) & 15]
        if op & 0x38 == 0x08:
            return Insn("DB" + cc, None, "Dn", "disp")
        if op & 0x3F in (0x3A, 0x3B, 0x3C):
            return Insn("TRAP" + cc)
        return Insn("S" + cc, "B", None, ea_lo(op))
    return Insn("SUBQ" if op & 0x100 else "ADDQ", SZ[sz], "#q", ea_lo(op))


def _line6(op):
    c = (op >> 8) & 15
    name = "BRA" if c == 0 else "BSR" if c == 1 else "B" + CC[c]
    d = op & 0xFF
    return Insn(name, {0: "W", 0xFF: "L"}.get(d, "S"))


def _line7(op):
    if op & 0x100:
        return None
    return Insn("MOVEQ", "L", "#q", "Dn")


# --- lines 8, 9, B, C, D: two-operand ALU ----------------------------------------
def _alu(op, name):
    opm = (op >> 6) & 7
    if opm in (0, 1, 2):
        return Insn(name, SZ[opm], ea_lo(op), "Dn")
    return Insn(name, SZ[opm - 4], "Dn", ea_lo(op))


def _line8(op):
    if op & 0xF1C0 == 0x80C0:
        return Insn("DIVU", "W", ea_lo(op), "Dn")
    if op & 0xF1C0 == 0x81C0:
        return Insn("DIVS", "W", ea_lo(op), "Dn")
    if op & 0xF1F0 == 0x8100:
        return Insn("SBCD", "B", "-(An)" if op & 8 else "Dn", "-(An)" if op & 8 else "Dn")
    if op & 0xF1F0 == 0x8140:
        return Insn("PACK", None, "-(An)" if op & 8 else "Dn", "-(An)" if op & 8 else "Dn")
    if op & 0xF1F0 == 0x8180:
        return Insn("UNPK", None, "-(An)" if op & 8 else "Dn", "-(An)" if op & 8 else "Dn")
    return _alu(op, "OR")


def _addsub(op, name):
    opm = (op >> 6) & 7
    if opm in (3, 7):
        return Insn(name + "A", "W" if opm == 3 else "L", ea_lo(op), "An")
    if op & 0x0130 == 0x0100:
        m = "-(An)" if op & 8 else "Dn"
        return Insn(name + "X", SZ[(op >> 6) & 3], m, m)
    return _alu(op, name)


def _lineB(op):
    opm = (op >> 6) & 7
    if opm in (3, 7):
        return Insn("CMPA", "W" if opm == 3 else "L", ea_lo(op), "An")
    if opm in (0, 1, 2):
        return Insn("CMP", SZ[opm], ea_lo(op), "Dn")
    if op & 0x38 == 0x08:
        return Insn("CMPM", SZ[opm - 4], "(An)+", "(An)+")
    return Insn("EOR", SZ[opm - 4], "Dn", ea_lo(op))


def _lineC(op):
    if op & 0xF1C0 == 0xC0C0:
        return Insn("MULU", "W", ea_lo(op), "Dn")
    if op & 0xF1C0 == 0xC1C0:
        return Insn("MULS", "W", ea_lo(op), "Dn")
    if op & 0xF1F0 == 0xC100:
        return Insn("ABCD", "B", "-(An)" if op & 8 else "Dn", "-(An)" if op & 8 else "Dn")
    if op & 0xF1F8 in (0xC140, 0xC148, 0xC188):
        return Insn("EXG", "L", "Rn", "Rn")
    return _alu(op, "AND")


# --- line E: shifts, rotates, bitfields --------------------------------------------
SHIFTS = ["AS", "LS", "ROX", "RO"]
BITFIELD = ["BFTST", "BFEXTU", "BFCHG", "BFEXTS", "BFCLR", "BFFFO", "BFSET", "BFINS"]


def _lineE(op):
    d = "L" if op & 0x100 else "R"
    if op & 0xF8C0 == 0xE8C0:
        return Insn(BITFIELD[(op >> 8) & 7], None, None, ea_lo(op))
    if op & 0xF8C0 == 0xE0C0:
        return Insn(SHIFTS[(op >> 9) & 3] + d, "W", None, ea_lo(op))
    sz = (op >> 6) & 3
    return Insn(SHIFTS[(op >> 3) & 3] + d, SZ[sz], "Dn" if op & 0x20 else "#q", "Dn")


# --- line F: FPU, MMU, cache, MOVE16 ------------------------------------------------
def _lineF(op):
    if op & 0xFFC0 == 0xF200:
        return Insn("FPU-gen", None, None, ea_lo(op))
    if op & 0xFFC0 == 0xF240:
        return Insn("FScc/FDBcc/FTRAPcc")
    if op & 0xFF80 == 0xF280:
        return Insn("FBcc", "L" if op & 0x40 else "W")
    if op & 0xFFC0 == 0xF300:
        return Insn("FSAVE", None, None, ea_lo(op))
    if op & 0xFFC0 == 0xF340:
        return Insn("FRESTORE", None, ea_lo(op))
    if op & 0xFFE0 == 0xF500:
        return Insn("PFLUSH")
    if op & 0xFFD8 == 0xF548:
        return Insn("PTEST")
    if op & 0xFF20 == 0xF400:
        return Insn("CINV")
    if op & 0xFF20 == 0xF420:
        return Insn("CPUSH")
    if op & 0xFFE0 == 0xF600 or op & 0xFFF8 == 0xF620:
        return Insn("MOVE16")
    return Insn("LINE-F")


_LINES = {0: _line0, 1: _move, 2: _move, 3: _move, 4: _line4, 5: _line5, 6: _line6, 7: _line7,
          8: _line8, 9: lambda op: _addsub(op, "SUB"), 0xA: lambda op: Insn("LINE-A"),
          0xB: _lineB, 0xC: _lineC, 0xD: lambda op: _addsub(op, "ADD"), 0xE: _lineE, 0xF: _lineF}


# --- instruction length ---------------------------------------------------------
INDEXED = ("(d8,An,Xn)", "(d8,PC,Xn)")


def _ext(name, size):
    """Extension bytes an effective address adds (brief format for indexed modes)."""
    if name in ("(d16,An)", "(d16,PC)", "abs.W") or name in INDEXED:
        return 2
    if name == "abs.L":
        return 4
    if name == "#imm":
        return 4 if size == "L" else 2
    return 0


def length(op):
    """Return (bytes, exact) for the instruction whose opword is `op`.

    `exact` is False when the length depends on extension words the opword
    does not describe: indexed modes (brief vs full format) and some FPU forms.
    """
    i = decode(op)
    m, sz = i.mnem, i.size
    exact = not (i.src in INDEXED or i.dst in INDEXED)
    fixed = {"MOVEP": 4, "CAS2": 6, "MOVEC": 4, "RTD": 4, "STOP": 4, "PACK": 4, "UNPK": 4, "TRAP": 2, "BKPT": 2,
             "PFLUSH": 2, "PTEST": 2, "CINV": 2, "CPUSH": 2}
    if m in fixed:
        return fixed[m], True
    if m in ("ORI", "ANDI", "EORI") and i.dst in ("CCR", "SR"):
        return 4, True
    if m in ("CMP2/CHK2", "MOVES", "MULx", "DIVx"):
        return 4 + _ext(i.src or i.dst, sz), exact
    if m == "CAS" or m == "MOVEM" or m.startswith("BF"):
        return 4 + _ext(i.dst if m != "MOVEM" or i.src == "list" else i.src, sz), exact
    if m == "LINK":
        return (4 if sz == "W" else 6), True
    if m.startswith("DB"):
        return 4, True
    if m.startswith("TRAP") and m not in ("TRAP", "TRAPV"):
        return {0x3A: 4, 0x3B: 6, 0x3C: 2}.get(op & 0x3F, 2), True
    if m in ("BRA", "BSR") or (m.startswith("B") and sz in ("S", "W", "L") and len(m) <= 3):
        return {"S": 2, "W": 4, "L": 6}[sz], True
    if m == "FPU-gen":
        e = ea_lo(op)
        return 4 + _ext(e, "L"), e != "#imm" and e not in INDEXED
    if m == "FBcc":
        return (4 if sz == "W" else 6), True
    if m == "FScc/FDBcc/FTRAPcc":
        return 4, False
    if m in ("FSAVE", "FRESTORE"):
        return 2 + _ext(i.src or i.dst, None), exact
    if m == "MOVE16":
        return (4 if op & 0xFFF8 == 0xF620 else 6), True
    # static bit ops carry a bit-number word; their EA is byte-sized
    if m in ("BTST", "BCHG", "BCLR", "BSET") and i.src == "#imm":
        return 4 + _ext(i.dst, "B"), exact
    return 2 + _ext(i.src, sz) + _ext(i.dst, sz), exact
