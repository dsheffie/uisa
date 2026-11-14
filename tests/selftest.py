#!/usr/bin/env python3
"""Round-trip every variant through encode -> decode -> disassemble -> assemble."""

import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import uisa  # noqa: E402


def main():
    spec = uisa.load()
    rnd = random.Random(7)
    regs = sorted(spec.regs.values())
    fails = 0
    for v in spec.variants:
        for _ in range(20):
            vals = {}
            for name, f in v.fmt.fields.items():
                if name in v.fixed or name == "rsv":
                    continue
                if name in ("rd", "rs1", "rs2"):
                    vals[name] = rnd.choice(regs)
                elif name == "imm":
                    signed, scale = spec.imm_info(v)
                    lo, hi = ((-(1 << (f.width - 1)), (1 << (f.width - 1)) - 1) if signed else (0, (1 << f.width) - 1))
                    if v.imm_max is not None:
                        hi = min(hi, v.imm_max)
                    vals[name] = rnd.randint(lo, hi) * scale
                else:
                    vals[name] = rnd.randint(0, (1 << f.width) - 1)
            if v.tied:
                vals["rs1"] = vals["rd"]
            w = spec.encode(v, vals)
            dv, dvals = spec.decode(w)
            if dv is not v or any(dvals.get(k) != val for k, val in vals.items()):
                print("decode mismatch %s %08x %s %s" % (v.mnemonic, w, vals, dvals))
                fails += 1
                continue
            text = spec.disassemble(w)
            # fields an instruction ignores (e.g. rs2 of a unary op) are not
            # printed, so compare after re-encoding the printed text
            w2 = spec.assemble_line(text)
            w2_dec = spec.disassemble(w2)
            if w2_dec != text:
                print("asm round-trip %s: %r -> %08x -> %r" % (v.mnemonic, text, w2, w2_dec))
                fails += 1
    # a tiny program with labels
    prog = spec.assemble("""
    loop: addi x1, x1, -1
          b.ne x1, x0, loop
          call sub
          gexit
    sub:  ret
    """)
    if spec.disassemble(prog[1]) != "b.ne x1, x0, -4" or spec.disassemble(prog[2]) != "call 8":
        print("label resolution failed:", [spec.disassemble(w) for w in prog])
        fails += 1
    print("selftest: %d variants, %d failures" % (len(spec.variants), fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
