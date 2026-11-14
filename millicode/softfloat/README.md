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
- The rest of the arithmetic Previous implements: FINT/FINTRZ, FGETEXP,
  FGETMAN, FSCALE, FMOD, FREM, FSGLMUL/FSGLDIV, FCMP/FTST, FABS/FNEG,
  int8/int16 and single/double/packed conversions.
- The FPU instruction handler itself: FP registers in hidden memory, FPCR,
  FPSR and FPIAR in their CSRs, decoding the FPU command word, operand
  fetch and conversion from every source format, condition codes and the
  accrued exception byte, FMOVE out, FMOVEM, FSAVE/FRESTORE.
- The 68040 does not implement the transcendental functions (FSIN, FETOX,
  ...) in hardware: it traps them to the OS's FPSP. Previous computes them
  directly instead (`softfloat_fpsp.c`).
