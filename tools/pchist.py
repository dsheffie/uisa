#!/usr/bin/env python3
"""Work with a per-PC histogram written by Previous (PREVIOUS_PCHIST=<file>).

  pchist.py hot <pchist> [--mode super|user] [--top N]
      List the hottest code regions (runs of nearby PCs), with each region's
      instruction forms, to find idle and polling loops.

  pchist.py filter <pchist> --exclude LO-HI[,LO-HI...] [--mode super|user|all] -o <ophist>
      Write an opword histogram (the format tools/ophist.py reads) that leaves
      out the given PC ranges. Ranges are hex, inclusive; by default they only
      apply to supervisor-mode samples.
"""

import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m68kdec  # noqa: E402


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                if "dropped" in line and not line.rstrip().endswith("dropped 0"):
                    print("warning: " + line.strip(), file=sys.stderr)
                continue
            pc, s, op, cnt = line.split()
            rows.append((int(pc, 16), int(s), int(op, 16), int(cnt)))
    return rows


def regions(rows, gap=32):
    """Group PCs into runs where consecutive PCs are at most `gap` bytes apart."""
    by_pc = collections.defaultdict(lambda: [0, collections.Counter()])
    for pc, s, op, cnt in rows:
        by_pc[pc][0] += cnt
        by_pc[pc][1][op] += cnt
    out, cur = [], None
    for pc in sorted(by_pc):
        if cur and pc - cur["hi"] <= gap:
            cur["hi"] = pc
        else:
            cur = {"lo": pc, "hi": pc, "count": 0, "pcs": []}
            out.append(cur)
        cur["count"] += by_pc[pc][0]
        cur["pcs"].append((pc, by_pc[pc][0], by_pc[pc][1]))
    return out


def cmd_hot(a):
    rows = [r for r in load(a.pchist) if a.mode == "all" or r[1] == (1 if a.mode == "super" else 0)]
    total = sum(r[3] for r in rows)
    regs = sorted(regions(rows), key=lambda r: -r["count"])
    print("total %s instructions: %d" % (a.mode, total))
    for r in regs[:a.top]:
        print("\n%08x-%08x  %5.2f%%  (%d instructions)" % (r["lo"], r["hi"], 100.0 * r["count"] / total, r["count"]))
        for pc, cnt, ops in sorted(r["pcs"], key=lambda x: -x[1])[:a.show]:
            op = ops.most_common(1)[0][0]
            print("   %08x  %5.2f%%  %04x  %s" % (pc, 100.0 * cnt / total, op, m68kdec.decode(op).form()))


def parse_ranges(text):
    out = []
    for part in text.split(","):
        lo, hi = part.split("-")
        out.append((int(lo, 16), int(hi, 16)))
    return out


def cmd_filter(a):
    ranges = parse_ranges(a.exclude)
    applies = {"super": (1,), "user": (0,), "all": (0, 1)}[a.mode]
    hist = collections.defaultdict(lambda: [0, 0])
    kept = dropped = 0
    for pc, s, op, cnt in load(a.pchist):
        if s in applies and any(lo <= pc <= hi for lo, hi in ranges):
            dropped += cnt
            continue
        hist[op][s] += cnt
        kept += cnt
    with open(a.out, "w") as f:
        f.write("# opword user super (filtered from %s, excluded %s)\n" % (os.path.basename(a.pchist), a.exclude))
        for op in sorted(hist):
            f.write("%04x %d %d\n" % (op, hist[op][0], hist[op][1]))
    print("kept %d, excluded %d (%.1f%%)" % (kept, dropped, 100.0 * dropped / max(1, kept + dropped)))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    h = sub.add_parser("hot")
    h.add_argument("pchist")
    h.add_argument("--mode", choices=["super", "user", "all"], default="super")
    h.add_argument("--top", type=int, default=8)
    h.add_argument("--show", type=int, default=8, help="PCs to list per region")
    f = sub.add_parser("filter")
    f.add_argument("pchist")
    f.add_argument("--exclude", required=True)
    f.add_argument("--mode", choices=["super", "user", "all"], default="super")
    f.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    {"hot": cmd_hot, "filter": cmd_filter}[a.cmd](a)


if __name__ == "__main__":
    main()
