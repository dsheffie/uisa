#!/usr/bin/env python3
"""Millicode spacing from a shadow report (# gap / # mclen / # mcop lines).

Usage: mcgap.py <report> [--top N]

  gap    the run of non-millicode (PLA and sequencer) instructions before each
         millicode instruction, as a log2 histogram: how many millicode entries
         fall in each bucket, and what share of all non-millicode instructions
         sit in runs of that length
  mclen  millicode words executed per millicode instruction, log2 histogram
  mcop   the opwords that execute the most millicode words
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m68kdec  # noqa: E402


def bucket(b):
    return "0" if b == 0 else "%d-%d" % (1 << (b - 1), (1 << b) - 1)


def hist(title, rows, unit):
    tc = sum(c for _, c, _ in rows)
    ts = sum(s for _, _, s in rows)
    print("%s: %d events, %d %s" % (title, tc, ts, unit))
    cc = cs = 0
    for b, c, s in rows:
        cc += c
        cs += s
        print("  %-16s %10d %5.1f%%  cum %5.1f%%    %s %5.1f%%  cum %5.1f%%" % (
            bucket(b), c, 100.0 * c / tc, 100.0 * cc / tc, unit, 100.0 * s / ts, 100.0 * cs / ts))
    return tc, ts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("report")
    ap.add_argument("--top", type=int, default=20)
    a = ap.parse_args()
    gap, mcl, ops = [], [], []
    for line in open(a.report):
        f = line.split()
        if line.startswith("# gap "):
            gap.append((int(f[2]), int(f[3]), int(f[4])))
        elif line.startswith("# mclen "):
            mcl.append((int(f[2]), int(f[3]), int(f[4])))
        elif line.startswith("# mcop "):
            ops.append((int(f[2], 16), int(f[3]), int(f[4])))
    if not gap:
        sys.exit("no # gap lines in %s" % a.report)
    hist("non-millicode run before each millicode instruction", gap, "insns")
    print()
    _, words = hist("millicode words per millicode instruction", mcl, "words")
    print("\ntop opwords by millicode words:")
    for op, n, w in sorted(ops, key=lambda x: -x[2])[:a.top]:
        print("  %04x %-26s %9d x %6.1f = %10d  %4.1f%%" % (op, m68kdec.decode(op).form(), n, w / n, w, 100.0 * w / words))


if __name__ == "__main__":
    main()
