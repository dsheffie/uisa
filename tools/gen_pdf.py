#!/usr/bin/env python3
"""Generate the uisa reference manual as a PDF (doc/uisa.pdf).

Laid out like the MIPS architecture manuals: an introduction, the
architecture tables, then one page per instruction with its encoding
diagram, assembly format, purpose, description, restrictions, operation and
exceptions, and a last chapter listing every 68k instruction form's flow.
Everything comes from spec/uisa.yaml (the introduction from doc/intro.md, the
68k flows from spec/flows.yaml via tools/flows.py), so the manual regenerates
with the spec:

    make pdf        # = python3 tools/gen_pdf.py

Writes build/pdf/uisa.tex, runs pdflatex there until the table of contents
settles, and copies the result to doc/uisa.pdf. The PDF carries no date or
random ID, so regenerating an unchanged spec gives an identical file. Needs a TeX installation
with tikz, listings, enumitem, fancyhdr, titlesec and hyperref.
"""

import collections
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, HERE)
import uisa  # noqa: E402

OUT_DIR = os.path.join(ROOT, "build", "pdf")
PDF = os.path.join(ROOT, "doc", "uisa.pdf")

# ---------------------------------------------------------------------------
# Text helpers
# ---------------------------------------------------------------------------
_ESC = {"\\": r"\textbackslash{}", "&": r"\&", "%": r"\%", "$": r"\$", "#": r"\#", "_": r"\_",
        "{": r"\{", "}": r"\}", "~": r"\textasciitilde{}", "^": r"\textasciicircum{}",
        "<": r"\textless{}", ">": r"\textgreater{}", "|": r"\textbar{}"}


def esc(s):
    return "".join(_ESC.get(c, c) for c in str(s))


def tt(s):
    return r"\texttt{%s}" % esc(s)


def inline(s):
    """Markdown-ish inline text: `code`, **bold**, *emphasis*."""
    out = []
    for k, part in enumerate(re.split(r"`([^`]*)`", s)):
        if k % 2:
            out.append(tt(part))
            continue
        part = esc(part)
        part = re.sub(r"\*\*(.+?)\*\*", r"\\textbf{\1}", part)
        part = re.sub(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])", r"\\emph{\1}", part)
        out.append(part)
    return "".join(out)


def md_to_tex(md):
    """The subset of Markdown doc/intro.md uses: headings, paragraphs,
    bullet and numbered lists, fenced code."""
    out, para, lst = [], [], None
    lines = md.splitlines()

    def flush_para():
        if para:
            out.append(inline(" ".join(para)) + "\n")
            para.clear()

    def close_list():
        nonlocal lst
        if lst:
            out.append("\\end{%s}" % lst)
            lst = None

    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("```"):
            flush_para(); close_list()
            i += 1
            block = []
            while i < len(lines) and not lines[i].startswith("```"):
                block.append(lines[i])
                i += 1
            out.append("\\begin{lstlisting}[style=plain]\n%s\n\\end{lstlisting}" % "\n".join(block))
            i += 1
            continue
        m = re.match(r"^(#+)\s+(.*)$", ln)
        if m:
            flush_para(); close_list()
            level = len(m.group(1))
            if level >= 2:
                out.append("\\%s{%s}" % ("section" if level == 2 else "subsection", inline(m.group(2))))
            i += 1
            continue
        m = re.match(r"^(\s*)(-|\d+\.)\s+(.*)$", ln)
        if m:
            flush_para()
            kind = "itemize" if m.group(2) == "-" else "enumerate"
            if lst != kind:
                close_list()
                out.append("\\begin{%s}" % kind)
                lst = kind
            item = [m.group(3)]
            i += 1
            while i < len(lines) and lines[i].startswith("  ") and lines[i].strip() and \
                    not re.match(r"^\s*(-|\d+\.)\s+", lines[i]):
                item.append(lines[i].strip())
                i += 1
            out.append("\\item " + inline(" ".join(item)))
            continue
        if not ln.strip():
            flush_para()
            if lst and not (i + 1 < len(lines) and re.match(r"^\s*(-|\d+\.)\s+", lines[i + 1])):
                close_list()
            i += 1
            continue
        if lst:
            close_list()
        para.append(ln.strip())
        i += 1
    flush_para(); close_list()
    return "\n".join(out)


def bits(v, w):
    return format(v, "0%db" % w)


# ---------------------------------------------------------------------------
# Encoding diagrams
# ---------------------------------------------------------------------------
def diagram(fmt, label, value):
    """A MIPS-style encoding box. label/value: field name -> text (value may
    be empty). Bit numbers above each field, widths below."""
    o = [r"\begin{center}\begin{tikzpicture}[x=0.5cm,y=1cm,font=\sffamily]"]
    for f in sorted(fmt.fields.values(), key=lambda f: -f.hi):
        x0, x1 = 31 - f.hi, 32 - f.lo
        mid = (x0 + x1) / 2
        size = r"\tiny" if f.width == 1 else r"\scriptsize" if f.width <= 3 else r"\small"
        o.append(r"\draw (%g,0) rectangle (%g,1);" % (x0, x1))
        lab, val = label.get(f.name, f.name), value.get(f.name, "")
        if val:
            o.append(r"\node[font=\sffamily%s] at (%g,0.68) {%s};" % (size, mid, lab))
            o.append(r"\node[font=\sffamily%s] at (%g,0.3) {%s};" % (size, mid, val))
        else:
            o.append(r"\node[font=\sffamily%s] at (%g,0.5) {%s};" % (size, mid, lab))
        if f.width == 1:
            o.append(r"\node[font=\sffamily\scriptsize,anchor=south] at (%g,1) {%d};" % (mid, f.hi))
        else:
            o.append(r"\node[font=\sffamily\scriptsize,anchor=south west,inner xsep=1pt] at (%g,1) {%d};" % (x0, f.hi))
            o.append(r"\node[font=\sffamily\scriptsize,anchor=south east,inner xsep=1pt] at (%g,1) {%d};" % (x1, f.lo))
        o.append(r"\node[font=\sffamily\small] at (%g,-0.28) {%d};" % (mid, f.width))
    o.append(r"\end{tikzpicture}\end{center}")
    return "\n".join(o)


def major_name(spec, value):
    for k, v in spec.majors.items():
        if v == value:
            return k
    return "?"


def encodings(spec, vs, upper, cond_table=None):
    """Diagrams for a group of variants, one per format. A fixed field with
    the same value in every variant shows its value; one that differs shows
    its name, with a table of the variants' values."""
    out = []
    for fname in sorted({v.fmt.name for v in vs}, key=lambda n: list(spec.formats).index(n)):
        sub = [v for v in vs if v.fmt.name == fname]
        fmt = sub[0].fmt
        label, value, varying = {}, {}, []
        for f in sorted(fmt.fields.values(), key=lambda f: -f.hi):
            vals = {v.fixed.get(f.name) for v in sub}
            if None in vals:
                continue                         # an operand field
            if len(vals) == 1:
                x = vals.pop()
                if f.name == "op":
                    label[f.name] = esc(major_name(spec, x))
                elif f.name == "fn":
                    label[f.name] = esc(upper)
                else:
                    label[f.name] = esc(f.name)
                value[f.name] = bits(x, f.width)
            else:
                varying.append(f)
        out.append(diagram(fmt, label, value))
        if varying:
            out.append(variant_table(spec, sub, varying, cond_table))
    return "\n".join(out)


def variant_table(spec, vs, fields, cond_table=None):
    conds = cond_table is not None and all(v.attrs.get("cond") for v in vs)
    cond_expr = {c["name"]: c["expr"] for c in cond_table or []}
    cols = []
    for v in vs:
        row = [tt(v.mnemonic)]
        for f in fields:
            x = v.fixed[f.name]
            row.append(bits(x, f.width) + (" (%s)" % esc(major_name(spec, x)) if f.name == "op" else ""))
        if conds:
            row.append(tt(cond_expr[v.attrs["cond"]]))
        cols.append(row)
    per = 1 if len(cols) <= 8 else 2 if len(cols) <= 24 else 3
    heads = ["Mnemonic"] + [esc(f.name) for f in fields] + (["Condition (\\texttt{COND})"] if conds else [])
    n = (len(cols) + per - 1) // per
    spec_cols = "l" * len(heads)
    o = [r"\begin{center}\small\begin{tabular}{%s}" % ("@{\\hspace{2.5em}}".join([spec_cols] * per)), r"\hline"]
    o.append(" & ".join(heads * per) + r" \\ \hline")
    for r in range(n):
        row = []
        for c in range(per):
            k = c * n + r
            row += cols[k] if k < len(cols) else [""] * len(heads)
        o.append(" & ".join(row) + r" \\")
    o.append(r"\hline\end{tabular}\end{center}")
    return "\n".join(o)


# ---------------------------------------------------------------------------
# Instruction pages
# ---------------------------------------------------------------------------
def syntax(v):
    ops = []
    for k in v.operands:
        if k == "mem":
            ops.append("imm(rs1)")
        else:
            ops.append(k)
    return ", ".join(ops)


def format_lines(vs):
    groups = []
    for v in vs:
        s = syntax(v)
        for g in groups:
            if g[0] == s:
                g[1].append(v.mnemonic)
                break
        else:
            groups.append([s, [v.mnemonic]])
    wide = max(len(", ".join(ms)) for _, ms in groups) > 34
    rows = [r"%s\texttt{%s} & \texttt{%s}\\" % (r"\raggedright" if wide else "", esc(", ".join(ms)), esc(s))
            for s, ms in groups]
    col = r"p{0.52\linewidth}" if wide else "l"
    return (r"\begin{tabular}[t]{@{}%s@{\hspace{1.5em}}l@{}}" % col + "\n".join(rows) + r"\end{tabular}")


def one_liner(v):
    """rd <- expr for a base op whose semantics is a single `r = expr;`."""
    sem = (v.attrs.get("sem") or "").strip()
    m = re.match(r"^r = (.*);$", sem)
    if not m or "\n" in sem:
        return None
    second = "rs2" if "rs2" in v.operands else "imm"
    e = m.group(1)
    e = re.sub(r"\ba\b", "rs1", e)
    e = re.sub(r"\bb\b", second, e)
    e = re.sub(r"\bIMM\b", "imm", e)
    return r"\texttt{rd} $\leftarrow$ \texttt{%s}" % esc(e)


def flags_text(e):
    """The flags an entry reads and writes, as the spec lists them."""
    def names(fs):
        expand = {"nzvc": ["N", "Z", "V", "C"], "x": ["X"]}
        fs = [x for f in fs for x in expand.get(f.lower(), [f])]
        return ", ".join(fs[:-1]) + " and " + fs[-1] if len(fs) > 1 else fs[0]
    out = []
    w = e.get("writes") or e.get("writes_flags")
    r = e.get("reads") or e.get("reads_flags")
    if w:
        out.append("writes " + names(w))
    if r:
        out.append("reads " + names(r))
    return "; ".join(out)


def imm_text(spec, vs):
    """How the immediate is formed, for the formats in a group."""
    out = []
    for fname in sorted({v.fmt.name for v in vs}, key=lambda n: list(spec.formats).index(n)):
        v = next(x for x in vs if x.fmt.name == fname)
        f = v.fmt.fields.get("imm")
        if f is None or "imm" not in v.operands and "mem" not in v.operands:
            continue
        signed = v.attrs.get("imm_signed", v.fmt.imm_signed)
        scale = v.fmt.imm_scale
        t = r"\texttt{imm} is the %d-bit %s field%s" % (f.width, "signed" if signed else "unsigned",
                                                         ", in units of %d bytes" % scale if scale != 1 else "")
        if len({x.fmt.name for x in vs}) > 1:
            t += " (format %s)" % fname
        out.append(t)
    return ("; ".join(out) + ".") if out else ""


GFORM_TEXT = {
    "merge": "merge form: \\texttt{rd} $\\leftarrow$ \\texttt{rd} with its low $n$ bits replaced by the result; "
             "two-address (the old \\texttt{rd} arrives on the \\texttt{rs1} port)",
    "zfill": "zero-fill form: \\texttt{rd} $\\leftarrow$ the result, zero-extended; does not read the old \\texttt{rd}",
    "full32": "32-bit form: \\texttt{rd} $\\leftarrow$ the result (three-address)",
    "compare": "compare: sets the flags only; no register result",
    "full": "sized operands, full 32-bit result (three-address)",
    "tiedfull": "sized operands, full 32-bit result that may keep the old \\texttt{rd} (two-address at every size)",
}


def mem_description(spec, vs):
    v = vs[0]
    k, n, wb = v.kind, v.attrs["bytes"], v.attrs.get("writeback")
    w = 8 * n
    out = []
    if k == "load":
        ext = "sign-extended" if v.attrs.get("signed") else "zero-extended"
        out.append(r"\texttt{rd} $\leftarrow$ the %d-bit operand at \texttt{rs1 + imm}%s." % (
            w, ", %s to 32 bits" % ext if n < 4 else ""))
    elif k == "store" and wb == "pre":
        out.append(r"The low %d bits of \texttt{rs2} are stored at \texttt{rs1 + imm}, and \texttt{rd} "
                   r"$\leftarrow$ \texttt{rs1 + imm} (the base register written back with the address)." % w)
    elif k == "store" and wb == "post":
        out.append(r"The low %d bits of \texttt{rs2} are stored at \texttt{rs1}, and \texttt{rd} "
                   r"$\leftarrow$ \texttt{rs1 + imm} (the base register written back after the access)." % w)
    elif k == "store":
        out.append(r"The low %d bits of \texttt{rd} are stored at \texttt{rs1 + imm}." % w)
    if v.attrs.get("conditional"):
        out.append(r"Only if the millicode predicate \texttt{u} is set: with \texttt{u} clear no access is "
                   r"made at all (no translation, no fault, no cache or ATC side effect)%s. Reading \texttt{u} "
                   r"orders it after the probes that wrote \texttt{u} by a data dependence." % (
                       r" and \texttt{rd} $\leftarrow$ 0" if k == "load" else ", nothing is stored, and it is not a "
                       r"guest-visible write"))
    if v.attrs.get("locked") == "begin":
        out.append("It begins a locked (atomic) sequence.")
    elif v.attrs.get("locked") == "end":
        out.append("It ends a locked (atomic) sequence%s." % (
            r", whether or not it stores (\texttt{u} clear still releases the lock)" if v.attrs.get("conditional") else ""))
    if any(x.attrs.get("umode") for x in vs):
        out.append(r"The \texttt{um} field selects the form: \texttt{um} = 0 writes only \texttt{rd}; "
                   r"1 (\texttt{.us}) also sets the millicode predicate \texttt{u} to the probe's success "
                   r"\texttt{ok}; 2 (\texttt{.ua}) sets \texttt{u} $\leftarrow$ \texttt{u \&\& ok}; 3 is reserved "
                   r"(illegal). \texttt{ok} is \texttt{PROBE\_OK} (see \emph{Flags}).")
    out.append(r"The \texttt{sp} field selects the address space: " +
               ", ".join(r"\texttt{.%s} %s" % (esc(s["suffix"]), esc(s["name"])) for s in spec.spaces) +
               " (see \\emph{Address spaces}).")
    return " ".join(out)


def mem_operation(v):
    k, n, wb = v.kind, v.attrs["bytes"], v.attrs.get("writeback")
    w = 8 * n
    if v.attrs.get("conditional"):
        if k == "load":
            ld = "LOAD(sp, rs1 + imm, %d)" % n
            if n < 4:
                ld = ("SX(%s, %d)" if v.attrs.get("signed") else "ZX(%s, %d)") % (ld, w)
            return "if (u)\n    rd = %s;\nelse\n    rd = 0;            /* no access, no fault */" % ld
        return ("if (u)\n    STORE(sp, rs1 + imm, %d, rd);\n/* else nothing: no access */" % n +
                ("\nUNLOCK();           /* ends the locked sequence either way */" if v.attrs.get("locked") == "end" else ""))
    if k == "load":
        ld = "LOAD(sp, rs1 + imm, %d)" % n
        if n < 4:
            ld = ("SX(%s, %d)" if v.attrs.get("signed") else "ZX(%s, %d)") % (ld, w)
        return "rd = %s;" % ld
    if k == "store" and wb == "pre":
        return "STORE(sp, rs1 + imm, %d, rs2);\nrd = rs1 + imm;" % n
    if k == "store" and wb == "post":
        return "STORE(sp, rs1, %d, rs2);\nrd = rs1 + imm;" % n
    if k == "store":
        return "STORE(sp, rs1 + imm, %d, rd);" % n
    if k == "cas":
        return ("old = LOAD(sp, rs1 + imm, %d);          /* atomic with the store */\n"
                "if (old == (rs2 & MSK(%d)))\n"
                "    STORE(sp, rs1 + imm, %d, rd);\n"
                "rd = old;" % (n, w, n))
    if k == "probe":
        wr = v.mnemonic.startswith("probe.w")
        op = "rd = MMUSR(TRANSLATE(sp, rs1 + imm, %s));   /* never faults */" % ("WRITE" if wr else "READ")
        if v.attrs.get("umode") is not None or "um" in v.fixed:
            op += ("\nok = PROBE_OK(rd, %d, USER(sp), TC.E); /* user: a user-mode function code */"
                   "\nif (um == 1) u = ok;                  /* .us */"
                   "\nif (um == 2) u = u && ok;             /* .ua */" % int(wr))
        return op
    return ""


def guest_operation(e):
    sem = e["sem"].rstrip()
    return sem + "\n/* then rd = merge(rd, r), zero-extend(r) or r, by form (see Description) */" \
        if e["kind"] in ("binary", "unary") else sem


def restrictions(spec, vs, e):
    out = []
    tied = [v.mnemonic for v in vs if v.attrs.get("tied") and not v.attrs.get("writeback")]
    if tied:
        out.append(r"Two-address: \texttt{rs1} must equal \texttt{rd} (%s)." % (
            "all forms" if len(tied) == len(vs) else ", ".join(tt(m) for m in tied)))
    if any(v.attrs.get("writeback") for v in vs):
        out.append(r"\texttt{rd} is the base register: the encoding's \texttt{rd} field must equal \texttt{rs1}.")
    if any(v.attrs.get("imm_max") is not None for v in vs):
        out.append(r"\texttt{imm} $\le$ %d." % max(v.attrs["imm_max"] for v in vs if v.attrs.get("imm_max") is not None))
    if any(v.attrs.get("serialize") for v in vs):
        out.append("Serializes at rename (executes non-speculatively at the head of the reorder buffer).")
    if e.get("precondition"):
        out.append(inline(e["precondition"]))
    if vs[0].kind in ("load", "store", "cas"):
        out.append("Guest-space accesses may be misaligned; hidden-space accesses must be naturally aligned.")
    return out or ["None."]


def exceptions(vs, e):
    v = vs[0]
    if v.attrs.get("conditional"):
        return [r"With \texttt{u} set, as the plain access: a guest access fault in the guest spaces abandons and "
                r"restarts the guest instruction. With \texttt{u} clear, none (no access is made)."]
    if v.kind in ("load", "store", "cas"):
        return [r"Guest access fault in the guest spaces (\texttt{gd}, \texttt{gp}, \texttt{ga}): the guest "
                "instruction is abandoned and restarted after the handler. In the hidden space, a misaligned "
                "access is a millicode fault."]
    if v.kind == "probe":
        return ["None: a probe reports a failed translation in its result."]
    if e.get("trap"):
        return [inline(e["trap"][0].upper() + e["trap"][1:]) + "."]
    name = e.get("name") or e.get("family")
    if name in ("gfault", "gfaultr"):
        return ["Raises the guest exception (control transfers to the millicode exception entry point)."]
    if name == "call":
        return ["Millicode fault if the hardware return stack overflows."]
    if name == "ret":
        return ["Millicode fault if the hardware return stack underflows."]
    return ["None."]


def section(label, body):
    return (r"\par\medskip\noindent{\bfseries %s}\par\nopagebreak\smallskip" % label +
            "\n\\begin{adjustwidth}{2.2em}{0pt}\n%s\n\\end{adjustwidth}" % body)


def page(spec, part, e, vs):
    name = e.get("name") or e.get("family")
    title = e["title"]
    o = [r"\insnpage{%s}{%s}{%s}" % (esc(name), esc(title), esc("%s: %s" % (name, title)))]
    pc = e.get("per_cond")
    cond_table = spec.guest_conds if pc == "guest" else spec.base_conds if pc else None
    o.append(encodings(spec, vs, name.split(".")[0].upper(), cond_table))
    o.append(r"\begin{insnfields}")
    o.append(r"\item[Format:] %s" % format_lines(vs))
    if e.get("desc"):
        o.append(r"\item[Purpose:] %s" % inline(e["desc"].split(". ")[0].rstrip(".") + "."))
    o.append(r"\end{insnfields}")

    desc = []
    v = vs[0]
    if part == "base":
        ol = one_liner(v)
        if ol:
            desc.append(ol)
        if e.get("desc"):
            desc.append(inline(e["desc"]))
        if e.get("note"):
            desc.append(inline(e["note"]))
        if e.get("per_cond"):
            desc.append("One encoding per %s condition; \\texttt{COND} in the Operation is the condition "
                        "of the encoding table above." % e["per_cond"])
        ft = flags_text(e)
        if ft:
            desc.append("Flags: " + ft + ".")
        it = imm_text(spec, vs)
        if it:
            desc.append(it)
        if not desc:
            desc.append("See \\emph{Operation}.")
    elif part == "mem":
        desc.append(mem_description(spec, vs))
        if e.get("desc"):
            desc.append(inline(e["desc"]))
        it = imm_text(spec, vs)
        if it:
            desc.append(it)
    else:
        forms = []
        for g in ("merge", "zfill", "full32", "compare", "full", "tiedfull"):
            ms = sorted({x.mnemonic for x in vs if x.attrs.get("gform") == g and x.attrs.get("flags_on")})
            if ms:
                forms.append(r"\item %s: %s." % (", ".join(tt(m) for m in ms), GFORM_TEXT[g]))
        desc.append("The 68k-semantic operation on $n$-bit operands (\\texttt{a} = \\texttt{rs1}, or the old "
                    "\\texttt{rd} in the two-address forms; \\texttt{b} = \\texttt{rs2} or the 6-bit unsigned "
                    "immediate of the \\texttt{i} forms). Forms:\n\\begin{itemize}[nosep]\n%s\n\\end{itemize}" %
                    "\n".join(forms))
        ft = flags_text(e)
        if ft:
            desc.append("Flags: " + ft + r". Each form has a \texttt{.nf} twin that writes no flags.")
        if e.get("desc"):
            desc.append(inline(e["desc"]))
    o.append(section("Description:", "\n\n".join(desc)))
    o.append(section("Restrictions:", "\n\n".join(restrictions(spec, vs, e))))
    if part == "mem":
        op = mem_operation(v)
    elif part == "guest":
        op = guest_operation(e)
    else:
        op = (e.get("sem") or "").strip()
    if op:
        o.append(section("Operation:", "\\begin{lstlisting}[style=sem]\n%s\n\\end{lstlisting}" % op))
    o.append(section("Exceptions:", "\n\n".join(exceptions(vs, e))))
    return "\n".join(o)


# ---------------------------------------------------------------------------
# Front matter: the architecture tables
# ---------------------------------------------------------------------------
NOTATION = [
    ("a, b", "The source operands: \\texttt{rs1}, and \\texttt{rs2} or the immediate."),
    ("r", "The result written to \\texttt{rd}."),
    ("OLD", "The old value of \\texttt{rd} (ops that read their destination)."),
    ("IMM", "The decoded immediate (sign-extended or not, scaled, per the format)."),
    ("pc, npc", "The millicode address of this instruction, and of the next one (defaults to pc + 4)."),
    ("GPC\\_INSN", "The guest address of the 68k instruction being executed."),
    ("GNPC\\_SET(v)", "Set the guest's next PC (guest branches)."),
    ("n, m", "Guest ops: the operand width in bits, and \\texttt{MSK(n)}."),
    ("xin, nin, zin, vin, cin", "The flags before the instruction."),
    ("X, N, Z, V, C", "The flags after it (each flag-writing guest op writes all of NZVC, and X where listed)."),
    ("uin, U (u)", "The millicode predicate \\texttt{u} before the instruction, and the value it writes."),
    ("COND", "The condition of a per-condition family."),
    ("MSK(n), MSB(v, n)", "The low $n$ bits set; bit $n-1$ of $v$."),
    ("SX(v, n), ZX(v, n)", "Sign- and zero-extend the low $n$ bits of $v$."),
    ("ROL32(a, k)", "Rotate left by $k$."),
    ("BF\\_*", "68k bitfields: a descriptor with the offset (from the MSB) in bits 4:0 and the width in bits 9:5."),
    ("CSR\\_READ, CSR\\_WRITE", "Control and status registers; writes keep only the implemented bits (\\texttt{CSR\\_WMASK})."),
    ("GREG\\_READ, GREG\\_WRITE", "Guest registers D0-D7, A0-A7 by number."),
    ("LOAD, STORE", "A memory access in address space \\texttt{sp}: guest spaces translate through the guest MMU and may fault."),
]


def front(spec, d):
    o = [r"\section{Notation}",
         "Each instruction page gives the operation as C, from the \\texttt{sem} of \\texttt{spec/uisa.yaml} "
         "(the same text the ISS and the functional model compile). It uses:",
         r"\begin{center}\small\begin{tabular}{@{}p{0.28\textwidth}p{0.66\textwidth}@{}}\hline"]
    for k, v in NOTATION:
        o.append(r"\texttt{%s} & %s \\" % (k, v))
    o.append(r"\hline\end{tabular}\end{center}")

    o.append(r"\section{Flags}")
    o.append("The renamed flag resources:")
    o.append(r"\begin{center}\small\begin{tabular}{@{}llp{0.72\textwidth}@{}}\hline Resource & Bits & \\ \hline")
    for f in d["flags"]:
        guest = all(b in d["ccr_layout"] for b in f["bits"])
        what = ("Guest flags, in the CCR at bit%s %s." % ("s" if len(f["bits"]) > 1 else "",
                                                          ", ".join("%s=%d" % (b, d["ccr_layout"][b]) for b in f["bits"]))
                if guest else
                "The millicode-private predicate: never guest-visible, not in the CCR layout "
                "(\\texttt{rdccr}/\\texttt{wrccr} do not touch it), not saved or restored (millicode is not "
                "interruptible and \\texttt{u} is dead at the end of a routine). Written by the \\texttt{.us}/"
                "\\texttt{.ua} probe forms, read by \\texttt{ld.c}, \\texttt{st.c}, \\texttt{stu.c}, "
                "\\texttt{b.u} and \\texttt{b.nu}, so a conditional access is ordered after its probes by a data "
                "dependence rather than a branch.")
        o.append(r"\texttt{%s} & %s & %s \\" % (esc(f["name"]), esc(" ".join(f["bits"])), what))
    o.append(r"\hline\end{tabular}\end{center}")
    o.append(r"\texttt{rdccr}/\texttt{wrccr} use the 68k CCR layout: " +
             ", ".join("%s = bit %d" % (k, v) for k, v in d["ccr_layout"].items()) + ".")
    o.append(r"A probe's success, \texttt{PROBE\_OK(mmusr, w, user, tce)} for its MMUSR result \texttt{mmusr}, "
             r"\texttt{w} set for \texttt{probe.w}, \texttt{user} for a user-mode access and \texttt{tce} for "
             r"translation enabled (TC.E). With translation disabled and no transparent translation register "
             r"matching, the access is untranslated: the MMUSR is 0 (as PTEST leaves it) and the access succeeds. "
             r"A TTR match has T and R set and W from the TTR, so the MMUSR terms decide it; with translation "
             r"enabled an MMUSR of 0 is an invalid descriptor:")
    o.append("\\begin{lstlisting}[style=sem]\n%s\n\\end{lstlisting}" % d["probe_ok"].replace(" || (", "\n|| (").replace(" && ", "\n&& "))

    o.append(r"\section{Instruction formats}")
    o.append("Every instruction is 32 bits. The fields of each format:")
    for name, fmt in spec.formats.items():
        imm = ""
        if "imm" in fmt.fields:
            imm = " (immediate %s%s)" % ("signed" if fmt.imm_signed else "unsigned",
                                         ", scaled by %d" % fmt.imm_scale if fmt.imm_scale != 1 else "")
        o.append(r"\subsection*{Format %s%s}" % (esc(name), esc(imm)))
        o.append(diagram(fmt, {}, {}))

    o.append(r"\section{Major opcodes}")
    o.append("Bits 31:26. Values not listed are reserved.")
    rows = sorted(spec.majors.items(), key=lambda kv: kv[1])
    o.append(table3(["Value", "Bits", "Major"], [(str(v), bits(v, 6), esc(k)) for k, v in rows]))

    o.append(r"\section{Registers}")
    o.append(r"\begin{center}\small\begin{tabular}{@{}lllp{0.55\textwidth}@{}}\hline Registers & Index & Count & \\ \hline")
    for c in d["reg_classes"]:
        nm = ("%s0-%s%d" % (c["prefix"], c["prefix"], c["count"] - 1)) if "prefix" in c else c["name"]
        o.append(r"\texttt{%s} & %d & %d & %s \\" % (esc(nm), c["base"], c.get("count", 1), inline(c.get("desc", ""))))
    o.append(r"\hline\end{tabular}\end{center}")

    o.append(r"\section{Address spaces}")
    o.append(r"\begin{center}\small\begin{tabular}{@{}llp{0.7\textwidth}@{}}\hline Suffix & Code & \\ \hline")
    for s in d["spaces"]:
        o.append(r"\texttt{.%s} & %d & %s \\" % (esc(s["suffix"]), s["code"], inline(s["desc"])))
    o.append(r"\hline\end{tabular}\end{center}")

    o.append(r"\section{Conditions}")
    for title, cs in (("Base conditions (\\texttt{b}, register compare)", d["base_conds"]),
                      ("Guest conditions (the 68k's, on the renamed flags)", d["guest_conds"])):
        o.append(r"\subsection*{%s}" % title)
        o.append(r"\begin{center}\small\begin{tabular}{@{}lll@{}}\hline Code & Name & Condition \\ \hline")
        for c in cs:
            o.append(r"%d & \texttt{%s} & \texttt{%s} \\" % (c["code"], esc(c["name"]), esc(c["expr"])))
        if cs is d["base_conds"]:     # the BR cond codes left free: explicit branches on u
            for op in d["base_ops"]:
                if op.get("major") == "BR" and "cond" in op:
                    o.append(r"%d & \texttt{%s} & \texttt{%s} (rs1, rs2 = 0) \\" % (op["cond"], esc(op["name"]),
                                                                                    esc(op["sem"])))
        o.append(r"\hline\end{tabular}\end{center}")

    o.append(r"\section{Control and status registers}")
    o.append(r"\begin{center}\small\begin{longtable}{@{}lllp{0.6\textwidth}@{}}\hline Number & Name & Implemented bits & \\ \hline\endhead")
    for c in d["csrs"]:
        wm = c.get("wmask")
        o.append(r"0x%02x & \texttt{%s} & %s & %s \\" % (c["num"], esc(c["name"]),
                                                          r"\texttt{0x%08x}" % wm if wm is not None else "all",
                                                          inline(c["desc"])))
    o.append(r"\hline\end{longtable}\end{center}")

    o.append(r"\section{Millicode entry points}")
    for ep in d["entry_points"]:
        o.append(r"\paragraph{%s} %s" % (esc(ep["name"]), inline(ep["desc"])))
        if ep.get("regs"):
            o.append(r"\begin{itemize}[nosep]")
            for r, desc in ep["regs"].items():
                o.append(r"\item \texttt{%s}: %s" % (esc(r), inline(desc)))
            o.append(r"\end{itemize}")
        if ep.get("note"):
            o.append(inline(ep["note"]))
    return "\n".join(o)

# ---------------------------------------------------------------------------
# The 68k flows (spec/flows.yaml, expanded by tools/flows.py)
# ---------------------------------------------------------------------------
LINES = [
    (0x0, "Bit manipulation, MOVEP and immediate"),
    (0x1, "MOVE.B"),
    (0x2, "MOVE.L and MOVEA.L"),
    (0x3, "MOVE.W and MOVEA.W"),
    (0x4, "Miscellaneous"),
    (0x5, "ADDQ, SUBQ, Scc, DBcc and TRAPcc"),
    (0x6, "Bcc, BSR and BRA"),
    (0x7, "MOVEQ"),
    (0x8, "OR, DIV and SBCD"),
    (0x9, "SUB and SUBX"),
    (0xA, "Unassigned: the line-A emulator trap"),
    (0xB, "CMP and EOR"),
    (0xC, "AND, MUL, ABCD and EXG"),
    (0xD, "ADD and ADDX"),
    (0xE, "Shift, rotate and bit field"),
    (0xF, "Coprocessor interface and 68040 extensions"),
]

RUNTIME_REFS = [
    (r"{d16@k} {d8@k} {abs.W@k} {abs.L@k}", "addressing-mode displacements and absolute addresses"),
    (r"{imm.B@k} {imm.W@k} {imm.L@k}", "immediates"),
    (r"{br.W@k} {br.L@k}", "branch displacement + 2, relative to the instruction"),
    (r"{pc16@k} {pc8@k}", "PC-relative: k + the 16-bit (or brief-format 8-bit) displacement"),
    (r"{xn@k}", "the index register named by a brief extension word"),
    (r"{bfb@k}", "the bitfield offset / 8"),
    (r"{bfm1@k}..{bfm4@k} {bfr@k} {bfi@k}", "packed bitfield descriptors (n memory bytes, register, low bits of a register)"),
    (r"{bfw@k}", "the width operand of a BFI-format op"),
    (r"{dx@k} {rn@k} {dh@k}", "the data register in bits 14:12, the general register in bits 15:12 (MOVEC), the data register in bits 2:0 (MULx.L Dh)"),
    (r"{kind@k|bf}", "a reference plus offset / 8"),
    (r"...+N", "any reference plus a constant"),
]


def flow_cell(fl):
    if fl.kind == "pla":
        return r"\newline ".join(tt(u) for u in fl.uops)
    if fl.kind == "sequencer":
        return r"\emph{MOVEM sequencer:} " + inline(fl.note or "one load or store per register in the mask, then the An update")
    if fl.kind == "millicode":
        return r"\emph{millicode} (%s)" % tt(fl.note)
    return r"\emph{not mapped:} " + esc(fl.note)


def flows_chapter():
    import flows
    import m68kdec
    ex = flows.Expander()
    forms, kinds_ops, kinds_forms = {}, collections.Counter(), collections.Counter()
    illegal_by_line = collections.Counter()
    for op in range(65536):
        fl = ex.expand(op)
        f = m68kdec.decode(op).form()
        if f == "ILLEGAL" and op != 0x4AFC:
            illegal_by_line[op >> 12] += 1
        kind = "fault" if f in ("ILLEGAL", "LINE-A", "LINE-F") else fl.kind
        kinds_ops[kind] += 1
        if f not in forms:
            forms[f] = (op, fl)
            kinds_forms[kind] += 1
    temps = ex.f["temps"]
    o = [r"\chapter{68k flows}\markright{68k flows}",
         r"Every 68k instruction form and the uops it becomes, expanded from \texttt{spec/flows.yaml} by "
         r"\texttt{tools/flows.py} (the same expansion as \texttt{flows.py show}). A form is a mnemonic with its "
         r"size and its source and destination addressing modes. Each form takes one of five paths:",
         r"\begin{itemize}[nosep]",
         r"\item \textbf{PLA}: the uops below, issued directly and retired as one atomic instruction;",
         r"\item \textbf{fault}: one \texttt{gfault} uop, raised as the instruction: ILLEGAL (vector 4), "
         r"line A (10) and line F (11), Section~\ref{sec:illegal};",
         r"\item \textbf{sequencer}: MOVEM, one load or store per register in the mask plus the address-register update;",
         r"\item \textbf{millicode}: the \texttt{insn} entry point (see the Architecture chapter), possibly with PLA flows for some "
         r"extension-word values, listed under the form;",
         r"\item \textbf{not mapped}: a legal instruction with no flow yet.",
         r"\end{itemize}",
         r"\begin{center}\small\begin{tabular}{@{}lrr@{}}\hline Path & Forms & Opwords \\ \hline"]
    for k, name in (("pla", "PLA"), ("fault", "fault"), ("sequencer", "sequencer"), ("millicode", "millicode"),
                    ("unmapped", "not mapped")):
        o.append(r"%s & %d & %d \\" % (name, kinds_forms[k], kinds_ops[k]))
    o.append(r"\hline\end{tabular}\end{center}")
    o.append(r"Each flow is shown for the lowest opword of its form, so register fields are 0 (\texttt{d0}, "
             r"\texttt{a0}). Flow temporaries: %s. A %s operand is a value taken at run time "
             r"from the extension word k bytes after the instruction's address:"
             % (", ".join(r"\texttt{%s} = \texttt{%s}" % (esc(k), esc(v)) for k, v in temps.items()), tt("{kind@k}")))
    o.append(r"\begin{center}\small\begin{tabular}{@{}lp{0.55\textwidth}@{}}\hline")
    for ref, what in RUNTIME_REFS:
        o.append(r"%s & %s \\" % (tt(ref), esc(what)))
    o.append(r"\hline\end{tabular}\end{center}")

    ill = forms["ILLEGAL"][1]
    o.append(r"\section{Illegal encodings}\label{sec:illegal}")
    o.append(r"An opword decodes as ILLEGAL when the 68040 defines no instruction for it, or when it names a "
             r"defined operation with an addressing mode outside that operation's category (data, memory, "
             r"control, alterable and their intersections; \texttt{tools/m68kdec.py}). It matches no other form "
             r"and its flow is one fault uop, %s, the illegal-instruction exception with the instruction's "
             r"address stacked (format 0). The ILLEGAL instruction itself (\texttt{4AFC}) is the same flow. "
             r"%d opwords decode this way; each line's section below gives its share. Line-A opwords and the "
             r"line-F opwords no coprocessor form claims are fault flows the same way, with vectors 10 and 11."
             % (tt("; ".join(ill.uops)), sum(illegal_by_line.values()) + 1))

    by_line = collections.defaultdict(list)
    for f, (op, fl) in forms.items():
        if f != "ILLEGAL":
            by_line[op >> 12].append((op, f, fl))
    head = (r"\begin{longtable}{@{}>{\raggedright\arraybackslash}p{0.27\textwidth}lr"
            r">{\raggedright\arraybackslash}p{0.5\textwidth}@{}}"
            r"\hline Form & Opword & Uops & Flow \\ \hline\endhead\hline\endfoot")
    for line, title in LINES:
        rows = sorted(by_line.get(line, []))
        o.append(r"\section{Line %X: %s}" % (line, esc(title)))
        if illegal_by_line[line]:
            o.append(r"%d of this line's 4096 opwords decode as ILLEGAL." % illegal_by_line[line])
        o.append(r"{\footnotesize" + head)
        for op, f, fl in rows:
            n = str(fl.count) if fl.kind == "pla" else ""
            group = [r"%s & \texttt{%04X} & %s & %s" % (tt(f), op, n, flow_cell(fl))]
            for label, v, compact in flows.labeled_variants(ex, op):
                group.append(r" & & %d & \emph{%s:}\newline %s" % (v.count, esc(label),
                                                                    r"\newline ".join(tt(u) for u in v.uops)))
            # a form and its variants stay on one page (\\* forbids the break)
            o.append(" \\\\*\n".join(group) + r" \\ \hline")
        o.append(r"\end{longtable}}")
    return "\n".join(o)


def table3(heads, rows):
    per = 3
    n = (len(rows) + per - 1) // per
    o = [r"\begin{center}\small\begin{tabular}{%s}\hline" % "@{\\hspace{2em}}".join(["lll"] * per),
         " & ".join(heads * per) + r" \\ \hline"]
    for r in range(n):
        cells = []
        for c in range(per):
            k = c * n + r
            cells += [rows[k][0], tt(rows[k][1]), tt(rows[k][2])] if k < len(rows) else ["", "", ""]
        o.append(" & ".join(cells) + r" \\")
    o.append(r"\hline\end{tabular}\end{center}")
    return "\n".join(o)


PREAMBLE = r"""\pdfinfoomitdate=1 \pdftrailerid{} \pdfsuppressptexinfo=-1
\documentclass[10pt,oneside]{report}
\usepackage[letterpaper,margin=1in,headheight=14pt]{geometry}
\usepackage[T1]{fontenc}
\usepackage{mathptmx}
\usepackage[scaled=0.92]{helvet}
\usepackage{courier}
\usepackage{tikz}
\usepackage{xcolor}
\usepackage{listings}
\usepackage{enumitem}
\usepackage{fancyhdr}
\usepackage{longtable}
\usepackage{array}
\usepackage{changepage}
\usepackage{titlesec}
\usepackage[hidelinks,bookmarksopen]{hyperref}
\titleformat{\section}{\sffamily\bfseries\Large}{\thesection}{0.6em}{}
\titleformat{\subsection}{\sffamily\bfseries\large}{\thesubsection}{0.6em}{}
\titleformat{\chapter}[display]{\sffamily\bfseries\Huge}{\Large Chapter \thechapter}{8pt}{}
\titlespacing*{\chapter}{0pt}{0pt}{24pt}
\lstdefinestyle{sem}{language=C,basicstyle=\small\ttfamily,columns=fullflexible,keepspaces=true,
  showstringspaces=false,keywordstyle=\bfseries,aboveskip=2pt,belowskip=2pt}
\lstdefinestyle{plain}{basicstyle=\small\ttfamily,columns=fullflexible,keepspaces=true,xleftmargin=1.5em}
\setlength{\parindent}{0pt}
\setlength{\parskip}{0.5ex}
\pagestyle{fancy}
\fancyhf{}
\fancyhead[L]{\sffamily\small @NAME@ @VERSION@}
\fancyhead[R]{\sffamily\small\nouppercase{\rightmark}}
\fancyfoot[C]{\sffamily\small\thepage}
\renewcommand{\headrulewidth}{0.4pt}
\newlist{insnfields}{description}{1}
\setlist[insnfields]{leftmargin=7em,labelwidth=6.5em,labelsep=0.5em,align=left,font=\normalfont,itemsep=2pt}
% one instruction page: mnemonic, name, bookmark text
\newcommand{\insnpage}[3]{\clearpage\markright{#1}\phantomsection\addcontentsline{toc}{section}{#3}%
  \noindent{\sffamily\bfseries\fontsize{30}{32}\selectfont #1}\hfill{\sffamily\bfseries\Large #2}\par
  \vspace{-4pt}\noindent\rule{\textwidth}{0.8pt}\par\vspace{6pt}}
\setcounter{tocdepth}{1}
\begin{document}
"""


def main():
    spec = uisa.load()
    d = spec.d
    for sec in ("base_ops", "mem_ops", "guest_ops"):
        for e in d[sec]:
            if not e.get("title"):
                raise SystemExit("gen_pdf: %s %s has no title" % (sec, e.get("name") or e.get("family")))
    o = [PREAMBLE.replace("@NAME@", esc(d.get("name", "uisa"))).replace("@VERSION@", esc(d.get("version", "")))]
    o.append(r"\begin{titlepage}\centering\vspace*{2in}"
             r"{\sffamily\bfseries\fontsize{40}{44}\selectfont %s\par}\vspace{0.4in}"
             r"{\sffamily\Large A micro-ISA for a 68040 micro-engine\par}\vspace{0.2in}"
             r"{\sffamily\large Instruction set reference, version %s\par}\vfill"
             r"{\small Generated from \texttt{spec/uisa.yaml} by \texttt{tools/gen\_pdf.py}.\par}"
             r"\end{titlepage}" % (esc(d.get("name", "uisa")), esc(d.get("version", ""))))
    o.append(r"\tableofcontents\clearpage")
    o.append(r"\chapter{Introduction}\markright{Introduction}")
    o.append(md_to_tex(open(os.path.join(ROOT, "doc", "intro.md")).read()))
    o.append(r"\chapter{Architecture}\markright{Architecture}")
    o.append(front(spec, d))

    parts = [("base", "base_ops", "Base operations",
              "A conventional RISC base: no flags, full 32-bit results. Millicode's own computation and control flow."),
             ("mem", "mem_ops", "Memory operations",
              "Big-endian loads, stores, locked sequences, compare-and-swap and MMU probes, in four address spaces."),
             ("guest", "guest_ops", "Guest operations",
              "68k-semantic ALU operations: sized operands, the five flags, merge and zero-fill forms.")]
    for part, sec, title, blurb in parts:
        o.append(r"\chapter{%s}\markright{%s}" % (title, title))
        o.append(blurb)
        entries = sorted(d[sec], key=lambda e: (e.get("name") or e.get("family")))
        for e in entries:
            fam = e.get("name") or e.get("family")
            vs = [v for v in spec.variants
                  if v.attrs.get("family") == fam and
                  ((part == "base" and v.kind == "base") or (part == "guest" and v.kind == "guest") or
                   (part == "mem" and v.kind not in ("base", "guest")))]
            if not vs:
                raise SystemExit("gen_pdf: no variants for %s" % fam)
            o.append(page(spec, part, e, vs))
    o.append(flows_chapter())
    o.append(r"\end{document}")

    os.makedirs(OUT_DIR, exist_ok=True)
    tex = os.path.join(OUT_DIR, "uisa.tex")
    with open(tex, "w") as f:
        f.write("\n".join(o) + "\n")
    toc_path = os.path.join(OUT_DIR, "uisa.toc")
    if os.path.exists(toc_path):
        os.remove(toc_path)
    prev = None
    for _ in range(4):        # until the table of contents (and so the page numbers) settles
        r = subprocess.run(["pdflatex", "-interaction=nonstopmode", "-halt-on-error", "uisa.tex"],
                           cwd=OUT_DIR, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode:
            sys.stdout.write(r.stdout[-4000:])
            raise SystemExit("gen_pdf: pdflatex failed (see build/pdf/uisa.log)")
        toc = open(toc_path).read() if os.path.exists(toc_path) else ""
        if toc == prev:
            break
        prev = toc
    shutil.copy(os.path.join(OUT_DIR, "uisa.pdf"), PDF)
    pages = re.search(r"Output written on uisa.pdf \((\d+) pages", r.stdout)
    print("uisa.pdf: %s pages -> doc/uisa.pdf" % (pages.group(1) if pages else "?"))


if __name__ == "__main__":
    main()
