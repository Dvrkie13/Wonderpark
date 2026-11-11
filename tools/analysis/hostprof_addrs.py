#!/usr/bin/env python3
"""Hot guest code by address, from build/hostprof_lines.txt (WONDERPARK_LOG=hostprof,
a build with --debug-info). Groups samples into 64-byte blocks and names each by
the symbol it is in, or "after <symbol>" when it lies past that symbol's size
(code no symbol covers: an unnamed function).

    python tools/analysis/hostprof_addrs.py [lines.txt] [top]"""
import bisect, re, sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
src = sys.argv[1] if len(sys.argv) > 1 else REPO / "build/hostprof_lines.txt"
top = int(sys.argv[2]) if len(sys.argv) > 2 else 30
syms = []
for l in open(REPO / "config/S2LE01/symbols.txt"):
    m = re.match(r"(\S+) = \.text:0x([0-9A-F]+); // size:0x([0-9A-F]+)", l)
    if m: syms.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
syms.sort()
starts = [s[0] for s in syms]
cache, blocks, total = {}, Counter(), 0


def guest_addr(path, ln):
    if path not in cache:
        addrs, cur = [], 0
        for text in open(path, errors="replace"):
            m = re.match(r"\s*// ([0-9A-F]{8}):", text)
            if m: cur = int(m.group(1), 16)
            addrs.append(cur)
        cache[path] = addrs
    a = cache[path]
    return a[min(ln, len(a)) - 1] if a else 0


for line in open(src):
    total += 1
    path, _, ln = line.strip().rpartition(":")
    if "chunk_" in path:
        blocks[guest_addr(path, int(ln)) & ~0x3F] += 1


def name(a):
    i = bisect.bisect_right(starts, a) - 1
    if i < 0: return "?"
    s, size, n = syms[i]
    return n if a < s + size else f"after {n} (+{a - s - size:X})"


for a, n in blocks.most_common(top):
    print(f"  {n * 100 / total:5.1f}%  {a:08X}  {name(a)}")
