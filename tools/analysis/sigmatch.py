#!/usr/bin/env python3
"""Name functions in PokePark 2's main.dol by matching them against a
reference executable that has symbols (PokePark 1, R8AE01).

Usage:
    python -I tools/analysis/sigmatch.py <ref main.dol> <ref symbols.txt> <target main.dol> [--out config/S2LE01/symbols.txt]

Method:
  1. Function boundaries in the target come from `bl` targets (plus the entry
     point); each runs to the next start.
  2. Each function gets a signature: its instruction words with addresses
     masked out (bl offsets, the low half of lis/addi/ori pairs and of
     loads/stores off r2/r13 and other bases). Functions whose masked
     bodies are identical and unique on both sides are matched.
  3. Call-graph propagation: in a matched pair, the n-th bl of the reference
     function and the n-th bl of the target function call the same function,
     so their targets get matched too (even when those bodies differ). Repeats
     until nothing new is found.

Output: "name = .text:0xADDR; // size:0xN match:<how>" lines, one per named
target function. Nothing from either executable's code is written.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path


class Dol:
    def __init__(self, path: Path):
        d = path.read_bytes()
        u = lambda o: struct.unpack_from(">I", d, o)[0]
        self.text = []
        self.secs = []
        for i in range(18):
            off, addr, size = u(i * 4), u(0x48 + i * 4), u(0x90 + i * 4)
            if size:
                self.secs.append((addr, d[off:off + size], i < 7))
                if i < 7:
                    self.text.append((addr, addr + size))
        self.entry = u(0xE0)

    def word(self, a: int):
        for s, b, t in self.secs:
            if t and s <= a < s + len(b):
                return struct.unpack_from(">I", b, a - s)[0]
        return None

    def in_text(self, a: int) -> bool:
        return any(s <= a < e for s, e in self.text)


def bl_target(a: int, w: int):
    if w >> 26 == 18 and (w & 3) == 1:
        li = w & 0x3FFFFFC
        if li & 0x2000000:
            li -= 0x4000000
        return (a + li) & 0xFFFFFFFF
    return None


def mask(w: int) -> int:
    op = w >> 26
    if op == 18:
        return w & 0xFC000003
    if op in (14, 15, 24, 25) or 32 <= op <= 55 or op in (56, 57, 60, 61):
        return w & 0xFFFF0000
    return w


def functions_from_bl(dol: Dol):
    starts = {dol.entry}
    for s, e in dol.text:
        for a in range(s, e, 4):
            t = bl_target(a, dol.word(a))
            if t is not None and dol.in_text(t):
                starts.add(t)
    starts = sorted(starts)
    out = {}
    for i, f in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else next(e for s, e in dol.text if s <= f < e)
        end = min(end, next(e for s, e in dol.text if s <= f < e))
        # trim trailing zero padding
        while end > f + 4 and dol.word(end - 4) == 0:
            end -= 4
        out[f] = end
    return out


def load_symbols(path: Path):
    funcs = {}
    for line in path.read_text().splitlines():
        m = re.match(r"(\S+) = \.(?:text|init):0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)", line)
        if m:
            funcs[int(m.group(2), 16)] = (m.group(1), int(m.group(3), 16))
    return funcs


def body(dol: Dol, start: int, end: int):
    return [dol.word(a) for a in range(start, end, 4)]


def calls(dol: Dol, start: int, end: int):
    return [t for a in range(start, end, 4) if (t := bl_target(a, dol.word(a))) is not None]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("ref_dol", type=Path)
    ap.add_argument("ref_symbols", type=Path)
    ap.add_argument("target_dol", type=Path)
    ap.add_argument("--out", type=Path)
    a = ap.parse_args()

    ref, tgt = Dol(a.ref_dol), Dol(a.target_dol)
    rsyms = load_symbols(a.ref_symbols)
    tfuncs = functions_from_bl(tgt)

    def sig(dol, s, e):
        return tuple(mask(w) for w in body(dol, s, e))

    rsig = defaultdict(list)
    for addr, (name, size) in rsyms.items():
        if size >= 8:
            rsig[sig(ref, addr, addr + size)].append(addr)
    tsig = defaultdict(list)
    for s, e in tfuncs.items():
        tsig[sig(tgt, s, e)].append(s)

    match: dict[int, tuple[int, str]] = {}  # target addr -> (ref addr, how)
    used_ref: set[int] = set()
    for k, rs in rsig.items():
        ts = tsig.get(k)
        if ts and len(ts) == 1 and len(rs) == 1 and len(k) >= 3:
            match[ts[0]] = (rs[0], "sig")
            used_ref.add(rs[0])
    n_sig = len(match)

    # Call-graph propagation.
    changed = True
    while changed:
        changed = False
        for t, (r, _) in list(match.items()):
            rc = calls(ref, r, r + rsyms[r][1])
            tc = calls(tgt, t, tfuncs.get(t, t))
            if len(rc) != len(tc) or not rc:
                continue
            for rt, tt in zip(rc, tc):
                if rt in rsyms and tt not in match and rt not in used_ref and tt in tfuncs:
                    match[tt] = (rt, "call")
                    used_ref.add(rt)
                    changed = True

    names = Counter(rsyms[r][0] for r, _ in match.values())
    lines = []
    for t in sorted(match):
        r, how = match[t]
        name = rsyms[r][0]
        if name.startswith(("fn_", "lbl_", "@")) or names[name] > 1:
            continue  # unnamed in the reference, or ambiguous
        lines.append(f"{name} = .text:0x{t:08X}; // size:0x{tfuncs[t] - t:X} match:{how}")

    print(f"target functions: {len(tfuncs)}, reference functions: {len(rsyms)}", file=sys.stderr)
    print(f"matched: {len(match)} ({n_sig} by signature, {len(match) - n_sig} by call graph); "
          f"named (excluding fn_ placeholders): {len(lines)}", file=sys.stderr)
    text = "# Generated by tools/analysis/sigmatch.py against PokePark 1 (R8AE01). Addresses and names only.\n" + "\n".join(lines) + "\n"
    if a.out:
        a.out.parent.mkdir(parents=True, exist_ok=True)
        a.out.write_text(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
