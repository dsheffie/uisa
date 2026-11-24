# SoftFloat in millicode

`softfloat.h` is the floatx80 (68881/68040 extended precision) arithmetic the
FPU millicode will be built from, written in the millicode C subset
(`compiler/README.md`). It is a port of the SoftFloat that Previous uses
(`previous/src/softfloat`: QEMU's SoftFloat 2a with WinUAE's `SOFTFLOAT_68K`
changes), configured as Previous configures it for a 68040, so co-simulation
against Previous can compare FP results and exception flags bit for bit.

The port keeps the original's structure, names, constants and `goto`s;
pointer out-parameters became references and `float_status *` became
`float_status &`. Every function is `inline`: SoftFloat nests helpers deeper
than the 4-entry millicode return stack, and inlining also lets register
allocation see each operation whole.

## Status

| operation | Previous function | status |
|---|---|---|
| FADD, FSUB | `floatx80_add`, `floatx80_sub` | ported, tested |
| FMUL | `floatx80_mul` | ported, tested |
| FDIV | `floatx80_div` | ported, tested |
| FSQRT | `floatx80_sqrt` | ported, tested |
| FMOVE (round to precision) | `floatx80_move` | ported, tested |
| FSGLMUL, FSGLDIV | `floatx80_sglmul`, `floatx80_sgldiv` (`roundSigAndPackFloatx80`) | ported, tested |
| FABS, FNEG | `floatx80_abs`, `floatx80_neg` | ported, tested |
| FCMP, FTST | `floatx80_cmp` (68040: `cmp_signed_nan`), `floatx80_tst` | ported, tested |
| int32 -> extended | `int32_to_floatx80` | ported, tested |
| extended -> int32 | `floatx80_to_int32` | ported, tested |
| rounding to 32/64/80 bits, all four modes, the 68k overflow/underflow rules | `roundAndPackFloatx80` (68K variant) | ported, tested through the above |

`tests/softfloat/test_fx80.py` (run by `make`) compiles `fx80_ops.c` with
ucc, runs each operation on the uisa ISS and through Previous's own
`softfloat.c` on the same inputs: random operands aimed at denormals,
unnormals, NaNs (quiet and signaling), infinities, cancellation, the jamming
shifts, and the overflow/underflow limits of each precision, in every
rounding mode and precision. Results and SoftFloat's exception-flag byte must
match exactly. `make` runs 20,000 cases per operation; pass a larger count
for a longer run (`python3 tests/softfloat/test_fx80.py 500000`).

## The instruction handler (millicode/fpu.c)

`fpu.c` is the FPU-gen handler (opwords F200-F23F) built on this port. It
follows Previous's `fpuop_arithmetic` step for step and is tested against it:
`tests/fpu` links Previous's own `cpu/fpp.c` (with stubbed CPU and memory
hooks) and runs random instructions and machine states through both, comparing
data and address registers, FP registers, FPCR, FPSR, FPIAR, the next PC and
the exception taken. `make` runs 20,000 cases (about 2 s); 300,000 pass, of
which 146,000 complete normally and are compared bit for bit.

Done: opclass 000/010 (FPm or `<ea>` to FPn) for the 68040's hardware set,
every source format but packed, every addressing mode with a brief index
word; FMOVE FPcr to/from Dn/An; the F-line and illegal traps for opmodes
with no instruction; FPSR condition codes, exception and accrued bytes.

State: FP0-FP7 in scratch memory (`CSR_SCRATCH_BASE` + 12n, FMOVEM extended
layout), FPCR/FPSR/FPIAR in their CSRs.

`fpu.c` is not in `dispatch.yaml` yet: the shadow model has no FP state and
rejects hidden-memory accesses, so routing FPU-gen to it now would turn
instructions that pass into failures. Hooking it up needs FP registers and
FPCR/FPSR/FPIAR in the shadow state (and Previous's hook to fill them),
hidden-memory support in the model, and program-space (`.gp`) loads looked
up in the IFETCH log.

## Not done

- The traps that need an FSAVE frame for the FPSP stop with a `brk` code
  instead: unimplemented instruction (`brk 20`), unimplemented data type
  (`brk 21`), and arithmetic exceptions (`brk 22`: an enabled exception, or
  overflow/underflow, which a 68040 always hands to the FPSP). These need the
  frame layouts Previous builds (`fsave_data`) and, for OVFL/UNFL/INEX,
  `saveFloatx80Internal`: the intermediate the BUSY frame carries.
- FMOVE FPn to `<ea>` (with int8/16/32, single and double conversion out),
  FMOVEM of data and control registers (`brk 23`), full-format index words
  (`brk 24`), FBcc/FScc/FDBcc/FTRAPcc, FSAVE/FRESTORE.
- Size: the handler is about 14k words, mostly the SoftFloat operations
  inlined once each (`op_mono` holds three copies of the rounding code).

## What the 68040 traps (and the millicode will too)

The millicode implements exactly the 68040's hardware FP set and traps the
rest to the OS's FPSP, as the real chip does and as Previous does
(`fpu_no_unimplemented`, `cpu/fpp.c`):

- **Unimplemented instructions** (`fault_if_unimplemented_680x0`): every
  FPU-gen opmode except FMOVE, FSQRT, FABS, FNEG, FADD, FSUB, FMUL, FDIV,
  FSGLMUL, FSGLDIV, FCMP, FTST and their FS*/FD* rounding variants. That
  covers the transcendentals, FINT/FINTRZ, FGETEXP/FGETMAN, FSCALE,
  FMOD/FREM and FMOVECR, so none of them is ported here.
- **Unimplemented data types** (`normalize_or_fault_if_no_denormal_support`,
  `fault_if_no_packed_support`): a denormal or unnormal source or
  destination operand, except an unnormal zero (which becomes a zero), and
  any packed-decimal operand. The arithmetic above therefore only ever sees
  normalized numbers, zeros, infinities and NaNs.
