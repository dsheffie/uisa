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


def cmd_lengths(a):
    """Dynamic instruction-length distribution.

    Lengths come from the opword (m68kdec.length). Where the opword cannot fix
    the length (indexed modes, some FPU forms), the gap to the next executed PC
    is used instead when it is plausible. Exact static lengths are also
    checked against those gaps, as a test of the length decoder.
    """
    ranges = parse_ranges(a.exclude) if a.exclude else []
    by_mode = {0: collections.defaultdict(lambda: [0, collections.Counter()]),
               1: collections.defaultdict(lambda: [0, collections.Counter()])}
    for pc, s, op, cnt in load(a.pchist):
        if s == 1 and any(lo <= pc <= hi for lo, hi in ranges):
            continue
        by_mode[s][pc][0] += cnt
        by_mode[s][pc][1][op] += cnt
    for s, name in ((0, "user"), (1, "supervisor")):
        pcs = sorted(by_mode[s])
        dist = collections.Counter()
        agree = disagree = 0
        idx_total = idx_full = 0
        bad = collections.Counter()
        for k, pc in enumerate(pcs):
            cnt, ops = by_mode[s][pc]
            op = ops.most_common(1)[0][0]
            static, exact = m68kdec.length(op)
            gap = pcs[k + 1] - pc if k + 1 < len(pcs) else None
            n = static
            if exact:
                if gap is not None and gap <= 22:
                    if gap == static:
                        agree += cnt
                    else:
                        disagree += cnt
                        bad[m68kdec.decode(op).form()] += cnt
            elif gap is not None and static <= gap <= 22:
                n = gap
            ins = m68kdec.decode(op)
            if ins.src in m68kdec.INDEXED or ins.dst in m68kdec.INDEXED:
                idx_total += cnt
                if n > static:
                    idx_full += cnt
            dist[n] += cnt
        total = sum(dist.values())
        mean = sum(n * c for n, c in dist.items()) / max(1, total)
        print("## %s: %d instructions, mean length %.2f bytes" % (name, total, mean))
        print("")
        print("| Bytes | % | Cumulative % |")
        print("|---|---|---|")
        cum = 0
        for n in sorted(dist):
            cum += dist[n]
            print("| %d | %.2f | %.2f |" % (n, 100.0 * dist[n] / total, 100.0 * cum / total))
        print("")
        checked = agree + disagree
        print("Length decoder vs. next-PC gap: %.2f%% of %d checkable instructions agree." %
              (100.0 * agree / max(1, checked), checked))
        if bad:
            print("Most common mismatches (often a taken branch or code that was never reached): " +
                  ", ".join("`%s` %.3f%%" % (f, 100.0 * c / total) for f, c in bad.most_common(5)))
        if idx_total:
            print("Indexed addressing: %.2f%% of instructions; %.1f%% of those appear to use the full format." %
                  (100.0 * idx_total / total, 100.0 * idx_full / idx_total))
        print("")


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
    ln = sub.add_parser("lengths")
    ln.add_argument("pchist")
    ln.add_argument("--exclude", default="", help="supervisor PC ranges to leave out (idle loop, ROM)")
    a = ap.parse_args()
    {"hot": cmd_hot, "filter": cmd_filter, "lengths": cmd_lengths}[a.cmd](a)


if __name__ == "__main__":
    main()
