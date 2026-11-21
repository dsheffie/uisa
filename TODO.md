# TODO

## Flows

- **EXG has no PLA flow.** Every EXG form (Dx,Dy / Ax,Ay / Dx,Ay) is
  `unmapped`: the `EXG` template in `spec/flows.yaml` names fixed registers
  (`d1`, `d2`) and `tools/flows.py` has no addressing-mode fragment for the
  `Rn` operands `m68kdec` gives it. Needs the two register fields substituted
  (Rx bits 11:9, Ry bits 2:0, D or A by the opmode). Seen in q800sim
  `--engine uisa`: 326 EXG Dn,An fell back to the interpreter booting the
  installed Debian system to login (none in the installer's kernel boot).

## Validation

- **csmith through ucc.** Compile csmith programs with `ucc` into uisa code
  and run them on the micro-engine (the C++ engine now, the RTL later),
  comparing each program's checksum with the host build's. That exercises the
  compiler and the engine on random C, beyond what the 68k workloads reach.

## Engine (q800sim `--engine uisa`)

- **The FPU runs on the interpreter.** `uisa_m68k_state` has no FP
  registers, so coprocessor-1 opwords (FPU ops, FSAVE, FRESTORE) never reach
  the FPU millicode: 154K of them in the installed-system boot to login.
- **Interrupts and access faults are taken by the interpreter**, not by the
  `interrupt` / `exception` millicode entry points. The shadow models both
  (`exec_event`); the engine does not drive them yet.
- **CAS2 is unexercised on real code.** The routine passes the shadow's
  random cases, but the Linux boot never executes CAS2, so q800sim's checker
  has not seen it.
