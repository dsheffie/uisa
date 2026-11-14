# PLA flow coverage

Histogram `ns22_boot_desktop.hist`: 262750897 instructions.

| Flow kind | % of instructions |
|---|---|
| pla | 95.45 |
| sequencer | 2.22 |
| millicode | 2.32 |
| unmapped | 0.00 |

PLA and sequencer flows average **1.73 uops per 68k instruction** (MOVEM counted as 5 registers).

| Uops | % of PLA instructions |
|---|---|
| 1 | 64.54 |
| 2 | 15.20 |
| 3 | 9.68 |
| 4 | 6.21 |
| 5 | 2.08 |
| 6+ | 2.29 |

## Top forms

| Form | % | Kind | Uops | Flow |
|---|---|---|---|---|
| `MOVE.L Dn,Dn` | 7.20 | pla | 1 | `mov.32 d0, d3` |
| `BEQ.S` | 3.95 | pla | 1 | `gbcc.eq 8` |
| `MOVE.B (An),Dn` | 3.85 | pla | 2 | `ld.8u.gd x28, 0(a0)`<br>`mov.8 d1, x28` |
| `OR.L Dn,Dn` | 3.80 | pla | 1 | `or.32 d0, d0, d1` |
| `MOVEQ.L #q,Dn` | 3.23 | pla | 1 | `movi.32 d6, 16` |
| `BNE.S` | 2.94 | pla | 1 | `gbcc.ne 8` |
| `MOVEA.L abs.L,An` | 2.91 | pla | 1 | `ld.32.gd a0, {abs.L@2}(x0)` |
| `MOVE.B Dn,Dn` | 2.62 | pla | 1 | `mov.8 d2, d1` |
| `MOVEA.L (d16,An),An` | 2.60 | pla | 1 | `ld.32.gd a0, {d16@2}(a1)` |
| `CMP.L Dn,Dn` | 2.56 | pla | 1 | `cmp.32 d1, d0` |
| `MOVE.L (d16,An),Dn` | 2.52 | pla | 2 | `ld.32.gd x28, {d16@2}(a0)`<br>`mov.32 d0, x28` |
| `CLR.L Dn` | 2.50 | pla | 1 | `mov.32 d1, x0` |
| `RTS` | 1.95 | pla | 3 | `ld.32.gd x27, 0(a7)`<br>`addi a7, a7, 4`<br>`gjr x27` |
| `ADDQ.L #q,Dn` | 1.79 | pla | 1 | `addi.32 d1, d1, 1` |
| `ASL.L #q,Dn` | 1.76 | pla | 1 | `asli.32 d1, d1, 8` |
| `BSR.L` | 1.71 | pla | 4 | `gpcaddi x27, 6`<br>`addi a7, a7, -4`<br>`st.32.gd x27, 0(a7)`<br>`gbcc.t {br.L@2}` |
| `ASL.L Dn,Dn` | 1.48 | pla | 1 | `asl.32 d0, d0, d6` |
| `BTST #imm,Dn` | 1.46 | pla | 1 | `btsti.32 d0, {imm.W@2}` |
| `ANDI.L #imm,Dn` | 1.46 | pla | 1 | `andi.32 d1, d1, {imm.L@2}` |
| `MOVE.B (d8,An,Xn),Dn` | 1.45 | pla | 4 | `sext16 x30, {xn@2}`<br>`sh2add x30, a0, x30`<br>`ld.8u.gd x28, {d8@2}(x30)`<br>`mov.8 d1, x28` |
| `LINK.W An,#imm` | 1.41 | pla | 4 | `addi a7, a7, -4`<br>`st.32.gd a6, 0(a7)`<br>`add a6, a7, x0`<br>`addi a7, a7, {imm.W@2}` |
| `UNLK An` | 1.41 | pla | 3 | `add a7, a6, x0`<br>`ld.32.gd a6, 0(a7)`<br>`addi a7, a7, 4` |
| `BHI.S` | 1.41 | pla | 1 | `gbcc.hi -78` |
| `EOR.L Dn,Dn` | 1.39 | pla | 1 | `eor.32 d0, d0, d1` |
| `SUB.L Dn,Dn` | 1.35 | pla | 1 | `sub.32 d0, d0, d4` |
| `MOVE.L #imm,Dn` | 1.24 | pla | 1 | `movi.32 d2, {imm.L@2}` |
| `MOVEA.L Dn,An` | 1.24 | pla | 1 | `add a1, d0, x0` |
| `MOVE.L (An),Dn` | 1.21 | pla | 2 | `ld.32.gd x28, 0(a3)`<br>`mov.32 d3, x28` |
| `CLR.L (An)+` | 1.15 | pla | 3 | `mov.32 x0, x0`<br>`st.32.gd x0, 0(a0)`<br>`addi a0, a0, 4` |
| `MOVEM.L list,-(An)` | 1.11 | sequencer | 6 | hardware sequencer: one load or store per register in the mask plus the An update; counted with movem_regs registers |
| `MOVE.L -(An),-(An)` | 1.10 | pla | 5 | `addi a0, a0, -4`<br>`ld.32.gd x28, 0(a0)`<br>`addi a1, a1, -4`<br>`mov.32 x0, x28`<br>`st.32.gd x28, 0(a1)` |
| `MOVEM.L (d16,An),list` | 1.06 | sequencer | 6 | hardware sequencer: one load or store per register in the mask plus the An update; counted with movem_regs registers |
| `BGE.S` | 1.02 | pla | 1 | `gbcc.ge -22` |
| `BRA.S` | 0.88 | pla | 1 | `gbcc.t 6` |
| `MOVE.L Dn,(d16,An)` | 0.86 | pla | 2 | `mov.32 x0, d3`<br>`st.32.gd d3, {d16@2}(a1)` |
| `MOVE.L Dn,-(An)` | 0.74 | pla | 3 | `addi a7, a7, -4`<br>`mov.32 x0, d2`<br>`st.32.gd d2, 0(a7)` |
| `MOVE.L (An)+,(An)+` | 0.73 | pla | 5 | `ld.32.gd x28, 0(a0)`<br>`mov.32 x0, x28`<br>`st.32.gd x28, 0(a1)`<br>`addi a1, a1, 4`<br>`addi a0, a0, 4` |
| `MOVEA.L (An),An` | 0.70 | pla | 1 | `ld.32.gd a0, 0(a0)` |
| `ADDQ.W #q,An` | 0.65 | pla | 1 | `addi a7, a7, 4` |
| `LEA.L abs.L,An` | 0.62 | pla | 1 | `addi a0, x0, {abs.L@2}` |
| `TST.L Dn` | 0.61 | pla | 1 | `mov.32 x0, d0` |
| `SUBQ.L #q,Dn` | 0.61 | pla | 1 | `subi.32 d0, d0, 1` |
| `BGT.S` | 0.59 | pla | 1 | `gbcc.gt -12` |
| `ADDI.L #imm,Dn` | 0.48 | pla | 1 | `addi.32 d3, d3, {imm.L@2}` |
| `MOVE.L An,-(An)` | 0.48 | pla | 3 | `addi a7, a7, -4`<br>`mov.32 x0, a2`<br>`st.32.gd a2, 0(a7)` |
