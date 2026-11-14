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

## Not done

- `saveFloatx80Internal`: the unrounded intermediate the 68040's exception
  frames carry when an enabled FP exception traps. Not needed until traps
  are enabled.
- int8/int16 and single/double conversions (the memory operand formats).
  Packed decimal is never converted: the 68040 traps it.
- The FPU instruction handler itself: FP registers in hidden memory, FPCR,
  FPSR and FPIAR in their CSRs, decoding the FPU command word, operand
  fetch and conversion from every source format, condition codes and the
  accrued exception byte, FMOVE out, FMOVEM, FSAVE/FRESTORE.

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
