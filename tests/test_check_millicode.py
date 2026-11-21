#!/usr/bin/env python3
"""Seeded bugs for tools/check_millicode.py.

Each case edits the real millicode sources in memory (never on disk), runs
the checker and expects an error naming the given resource at the given
location (label+offset of the reading instruction or call site). The
unmodified millicode must check clean. -v prints the checker's report for
each case.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import check_millicode  # noqa: E402
import gen_millicode  # noqa: E402
import uisa  # noqa: E402

VERBOSE = "-v" in sys.argv
spec = uisa.load()
d = gen_millicode.load_dispatch()
TEXTS = [(src, gen_millicode.source_text(src)) for src in d["sources"]]
ALLOW = check_millicode.load_allow(os.path.join(HERE, "..", "millicode"))
failures = 0


def run(edits, allow=None):
    texts = []
    for src, text in TEXTS:
        for esrc, old, new in edits:
            if esrc == src:
                assert text.count(old) == 1, "%s: %r occurs %d times" % (src, old, text.count(old))
                text = text.replace(old, new)
        texts.append((src, text))
    return check_millicode.check(spec, d, texts, ALLOW if allow is None else allow)


def case(name, edits, want=(), allow=None, clean=False):
    """want: [(location, resource, expected text in the message or None)]"""
    global failures
    c = run(edits, allow)
    msgs = ["\n".join(lines) for _, lines in c.errors]
    ok = True
    if clean and msgs:
        ok = False
    for loc, res, extra in want:
        hit = [m for m in msgs
               if (m.startswith(res + " ") or res in m.split("\n")[0])
               and (not loc or ("at   %s " % loc) in m) and (extra is None or extra in m)]
        if not hit:
            ok = False
    if not ok:
        failures += 1
    print("%s: %s" % ("ok" if ok else "FAIL", name))
    if VERBOSE or not ok:
        for m in msgs:
            print("    error: " + m.replace("\n", "\n    "))
        if not msgs:
            print("    (no errors)")


case("the real millicode is clean", [], clean=True)

case("register definition deleted (mull: Dh index x9)",
     [("integer.s", "        andi    x9, x10, 7              ; Dh\n", "")],
     [("mull64_done+0x4", "x9", None)])

case("handler reads a temp the insn contract does not supply (trap: x6)",
     [("exception.s", "trap:\n        andi    x5, x1, 15", "trap:\n        andi    x5, x6, 15")],
     [("trap", "x6", "insn handler trap")])

case("helper called before one of its inputs is defined (tas_mem: x21 for ea_addr)",
     [("mmu.s", "        addi    x21, x0, 1\n        addi    x22, x0, 2\n        call    ea_addr\n        ldl.8.gd",
       "        addi    x22, x0, 2\n        call    ea_addr\n        ldl.8.gd")],
     [("tas_mem+0x8", "x21", "chain ea_addr -> ea_incr")])

case("ld.c with no probe .us before it (u undefined)",
     [("mmu.s", "ptest_done:\n        csrw    mmusr, x7",
       "ptest_done:\n        ld.c.32.gd x7, 0(x6)\n        csrw    mmusr, x7")],
     [("ptest_done", "u", None)])

case("probe.r.ua as the first u writer (reads undefined u)",
     [("mmu.s", "        probe.r.ga x7, 0(x6)", "        probe.r.ua.ga x7, 0(x6)")],
     [("ptestr", "u", None)])

case("definition on one side of a branch only (ptest_insn: x7)",
     [("mmu.s", "        probe.w.ga x7, 0(x6)", "        probe.w.ga x8, 0(x6)")],
     [("ptest_done", "x7", None)])

case("u defined by probe.r.us before ld.c is clean",
     [("mmu.s", "        probe.r.ga x7, 0(x6)\nptest_done:",
       "        probe.r.us.ga x7, 0(x6)\n        ld.c.32.gd x7, 0(x6)\n        jal     x0, exit\nptest_done:")],
     clean=True)

case("u defined on one side of a branch only (st.c after the join)",
     [("mmu.s", "        probe.w.ga x7, 0(x6)", "        probe.w.us.ga x7, 0(x6)"),
      ("mmu.s", "        csrw    mmusr, x7\n", "        csrw    mmusr, x7\n        st.c.32.gd x7, 0(x6)\n")],
     [("ptest_done+0x4", "u", None)])

case("b.u in a callee with u undefined at the call site",
     [("integer.s", "cond_eval:", "cond_eval:\n        b.u     cc_t"),
      ("exception.s", "        call    cond_eval\n        b.eq    x10, x0, exit",
       "        call    cond_eval\n        b.eq    x10, x0, exit\n        probe.r.us.gd x5, 0(x4)\n        call    cond_eval")],
     [("trapcc+0x8", "u", "chain cond_eval")])

case("ucc-compiled code is checked (bitfield.c reads x30 at entry)",
     [("bitfield.c", "\nbitfield:\n", "\nbitfield:\n        add x5, x30, x0\n")],
     [("bitfield", "x30", "insn handler bitfield")])

case("a suppression without a reason is an error",
     [("mmu.s", "        rdg     x6, x8                  ; address", "        rdg     x6, x8 ; ubd-ok: x8")],
     [("", "needs a reason", None)])

case("allow-list entry suppresses a finding",
     [("exception.s", "trap:\n        andi    x5, x1, 15", "trap:\n        andi    x5, x6, 15")],
     allow=ALLOW + [{"at": "trap", "reg": "x6", "reason": "seeded"}], clean=True)

case("recursion is reported",
     [("ea.s", "get_ext2:\n        srli    x18, x2, 16", "get_ext2:\n        call    get_ext\n        srli    x18, x2, 16")],
     [("", "recursion", None)])

sys.exit(1 if failures else 0)
