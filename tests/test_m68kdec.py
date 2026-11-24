#!/usr/bin/env python3
"""Spot-check the 68k opword decoder against hand-assembled encodings."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import m68kdec  # noqa: E402

CASES = {
    0x4E75: "RTS",
    0x2018: "MOVE.L (An)+,Dn",
    0x3E3C: "MOVE.W #imm,Dn",
    0x204F: "MOVEA.L An,An",
    0x2F00: "MOVE.L Dn,-(An)",
    0x4CDF: "MOVEM.L (An)+,list",
    0x48E7: "MOVEM.L list,-(An)",
    0xD080: "ADD.L Dn,Dn",
    0xD1C8: "ADDA.L An,An",
    0xD388: "ADDX.L -(An),-(An)",
    0x4E56: "LINK.W An,#imm",
    0x4E5E: "UNLK An",
    0x6700: "BEQ.W",
    0x66FE: "BNE.S",
    0x61FF: "BSR.L",
    0x51C8: "DBF Dn,disp",
    0x57C0: "SEQ.B Dn",
    0x7001: "MOVEQ.L #q,Dn",
    0xE188: "LSL.L #q,Dn",
    0xE2A8: "LSR.L Dn,Dn",
    0xE3D0: "LSL.W (An)",
    0xE9C0: "BFEXTU Dn",
    0x41E8: "LEA.L (d16,An),An",
    0x41FB: "LEA.L (d8,PC,Xn),An",
    0xB0B9: "CMP.L abs.L,Dn",
    0xB308: "CMPM.B (An)+,(An)+",
    0xB141: "EOR.W Dn,Dn",
    0x4A80: "TST.L Dn",
    0x4AFC: "ILLEGAL",
    0x0C80: "CMPI.L #imm,Dn",
    0x0800: "BTST #imm,Dn",
    0x0110: "BTST Dn,(An)",
    0x0188: "MOVEP.W Dn,(d16,An)",
    0x0ED0: "CAS.L Dc,Du,(An)",
    0x4E7A: "MOVEC.L Rc,Rn",
    0x4E73: "RTE",
    0x4880: "EXT.W Dn",
    0x49C0: "EXTB.L Dn",
    0x4840: "SWAP.W Dn",
    0x4850: "PEA.L (An)",
    0x4C00: "MULx.L Dn,Dn",
    0xC0C1: "MULU.W Dn,Dn",
    0x81C1: "DIVS.W Dn,Dn",
    0xC101: "ABCD.B Dn,Dn",
    0xC141: "EXG.L Rn,Rn",
    0x4280: "CLR.L Dn",
    0x4E90: "JSR (An)",
    0x5280: "ADDQ.L #q,Dn",
    0x5388: "SUBQ.L #q,An",
    0xF280: "FBcc.W",
    0xF200: "FPU-gen Dn",
    0xF518: "PFLUSH",
    0xF4F8: "CPUSH",
    0xA000: "LINE-A",
    0x46FC: "MOVE.W #imm,SR",
    0x40C0: "MOVE.W SR,Dn",
}


LENGTHS = {
    0x4E75: 2, 0x2018: 2, 0x3E3C: 4, 0x203C: 6, 0x41E8: 4, 0xB0B9: 6, 0x6700: 4, 0x66FE: 2,
    0x61FF: 6, 0x51C8: 4, 0x4CDF: 4, 0x4E56: 4, 0x4808: 6, 0x0C80: 6, 0x0800: 4, 0x46FC: 4,
    0x4C00: 4, 0x23C8: 6, 0x2B6E: 6, 0xE9C0: 4, 0x0EF9: 8, 0xF280: 4, 0x0CFC: 6, 0x4E7A: 4,
}


def main():
    fails = 0
    for op, want in LENGTHS.items():
        got = m68kdec.length(op)[0]
        if got != want:
            print("%04x length: got %d want %d" % (op, got, want))
            fails += 1
    for op, want in CASES.items():
        got = m68kdec.decode(op).form()
        if got != want:
            print("%04x: got %r want %r" % (op, got, want))
            fails += 1
    print("test_m68kdec: %d cases, %d failures" % (len(CASES) + len(LENGTHS), fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
