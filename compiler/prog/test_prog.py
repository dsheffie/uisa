#!/usr/bin/env python3
"""Differential tests of `ucc --program`: host C compiler vs ucc + uisa ISS.

usage: test_prog.py [random-programs [first-seed]] [--csmith N] [--keep]

Each program is compiled twice: natively (cc, with compiler/prog/host_prog.h
for ucc's type names and -fwrapv) and with `ucc --program`, built into an
image (uimage.py) and run on the ISS (prun). main's result (the CRC of every
global, for generated programs) must agree.

Programs: compiler/prog/tests/*.c (hand-written), compiler/prog/tests_csmith/*.c
(csmith-format programs), random programs from gen_prog.py (seeds first-seed,
first-seed+1, ...), and with --csmith N, N programs from csmith (`csmith` on
PATH, or CSMITH=/path/to/csmith).

csmith-format programs include "csmith.h": both builds preprocess them with the
host C preprocessor and -I compiler/prog/csmith (the shim runtime), the ucc
build with -D__UCC__.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import gen_prog  # noqa: E402
import uimage  # noqa: E402

UCC = os.path.join(ROOT, "build", "compiler", "ucc")
PRUN = os.path.join(ROOT, "build", "prun")
OUT = os.path.join(ROOT, "build", "prog_tests")
# csmith programs may run for a very long time (or not end); like csmith's own
# scripts, those are skipped rather than counted
HOST_SECONDS = 5
ISS_STEPS = 500000000


class Skip(Exception):
    pass


def build_prun():
    src = os.path.join(HERE, "prun.cc")
    deps = [src, os.path.join(ROOT, "compiler", "tests", "iss.h"), os.path.join(ROOT, "build", "uisa_sem.inc")]
    if os.path.exists(PRUN) and all(os.path.getmtime(PRUN) >= os.path.getmtime(d) for d in deps):
        return
    cmd = ["c++", "-std=c++17", "-O2", "-w", "-I" + os.path.join(ROOT, "build"),
           "-I" + os.path.join(ROOT, "compiler", "tests"), "-o", PRUN, src]
    if subprocess.call(cmd) != 0:
        sys.exit("test_prog: prun does not build")


SHIM = os.path.join(HERE, "csmith")
CSMITH_FLAGS = ["--no-unions", "--no-bitfields", "--no-volatiles", "--no-packed-struct", "--no-float"]


def preprocess(src, tag, ucc):
    out = os.path.join(OUT, tag + (".ucc.i" if ucc else ".host.i"))
    cmd = ["cc", "-E", "-P", "-I", SHIM] + (["-D__UCC__"] if ucc else ["-Dmain=prog_main"]) + ["-o", out, src]
    if subprocess.call(cmd) != 0:
        sys.exit("test_prog: cannot preprocess " + src)
    return out


def host_result(src, tag, csmith=False):
    obj = os.path.join(OUT, tag + ".o")
    exe = os.path.join(OUT, tag + ".host")
    if csmith:
        cc = ["cc", "-std=c99", "-w", "-O1", "-fwrapv", "-c", "-o", obj, preprocess(src, tag, False)]
    else:
        cc = ["cc", "-std=c99", "-w", "-O1", "-fwrapv", "-include", os.path.join(HERE, "host_prog.h"),
              "-Dmain=prog_main", "-c", "-o", obj, src]
    if subprocess.call(cc) != 0:
        return None, "host compile failed"
    if subprocess.call(["cc", "-o", exe, obj, os.path.join(HERE, "host_main.c")]) != 0:
        return None, "host link failed"
    try:
        p = subprocess.run([exe], capture_output=True, text=True, timeout=HOST_SECONDS)
    except subprocess.TimeoutExpired:
        raise Skip("runs longer than %d s natively" % HOST_SECONDS)
    if p.returncode != 0:
        return None, "host run failed (%d)" % p.returncode
    return p.stdout.split()[1], None


def uisa_result(src, tag, flags, spec, csmith=False):
    asm = os.path.join(OUT, tag + ".s")
    img = os.path.join(OUT, tag + ".hex")
    if csmith:
        src = preprocess(src, tag, True)
    p = subprocess.run([UCC, "--program"] + flags + ["-o", asm, src], capture_output=True, text=True)
    if p.returncode != 0:
        return None, "ucc failed:\n" + p.stderr, 0
    with open(asm) as f:
        segs, _ = uimage.build(f.read(), spec)
    uimage.write_hex(segs, img)
    p = subprocess.run([PRUN, img, str(ISS_STEPS)], capture_output=True, text=True, timeout=600)
    words = p.stdout.split()
    if "step limit reached" in p.stdout:
        raise Skip("more than %d ISS steps" % ISS_STEPS)
    if p.returncode != 0 or words[:1] != ["result"]:
        return None, "prun: " + p.stdout.strip(), 0
    return words[1], None, int(words[3])


def main():
    argv = sys.argv[1:]
    ncsmith = 0
    if "--csmith" in argv:
        k = argv.index("--csmith")
        ncsmith = int(argv[k + 1])
        del argv[k:k + 2]
    args = [a for a in argv if not a.startswith("-")]
    keep = "--keep" in argv
    flags = []
    count = int(args[0]) if args else 200
    first = int(args[1]) if len(args) > 1 else 1
    os.makedirs(OUT, exist_ok=True)
    build_prun()
    spec = uimage.uisa.load()
    progs = []   # (tag, source, csmith format)
    for d, cs in (("tests", False), ("tests_csmith", True)):
        for name in sorted(os.listdir(os.path.join(HERE, d))):
            if name.endswith(".c"):
                progs.append((name[:-2], os.path.join(HERE, d, name), cs))
    for seed in range(first, first + count):
        path = os.path.join(OUT, "gen_%d.c" % seed)
        with open(path, "w") as f:
            f.write(gen_prog.Gen(seed).program())
        progs.append(("gen_%d" % seed, path, False))
    if ncsmith:
        csmith = os.environ.get("CSMITH", "csmith")
        for k in range(ncsmith):
            path = os.path.join(OUT, "csmith_%d.c" % k)
            with open(path, "w") as f:
                if subprocess.call([csmith, "--seed", str(first + k)] + CSMITH_FLAGS, stdout=f) != 0:
                    sys.exit("test_prog: %s failed (is csmith installed?)" % csmith)
            progs.append(("csmith_%d" % k, path, True))
    fails = 0
    skips = 0
    steps = []
    for tag, src, cs in progs:
        try:
            want, why = host_result(src, tag, cs)
            if why:
                print("FAIL %s: %s" % (tag, why))
                fails += 1
                continue
            got, why, n = uisa_result(src, tag, flags, spec, cs)
        except Skip as e:
            print("skip %s: %s" % (tag, e))
            skips += 1
            continue
        if why or got != want:
            print("FAIL %s: %s" % (tag, why or "result %s, host %s" % (got, want)))
            fails += 1
            continue
        steps.append(n)
        if not tag.startswith(("gen_", "csmith_")):
            print("prog %-10s ok (%s, %d steps)" % (tag, got, n))
        elif not keep:
            for ext in (".c", ".o", ".host", ".s", ".hex", ".ucc.i", ".host.i"):
                try:
                    os.remove(os.path.join(OUT, tag + ext))
                except OSError:
                    pass
    print("test_prog: %d programs, %d failures%s%s" % (len(progs), fails,
          ", %d skipped (too long)" % skips if skips else "",
          ", up to %d ISS steps" % max(steps) if steps else ""))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
