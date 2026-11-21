#!/usr/bin/env python3
"""Static use-before-def checker for the millicode image.

Assembles the millicode exactly as tools/gen_millicode.py does (hand-written
.s and ucc-compiled .c, one image), builds its control-flow graph from every
entry (the dispatch handlers, entered through the spec's `insn` entry point,
and the labels of millicode/dispatch.yaml `entries`, each with its own entry
point's contract) and runs a forward "definitely defined" dataflow over the
millicode temporaries x1-x31 and the millicode predicate u.

Operand roles come from the spec (tools/uisa.py): Spec.reads / Spec.writes_rd
give the register fields an instruction reads and writes (the same roles
tools/gen.py hands the model's renamer: store data in rd, cas and bfinsi
reading rd, rd tied to rs1 for the merge and writeback forms, ...), and each
variant's reads_flags / writes_flags its flag resources (u for the probe
.us/.ua forms, ld.c, st.c, stu.c, b.u, b.nu). Guest registers (D0-A7, gpc,
gsr), nzvc and x are architectural and always defined; x0 reads are always
fine and x0 writes are discarded.

Control flow, by the spec's `cti` kind:
  branch          target and fall-through (bcc.t: target only, bcc.f: fall-through)
  jump (jal)      target only; rd (the link) is written
  jump_ind        bx: the 2^wid one-instruction slots after it; jalr: unknown
                  targets, reported as an error
  call            the callee is summarised (below); execution resumes after the
                  call when the callee can return
  ret             leaves the routine (an error when reached with an empty stack)
  gexit, gfault   end the path (gfault enters the `exception` entry point, which
                  is analysed separately with its own contract)
  gbranch, gjump  set the guest next PC only: fall-through
  brk, halt       (no cti, but they stop the machine) end the path

Calls: each callee gets a summary, iterated to a fixed point over the call
graph: the resources it reads before defining on some path (with a witness
read and call chain), and those it defines on every path to `ret`. At a call
site the callee's upward-exposed reads must be definitely defined; after it
its must-defs are added. Recursion and call chains deeper than the hardware
return stack (params.call_stack_depth) are errors.

Suppressions (for false positives of a path-insensitive analysis; each needs
a reason): a comment on the reading instruction (or the call site) in a .s
source,
    add x6, x5, x0      ; ubd-ok: x5 (only reached when ... set x5)
or, for ucc-compiled code whose comments do not survive compilation, an entry
in millicode/ubd_allow.yaml:
    - {at: bitfield__13, reg: x5, reason: "..."}   # any instruction of the block
    - {at: bitfield+0x24, reg: u, reason: "..."}   # one instruction
Unused suppressions are warnings. Unreachable instructions are warnings.

Exit status 1 on any unsuppressed error.
"""

import argparse
import os
import re
import sys
import time
from collections import defaultdict

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_millicode  # noqa: E402
import uisa  # noqa: E402

ROOT = os.path.join(HERE, "..")
NREG = 32                 # millicode temporaries x1..x31 are bits 1..31
U = 32                    # the millicode predicate u
TOP = ((1 << 33) - 1) & ~1
ANN_RE = re.compile(r"ubd-ok:\s*([^()]*?)\s*(?:\((.*)\))?\s*$")


def res_name(r):
    return "u" if r == U else "x%d" % r


def res_index(name):
    name = name.strip().lower()
    if name == "u":
        return U
    m = re.match(r"^x([0-9]+)$", name)
    if m and 1 <= int(m.group(1)) < NREG:
        return int(m.group(1))
    raise ValueError("not a millicode temporary or u: %r" % name)


def bits(mask):
    return [r for r in range(1, 33) if mask >> r & 1]


class Insn:
    __slots__ = ("idx", "word", "v", "vals", "uses", "defs", "succs", "kind",
                 "callee", "src", "line", "text", "error")


class Image:
    """The assembled millicode with per-instruction source, roles and edges."""

    def __init__(self, spec, d, texts):
        self.spec, self.d = spec, d
        self.base = int(d.get("base", 0))
        text = "".join(t + "\n" for _, t in texts)
        self.words, labels = spec.assemble_program(text, self.base)
        self.labels = labels
        self.label_at = defaultdict(list)          # index -> labels, source order
        for name, addr in labels.items():
            self.label_at[(addr - self.base) // 4].append(name)
        self.sorted_labels = sorted((i, ls[0]) for i, ls in self.label_at.items())
        self.ann = {}                               # index -> (mask, reason, src, line)
        self.ann_errors = []
        self._map_sources(texts)
        self.insns = [self._decode(i, w) for i, w in enumerate(self.words)]

    # The assembler's first pass, per source, to map each word to its line.
    def _map_sources(self, texts):
        self.where = []
        for src, text in texts:
            for n, raw in enumerate(text.splitlines(), 1):
                line = raw.split(";")[0].strip()
                while ":" in line.split(" ")[0]:
                    line = line.partition(":")[2].strip()
                if not line:
                    if "ubd-ok" in raw:
                        self.ann_errors.append("%s:%d: ubd-ok annotation not on an instruction line" % (src, n))
                    continue
                idx = len(self.where)
                self.where.append((src, n, line))
                comment = raw.partition(";")[2]
                if "ubd-ok" in comment:
                    m = ANN_RE.search(comment)
                    try:
                        if not m or not m.group(2) or not m.group(2).strip():
                            raise ValueError("needs a reason: ubd-ok: <regs> (<reason>)")
                        mask = 0
                        for name in re.split(r"[,\s]+", m.group(1).strip()):
                            mask |= 1 << res_index(name)
                        self.ann[idx] = [mask, m.group(2).strip(), "%s:%d" % (src, n), 0]
                    except ValueError as e:
                        self.ann_errors.append("%s:%d: bad ubd-ok annotation: %s" % (src, n, e))
        assert len(self.where) == len(self.words), "source map out of step with the assembler"

    def name(self, i):
        """label+offset of instruction i."""
        lo, hi, best = 0, len(self.sorted_labels) - 1, None
        while lo <= hi:
            mid = (lo + hi) // 2
            if self.sorted_labels[mid][0] <= i:
                best, lo = self.sorted_labels[mid], mid + 1
            else:
                hi = mid - 1
        if best is None:
            return "0x%x" % (self.base + 4 * i)
        off = 4 * (i - best[0])
        return best[1] + ("+0x%x" % off if off else "")

    def addr(self, i):
        return self.base + 4 * i

    def target_name(self, i):
        ls = self.label_at.get(i)
        return ls[0] if ls else self.name(i)

    def describe(self, i):
        ins = self.insns[i]
        return "%s (0x%04x) [%s:%d]  %s" % (self.name(i), self.addr(i), ins.src, ins.line, ins.text)

    def _decode(self, i, word):
        spec = self.spec
        ins = Insn()
        ins.idx, ins.word = i, word
        ins.src, ins.line, _ = self.where[i]
        ins.error = None
        ins.callee = None
        v, vals = spec.decode(word)
        ins.v, ins.vals = v, vals
        ins.uses = ins.defs = 0
        ins.succs = []
        ins.kind = "seq"
        if v is None:
            ins.text = ".word 0x%08x" % word
            ins.kind = "stop"
            ins.error = "undecodable word"
            return ins
        ins.text = spec.disassemble(word)
        # (FP register fields name the FP namespace, not x registers: not tracked)
        for f in spec.int_reads(v):
            r = vals.get(f, 0)
            if 0 < r < NREG:
                ins.uses |= 1 << r
        if spec.writes_int_rd(v):
            r = vals.get("rd", 0)
            if 0 < r < NREG:
                ins.defs |= 1 << r
        if "u" in v.reads_flags:
            ins.uses |= 1 << U
        if "u" in v.writes_flags:
            ins.defs |= 1 << U
        n = len(self.words)
        nxt = [i + 1]
        tgt = None
        if v.cti in ("branch", "jump", "call"):
            off = vals["imm"]
            tgt = i + off // 4 if off % 4 == 0 else None
            if tgt is None or not 0 <= tgt < n:
                ins.error = "control transfer outside the image"
                tgt = None
            elif v.cti != "call":
                ins.text += "  -> " + self.target_name(tgt)
            else:
                ins.text = "call " + self.target_name(tgt)
        cti = v.cti
        if cti == "branch":
            if v.mnemonic == "bcc.t":
                ins.succs = [tgt]
            elif v.mnemonic == "bcc.f":
                ins.succs = nxt
            else:
                ins.succs = nxt + [tgt]
        elif cti == "jump":
            ins.succs = [tgt]
        elif cti == "jump_ind":
            if v.mnemonic == "bx":
                wid = vals["wid"]
                if not 1 <= wid <= 15:
                    ins.error = "bx width %d out of range" % wid
                ins.succs = [i + 1 + k for k in range(1 << wid)]
                if ins.succs and ins.succs[-1] >= n:
                    ins.error = "bx slot table runs past the image"
            else:
                ins.kind = "stop"
                ins.error = "indirect jump (%s): targets unknown to the checker" % v.mnemonic
        elif cti == "call":
            ins.kind = "call"
            ins.callee = tgt
            ins.succs = nxt
        elif cti == "ret":
            ins.kind = "ret"
        elif cti in ("gexit", "gfault"):
            ins.kind = "stop"
        elif cti in (None, "gbranch", "gjump"):
            if v.mnemonic in ("brk", "halt"):
                ins.kind = "stop"
            else:
                ins.succs = nxt
        else:
            ins.kind = "stop"
            ins.error = "unknown cti kind %r" % cti
        ins.succs = [s for s in ins.succs if s is not None]
        if any(s >= n for s in ins.succs):
            ins.error = ins.error or "execution falls off the end of the image"
            ins.succs = [s for s in ins.succs if s < n]
        return ins


class Checker:
    def __init__(self, img, allow=None):
        self.img = img
        self.errors = []            # (key, message lines)
        self.warnings = []
        self.suppressed = 0
        self.allow = self._load_allow(allow or [])
        self.regions = {}

    def _load_allow(self, entries):
        img = self.img
        out = {}
        for k, e in enumerate(entries):
            what = "ubd_allow entry %d" % (k + 1)
            reason = str(e.get("reason") or "").strip()
            if not reason:
                img.ann_errors.append("%s: needs a reason" % what)
                continue
            try:
                mask = 0
                for name in re.split(r"[,\s]+", str(e["reg"]).strip()):
                    mask |= 1 << res_index(name)
            except (KeyError, ValueError) as ex:
                img.ann_errors.append("%s: bad reg: %s" % (what, ex))
                continue
            at = str(e.get("at", ""))
            m = re.match(r"^([A-Za-z_][\w.]*)(?:\+(0x[0-9a-fA-F]+|\d+))?$", at)
            if not m or m.group(1) not in img.labels:
                img.ann_errors.append("%s: no label %r" % (what, at))
                continue
            start = (img.labels[m.group(1)] - img.base) // 4
            if m.group(2):
                idxs = [start + int(m.group(2), 0) // 4]
            else:
                idxs = [start]
                j = start + 1
                while j < len(img.words) and j not in img.label_at:
                    idxs.append(j)
                    j += 1
            rec = [mask, reason, what + " (%s)" % at, 0]
            for i in idxs:
                if i in out or i in img.ann:
                    old = out.get(i) or img.ann[i]
                    old[0] |= mask
                else:
                    out[i] = rec
        return out

    def suppression(self, i):
        return self.img.ann.get(i) or self.allow.get(i)

    # --- regions ----------------------------------------------------------
    def region(self, root):
        if root in self.regions:
            return self.regions[root]
        insns = self.img.insns
        seen, order, stack = {root}, [], [root]
        while stack:
            i = stack.pop()
            order.append(i)
            for s in insns[i].succs:
                if s not in seen:
                    seen.add(s)
                    stack.append(s)
        preds = defaultdict(list)
        for i in order:
            for s in insns[i].succs:
                preds[s].append(i)
        self.regions[root] = (sorted(order), preds)
        return self.regions[root]

    # --- dataflow -----------------------------------------------------------
    def flow(self, root, init):
        """IN sets of the must-defined analysis of the routine at root."""
        insns, summ = self.img.insns, self.summary
        order, preds = self.region(root)
        IN = {i: TOP for i in order}
        OUT = {i: TOP for i in order}
        work = list(reversed(order))
        onwork = set(order)
        while work:
            i = work.pop()
            onwork.discard(i)
            m = init if i == root else TOP
            for p in preds[i]:
                m &= OUT[p]
            IN[i] = m
            ins = insns[i]
            if ins.kind == "call":
                out = m | summ[ins.callee][0]
            else:
                out = m | ins.defs
            if out != OUT[i]:
                OUT[i] = out
                for s in ins.succs:
                    if s not in onwork:
                        onwork.add(s)
                        work.append(s)
        return IN

    def events(self, root, init, final=False):
        """(must-defs at ret, {res: witness}, [(i, res, witness or None)])."""
        insns = self.img.insns
        IN = self.flow(root, init)
        order, _ = self.region(root)
        mustdef, ue, ev = TOP, {}, []
        for i in order:
            ins = insns[i]
            sup = self.suppression(i)
            supmask = sup[0] if sup else 0
            if ins.kind == "call":
                cm, cue = self.summary[ins.callee][:2]
                missing = 0
                for r, w in cue.items():
                    if not IN[i] >> r & 1:
                        missing |= 1 << r
                        if not supmask >> r & 1:
                            ev.append((i, r, w))
                            ue.setdefault(r, (w[0], (root,) + w[1]))
                if final and sup and missing & supmask:
                    sup[3] = 1
            else:
                missing = ins.uses & ~IN[i]
                for r in bits(missing & ~supmask):
                    ev.append((i, r, None))
                    ue.setdefault(r, (i, (root,)))
                if final and sup and missing & supmask:
                    sup[3] = 1
            if ins.kind == "ret":
                mustdef &= IN[i]
        return mustdef, ue, ev

    def run(self):
        img = self.img
        spec, d = img.spec, img.d
        eps = {e["name"]: e for e in spec.d["entry_points"]}

        def contract(name):
            m = 0
            for reg in (eps[name].get("regs") or {}):
                r = spec.regs.get(reg)
                if r is not None and 0 < r < NREG:
                    m |= 1 << r
            return m

        roots = []                         # (index, init, description)
        by_label = defaultdict(list)
        for op, label in (d.get("handlers") or {}).items():
            by_label[label].append(op)
        for label, ops in by_label.items():
            roots.append((self._label(label), contract("insn"),
                          "insn handler %s (%s)" % (label, ", ".join(ops))))
        for name, label in (d.get("entries") or {}).items():
            roots.append((self._label(label), contract(name), "entry point %s (%s)" % (name, label)))

        # callees reachable from the roots
        callees, stack = set(), [r[0] for r in roots]
        visited = set()
        while stack:
            f = stack.pop()
            if f in visited:
                continue
            visited.add(f)
            for i in self.region(f)[0]:
                c = img.insns[i].callee
                if c is not None:
                    callees.add(c)
                    stack.append(c)

        # callee summaries to a fixed point: (must-defs at ret, upward-exposed reads)
        self.summary = {f: (TOP, {}) for f in callees}
        changed = True
        while changed:
            changed = False
            for f in sorted(callees):
                mustdef, ue, _ = self.events(f, 0)
                old = self.summary[f]
                if mustdef != old[0] or set(ue) != set(old[1]):
                    self.summary[f] = (mustdef, ue)
                    changed = True
        for f in sorted(callees):
            self.events(f, 0, final=True)       # mark the suppressions used by callees

        # findings, grouped by (location, resource)
        found = {}
        for root, init, desc in roots:
            _, _, ev = self.events(root, init, final=True)
            for i, r, w in ev:
                key = (i, r)
                if key not in found:
                    found[key] = (w, [])
                found[key][1].append(desc)
            for i in self.region(root)[0]:
                if img.insns[i].kind == "ret":
                    self.error((i, "ret"), ["ret reached with an empty return stack",
                                            "  at   " + img.describe(i), "  via  " + desc])
        for (i, r), (w, descs) in sorted(found.items()):
            if w is None:
                lines = ["%s read before it is defined" % res_name(r), "  at   " + img.describe(i)]
            else:
                ui, chain = w
                lines = ["%s may be undefined at a call that reads it" % res_name(r),
                         "  at   " + img.describe(i),
                         "  read %s" % img.describe(ui),
                         "  chain " + " -> ".join(img.target_name(c) for c in chain)]
            for desc in descs[:4]:
                lines.append("  via  " + desc)
            if len(descs) > 4:
                lines.append("  via  ... and %d more entries" % (len(descs) - 4))
            self.error((i, r), lines)

        # call graph: recursion and return-stack depth
        depth_limit = int(spec.params.get("call_stack_depth", 4))
        memo, onstack = {}, []

        def depth(f):
            if f in memo:
                return memo[f]
            if f in onstack:
                cyc = onstack[onstack.index(f):] + [f]
                self.error(("rec", f), ["recursion: " + " -> ".join(img.target_name(c) for c in cyc)])
                memo[f] = 0
                return 0
            onstack.append(f)
            best, path = 0, ()
            for i in self.region(f)[0]:
                c = img.insns[i].callee
                if c is not None:
                    dc = depth(c) + 1
                    if dc > best:
                        best, path = dc, (c,) + self.deep.get(c, ())
            onstack.pop()
            memo[f] = best
            self.deep[f] = path
            return best
        self.deep = {}
        self.max_depth = 0
        for root, _, desc in roots:
            self.max_depth = max(self.max_depth, depth(root))
            if depth(root) > depth_limit:
                self.error(("depth", root), ["call depth %d exceeds the %d-entry return stack" % (memo[root], depth_limit),
                                             "  chain " + " -> ".join(img.target_name(c) for c in (root,) + self.deep.get(root, ())),
                                             "  via  " + desc])

        # structural problems on reachable instructions; unreachable code
        reach = set()
        for f in list(self.regions):
            reach.update(self.regions[f][0])
        for i in sorted(reach):
            if img.insns[i].error:
                self.error((i, "struct"), [img.insns[i].error, "  at   " + img.describe(i)])
        i, n = 0, len(img.words)
        while i < n:
            if i in reach:
                i += 1
                continue
            j = i
            while j + 1 < n and j + 1 not in reach:
                j += 1
            self.warnings.append("unreachable: %d instruction%s at %s .. %s" % (
                j - i + 1, "" if j == i else "s", img.describe(i), img.name(j)))
            i = j + 1
        for msg in img.ann_errors:
            self.error(("ann", msg), [msg])
        for i, s in sorted(list(img.ann.items()) + list(self.allow.items())):
            if s[3]:
                self.suppressed += 1
            else:
                self.warnings.append("unused suppression %s: %s (%s) at %s" % (
                    s[2], ", ".join(res_name(r) for r in bits(s[0])), s[1], img.describe(i)))
        return self

    def _label(self, label):
        if label not in self.img.labels:
            raise SystemExit("check_millicode: no label %r" % label)
        return (self.img.labels[label] - self.img.base) // 4

    def error(self, key, lines):
        self.errors.append((key, lines))


def check(spec, d, texts, allow=None):
    """Run the checker on in-memory sources: texts = [(source name, assembly)]."""
    return Checker(Image(spec, d, texts), allow).run()


def load_allow(mc_dir):
    path = os.path.join(mc_dir, "ubd_allow.yaml")
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return yaml.safe_load(f) or []


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--millicode", default=os.path.join(ROOT, "millicode"),
                    help="millicode directory (dispatch.yaml, sources, ubd_allow.yaml)")
    ap.add_argument("--no-unreachable", action="store_true", help="do not warn about unreachable code")
    ap.add_argument("-q", "--quiet", action="store_true", help="print only errors and the summary line")
    args = ap.parse_args()
    t0 = time.time()
    spec = uisa.load()
    d = gen_millicode.load_dispatch(args.millicode)
    out_dir = None
    if os.path.abspath(args.millicode) != os.path.abspath(os.path.join(ROOT, "millicode")):
        out_dir = os.path.join(args.millicode, "build")
    texts = [(src, gen_millicode.source_text(src, args.millicode, out_dir)) for src in d["sources"]]
    gen_millicode.check_labels(texts)
    c = check(spec, d, texts, load_allow(args.millicode))
    for _, lines in c.errors:
        print("error: " + "\n".join(lines))
    if not args.quiet:
        for w in c.warnings:
            if args.no_unreachable and w.startswith("unreachable"):
                continue
            print("warning: " + w)
    print("check_millicode: %d instructions, %d entries, call depth %d, %d error%s, %d warning%s, %d suppressed (%.2fs)" % (
        len(c.img.words), len((d.get("handlers") or {})) + len(d.get("entries") or {}), c.max_depth,
        len(c.errors), "" if len(c.errors) == 1 else "s",
        len(c.warnings), "" if len(c.warnings) == 1 else "s", c.suppressed, time.time() - t0))
    sys.exit(1 if c.errors else 0)


if __name__ == "__main__":
    main()
