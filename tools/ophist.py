#!/usr/bin/env python3
"""Summarize an opword histogram written by Previous (PREVIOUS_OPHIST=<file>).

Usage: ophist.py <hist> [--mode all|user|super] [--top N] > report.md
"""

import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m68kdec  # noqa: E402


def load(path):
    counts = {}
    with open(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            op, user, sup = line.split()
            counts[int(op, 16)] = (int(user), int(sup))
    return counts


def table(title, rows, total, top, out):
    out.append("## " + title)
    out.append("")
    out.append("| # | Item | Count | % | Cumulative % |")
    out.append("|---|---|---|---|---|")
    cum = 0
    for i, (k, c) in enumerate(rows[:top]):
        cum += c
        out.append("| %d | `%s` | %d | %.2f | %.2f |" % (i + 1, k, c, 100.0 * c / total, 100.0 * cum / total))
    out.append("")


def coverage(rows, total, targets=(50, 80, 90, 95, 99, 99.9)):
    res, cum, i = [], 0, 0
    for t in targets:
        while i < len(rows) and 100.0 * cum / total < t:
            cum += rows[i][1]
            i += 1
        res.append((t, i))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hist")
    ap.add_argument("--mode", choices=["all", "user", "super"], default="all")
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args()

    raw = load(a.hist)
    pick = {"all": lambda u, s: u + s, "user": lambda u, s: u, "super": lambda u, s: s}[a.mode]
    counts = {op: pick(u, s) for op, (u, s) in raw.items() if pick(u, s)}
    total = sum(counts.values())
    user_total = sum(u for u, _ in raw.values())
    sup_total = sum(s for _, s in raw.values())

    by_mnem = collections.Counter()
    by_form = collections.Counter()
    by_ea = collections.Counter()
    mem_ops = 0
    for op, c in counts.items():
        ins = m68kdec.decode(op)
        by_mnem[ins.mnem + ("." + ins.size if ins.size else "")] += c
        by_form[ins.form()] += c
        for role, e in (("src", ins.src), ("dst", ins.dst)):
            if e:
                by_ea["%s %s" % (role, e)] += c
        if m68kdec.is_memory(ins.src) or m68kdec.is_memory(ins.dst) or ins.mnem in ("RTS", "JSR", "BSR", "PEA", "LINK", "UNLK", "RTE", "RTD", "RTR"):
            mem_ops += c

    out = ["# 68k dynamic instruction histogram", ""]
    out.append("Source: `%s`, mode: **%s**." % (os.path.basename(a.hist), a.mode))
    out.append("")
    out.append("| | Instructions |")
    out.append("|---|---|")
    out.append("| user | %d (%.1f%%) |" % (user_total, 100.0 * user_total / max(1, user_total + sup_total)))
    out.append("| supervisor | %d (%.1f%%) |" % (sup_total, 100.0 * sup_total / max(1, user_total + sup_total)))
    out.append("| in this report | %d |" % total)
    out.append("| distinct opwords | %d |" % len(counts))
    out.append("| touch memory | %.1f%% |" % (100.0 * mem_ops / max(1, total)))
    out.append("")

    forms = by_form.most_common()
    out.append("## Coverage by instruction form")
    out.append("")
    out.append("Forms are mnemonic.size plus source and destination addressing modes.")
    out.append("")
    out.append("| Dynamic coverage | Forms needed |")
    out.append("|---|---|")
    for t, n in coverage(forms, total):
        out.append("| %g%% | %d |" % (t, n))
    out.append("")

    table("Top mnemonics", by_mnem.most_common(), total, a.top, out)
    table("Top instruction forms", forms, total, a.top, out)
    table("Addressing modes (operand uses)", by_ea.most_common(), sum(by_ea.values()), a.top, out)
    print("\n".join(out))


if __name__ == "__main__":
    main()
