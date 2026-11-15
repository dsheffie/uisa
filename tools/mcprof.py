#!/usr/bin/env python3
"""Millicode execution profile from a shadow report (# mcprof lines).

Usage: mcprof.py <report> [--top N] [--words]

Attributes each executed millicode word to the label region it is in (the
nearest preceding label in build/uisa_millicode.map) and to the routine that
owns that region (the nearest preceding dispatch handler or entry point), and
classes the instructions so dispatch overhead stands out:
  branch   conditional branches (b.*)
  const    addi rd, x0, K (mostly compare-chain constants)
  field    srli/andi/slli field decoding
  call     call / ret / jal / bx
  other    everything else
"""

import argparse
import collections
import os
import sys

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
import uisa  # noqa: E402


def load_image():
    """Disassembly of each millicode word, from MC_IMAGE in build/uisa_millicode.inc."""
    text = open(os.path.join(ROOT, "build", "uisa_millicode.inc")).read()
    body = text[text.index("MC_IMAGE"):]
    body = body[body.index("{") + 1:body.index("};")]
    spec = uisa.load()
    return [spec.disassemble(int(w, 16)) for w in body.replace("\n", " ").split(",") if w.strip()]


def klass(dis):
    m = dis.split()[0]
    if m.startswith("b.") or m in ("gbz", "gbnz"):
        return "branch"
    if m == "addi" and ", x0, " in dis:
        return "const"
    if m in ("srli", "andi", "slli", "bfextui", "bfextsi"):
        return "field"
    if m in ("call", "ret", "jal", "bx", "jalr"):
        return "call"
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("report")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--words", action="store_true", help="also list the hottest words")
    a = ap.parse_args()
    prof = {}
    for line in open(a.report):
        if line.startswith("# mcprof "):
            _, _, off, n = line.split()
            prof[int(off, 16)] = int(n)
    if not prof:
        sys.exit("no # mcprof lines in %s" % a.report)
    labels = []
    for line in open(os.path.join(ROOT, "build", "uisa_millicode.map")):
        off, name = line.split()
        labels.append((int(off, 16), name))
    labels.sort()
    d = yaml.safe_load(open(os.path.join(ROOT, "millicode", "dispatch.yaml")))
    roots = set(d["handlers"].values()) | set((d.get("entries") or {}).values())
    roots |= {"exit", "privilege", "illegal", "unsupported", "div_zero", "div_overflow", "line_f"}
    image = load_image()
    # subroutines (call targets) count as routines of their own
    by_off = {o: n for o, n in labels}
    for i, dis in enumerate(image):
        if dis.startswith("call "):
            t = i * 4 + int(dis.split()[1])
            if t in by_off:
                roots.add(by_off[t])

    def owner(off):
        lab = rout = "?"
        for o, n in labels:
            if o > off:
                break
            lab = n
            if n in roots:
                rout = n
        return lab, rout

    total = sum(prof.values())
    by_label, by_routine = collections.Counter(), collections.Counter()
    kinds = collections.defaultdict(collections.Counter)
    for off, n in prof.items():
        lab, rout = owner(off)
        by_label[lab] += n
        by_routine[rout] += n
        k = klass(image[off // 4]) if off // 4 < len(image) else "other"
        kinds[rout][k] += n
        kinds["(all)"][k] += n
    print("millicode instructions executed: %d" % total)
    ka = kinds["(all)"]
    print("  by kind: " + ", ".join("%s %.1f%%" % (k, 100.0 * ka[k] / total)
                                    for k in ("branch", "const", "field", "call", "other")))
    print("\nby routine:")
    for r, n in by_routine.most_common(a.top):
        k = kinds[r]
        print("  %-16s %12d  %5.1f%%   branch %4.1f%% const %4.1f%% field %4.1f%% call %4.1f%%" % (
            r, n, 100.0 * n / total, 100.0 * k["branch"] / n, 100.0 * k["const"] / n,
            100.0 * k["field"] / n, 100.0 * k["call"] / n))
    print("\nby label:")
    for lab, n in by_label.most_common(a.top):
        print("  %-20s %12d  %5.1f%%" % (lab, n, 100.0 * n / total))
    if a.words:
        print("\nhottest words:")
        for off, n in sorted(prof.items(), key=lambda kv: -kv[1])[:a.top]:
            print("  %06x %-14s %12d  %s" % (off, owner(off)[0], n, image[off // 4]))


if __name__ == "__main__":
    main()
