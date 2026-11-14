# uisa: a micro-ISA for a 68040 micro-engine

Status: draft 0.1. This document has two parts. The first part (this
chapter) is hand-written rationale. The second part, *Reference*, is generated
from `spec/uisa.yaml`, which is the only place the ISA is defined.

## Goal

Run 68040 software, NeXTSTEP in particular, on an out-of-order core derived
from superdupersparc. The core is a 32-bit renamed machine with flags; it
executes only this micro-ISA. Guest instructions reach it two ways:

- **PLA path.** Common 68k instructions are length-decoded in hardware and
  translated by PLAs directly into internal uops. Those uops never have an
  encoding; they use the same operations described here.
- **Millicode path.** Everything else (complex addressing modes, MOVEM,
  bitfields, BCD, exceptions, MMU and cache control, floating point) jumps to
  millicode: ordinary micro-ISA code stored in hidden memory, written in a
  C subset and compiled by our own compiler.

Bring-up starts with *software decode*: a millicode interpreter fetches guest
instruction bytes with guest-program-space loads and dispatches on the opword.
The PLA path is then added for the instructions profiles say matter.

## Design principles

1. **A clean RISC base.** Fixed 32-bit instructions, load/store, three-operand
   ALU ops, no delay slots, no register windows, no condition codes on the
   base ops. Millicode's own control flow uses compare-and-branch.
2. **Guest semantics as explicit operations.** Everything the 68k does that a
   RISC does not (sized operands, the five flags, X, sticky Z, BCD, rotate
   through X, address spaces with function codes) is a first-class operation,
   so neither the PLA nor the compiler emulates it with long sequences.
3. **Nothing implicit that renaming has to undo.** Partial register writes,
   partial flag writes and data-dependent register numbers each cost a source
   operand or a serialization in a renamed machine. The ISA makes each of those
   visible at the instruction level (merge forms, the split X/NZVC flag
   resources, `rdg`/`wrg`) so the hardware never discovers them late.
4. **Everything machine-readable.** Decoders, the ISS, the assembler and the
   reference chapters of this document are generated from one YAML file.

## Register namespace

Register specifiers are 6 bits. x0-x31 belong to millicode (x0 is zero); the
guest's D0-D7, A0-A7, PC and SR system byte have their own architectural
indices. This keeps all 31 usable x registers available to millicode, which is
stackless: a handler whose live values exceed 31 registers fails to compile
rather than spilling. Guest registers are reachable from every instruction,
so the PLA's uops and millicode name them the same way.

A guest register whose number is only known at run time (an extension-word
index register, MOVEM, the `Dr:Dq` pair of 64-bit multiply/divide) goes
through `rdg`/`wrg`. Those serialize at rename, so they are expected to be
rare. Per-opword handler specialization keeps most register numbers static.

## Flags

The 68k flags are two renamed resources: NZVC and X. Splitting X out means the
many instructions that leave X alone (CMP, the logical ops, MOVE, rotates) do
not depend on whichever older instruction last wrote X. Every flag-writing op
writes all of NZVC, so NZVC never needs a merge. Ops that use X or sticky Z
(ADDX, SUBX, NEGX, ROXL/ROXR, ABCD/SBCD/NBCD) read the old flags explicitly,
and so do shifts, because a zero shift count leaves X unchanged.

Every guest op has a variant (`.nf`) that writes no flags. Most 68k flag
results are overwritten before anything reads them, so the PLA and the
compiler emit `.nf` whenever the flags are dead.

## Sized operations

A renamed machine always allocates a new physical register for the
destination. A byte or word operation must therefore copy the upper bits of
the old destination into the new one: the merge is unavoidable. The ISA makes
it explicit:

- `add.8 d0, d1` is two-address: the old destination arrives on the rs1 port
  (rs1 must equal rd), the operation runs at 8 bits, flags are taken at bit 7,
  and the result is merged into the low byte. One instruction, two register
  reads, no third port.
- `add.8z x5, x6, x7` is the zero-fill form for millicode temporaries whose
  upper bits are dead. It does not read the old destination, so it does not
  wait for whatever last wrote that register.
- `add.32` is ordinary three-address.

Sized ops operate on the low n bits of their sources, so right shifts,
rotates and rotates through X need no separate extension. Multiply and divide
by word produce full 32-bit results (`mulu.16`, `divu.16`), matching the 68k.

## Memory and address spaces

All memory is big-endian. Each load and store names an address space:
hidden (physical, untranslated: millicode, its data, devices), guest data,
guest program, or guest alternate (function code from SFC/DFC, for MOVES).
The guest MMU can never produce a hidden address. Guest accesses may be
misaligned; hardware may split them or take an invisible millicode assist.

TAS, CAS and CAS2 use locked sequences: `ldl.*` begins one, `stu.*` or
`unlock` ends it. They execute non-speculatively at the head of the ROB. The
machine is single-core, so locking only has to exclude DMA.

`probe.r`/`probe.w` translate without accessing and return a status instead
of faulting. They build PTEST and let millicode check every address it will
touch before it changes any guest state.

## Millicode rules

1. **Stackless.** No memory stack and no spilling. Calls use a hardware return
   stack (`call`/`ret`), depth set by `call_stack_depth`; overflow is an
   internal millicode fault. The compiler allocates registers across the whole
   call graph, which is acyclic because recursion is not allowed. Explicit
   scratch storage in hidden memory is allowed for state that is genuinely
   larger than the register file (MOVEM, exception frames, FSAVE).
2. **Faults before commits.** A guest-space access can fault, and a fault
   restarts the guest instruction from the beginning. Millicode therefore
   performs (or probes) every access that can fault before its first write to
   guest state. The compiler checks this on its SSA form. PLA flows are short
   enough to retire atomically.
3. **No state across gexit.** x registers are dead after `gexit`. Millicode
   state that must persist lives in CSRs or hidden memory.
4. **Interrupts only at guest boundaries.** Millicode runs to completion.

## Entry and exit

The hardware decoder enters millicode with the opword, up to four extension
words and the next sequential PC already in registers (see *Millicode entry
points*). At entry `gpc` holds the instruction's own address; millicode writes
the next PC to `gpc` and executes `gexit`, which retires the guest
instruction, checks interrupts and trace, and resumes the guest stream.

`gfault`/`gfaultr` raise a guest exception: control transfers to the
exception entry point, which is itself millicode and builds the 68k stack
frame.

## Open questions

- Hidden memory base and size, and whether hot handlers live in on-chip RAM.
- Whether the 68040 table walk is hardware (from the rv64core/superdupersparc
  walker) or a millicode TLB refill using `probe` and a TLB-write operation
  not yet defined here.
- The FP extension (majors 48-55 are reserved). Start with SoftFloat-style
  extended precision in millicode; add hardware double precision if NeXTSTEP
  profiles need it.
- Exact N and V results of the BCD ops, which the 68040 leaves undefined;
  match the reference model (Previous) once co-simulation runs.
- Whether 16-bit displacements in the PLA's uops need a matching millicode
  encoding (the M format has a 12-bit displacement).

## Regenerating

```
make            # validate the spec, regenerate build/ and doc/uisa.md, run the tests
```

Edit `spec/uisa.yaml` (the ISA) or `doc/intro.md` (the rationale); never edit
`build/` or `doc/uisa.md`. The generated doc is committed so it can be read on
GitHub; commit it together with the spec change that produced it.

<!-- Everything below is generated from spec/uisa.yaml by tools/gen.py. -->

# Reference (generated)

Spec version **0.1-draft**. 560 concrete instruction variants.

## Parameters

| Parameter | Value |
|---|---|
| `xlen` | 32 |
| `ilen` | 32 |
| `endian` | big |
| `call_stack_depth` | 4 |
| `reg_spec_bits` | 6 |
| `hidden_base` | TBD |
| `hidden_size` | TBD |

## Register namespace

Register specifiers are 6 bits. Indices not listed are reserved.

| Index | Names | Description |
|---|---|---|
| 0-31 | `x0`-`x31` | Millicode general registers. x0 reads as zero and ignores writes. Not preserved across gexit. |
| 32-39 | `d0`-`d7` | Guest data registers D0-D7. |
| 40-47 | `a0`-`a7` | Guest address registers A0-A7. a7 is the active stack pointer; the inactive USP/ISP/MSP live in CSRs. |
| 48 | `gpc` | Guest PC. Holds the current instruction's address on millicode entry; millicode writes the next PC before gexit. |
| 49 | `gsr` | Guest SR system byte (T1 T0 S M - I2 I1 I0) in bits 15:8. The CCR byte lives in the renamed flags, not here. |

## Flags

Two renamed flag resources: `nzvc` (N Z V C), `x` (X).
`rdccr`/`wrccr` use the 68k CCR layout: X=bit 4, N=bit 3, Z=bit 2, V=bit 1, C=bit 0.

## Formats

| Format | Fields (msb to lsb) | Immediate |
|---|---|---|
| R | `op[31:26]` `rd[25:20]` `rs1[19:14]` `rs2[13:8]` `fn[7:0]` |  |
| I | `op[31:26]` `rd[25:20]` `rs1[19:14]` `imm[13:0]` | signed |
| U | `op[31:26]` `rd[25:20]` `imm[19:0]` | unsigned |
| J | `op[31:26]` `rd[25:20]` `imm[19:0]` | signed, x4 |
| L | `op[31:26]` `imm[25:0]` | signed, x4 |
| B | `op[31:26]` `rs1[25:20]` `rs2[19:14]` `cond[13:11]` `imm[10:0]` | signed, x4 |
| C | `op[31:26]` `cond[25:22]` `imm[21:0]` | signed, x4 |
| G | `op[31:26]` `rd[25:20]` `rs1[19:14]` `rs2[13:8]` `fn[7:3]` `sz[2:1]` `f[0:0]` |  |
| GI | `op[31:26]` `rd[25:20]` `rs1[19:14]` `fn[13:9]` `sz[8:7]` `f[6:6]` `imm[5:0]` | unsigned |
| M | `op[31:26]` `rd[25:20]` `rs1[19:14]` `sp[13:12]` `imm[11:0]` | signed |
| S | `op[31:26]` `rd[25:20]` `rs1[19:14]` `fn[13:8]` `imm[7:0]` | unsigned |
| BFI | `op[31:26]` `rd[25:20]` `rs1[19:14]` `fn[13:12]` `f[11:11]` `off[10:6]` `wid[5:1]` `rsv[0:0]` |  |

Base ops can override immediate signedness (e.g. logical immediates are zero-extended).

## Major opcode map

| | +0 | +1 | +2 | +3 | +4 | +5 | +6 | +7 |
|---|---|---|---|---|---|---|---|---|
| **0** | ALU | ADDI | ANDI | ORI | XORI | SLTI | SLTIU | SLLI |
| **8** | SRLI | SRAI | RORI | LUI | AUIPC | JAL | JALR | CALL |
| **16** | BR | BCC | GOP | GOPZ | GOPI | GOPIZ | LD8U | LD8S |
| **24** | LD16U | LD16S | LD32 | ST8 | ST16 | ST32 | LDL8 | LDL16 |
| **32** | LDL32 | STU8 | STU16 | STU32 | PROBER | PROBEW | SYS | BF |
| **40** | BFI | - | - | - | - | - | - | - |
| **48** | *FP* | *FP* | *FP* | *FP* | *FP* | *FP* | *FP* | *FP* |
| **56** | - | - | - | - | - | - | - | - |

## Base operations

No flags, full 32-bit results, unless the row says otherwise. Operand names: `a` = rs1, `b` = rs2 or the decoded immediate.

| Mnemonic | Format | Encoding | Operands | Semantics | Notes |
|---|---|---|---|---|---|
| `add` | R | ALU fn=0 | rd, rs1, rs2 | `r = a + b;` |  |
| `sub` | R | ALU fn=1 | rd, rs1, rs2 | `r = a - b;` |  |
| `and` | R | ALU fn=2 | rd, rs1, rs2 | `r = a & b;` |  |
| `or` | R | ALU fn=3 | rd, rs1, rs2 | `r = a \| b;` |  |
| `xor` | R | ALU fn=4 | rd, rs1, rs2 | `r = a ^ b;` |  |
| `andn` | R | ALU fn=5 | rd, rs1, rs2 | `r = a & ~b;` |  |
| `orn` | R | ALU fn=6 | rd, rs1, rs2 | `r = a \| ~b;` |  |
| `sll` | R | ALU fn=7 | rd, rs1, rs2 | `r = a << (b & 31);` |  |
| `srl` | R | ALU fn=8 | rd, rs1, rs2 | `r = a >> (b & 31);` |  |
| `sra` | R | ALU fn=9 | rd, rs1, rs2 | `r = (uint32_t)((int32_t)a >> (b & 31));` |  |
| `rol` | R | ALU fn=10 | rd, rs1, rs2 | `r = ROL32(a, b & 31);` |  |
| `ror` | R | ALU fn=11 | rd, rs1, rs2 | `r = ROL32(a, (32 - (b & 31)) & 31);` |  |
| `slt` | R | ALU fn=12 | rd, rs1, rs2 | `r = (int32_t)a < (int32_t)b;` |  |
| `sltu` | R | ALU fn=13 | rd, rs1, rs2 | `r = a < b;` |  |
| `min` | R | ALU fn=14 | rd, rs1, rs2 | `r = (int32_t)a < (int32_t)b ? a : b;` |  |
| `max` | R | ALU fn=15 | rd, rs1, rs2 | `r = (int32_t)a > (int32_t)b ? a : b;` |  |
| `minu` | R | ALU fn=16 | rd, rs1, rs2 | `r = a < b ? a : b;` |  |
| `maxu` | R | ALU fn=17 | rd, rs1, rs2 | `r = a > b ? a : b;` |  |
| `mul` | R | ALU fn=18 | rd, rs1, rs2 | `r = a * b;` |  |
| `mulh` | R | ALU fn=19 | rd, rs1, rs2 | `r = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32);` |  |
| `mulhu` | R | ALU fn=20 | rd, rs1, rs2 | `r = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);` |  |
| `div` | R | ALU fn=21 | rd, rs1, rs2 | `r = (b == 0) ? 0xffffffffu : ((a == 0x80000000u && b == 0xffffffffu) ? a : (uint32_t)((int32_t)a / (int32_t)b));` | Never faults. Division by zero returns all ones. |
| `divu` | R | ALU fn=22 | rd, rs1, rs2 | `r = (b == 0) ? 0xffffffffu : a / b;` | Never faults. |
| `rem` | R | ALU fn=23 | rd, rs1, rs2 | `r = (b == 0) ? a : ((a == 0x80000000u && b == 0xffffffffu) ? 0 : (uint32_t)((int32_t)a % (int32_t)b));` |  |
| `remu` | R | ALU fn=24 | rd, rs1, rs2 | `r = (b == 0) ? a : a % b;` |  |
| `clz` | R | ALU fn=25 | rd, rs1 | `r = a ? __builtin_clz(a) : 32;` |  |
| `ctz` | R | ALU fn=26 | rd, rs1 | `r = a ? __builtin_ctz(a) : 32;` |  |
| `cpop` | R | ALU fn=27 | rd, rs1 | `r = __builtin_popcount(a);` |  |
| `rev8` | R | ALU fn=28 | rd, rs1 | `r = __builtin_bswap32(a);` |  |
| `sext8` | R | ALU fn=29 | rd, rs1 | `r = (uint32_t)SX(a, 8);` |  |
| `sext16` | R | ALU fn=30 | rd, rs1 | `r = (uint32_t)SX(a, 16);` |  |
| `zext8` | R | ALU fn=31 | rd, rs1 | `r = a & 0xffu;` |  |
| `zext16` | R | ALU fn=32 | rd, rs1 | `r = a & 0xffffu;` |  |
| `czeqz` | R | ALU fn=33 | rd, rs1, rs2 | `r = (b == 0) ? 0 : a;` | Conditional zero (branch-free select building block). |
| `cznez` | R | ALU fn=34 | rd, rs1, rs2 | `r = (b != 0) ? 0 : a;` |  |
| `mrg8` | R | ALU fn=35 | rd, rs1, rs2 | `r = (a & ~0xffu) \| (b & 0xffu);` | Explicit byte merge: upper 24 bits from rs1, low byte from rs2. |
| `mrg16` | R | ALU fn=36 | rd, rs1, rs2 | `r = (a & ~0xffffu) \| (b & 0xffffu);` |  |
| `rdccr` | R | ALU fn=37 | rd | `r = CCR_PACK(xin, nin, zin, vin, cin);` | Copy the renamed flags into a register (CCR layout). (reads nzvc+x) |
| `wrccr` | R | ALU fn=38 | rs1 | `CCR_UNPACK(a, X, N, Z, V, C);` | Load the renamed flags from a register (CCR layout). (writes nzvc+x) |
| `rdg` | R | ALU fn=39 | rd, rs1 | `r = GREG_READ(a & 31);` | Read guest register g[rs1 & 31] (namespace index 32 + n). Index is data, so rename serializes. (serializes) |
| `wrg` | R | ALU fn=40 | rs1, rs2 | `GREG_WRITE(a & 31, b);` | Write rs2 to guest register g[rs1 & 31]. Serializes. (serializes) |
| `bfmask` | R | ALU fn=41 | rd, rs2 | `r = BF_MASK(b);` | Mask of the 68k bitfield described by rs2 (offset in bits 4:0 from the MSB, width in bits 9:5, 0 means 32), wrapping like a register bitfield. |
| `sel.<cc>` | R | ALU fn=0x40+cc | rd, rs1, rs2 | `r = COND ? a : b;` | rd = cond ? rs1 : rs2, on the guest condition. cc: t f hi ls cc cs ne eq vc vs pl mi ge lt gt le. (reads nzvc) |
| `s.<cc>` | R | ALU fn=0x50+cc | rd | `r = COND ? 0xffffffffu : 0;` | rd = cond ? all ones : 0 (Scc building block; follow with mov.8 to merge). cc: t f hi ls cc cs ne eq vc vs pl mi ge lt gt le. (reads nzvc) |
| `addi` | I | ADDI | rd, rs1, imm | `r = a + b;` |  |
| `andi` | I | ANDI | rd, rs1, imm | `r = a & b;` | Immediate is zero-extended. |
| `ori` | I | ORI | rd, rs1, imm | `r = a \| b;` | Immediate is zero-extended. |
| `xori` | I | XORI | rd, rs1, imm | `r = a ^ b;` | Immediate is zero-extended. |
| `slti` | I | SLTI | rd, rs1, imm | `r = (int32_t)a < (int32_t)b;` |  |
| `sltiu` | I | SLTIU | rd, rs1, imm | `r = a < b;` |  |
| `slli` | I | SLLI | rd, rs1, imm | `r = a << (b & 31);` |  |
| `srli` | I | SRLI | rd, rs1, imm | `r = a >> (b & 31);` |  |
| `srai` | I | SRAI | rd, rs1, imm | `r = (uint32_t)((int32_t)a >> (b & 31));` |  |
| `rori` | I | RORI | rd, rs1, imm | `r = ROL32(a, (32 - (b & 31)) & 31);` |  |
| `lui` | U | LUI | rd, imm | `r = b << 12;` |  |
| `auipc` | U | AUIPC | rd, imm | `r = pc + (b << 12);` |  |
| `jal` | J | JAL | rd, imm | `r = pc + 4; npc = pc + b;` |  |
| `jalr` | I | JALR | rd, rs1, imm | `r = pc + 4; npc = (a + b) & ~3u;` |  |
| `call` | L | CALL | imm | `CSTACK_PUSH(pc + 4); npc = pc + b;` | Push the return address on the hardware millicode return stack. Overflow is a millicode fault. |
| `b.<cc>` | B | BR cond=cc | rs1, rs2, imm | `if (COND) npc = pc + b;` | Compare two registers and branch. Millicode control flow; never reads guest flags. cc: eq ne lt ge ltu geu. |
| `bcc.<cc>` | C | BCC cond=cc | imm | `if (COND) npc = pc + b;` | Branch on a guest condition. cc: t f hi ls cc cs ne eq vc vs pl mi ge lt gt le. (reads nzvc) |
| `bfextu` | R | BF fn=0 | rd, rs1, rs2 | `r = BF_EXTU(a, b);` | Extract the field described by rs2 (offset 4:0, width 9:5), zero-extended. |
| `bfexts` | R | BF fn=1 | rd, rs1, rs2 | `r = BF_EXTS(a, b);` |  |
| `bfffo` | R | BF fn=2 | rd, rs1, rs2 | `r = BF_FFO(a, b);` | Offset of the first 1 bit in the field, or offset+width if none (68k BFFFO). |
| `bfextui` | BFI | BFI fn=0 | rd, rs1, off, wid | `r = BF_EXTU(a, b);` |  |
| `bfextsi` | BFI | BFI fn=1 | rd, rs1, off, wid | `r = BF_EXTS(a, b);` |  |
| `bfinsi` | BFI | BFI fn=2 | rd, rs1, off, wid | `r = BF_INS(OLD, a, b);` | Insert the low wid bits of rs1 into the field of rd. rd is also a source (old value), so the op reads rd and rs1. |
| `gexit` | S | SYS fn=0 |  |  | End of the guest instruction: retire it, take pending interrupts/trace, resume the guest stream at gpc. |
| `gfault` | S | SYS fn=1 | imm |  | Raise guest exception vector imm8. Transfers to the exception entry point. |
| `gfaultr` | S | SYS fn=16 | rs1 |  | Raise the guest exception whose vector is in rs1 (TRAP #n, computed vectors). |
| `ret` | S | SYS fn=2 |  | `npc = CSTACK_POP();` | Return through the hardware millicode return stack. Underflow is a millicode fault. |
| `csrr` | S | SYS fn=3 | rd, csr | `r = CSR_READ(IMM);` |  |
| `csrw` | S | SYS fn=4 | csr, rs1 | `CSR_WRITE(IMM, a);` | (serializes) |
| `csrs` | S | SYS fn=5 | csr, rs1 | `CSR_WRITE(IMM, CSR_READ(IMM) \| a);` | (serializes) |
| `csrc` | S | SYS fn=6 | csr, rs1 | `CSR_WRITE(IMM, CSR_READ(IMM) & ~a);` | (serializes) |
| `unlock` | S | SYS fn=7 |  |  | End a locked sequence without a store (failed CAS compare). (serializes) |
| `fence` | S | SYS fn=8 |  |  | Order all earlier memory accesses before all later ones. (serializes) |
| `tlbflush` | S | SYS fn=9 | rs1, imm |  | Guest TLB flush. imm bit0: page (address in rs1) vs all; bit1: non-global entries only; bit2: user-only (FC). Covers PFLUSH/PFLUSHN/PFLUSHA. (serializes) |
| `cpush` | S | SYS fn=10 | rs1, imm |  | Write back and invalidate guest cache lines. imm bits1:0 cache (1=D, 2=I, 3=both); bits3:2 scope (0=line, 1=page, 2=all). Address in rs1. (serializes) |
| `cinv` | S | SYS fn=11 | rs1, imm |  | Invalidate without write back. Same imm encoding as cpush. (serializes) |
| `irqack` | S | SYS fn=12 | rd, rs1 |  | Acknowledge the interrupt at level rs1; rd = vector (autovector if none supplied). (serializes) |
| `stop` | S | SYS fn=13 |  |  | Wait for an interrupt (68k STOP, after millicode has loaded SR). (serializes) |
| `halt` | S | SYS fn=14 | imm |  | Stop the machine (simulation / debug). imm is a status code. (serializes) |
| `brk` | S | SYS fn=15 | imm |  | Debug breakpoint. (serializes) |

## Guest-semantic operations

Each family expands into sized forms (see *Sized operations* above). `<f>.8`/`<f>.16` merge into rd and are two-address (rs1 = rd); `<f>.32` is three-address; `<f>.8z`/`<f>.16z` zero-fill and do not read the old rd. Families marked *imm* also have `<f>i.<sz>[z]` forms with a 6-bit unsigned immediate. Every form has a `.nf` variant that writes no flags.

| Family | fn | Kind | Sizes | imm | Reads | Writes | Notes |
|---|---|---|---|---|---|---|---|
| `add` | 0 | binary | 8,16,32 | yes | - | N Z V C X |  |
| `addx` | 1 | binary | 8,16,32 |  | X Z | N Z V C X | Z is sticky: cleared if the result is nonzero, otherwise unchanged. |
| `sub` | 2 | binary | 8,16,32 | yes | - | N Z V C X |  |
| `subx` | 3 | binary | 8,16,32 |  | X Z | N Z V C X |  |
| `neg` | 4 | unary | 8,16,32 |  | - | N Z V C X |  |
| `negx` | 5 | unary | 8,16,32 |  | X Z | N Z V C X |  |
| `cmp` | 6 | compare | 8,16,32 | yes | - | N Z V C | Flags of rs1 - rs2. X unaffected. |
| `and` | 7 | binary | 8,16,32 | yes | - | N Z V C |  |
| `or` | 8 | binary | 8,16,32 | yes | - | N Z V C |  |
| `eor` | 9 | binary | 8,16,32 | yes | - | N Z V C |  |
| `not` | 10 | unary | 8,16,32 |  | - | N Z V C |  |
| `mov` | 11 | unary | 8,16,32 |  | - | N Z V C | MOVE / TST. mov.8z x0, rs gives TST.B. |
| `lsl` | 12 | binary | 8,16,32 | yes | X | N Z V C X | Count is rs2 mod 64. A zero count clears C and leaves X; X is therefore always read. |
| `lsr` | 13 | binary | 8,16,32 | yes | X | N Z V C X |  |
| `asl` | 14 | binary | 8,16,32 | yes | X | N Z V C X | V is set if the most significant bit changes at any time during the shift. |
| `asr` | 15 | binary | 8,16,32 | yes | X | N Z V C X |  |
| `rol` | 16 | binary | 8,16,32 | yes | - | N Z V C | X unaffected. |
| `ror` | 17 | binary | 8,16,32 | yes | - | N Z V C |  |
| `roxl` | 18 | binary | 8,16,32 | yes | X | N Z V C X | Rotate through X (n+1 bits). A zero count sets C to X. |
| `roxr` | 19 | binary | 8,16,32 | yes | X | N Z V C X |  |
| `abcd` | 20 | binary | 8 |  | X Z | N Z V C X | Packed BCD add with X. N and V are undefined on the 68040; the values here are placeholders to be matched to the reference model. |
| `sbcd` | 21 | binary | 8 |  | X Z | N Z V C X | Packed BCD rs1 - rs2 - X. N and V undefined (placeholders). |
| `nbcd` | 22 | unary | 8 |  | X Z | N Z V C X |  |
| `mulu` | 23 | full | 16,32 |  | - | N Z V C | .16: 16x16->32 (MULU.W). .32: low 32 bits, V on overflow (MULU.L 32-bit result). High half: base mulhu. |
| `muls` | 24 | full | 16,32 |  | - | N Z V C |  |
| `divu` | 25 | tiedfull | 16,32 |  | - | N Z V C | .16: 32/16 -> remainder:quotient (DIVU.W); on overflow rd is unchanged and V is set. .32: quotient only; remainder via base remu. Precondition: divisor != 0 (millicode checks and raises the zero-divide exception first) |
| `divs` | 26 | tiedfull | 16,32 |  | - | N Z V C | Precondition: divisor != 0 |
| `ext` | 27 | unary | 16,32 |  | - | N Z V C | .16: EXT.W (byte to word, merged). .32: EXT.L (word to long). |
| `extb` | 28 | unary | 32 |  | - | N Z V C | EXTB.L (byte to long). |

### Guest-op semantics

C reference semantics, exactly as compiled into the generated ISS header. `n` = operand width, `m` = MSK(n), `xin`..`cin` = incoming flags.

**`add`**

```c
uint64_t s = (uint64_t)(a & m) + (b & m);
r = (uint32_t)s & m;
C = (s >> n) & 1; X = C;
V = MSB((a ^ r) & (b ^ r), n);
N = MSB(r, n); Z = (r == 0);
```

**`addx`**

```c
uint64_t s = (uint64_t)(a & m) + (b & m) + xin;
r = (uint32_t)s & m;
C = (s >> n) & 1; X = C;
V = MSB((a ^ r) & (b ^ r), n);
N = MSB(r, n); Z = zin && (r == 0);
```

**`sub`**

```c
r = (a - b) & m;
C = (b & m) > (a & m); X = C;
V = MSB((a ^ b) & (a ^ r), n);
N = MSB(r, n); Z = (r == 0);
```

**`subx`**

```c
r = (a - b - xin) & m;
C = (uint64_t)(b & m) + xin > (a & m); X = C;
V = MSB((a ^ b) & (a ^ r), n);
N = MSB(r, n); Z = zin && (r == 0);
```

**`neg`**

```c
r = (0u - b) & m;
C = (b & m) != 0; X = C;
V = MSB(b & r, n);
N = MSB(r, n); Z = (r == 0);
```

**`negx`**

```c
r = (0u - b - xin) & m;
C = ((uint64_t)(b & m) + xin) != 0; X = C;
V = MSB(b & r, n);
N = MSB(r, n); Z = zin && (r == 0);
```

**`cmp`**

```c
r = (a - b) & m;
C = (b & m) > (a & m);
V = MSB((a ^ b) & (a ^ r), n);
N = MSB(r, n); Z = (r == 0);
```

**`and`**

```c
r = (a & b) & m; N = MSB(r, n); Z = (r == 0); V = 0; C = 0;
```

**`or`**

```c
r = (a | b) & m; N = MSB(r, n); Z = (r == 0); V = 0; C = 0;
```

**`eor`**

```c
r = (a ^ b) & m; N = MSB(r, n); Z = (r == 0); V = 0; C = 0;
```

**`not`**

```c
r = ~b & m; N = MSB(r, n); Z = (r == 0); V = 0; C = 0;
```

**`mov`**

```c
r = b & m; N = MSB(r, n); Z = (r == 0); V = 0; C = 0;
```

**`lsl`**

```c
unsigned k = b & 63; uint64_t v = a & m;
if (k == 0) { r = (uint32_t)v; C = 0; X = xin; }
else { C = (k <= n) ? ((v >> (n - k)) & 1) : 0; X = C; r = (k < n) ? (uint32_t)((v << k) & m) : 0; }
V = 0; N = MSB(r, n); Z = (r == 0);
```

**`lsr`**

```c
unsigned k = b & 63; uint64_t v = a & m;
if (k == 0) { r = (uint32_t)v; C = 0; X = xin; }
else { C = (k <= n) ? ((v >> (k - 1)) & 1) : 0; X = C; r = (k < n) ? (uint32_t)(v >> k) : 0; }
V = 0; N = MSB(r, n); Z = (r == 0);
```

**`asl`**

```c
unsigned k = b & 63; uint64_t v = a & m;
if (k == 0) { r = (uint32_t)v; C = 0; X = xin; V = 0; }
else {
  C = (k <= n) ? ((v >> (n - k)) & 1) : 0; X = C;
  r = (k < n) ? (uint32_t)((v << k) & m) : 0;
  if (k >= n) V = (v != 0);
  else { uint64_t hm = MSK(k + 1) << (n - 1 - k); uint64_t t = v & hm; V = !(t == 0 || t == hm); }
}
N = MSB(r, n); Z = (r == 0);
```

**`asr`**

```c
unsigned k = b & 63; int64_t sv = SX(a, n);
if (k == 0) { r = a & m; C = 0; X = xin; }
else { C = (k <= n) ? ((sv >> (k - 1)) & 1) : MSB(a, n); X = C; r = (uint32_t)(sv >> (k < 63 ? k : 63)) & m; }
V = 0; N = MSB(r, n); Z = (r == 0);
```

**`rol`**

```c
unsigned k = b & 63; uint64_t v = a & m;
if (k == 0) { r = (uint32_t)v; C = 0; }
else { unsigned s = k % n; r = (uint32_t)(s ? (((v << s) | (v >> (n - s))) & m) : v); C = r & 1; }
V = 0; N = MSB(r, n); Z = (r == 0);
```

**`ror`**

```c
unsigned k = b & 63; uint64_t v = a & m;
if (k == 0) { r = (uint32_t)v; C = 0; }
else { unsigned s = k % n; r = (uint32_t)(s ? (((v >> s) | (v << (n - s))) & m) : v); C = MSB(r, n); }
V = 0; N = MSB(r, n); Z = (r == 0);
```

**`roxl`**

```c
unsigned k = (b & 63) % (n + 1); uint64_t w = ((uint64_t)xin << n) | (a & m); uint64_t wm = MSK(n + 1);
if (k) w = ((w << k) | (w >> (n + 1 - k))) & wm;
r = (uint32_t)(w & m); X = (w >> n) & 1; C = X;
V = 0; N = MSB(r, n); Z = (r == 0);
```

**`roxr`**

```c
unsigned k = (b & 63) % (n + 1); uint64_t w = ((uint64_t)xin << n) | (a & m); uint64_t wm = MSK(n + 1);
if (k) w = ((w >> k) | (w << (n + 1 - k))) & wm;
r = (uint32_t)(w & m); X = (w >> n) & 1; C = X;
V = 0; N = MSB(r, n); Z = (r == 0);
```

**`abcd`**

```c
int lo = (a & 15) + (b & 15) + xin;
if (lo > 9) lo += 6;
int s = lo + (a & 0xf0) + (b & 0xf0);
C = s > 0x99; X = C;
if (C) s -= 0xa0;
r = (uint32_t)s & 0xff;
Z = zin && (r == 0); N = MSB(r, 8); V = 0;
```

**`sbcd`**

```c
int lo = (int)(a & 15) - (int)(b & 15) - (int)xin;
int s = (int)(a & 0xf0) - (int)(b & 0xf0) + lo;
if (lo < 0) s -= 6;
C = s < 0; X = C;
if (C) s += 0xa0;
r = (uint32_t)s & 0xff;
Z = zin && (r == 0); N = MSB(r, 8); V = 0;
```

**`nbcd`**

```c
int lo = 0 - (int)(b & 15) - (int)xin;
int s = 0 - (int)(b & 0xf0) + lo;
if (lo < 0) s -= 6;
C = s < 0; X = C;
if (C) s += 0xa0;
r = (uint32_t)s & 0xff;
Z = zin && (r == 0); N = MSB(r, 8); V = 0;
```

**`mulu`**

```c
if (n == 16) { r = (a & 0xffff) * (b & 0xffff); V = 0; }
else { uint64_t p = (uint64_t)a * b; r = (uint32_t)p; V = (p >> 32) != 0; }
N = MSB(r, 32); Z = (r == 0); C = 0;
```

**`muls`**

```c
if (n == 16) { r = (uint32_t)(SX(a, 16) * SX(b, 16)); V = 0; }
else { int64_t p = (int64_t)(int32_t)a * (int32_t)b; r = (uint32_t)p; V = p != (int64_t)(int32_t)r; }
N = MSB(r, 32); Z = (r == 0); C = 0;
```

**`divu`**

```c
if (n == 16) {
  uint32_t d = b & 0xffff, q = a / d, rm = a % d;
  if (q > 0xffff) { r = a; V = 1; N = 0; Z = 0; }
  else { r = (rm << 16) | q; V = 0; N = MSB(q, 16); Z = (q == 0); }
} else { r = a / b; V = 0; N = MSB(r, 32); Z = (r == 0); }
C = 0;
```

**`divs`**

```c
if (n == 16) {
  int64_t d = SX(b, 16), q = (int64_t)(int32_t)a / d, rm = (int64_t)(int32_t)a % d;
  if (q > 32767 || q < -32768) { r = a; V = 1; N = 0; Z = 0; }
  else { r = (((uint32_t)rm & 0xffff) << 16) | ((uint32_t)q & 0xffff); V = 0; N = MSB((uint32_t)q, 16); Z = (q == 0); }
} else if (a == 0x80000000u && b == 0xffffffffu) { r = a; V = 1; N = 0; Z = 0; }
else { r = (uint32_t)((int32_t)a / (int32_t)b); V = 0; N = MSB(r, 32); Z = (r == 0); }
C = 0;
```

**`ext`**

```c
r = (uint32_t)SX(b, n / 2) & m; N = MSB(r, n); Z = (r == 0); V = 0; C = 0;
```

**`extb`**

```c
r = (uint32_t)SX(b, 8); N = MSB(r, 32); Z = (r == 0); V = 0; C = 0;
```

## Conditions

| Code | Guest (`bcc`, `sel`, `s`) | Expression | Base (`b`) | Expression |
|---|---|---|---|---|
| 0 | `t` | `1` | `eq` | `a == b` |
| 1 | `f` | `0` | `ne` | `a != b` |
| 2 | `hi` | `!C && !Z` | `lt` | `(int32_t)a < (int32_t)b` |
| 3 | `ls` | `C || Z` | `ge` | `(int32_t)a >= (int32_t)b` |
| 4 | `cc` | `!C` | `ltu` | `a < b` |
| 5 | `cs` | `C` | `geu` | `a >= b` |
| 6 | `ne` | `!Z` |  |  |
| 7 | `eq` | `Z` |  |  |
| 8 | `vc` | `!V` |  |  |
| 9 | `vs` | `V` |  |  |
| 10 | `pl` | `!N` |  |  |
| 11 | `mi` | `N` |  |  |
| 12 | `ge` | `N == V` |  |  |
| 13 | `lt` | `N != V` |  |  |
| 14 | `gt` | `!Z && (N == V)` |  |  |
| 15 | `le` | `Z || (N != V)` |  |  |

## Memory operations

Mnemonic suffix selects the address space: `.h` hidden, `.gd` gdata, `.gp` gprog, `.ga` galt.
Syntax: `ld.32.gd rd, disp(rs1)`; stores take the data register in the rd position.

| Space | Code | Description |
|---|---|---|
| `.h` | 0 | Physical, untranslated. Hidden memory and devices. The guest MMU can never produce these addresses. |
| `.gd` | 1 | Guest data space: translated with the guest's current privilege (S bit); data function code. |
| `.gp` | 2 | Guest program space: as gdata with the program function code (PC-relative operands, extension words). |
| `.ga` | 3 | Guest alternate space: function code from SFC (loads) or DFC (stores), for MOVES. |

| Op | Major | Kind | Bytes | Notes |
|---|---|---|---|---|
| `ld.8u` | LD8U | load | 1 |  |
| `ld.8s` | LD8S | load (signed) | 1 |  |
| `ld.16u` | LD16U | load | 2 |  |
| `ld.16s` | LD16S | load (signed) | 2 |  |
| `ld.32` | LD32 | load | 4 |  |
| `st.8` | ST8 | store | 1 |  |
| `st.16` | ST16 | store | 2 |  |
| `st.32` | ST32 | store | 4 |  |
| `ldl.8` | LDL8 | load lock-begin | 1 | Locked load: begins an atomic sequence (TAS, CAS, CAS2). Executes non-speculatively. |
| `ldl.16` | LDL16 | load lock-begin | 2 |  |
| `ldl.32` | LDL32 | load lock-begin | 4 |  |
| `stu.8` | STU8 | store lock-end | 1 | Unlocking store: ends the atomic sequence. |
| `stu.16` | STU16 | store lock-end | 2 |  |
| `stu.32` | STU32 | store lock-end | 4 |  |
| `probe.r` | PROBER | probe | 0 | Translate for read without accessing; rd = 0 if the access would succeed, else a fault status. Never faults. |
| `probe.w` | PROBEW | probe | 0 |  |

## Control and status registers

| Number | Name | Description |
|---|---|---|
| 0x00 | `vbr` | 68k vector base register. |
| 0x01 | `sfc` | Source function code (galt loads). |
| 0x02 | `dfc` | Destination function code (galt stores). |
| 0x03 | `cacr` | 68040 cache control. |
| 0x04 | `tc` | 68040 MMU translation control. |
| 0x05 | `itt0` | Instruction transparent translation 0. |
| 0x06 | `itt1` | Instruction transparent translation 1. |
| 0x07 | `dtt0` | Data transparent translation 0. |
| 0x08 | `dtt1` | Data transparent translation 1. |
| 0x09 | `mmusr` | MMU status (PTEST result). |
| 0x0a | `urp` | User root pointer. |
| 0x0b | `srp` | Supervisor root pointer. |
| 0x0c | `usp` | Inactive user stack pointer. |
| 0x0d | `isp` | Inactive interrupt stack pointer. |
| 0x0e | `msp` | Inactive master stack pointer. |
| 0x20 | `fpcr` | Reserved for the FP extension. |
| 0x21 | `fpsr` | Reserved for the FP extension. |
| 0x22 | `fpiar` | Reserved for the FP extension. |
| 0x40 | `mstatus` | Micro-engine status: pending interrupt level, trace pending, locked-sequence active. |
| 0x41 | `fault_addr` | Guest access fault: faulting address. |
| 0x42 | `fault_status` | Guest access fault: read/write, size, space and function code, cause. |
| 0x43 | `fault_pc` | Guest PC of the instruction that faulted. |
| 0x44 | `mcycle` | Cycle counter. |
| 0x45 | `ginstret` | Guest instructions retired. |
| 0x46 | `dispatch_base` | Hidden-memory base of the opword dispatch table. |
| 0x47 | `scratch_base` | Hidden-memory base of millicode scratch storage. |

## Millicode entry points

**insn**: Guest instruction not handled by the PLA. Target = dispatch_base[opword].

- `x1`: opword (zero-extended)
- `x2`: extension words 0:1 (word 0 in bits 31:16)
- `x3`: extension words 2:3
- `x4`: address of the next sequential guest instruction

**exception**: Guest exception raised by a guest access fault, gfault, or a decode-time condition (illegal, privilege, line A/F).

- `x1`: vector number
- `x2`: guest PC to stack
- `x3`: fault address (access errors)
- `x4`: fault status (access errors)

**interrupt**: Taken at a guest instruction boundary when the pending level exceeds the SR mask.

- `x1`: interrupt level

**trace**: Taken after gexit when trace is pending.

- `x1`: address of the traced instruction

**reset**: Power-on and RESET. All registers undefined.

**mfault**: Internal millicode error (misaligned hidden access, return-stack overflow/underflow, illegal micro-instruction). Halts for debug.

- `x1`: cause
- `x2`: millicode PC

