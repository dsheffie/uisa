# ucc: the uisa millicode compiler

`ucc` compiles a small subset of C to uisa millicode assembly that
`tools/uisa.py` assembles. It exists because millicode that is too large to
write by hand (SoftFloat-style extended-precision FP first) still has to obey
the millicode rules: no memory stack, no spilling, a 4-deep hardware return
stack, and register allocation across the whole program.

A flex/bison front end builds an AST whose nodes emit three-address code
straight into basic blocks. The middle end builds SSA (Lengauer-Tarjan
dominators, dominance frontiers, liveness-pruned phi placement, renaming
along the dominator tree) and optimizes it. The back end selects uisa
instructions, allocates registers for the whole program at once while the
code is still in SSA, and only then takes it out of SSA with parallel copies.

```
make compiler                       # CMake build into build/compiler (needs flex, bison 3, z3, python3 + PyYAML)
build/compiler/ucc [-o out.s] [-v] [--dump-tac] [-O0|-O2] [--rlimit n] file.c
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
- `struct name { fields };` with scalar or struct fields, value semantics
  (copy, assign, pass, return; `.field` access and assignment), and
  `typedef type name;`. After its definition a struct's name is a type
  name, as in C++.
- `inline` functions are expanded at every call: no call instruction, no
  return-stack depth, no register-allocation boundary. Only inline functions
  take reference parameters (`u64 &z`), which name the caller's variable, as
  SoftFloat's pointer out-parameters do, and only they return values wider
  than two words. Recursion through inline functions is an error.
- Called (non-inline) functions take any number of argument words (a u64 is
  two, a struct its fields' words) and return at most two words.
- `#include "file"` (relative to the including file; each file is read at
  most once; `#pragma once` is accepted and redundant). Diagnostics name the
  included file and its line.
- `entry void name(void)` declares a millicode entry point: it is dispatched
  to (never called), may use the entry-ABI intrinsics, cannot return, and
  every path must end in a no-return intrinsic (`__gexit`, `__gfault`, ...).
- `extern T name(params) asm("ARGS -> RESULTS; reads R...; clobbers R...;
  depth N");` declares a hand-written routine in `millicode/*.s` and its
  register contract, so C millicode can call shared assembly helpers. ARGS
  and RESULTS name one x register per word (a two-word struct result comes
  back in two); `reads` lists entry-ABI registers (x1-x4) the routine reads,
  which the compiler passes implicitly and which keep their values up to the
  call (so only entry functions may call such a routine); `clobbers` lists
  the other registers it may write (`x16-x19` ranges allowed); `depth` is
  the return-stack entries it uses, its own call included (default 1). The
  argument, result and clobbered registers are all treated as written by the
  call: nothing live across it is allocated to them, and a fixed register
  (an entry-ABI value) still needed after the call is an error. Example
  (`millicode/bitfield.c`):
  `extern ea_res ea_addr(u32 ea, u32 size, u32 ext_off) asm("x20 x21 x22 ->
  x23 x4; reads x2 x3 x4; clobbers x16-x19 x24-x26; depth 3");`
- Not in the millicode language: pointers, arrays, unions, floats, globals
  that are not constants, the comma operator, char literals, string literals
  outside `asm("...")`, and the preprocessor beyond `#include`. Pointers,
  arrays, mutable globals and the comma operator exist in program mode
  (below) and are errors in millicode.

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

A `switch` with three or more cases becomes an n-way `bx` jump table (one
`jal` slot per value) when its cases are small: `switch ((x >> k) & (2^w -
1))` (k and the mask literals or named constants, w <= 15) indexes the table
with that field of x directly, with no range check; any other 32-bit switch
whose cases all lie in 0..63 checks the range (one branch to default) and
then indexes. Other switches (negative or larger cases, 64-bit values, fewer
cases) are compare chains.

`(x >> k) & (2^w - 1)` compiles to one `bfextui` (the assembler's `extract
rd, x, k, w`), as does a mask too wide for `andi`.

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

## The idea: allocate in SSA, so "fits without spilling" is a static check

Millicode has no stack, so it cannot spill. The compiler therefore has to
decide, before it allocates anything, whether every function fits in the 31
allocatable registers, and when one does not, say exactly where. For
register allocation in general that question is graph colouring, which is
NP-complete: the only way to know whether k registers suffice is to try.
SSA form removes the difficulty:

- **The interference graph of an SSA program is chordal.** In strict SSA
  every use is dominated by the value's single definition, so each live range
  is a connected subtree of the dominator tree, and the intersection graph of
  subtrees of a tree is chordal (Hack, Grund and Goos; Bouchez, Darte and
  Rastello; Brisk et al., all 2005-06).
- **For a chordal graph the chromatic number equals the largest clique**,
  and colouring the vertices greedily in a perfect elimination order uses
  exactly that many colours. For SSA that order is simply the definitions in
  dominator-tree order.
- **The largest clique is the most values live at one program point.**
  Subtrees of a tree that pairwise intersect share a common node (the Helly
  property), so a clique of live ranges is a set of values all live at the
  same point. MaxLive, the size of the largest live set, falls out of one
  liveness scan.

So a function needs exactly MaxLive registers, no more and no fewer. ucc
computes it before allocation and rejects a function over 31 with the names
of the values live at the peak, and when the check passes the greedy
allocator (stage 1 below) is guaranteed to succeed; z3 only reduces moves.
The rest of the design keeps that bound exact:

- **Leave SSA after allocation.** Phis become parallel copies on the
  incoming edges, which only permute values already in registers: a cycle
  goes through a register free on that edge when there is one, otherwise
  three-xor swaps. Leaving SSA never needs an extra register.
- **Calls.** Without a stack, a value live across a call must sit in a
  register that the callee, and everything it calls, never writes. A
  function's need is therefore MaxLive, or for some call the values live
  across it plus the callee's own need, whichever is larger; with recursion
  ruled out the call graph is acyclic, so this is computed callees first and
  checked against 31 too. There is no fixed calling convention: every
  function's parameter and result registers are part of the one
  whole-program solution.
- **Constraints that would break chordality are preferences.** Copies, tied
  operands (merge forms), phi operands, call arguments and results only ask
  for the same register; when they cannot have it the emitter adds a move.
  The values pinned to registers (entry-ABI values in x1-x4) are all defined
  at function entry, the start of the elimination order.
- **Optimizations answer to the same bound.** Value numbering and loop
  hoisting lengthen live ranges, so constants are shared only within a block
  and loop hoists are undone in any function they push over the budget.

Program mode (`--program`) has a stack and spills, so it uses a conventional
Chaitin-Briggs allocator instead (below).

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
- **How the allocation is found.** `-O0` (the default, used by `make` and
  the tests) stops after stage 1: a valid allocation in a fraction of a
  second. `-O2` is for deploy builds (`make release` compiles the millicode
  with it) and runs all three stages. Everything is deterministic (z3 runs
  under a resource budget, `--rlimit`, default 300M, never a wall-clock
  timeout, and the problem is keyed by value index, so the same source gives
  the same millicode on every machine):
  1. *Greedy.* Functions callees first, values in dominance order (chordal
     coloring), each taking a register its colored preference partners use;
     then single values move where more partners are. Always feasible when
     the pressure checks pass; it is z3's starting point and the fallback.
  2. *Whole program* (`-O2`). Up to 700 values, z3 gets a third of the budget to
     solve the whole problem and prove it optimal (`t_u64div`, 649 values:
     30 s; `t_u64div2`, 570: 14 s; bitfield.c: well under a second).
  3. *Neighbourhoods* (`-O2`). Otherwise, each step frees a small set of values,
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
- **SSA optimization.** Repeated to a fixed point: constant folding and
  algebraic simplification, copy propagation, dead-code removal, CFG
  cleanup, and global value numbering: a walk of the dominator tree with a
  scoped table of the values computed so far (ALU operations, extensions,
  frame addresses and the pure base intrinsics, with commutative operands
  ordered), so an expression that a dominating one already computed is
  reused instead of recomputed. Anything that reads state is never merged
  (guest registers, CSRs, guest flags, memory intrinsics); program-mode loads
  are reused within a block until the next store or call. After
  legalization a second pass merges the constants that legalization put in
  registers: within each block in millicode (a constant kept live across
  blocks would hold a register everywhere, and millicode cannot spill),
  across the dominator tree in program mode. Together they remove about 4%
  of the millicode (`fpu.c`, the softfloat tests) and 12% of program-mode
  code, which also runs about 17% fewer instructions.
- **Loop-invariant code motion.** Once the rest has settled, and again
  after legalization (so materialized constants and `lui` address halves
  move too): pure values whose operands are all defined outside a loop move
  to its preheader (the one outside predecessor, or a new block on that
  edge), innermost loops first. uisa ALU operations never fault, so a value
  the loop might not have computed is safe to compute early; loads and state
  reads stay. Hoisting stops when a loop's estimated pressure (SSA liveness)
  reaches the register budget. Program mode leaves loops that call alone (a
  value live across the call needs a callee-saved register, and its save
  and restore cost more code than the hoist saves). Millicode logs every
  hoist; when a function, with what it calls, then exceeds the registers,
  its hoists go back to the top of their loop headers before allocation
  (`-v` reports it). Program mode runs about 15% fewer instructions with
  code size unchanged.
- **Code hoisting.** When every successor of a branch is reached only from
  it and all of them compute the same pure value from operands available at
  the branch, the value is computed once before the branch. In program mode
  the same applies after legalization to constants (not in millicode, for
  the reason constants are only merged within a block).
- **Tail merging.** On the final assembly, after allocation, where
  identical text is identical work: two blocks that end in the same
  instructions and continue to the same place (the same jump target or
  fall-through label, or the same return or no-return instruction) keep one
  copy, and the other jumps into it. Then branches to blocks that only jump
  go straight to the destination, jumps to the next label disappear, and
  blocks nothing reaches are deleted. A `bx` jump table is never touched.
  It trades a jump executed on the merged path for the instructions saved.
- **Out of SSA.** Critical edges are split; phis become parallel copies on
  the incoming edge. A cycle of three or more copies goes through a scratch
  register when one is free on that edge (one the function or its callees
  already write, so callers never keep a value there across the call): k + 1
  moves for k copies. Two-copy cycles, and cycles with no free register, are
  broken with three-xor swaps.
- **Immediates and branches.** 14-bit ALU immediates (signed for `addi/slti`,
  zero-extended for `andi/ori/xori`), 12-bit memory displacements, 11-bit
  compare-and-branch displacements: a branch that does not reach becomes the
  inverted branch around a `jal`.
- Uses of variables that may be unassigned, falling off the end of a non-void
  function or an entry function, and calling an entry function are errors.

## Program mode (`--program`)

`ucc --program` compiles ordinary C programs (csmith tests, benchmarks) to
code that runs on the uisa ISS and the uisa RTL. Millicode mode, the default,
is unchanged: same rules, same z3 allocator, byte-identical output.

```
build/compiler/ucc --program -o prog.s prog.c       # code + data section
python3 compiler/prog/uimage.py prog.s -o prog.hex   # program image
build/prun prog.hex                                  # run on the ISS: "result <x10> steps <n>"
python3 compiler/prog/test_prog.py [N [seed]] [--csmith N]   # differential tests vs the host cc
```

**Pipeline.** The front end, SSA construction and SSA cleanups are shared
with millicode. Then (`progback.cpp`): out of SSA first (critical edges split,
phis become parallel copies sequentialized before allocation, a cycle through
a new temporary); ABI lowering; per function, Chaitin-Briggs graph colouring
with iterated register coalescing (George & Appel: conservative Briggs and
George coalescing, freezing, optimistic spilling); spilled values get stack
slots and short-lived temporaries, and allocation repeats until nothing
spills; then the frame is laid out and the code emitted. The recursion and
return-stack rules do not apply: calls are `jal x1, f` and returns
`jalr x0, x1, 0`, with ra saved in the frame, so recursion and call depth are
limited only by the stack.

**Calling convention.**

| registers | use |
|---|---|
| x0 | zero |
| x1 | return address (saved in the frame by non-leaf functions) |
| x2 | stack pointer (grows down, 8-byte aligned frames) |
| x10-x17 | argument words 1-8; x10-x11 result words |
| x3-x7, x10-x17, x28-x30 | caller-saved (a call clobbers them) |
| x8, x9, x18-x27 | callee-saved (saved and restored when a function uses them) |
| x31 | reserved for the emitter (frame offsets beyond the 12-bit displacement) |

Argument words past the eighth go on the stack: word 9 at `sp + 0` of the
caller at the call, word 10 at `sp + 4`, ... A 64-bit value is two words (low
first); a struct its fields' words. A result of more than two words (a large
struct) goes through a hidden first argument: the caller passes the address
of a frame object and the callee stores the result there.

Frame (from sp after the prologue, upward): outgoing stack arguments; locals
in memory (arrays, address-taken variables); spill slots; saved callee-saved
registers; saved ra. The incoming stack arguments follow the frame.

**Memory map.** Everything is in the hidden (physical) space, accessed with
`ld.*.h` / `st.*.h`; memory is flat and identity-mapped, big-endian:

| address | |
|---|---|
| `0x00000000` | code: `_start` (sets sp, `jal x1, main`, `halt 0`), then the functions |
| `0x00100000` | data: every global, initialized (zero where no initializer says otherwise) |
| below `0x00800000` | the stack (sp starts at `0x00800000`) |

Globals are addressed absolutely (`lui` + displacement), so a global's
address is a link-time constant usable in initializers (`u32 *p = &g[3];`).

**Image and entry convention.** `uimage.py` assembles the code with
`tools/uisa.py` and writes a text image: `@<byte address>` lines start a
segment, each following line is one big-endian 32-bit word (`#` lines are
comments). Execution starts at address 0; the program ends with `halt 0`
and main's return value in x10 (for csmith programs, the checksum).
`compiler/prog/prun.cc` is the reference loader and ISS; it rejects
misaligned or out-of-range accesses, guest-space accesses and millicode-only
control flow. The instructions used are the uisa base operations, `jal`,
`jalr`, `lui`, `halt`, and the `.h` loads and stores.

**Language additions** (program mode only):
- Global variables with initializers (constant expressions, including
  addresses of globals and their elements), `static` (ignored: one
  translation unit) and `const` on aggregates (read-only data is ordinary
  data).
- Pointers (any depth, to integers, structs, pointers and `void`), `&`, `*`,
  `->`, pointer arithmetic scaled by the pointee size, pointer differences,
  comparisons, casts between pointers and integers.
- Arrays (any number of dimensions, of integers, pointers and structs), global
  and local, with braced initializers (the elements not named are zero);
  `a[i]` is `*(a + i)`; arrays decay to pointers; array parameters are
  pointers.
- Array fields in structs (`struct s { u16 v[4]; u8 m[2][3]; };`, dimensions
  are integer constant expressions). A struct value carries its arrays:
  assignment, by-value arguments and results copy every element (a result
  over 2 words goes through the hidden pointer, so large structs are copied
  through memory); `s.v` on its own decays to a pointer as any array does.
- `sizeof(type)`, `sizeof(type[n]...)` and `sizeof expr`: a `u32` constant
  from program mode's layout (so it differs from the LP64 host for pointers
  and for structs with 64-bit fields). The operand is not evaluated.
- Locals whose address is taken, arrays, structs that contain arrays and
  braced-initialized locals live in the frame (so do by-value struct
  parameters that contain arrays); every other local stays in registers.
- The comma operator; prototypes (`int f(int);`); `main` may take arguments
  (they are not set).
- C's integer keywords and their combinations with LP64 sizes, as on the
  host: `char` 8 (signed), `short` 16, `int` 32, `long` and `long long` 64,
  `signed`/`unsigned`; an `L` suffix makes a constant 64-bit like `LL`. So a
  csmith program means the same thing on the host and on uisa.
- Layout: big-endian, natural alignment capped at 4 (64-bit values are two
  words, high word first), structs padded as C pads them. Pointers are 32
  bits.
- Intrinsics: only the uisa base operations (no guest registers, flags,
  memory spaces or millicode control).

Not supported yet: function pointers, unions, floats, bitfields, `volatile` (csmith is run without unions,
bitfields, floats and volatiles), pointers to arrays (`int (*p)[4]`), string
and char literals, varargs.

**Tests** (`compiler/prog/`). `test_prog.py` compiles each program twice:
natively with the host `cc` (`host_prog.h` provides ucc's type names,
`-fwrapv`) and with `ucc --program` + `uimage.py` + `prun`, and compares the
results. Programs: `tests/*.c` (hand-written: calls and recursion, memory and
pointers, spilling, large frames and stack arguments, array fields and
sizeof), `tests_csmith/*.c`
(csmith-format), then random programs from `gen_prog.py`, a csmith-style
generator (globals, arrays, structs, pointers to pointers, loops, switch,
recursion; UB-free by construction; main returns the CRC of every global).
With csmith installed (`csmith` on PATH or `CSMITH=...`), `--csmith N` also
runs N csmith programs: both builds are preprocessed with
`compiler/prog/csmith/csmith.h`, a stand-in for csmith's runtime (its safe
math names, the CRC, `platform_main_end` returning the checksum). csmith has
no MacPorts port; build it from source without sudo:

```
git clone https://github.com/csmith-project/csmith ~/code/csmith
cmake -S ~/code/csmith -B ~/code/csmith/build -DCMAKE_INSTALL_PREFIX=$HOME/code/csmith/local
cmake --build ~/code/csmith/build -j8 && cmake --install ~/code/csmith/build
CSMITH=~/code/csmith/local/bin/csmith python3 compiler/prog/test_prog.py 0 1 --csmith 200
```

(`local`, not `install`: on a case-insensitive file system that name is
csmith's `INSTALL` file.) csmith programs can run for minutes or forever;
like csmith's own scripts, the driver skips (and counts) any program that
runs longer than 5 s natively or 500M ISS steps.

## Layout

| file | |
|---|---|
| `scanner.l`, `parser.y` | front end |
| `ast.h`, `ast.cpp` | AST, semantic checks, C conversions, TAC generation |
| `tac.h`, `tac.cpp` | the three-address code (one `Reg` class before and after SSA) |
| `codegen.h`, `codegen.cpp` | CFG cleanup, dominators, SSA construction, SSA optimization (GVN, LICM) |
| `backend.h`, `backend.cpp` | legalization, liveness/interference, pressure checks, emission |
| `regalloc.cpp` | the z3 allocation model |
| `progback.cpp`, `emit.h` | program mode: out of SSA, ABI, iterated register coalescing, frames; shared emission |
| `prog/` | program mode tools and tests: `uimage.py`, `prun.cc`, `test_prog.py`, `gen_prog.py`, the csmith shim |
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
  moves), automatic inlining (only `inline` functions are inlined), calls
  into hand-written millicode, and pointers or arrays through intrinsics
  only.
