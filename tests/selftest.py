#!/usr/bin/env python3
"""Round-trip every variant through encode -> decode -> disassemble -> assemble."""

import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import uisa  # noqa: E402


def test_u(spec):
    """The millicode predicate u: probe forms, conditional loads / stores,
    b.u / b.nu: exact encodings, reserved field values, operand ranges, the
    flag metadata, and the spec's u-declaration check."""
    import copy
    import yaml
    fails = 0

    def expect(cond, what):
        nonlocal fails
        if not cond:
            print("u:", what)
            fails += 1

    enc = {
        "probe.r.us.gd x5, 0(x10)":     (36 << 26) | (5 << 20) | (10 << 14) | (1 << 12) | (1 << 10),
        "probe.w.ua.ga x6, -512(x11)":  (37 << 26) | (6 << 20) | (11 << 14) | (3 << 12) | (2 << 10) | 0x200,
        "probe.r.h x7, 511(x6)":        (36 << 26) | (7 << 20) | (6 << 14) | (0 << 12) | (0 << 10) | 511,
        "ld.c.8u.gd x11, 0(x10)":       (54 << 26) | (11 << 20) | (10 << 14) | (1 << 12) | (0 << 9),
        "ld.c.16s.gp x11, 255(x10)":    (54 << 26) | (11 << 20) | (10 << 14) | (2 << 12) | (3 << 9) | 255,
        "ld.c.32.gd x11, -1(x10)":      (54 << 26) | (11 << 20) | (10 << 14) | (1 << 12) | (4 << 9) | 0x1ff,
        "st.c.16.gp x13, 255(x2)":      (53 << 26) | (13 << 20) | (2 << 14) | (2 << 12) | (1 << 10) | 255,
        "st.c.8.gd d0, 0(a0)":          (53 << 26) | (32 << 20) | (40 << 14) | (1 << 12),
        "stu.c.32.ga x13, -256(x2)":    (53 << 26) | (13 << 20) | (2 << 14) | (3 << 12) | (2 << 10) | (1 << 9) | 0x100,
        "b.u 8":                        (16 << 26) | (6 << 11) | 2,
        "b.nu -4":                      (16 << 26) | (7 << 11) | 0x7ff,
    }
    for text, word in enc.items():
        w = spec.assemble_line(text)
        expect(w == word, "%s: %08x, expected %08x" % (text, w, word))
        expect(spec.disassemble(word) == text, "%s disassembles as %r" % (text, spec.disassemble(word)))
    # st.c carries its data in rd, as st.N
    expect(spec.reads(spec.by_mnemonic["st.c.32.gd"]) == spec.reads(spec.by_mnemonic["st.32.gd"]) == ["rs1", "rd"],
           "st.c reads rs1 and rd (the data), as st.32")
    expect(not spec.writes_rd(spec.by_mnemonic["st.c.32.gd"]) and spec.writes_rd(spec.by_mnemonic["ld.c.32.gd"]),
           "st.c writes no register, ld.c writes rd")
    # reserved field values are illegal
    for sp in range(4):
        for major in (36, 37):
            expect(spec.match((major << 26) | (sp << 12) | (3 << 10) | 5) is None, "probe um=3 is illegal")
            for um in range(3):
                expect(spec.match((major << 26) | (sp << 12) | (um << 10)) is not None, "probe um=%d is legal" % um)
        for lk in range(8):
            v = spec.match((54 << 26) | (sp << 12) | (lk << 9) | 7)
            expect((v is None) == (lk >= 5), "ld.c lk=%d" % lk)
        for sz in range(4):
            for ul in range(2):
                v = spec.match((53 << 26) | (sp << 12) | (sz << 10) | (ul << 9) | 7)
                expect((v is None) == (sz == 3), "st.c sz=%d ul=%d" % (sz, ul))
                if v is not None:
                    expect(v.mnemonic.startswith("stu.c." if ul else "st.c.") and v.bytes == (1, 2, 4)[sz],
                           "st.c sz=%d ul=%d is %s" % (sz, ul, v.mnemonic))
    for major in range(49, 53):
        expect(all(spec.match((major << 26) | x) is None for x in (0, 0x3ffffff)), "major %d stays reserved (FP)" % major)
    # major 48 is FP: its register fields name FP registers where the op says so
    fpenc = {
        "fadd.d f8, f9, f10":   (48 << 26) | (8 << 20) | (9 << 14) | (10 << 8) | 0,
        "fsqrt.d f0, f15":      (48 << 26) | (0 << 20) | (15 << 14) | 4,
        "fsethi f9, x5":        (48 << 26) | (9 << 20) | (5 << 14) | 16,
        "fgetlo d3, f7":        (48 << 26) | (35 << 20) | (7 << 14) | 19,
    }
    for text, word in fpenc.items():
        w = spec.assemble_line(text)
        expect(w == word, "%s: %08x, expected %08x" % (text, w, word))
        expect(spec.disassemble(word) == text, "%s disassembles as %r" % (text, spec.disassemble(word)))
    expect(spec.reads(spec.by_mnemonic["fsethi"]) == ["rs1", "rd"] and spec.int_reads(spec.by_mnemonic["fsethi"]) == ["rs1"],
           "fsethi reads x rs1 and FP rd")
    expect(spec.writes_rd(spec.by_mnemonic["fgethi"]) and spec.writes_int_rd(spec.by_mnemonic["fgethi"]) and
           not spec.writes_int_rd(spec.by_mnemonic["fadd.d"]), "fgethi writes an x register, fadd.d an FP one")
    # b.u / b.nu ignore rs1 and rs2; the compare branches keep cond 0-5
    expect(spec.match((16 << 26) | (5 << 20) | (9 << 14) | (6 << 11)).mnemonic == "b.u", "b.u ignores rs1/rs2")
    expect(spec.match((16 << 26) | (7 << 11) | (63 << 20)).mnemonic == "b.nu", "b.nu ignores rs1/rs2")
    for c in spec.base_conds:
        v = spec.by_mnemonic["b." + c["name"]]
        expect(v.fixed["cond"] == c["code"] < 6 and v.operands == ["rs1", "rs2", "imm"] and not v.reads_flags,
               "b.%s unchanged and not a reader of u" % c["name"])
    # displacement ranges: probes 10 bits, ld.c / st.c 9 bits
    for text, ok in (("probe.r.gd x1, 511(x2)", True), ("probe.r.gd x1, 512(x2)", False),
                     ("probe.w.us.gd x1, -512(x2)", True), ("probe.w.us.gd x1, -513(x2)", False),
                     ("ld.c.32.gd x1, 255(x2)", True), ("ld.c.32.gd x1, 256(x2)", False),
                     ("st.c.8.gd x1, -256(x2)", True), ("stu.c.8.gd x1, -257(x2)", False)):
        try:
            spec.assemble_line(text)
            got = True
        except uisa.SpecError:
            got = False
        expect(got == ok, "%s %s" % (text, "should assemble" if ok else "is out of range"))
    # exactly these read / write u
    rd_u = sorted(v.mnemonic for v in spec.variants if "u" in v.reads_flags)
    wr_u = sorted(v.mnemonic for v in spec.variants if "u" in v.writes_flags)
    sfx = [s["suffix"] for s in spec.spaces]
    want_rd = sorted(["b.u", "b.nu"] + ["%s.%s" % (f, x) for x in sfx for f in
                                        ("probe.r.ua", "probe.w.ua", "ld.c.8u", "ld.c.8s", "ld.c.16u", "ld.c.16s",
                                         "ld.c.32", "st.c.8", "st.c.16", "st.c.32", "stu.c.8", "stu.c.16", "stu.c.32")])
    want_wr = sorted("%s.%s" % (f, x) for x in sfx for f in ("probe.r.us", "probe.r.ua", "probe.w.us", "probe.w.ua"))
    expect(rd_u == want_rd, "readers of u: %s" % rd_u)
    expect(wr_u == want_wr, "writers of u: %s" % wr_u)
    expect(all(v.locked == "end" for v in spec.variants if v.mnemonic.startswith("stu.c.")), "stu.c ends the lock")
    # the motivating sequence assembles
    prog = spec.assemble("""
          probe.r.us.gd  x5, 0(a0)
          probe.r.ua.gd  x6, 0(a1)
          ld.c.32.gd     x11, 0(a0)
          ld.c.32.gd     x12, 0(a1)
          st.c.32.gd     x13, 0(a2)
          b.nu           fault
          gexit
    fault: gfault 2
    """)
    expect(spec.disassemble(prog[5]) == "b.nu 8", "b.nu label: %s" % spec.disassemble(prog[5]))
    # the spec's declared u reads / writes must match the semantics
    with open(uisa.SPEC_PATH) as f:
        d0 = yaml.safe_load(f)

    def rejects(mutate, what):
        d = copy.deepcopy(d0)
        mutate(d)
        try:
            uisa.Spec(d)
        except uisa.SpecError:
            return
        expect(False, "spec check accepts " + what)

    def op(d, sec, name):
        return next(e for e in d[sec] if e.get("name") == name or e.get("family") == name)
    rejects(lambda d: op(d, "base_ops", "b.u").pop("reads_flags"), "b.u without reads_flags: [u]")
    rejects(lambda d: op(d, "base_ops", "b").__setitem__("reads_flags", ["u"]), "b.<cc> declaring a u read")
    rejects(lambda d: op(d, "mem_ops", "ld.c.32").__setitem__("sem", "r = LOAD(sp, a + IMM, 4);"), "ld.c not reading u")
    rejects(lambda d: op(d, "mem_ops", "st.c.8").pop("reads_flags"), "st.c without reads_flags: [u]")
    rejects(lambda d: op(d, "mem_ops", "probe.r").__setitem__("sem", "r = MMUSR(TRANSLATE(sp, a + IMM, READ));"),
            "a probe with u forms whose sem never writes u")
    rejects(lambda d: op(d, "guest_ops", "add").__setitem__("sem", op(d, "guest_ops", "add")["sem"] + "U = 1;"),
            "a guest op writing u")
    rejects(lambda d: op(d, "base_ops", "add").__setitem__("sem", "r = a + b + uin;"), "add reading u undeclared")
    return fails


def main():
    spec = uisa.load()
    rnd = random.Random(7)
    regs = sorted(spec.regs.values())
    fp_regs = sorted(spec.fp_regs.values())
    fails = 0
    for v in spec.variants:
        for _ in range(20):
            vals = {}
            for name, f in v.fmt.fields.items():
                if name in v.fixed or name == "rsv":
                    continue
                if name in ("rd", "rs1", "rs2"):
                    vals[name] = rnd.choice(fp_regs if name in spec.fp_fields(v) else regs)
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
    fails += test_u(spec)
    print("selftest: %d variants, %d failures" % (len(spec.variants), fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
