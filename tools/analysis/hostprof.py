#!/usr/bin/env python3
"""Summarise build/hostprof_lines.txt (from WONDERPARK_LOG=hostprof): host
samples by guest function (generated-code lines mapped back through their
// ADDR: comments and config/S2LE01/symbols.txt), and by runtime function
line for everything else."""
import bisect, re, sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
lines = Counter(l.strip() for l in open(sys.argv[1] if len(sys.argv) > 1 else REPO / "build/hostprof_lines.txt"))
total = sum(lines.values())
syms = sorted((int(m.group(2), 16), m.group(1)) for l in open(REPO / "config/S2LE01/symbols.txt")
              if (m := re.match(r"(\S+) = \.text:0x([0-9A-F]+)", l)))
starts = [a for a, _ in syms]
cache = {}
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
guest, host, kinds = Counter(), Counter(), Counter()
for key, n in lines.items():
    path, _, ln = key.rpartition(":")
    if "chunk_" in path:
        addr = guest_addr(path, int(ln))
        i = bisect.bisect_right(starts, addr) - 1
        guest[syms[i][1] if i >= 0 else f"{addr:08X}"] += n
        # what kind of generated statement it is
        text = open(path, errors="replace").read().split("\n")[int(ln) - 1] if int(ln) else ""
        kind = "mem" if "mem_read" in text or "mem_write" in text else "fp" if "ppc_f" in text or "ppc_ps" in text or "dolrecomp_f" in text else "switch/goto" if "case 0x" in text or "goto" in text or "switch" in text else "other"
        kinds[kind] += n
    else:
        host[f"{Path(path).name}:{ln}"] += n
print(f"{total} samples; generated code {sum(guest.values())*100/total:.0f}%")
print("generated-code statements by kind:", ", ".join(f"{k} {v*100/total:.1f}%" for k, v in kinds.most_common()))
print("top guest functions:")
for k, v in guest.most_common(20): print(f"  {v*100/total:5.1f}%  {k}")
print("top host lines:")
for k, v in host.most_common(15): print(f"  {v*100/total:5.1f}%  {k}")
