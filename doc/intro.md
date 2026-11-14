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
make            # validate the spec, regenerate build/, run the tests
```

Edit `spec/uisa.yaml` (the ISA) or this file (the rationale); never edit
`build/`.
