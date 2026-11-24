#!/usr/bin/env python3
"""Build a program image from `ucc --program` output.

usage: uimage.py prog.s -o prog.hex

The .s file is the code (assembled by tools/uisa.py, starting at address 0
with `_start`) followed by an optional data section: a `.data <address>` line
and `.word 0x........` lines. The image is written as text:

    @<byte address, hex>      start of a segment
    <32-bit word, hex>        one per line, big-endian, consecutive addresses

Lines starting with `#` are comments. Code is one segment at 0; data is one
segment at its address. Everything is in the uisa hidden (physical) space.
"""

import argparse
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import uisa  # noqa: E402


def build(text, spec=None):
    """Return ([(address, [words])], labels) for the program text."""
    spec = spec or uisa.load()
    code, data, base = [], [], None
    for raw in text.splitlines():
        line = raw.split(";")[0].strip()
        if line.startswith(".data"):
            base = int(line.split()[1], 0)
            continue
        if base is None:
            code.append(raw)
        elif line.startswith(".word"):
            data.append(int(line.split()[1], 0))
        elif line:
            raise SystemExit("uimage: unexpected line in the data section: %r" % raw)
    words, labels = spec.assemble_program("\n".join(code))
    segs = [(0, words)]
    if data:
        if base < 4 * len(words):
            raise SystemExit("uimage: code (%d bytes) overlaps the data section at 0x%x" % (4 * len(words), base))
        segs.append((base, data))
    return segs, labels


def write_hex(segs, path):
    with open(path, "w") as f:
        f.write("# uisa program image: @byte-address, then big-endian 32-bit words\n")
        for addr, words in segs:
            f.write("@%08x\n" % addr)
            for w in words:
                f.write("%08x\n" % w)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("src")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    with open(a.src) as f:
        segs, _ = build(f.read())
    write_hex(segs, a.out)


if __name__ == "__main__":
    main()
