#!/usr/bin/env python3
"""Summarize a shadow-model report written by Previous (PREVIOUS_UISA_SHADOW).

Usage: shadow_report.py <report> [--top N]
"""

import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m68kdec  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("report")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--details", type=int, default=40)
    a = ap.parse_args()
    cols, rows, details, kinds = None, [], [], ""
    with open(a.report) as f:
        for line in f:
            if line.startswith("# kinds:"):
                kinds = line[len("# kinds:"):].strip()
            elif line.startswith("# opword"):
                cols = line[2:].split()[2:]
            elif line.startswith("! "):
                details.append(line[2:].rstrip())
            elif not line.startswith("#") and line.strip():
                p = line.split()
                rows.append((int(p[0], 16), int(p[1]), [int(x) for x in p[2:]]))
    shadowed = collections.Counter()
    per_form = collections.defaultdict(lambda: collections.Counter())
    for op, n, res in rows:
        form = m68kdec.decode(op).form()
        for c, v in zip(cols, res):
            if v:
                shadowed[c] += v
                per_form[form][c] += v
    total = sum(v for c, v in shadowed.items() if c != "aborted")
    print("instructions by flow kind: %s" % kinds)
    print("PLA instructions shadowed: %d" % total)
    for c in cols:
        if shadowed[c]:
            print("  %-16s %12d  %6.3f%%" % (c, shadowed[c], 100.0 * shadowed[c] / max(1, total)))
    bad = [(sum(v for c, v in cnt.items() if c not in ("match", "aborted")), form, cnt) for form, cnt in per_form.items()]
    bad = sorted((b for b in bad if b[0]), reverse=True)
    if bad:
        print("\nforms with mismatches:")
        for n, form, cnt in bad[:a.top]:
            kinds_s = ", ".join("%s %d" % (c, v) for c, v in cnt.items() if c not in ("match",) and v)
            print("  %-34s %10d mismatched of %d  (%s)" % (form, n, sum(cnt.values()) - cnt["aborted"], kinds_s))
    if details:
        print("\nfirst details:")
        for d in details[:a.details]:
            op = int(d.split()[0], 16)
            print("  %-30s %s" % (m68kdec.decode(op).form(), d))


if __name__ == "__main__":
    main()
