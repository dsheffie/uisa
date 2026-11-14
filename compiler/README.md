# ucc: the uisa millicode compiler

`ucc` compiles a small subset of C to uisa millicode assembly that
`tools/uisa.py` assembles. It exists because millicode that is too large to
write by hand (SoftFloat-style extended-precision FP first) still has to obey
the millicode rules: no memory stack, no spilling, a 4-deep hardware return
stack, and register allocation across the whole program.

It is a fork of the Decaf compiler (`decaf/proj4`): the flex/bison front end,
the AST-emits-TAC structure, and Decaf's SSA construction (Lengauer-Tarjan
dominators, dominance frontiers, liveness-pruned phi placement, renaming
along the dominator tree) survive. The Java object model and the LLVM back end
are gone. The new back end selects uisa instructions, takes the program out
of SSA with parallel copies, and allocates registers for the whole program at
once with z3.

```
make compiler                       # CMake build into build/compiler (needs flex, bison 3, z3, python3 + PyYAML)
build/compiler/ucc [-o out.s] [-v] [--dump-tac] [--rlimit n] file.c
python3 compiler/tests/test_ucc.py [iterations [name]]  # compiler tests (also run by `make`)
```

z3 is found under `/opt/local` (MacPorts: `sudo port install z3`),
`/opt/homebrew` or `/usr/local`. `-v` prints the size of the allocation
problem, the move cost z3 reached, the greedy allocation's cost and the z3
resource units used.

Millicode written in C lives in `millicode/*.c`. When a `.c` file is listed in
`millicode/dispatch.yaml`, `tools/gen_millicode.py` compiles it to
`build/millicode/<name>.s` and assembles it with the hand-written sources.
Labels must be unique across all sources (checked). `millicode/bitfield.c`
(BFTST/BFEXTU/BFEXTS/BFFFO) is the first routine built this way.

## Language

- Types: `u8 u16 u32 u64 s8 s16 s32 s64`, plus `int` (= `s32`), `unsigned`
  (= `u32`) and `void`. Every value is kept canonical for its type (u8
  zero-extended, s8 sign-extended, ...), so promotion costs nothing.
- Integer constants follow C with `s32 u32 s64 u64` in place of `int
  unsigned long long unsigned long long`: the first type that holds the value
  (unsigned only for hex/octal or with a `u` suffix); `ll`/`ull` force 64 bits.
- C's integer promotions and usual arithmetic conversions; casts. `>>` is
  arithmetic on signed and logical on unsigned (promoted) operands; `/ %`
  divide signed or unsigned the same way. `s64` holds every `u32`, so
  `s64 op u32` is `s64`, as in C.
- Operators: `+ - * / % & | ^ ~ << >> ! && || ?:`, comparisons, `=` and the
  compound assignments, prefix and postfix `++ --`.
- Statements: blocks, declarations anywhere (with initializers), `if/else`,
  `while`, `do/while`, `for` (with a declaration), `switch` with fallthrough
  (`case` labels directly inside the switch body), `break`, `continue`,
  `return`.
- Functions with parameters and a return value; global and local `const`
  integer declarations (constant expressions). Functions may be called before
  their definition.
- `entry void name(void)` declares a millicode entry point: it is dispatched
  to (never called), may use the entry-ABI intrinsics, cannot return, and
  every path must end in a no-return intrinsic (`__gexit`, `__gfault`, ...).
- Not in the language: pointers, arrays, structs, floats,
  globals that are not constants, `goto`, the comma operator, char and string
  literals, the preprocessor.

## 64-bit types

The front end splits every `u64`/`s64` value into two 32-bit words (a
variable becomes two source variables), so SSA, allocation and emission only
ever see 32-bit values and each word is allocated, and dies, on its own.

| operation | lowering |
|---|---|
| `+ -` | word add/sub, carry/borrow with `sltu` (4 instructions) |
| `* ` | `mul` + `mulhu` + two cross `mul`s; `(u64)a * b` with 32-bit operands is `mul` + `mulhu` |
| `<< >>` constant | two or three word shifts |
| `<< >>` variable | branch-free, 14 instructions; the count is taken mod 64 |
| `/ %` | inline, no loop over bits (Hacker's Delight `divdu`/`divlu`): both high words 0 is one `divu`/`remu`; a 32-bit divisor is one `divu` for the high word plus a 64/32 step that estimates two 16-bit quotient digits with `divu` and corrects each at most twice; a wider divisor is normalized with `clz`, the same 64/32 step estimates the 32-bit quotient, and one multiply-subtract fixes it. Both paths share one copy of the 64/32 step; a divisor known to fit 32 bits (`u64 / u32`) emits only that path. About 110 instructions per division site, 25-60 executed. Signed divides the magnitudes. Unsigned powers of two become shifts and masks. Division by zero is undefined, as in C |
| comparisons | as branches: high words first, then the low words unsigned; as values: `slt`/`sltu` on the high words, combined |
| calls | a 64-bit argument takes two parameter registers (low word first); a 64-bit result comes back in two return registers, read by `Call` + `CallRes` and copied out as one parallel copy |

Intrinsic arguments take the low word of a 64-bit value (C conversion to
`u32`).

## Intrinsics

`compiler/gen_intrinsics.py` generates the intrinsic table from
`spec/uisa.yaml` through `tools/uisa.py`; nothing is hand-maintained. Every
uisa instruction except millicode control flow (`jal jalr call ret b.* bcc.*`,
which the compiler emits itself) and `lui/auipc` is available as
`__<mnemonic>` with `.` replaced by `_`:

| form                              | example                         | C signature                      |
|-----------------------------------|---------------------------------|----------------------------------|
| base op                           | `clz rd, rs1`                   | `u32 __clz(u32)`                 |
| guest op, three-address / zfill   | `add.32 rd, rs1, rs2`           | `u32 __add_32(u32 a, u32 b)`     |
| guest op, merge (rd tied to rs1)  | `add.8 rd, rs2`                 | `u32 __add_8(u32 old, u32 b)`    |
| guest compare (flags only)        | `cmp.16 rs1, rs2`               | `void __cmp_16(u32, u32)`        |
| immediate forms                   | `addi.32 rd, rs1, imm6`         | `u32 __addi_32(u32, const)`      |
| load / probe                      | `ld.32.gd rd, d(rs1)`           | `u32 __ld_32_gd(u32 addr)`       |
| store                             | `st.16.gd rd, d(rs1)`           | `void __st_16_gd(u32 addr, u32)` |
| store with base writeback         | `st.32.pre.gd rs2, d(rs1)`      | `u32 __st_32_pre_gd(u32 base, u32 v, const d)` |
| system                            | `gfault imm`, `csrr rd, csr`    | `void __gfault(const)`, `u32 __csrr(const)` |

- Arguments that are immediates in the instruction must be constant
  expressions in range (checked against the spec's field widths and scales).
- Address arguments: `base + constant` folds into the 12-bit displacement;
  constant addresses use `x0` or `lui`.
- Tied forms (merge, `divu.16`, `bfinsi`, writeback stores, `gpushr`) take the
  old destination value as their first argument; the allocator prefers to
  give the result the same register and otherwise inserts one move.
- `__rdg(i)` / `__wrg(i, v)` with a constant `i` name the guest register
  directly (`d3`, `gpc`, ...) and the read feeds its uses directly when no
  write can intervene; with a run-time index they emit `rdg`/`wrg`.
- Entry-ABI values (entry functions only): `__opword()` (x1), `__ext01()`
  (x2), `__ext23()` (x3), `__next_pc()` (x4), from the spec's `insn` entry
  point.
- Predefined constants from the spec: `D0..D7 A0..A7 GPC GSR` (indices for
  `__rdg`/`__wrg`), `CSR_<NAME>` for every CSR, `CCR_X CCR_N CCR_Z CCR_V
  CCR_C` (bit masks in the `rdccr`/`wrccr` layout).
- `__gexit __gfault __gfaultr __halt __brk` end control flow.
- Flags are not modelled as values: intrinsics that read or write the renamed
  flags keep their program order, and the compiler never reorders anything.

## Rules the compiler enforces

- **No recursion.** The call graph must be acyclic; a cycle is reported with
  its path.
- **Return-stack depth.** Every chain of nested calls fits
  `call_stack_depth` (4) from the spec, counting the helper's own frame for
  helpers.
- **No spilling.** Values live at once never exceed the 31 allocatable x
  registers (`x1-x31`). Over budget is a compile error that names the live
  values, before allocation; a second check adds up values live across each
  call and what the callee chain needs.
- **Whole-program allocation.** One z3 problem covers every function. Hard
  constraints: interfering values differ; entry-ABI values stay in x1-x4; a
  value live across a call never sits in a register the callee or anything it
  calls writes. Soft constraints, weighted by loop depth: phi operands,
  copies, tied operands, call arguments and results, and return values share
  a register. Each function's parameter and return registers are part of the
  solution, so there is no fixed calling convention and no caller/callee-saved
  split. The result is verified independently of z3 before emission.
- **How the allocation is found.** Three stages, all deterministic (z3 runs
  under a resource budget, `--rlimit`, default 300M, never a wall-clock
  timeout, and the problem is keyed by value index, so the same source gives
  the same millicode on every machine):
  1. *Greedy.* Functions callees first, values in dominance order (chordal
     coloring), each taking a register its colored preference partners use;
     then single values move where more partners are. Always feasible when
     the pressure checks pass; it is z3's starting point and the fallback.
  2. *Whole program.* Up to 700 values, z3 gets a third of the budget to
     solve the whole problem and prove it optimal (`t_u64div`, 649 values:
     30 s; `t_u64div2`, 570: 14 s; bitfield.c: well under a second).
  3. *Neighbourhoods.* Otherwise, each step frees a small set of values,
     fixes every other one, and z3 solves that piece optimally as its own
     small problem (fixed neighbours become excluded registers, preferences
     to fixed values become one-register soft literals, call clobbers become
     exact exclusions). Loops go first, innermost first, each freeing the
     values of its own blocks (nested loops are already settled) and their
     interference neighbours, so the moves that execute most are optimal
     given their surroundings; then the values around the heaviest remaining
     moves, until nothing improves or the budget is spent.

  `-v` reports the stage reached, how many loops were settled optimally and
  the remaining move cost per loop depth. On a SoftFloat-sized benchmark
  (`compiler/tests/bench_alloc.c`,
  1674 values, four outer loops around inlined 64-bit divisions, inner
  correction loops) the weighted move cost goes from 1060 (greedy) to about
  460 in 80 s, with every inner loop move removed; what remains is in the
  large outer loop bodies.
- **Out of SSA.** Critical edges are split; phis become parallel copies on
  the incoming edge, cycles are broken with three-xor swaps (no scratch
  register).
- **Immediates and branches.** 14-bit ALU immediates (signed for `addi/slti`,
  zero-extended for `andi/ori/xori`), 12-bit memory displacements, 11-bit
  compare-and-branch displacements: a branch that does not reach becomes the
  inverted branch around a `jal`.
- Uses of variables that may be unassigned, falling off the end of a non-void
  function or an entry function, and calling an entry function are errors.

## Layout

| file | |
|---|---|
| `scanner.l`, `parser.y` | front end (from Decaf) |
| `ast.h`, `ast.cpp` | AST, semantic checks, C conversions, TAC generation |
| `tac.h`, `tac.cpp` | the three-address code (one `Reg` class before and after SSA) |
| `codegen.h`, `codegen.cpp` | CFG cleanup, dominators, SSA construction, SSA cleanups (from Decaf) |
| `backend.h`, `backend.cpp` | legalization, liveness/interference, pressure checks, emission |
| `regalloc.cpp` | the z3 allocation model |
| `gen_intrinsics.py` | intrinsic table and ISA constants from `spec/uisa.yaml` |
| `tests/` | `test_ucc.py`: differential tests (ucc + uisa ISS vs native, random guest states) and expected compile errors |

## Not done yet

- **Allocation at SoftFloat scale.** Large loop bodies (hundreds of values in
  one loop) are only improved piecewise. Splitting such a loop into
  overlapping windows along its blocks, or pre-coalescing safe copies to
  shrink it, would let the loop pass settle them too.
- **Scratch memory.** Explicit hidden-memory storage (`scratch_base`) for
  state larger than the register file; no syntax for it yet.
- **The SoftFloat milestone.** 80-bit extended precision for the 68040 FPU is
  the next real program; it will probably need arrays in scratch memory, and will show whether z3 allocation scales (bitfield.c: 150
  values, well under a second).
- **Faults before commits** is not checked yet (intro.md says the compiler
  will check it on SSA).
- Dead-flag selection of `.nf` forms, writing results straight into guest
  registers (`__wrg(D2, x + 1)` currently computes into an x register and
  moves), inlining of small helpers, calls into hand-written millicode, and
  pointers or arrays through intrinsics only.
