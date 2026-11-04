#!/usr/bin/env python3
"""One-time build pipeline: disc image -> Wonderpark.exe, all on this PC.

Usage:
    python builder/pipeline.py <disc image> [--dtk path]

Stages (the launcher shows one star point per stage):
  1. Verifying disc                  tools/extract_disc.py --verify-only
  2. Extracting main.dol and files   tools/extract_disc.py
  3. Recompiling PowerPC to C++      tools/recomp.py
  4. Compiling Wonderpark.exe        tools/build_game.py (clang-cl when available)

Machine-readable progress for the launcher, one line each:
    @stage <1-4> <name>
    @progress <overall 0-100> <message>
    @done <path to exe>        or   @error <message>
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PY = sys.executable
STAGES = ["Verifying disc", "Extracting main.dol and files", "Recompiling PowerPC to C++", "Compiling Wonderpark.exe"]
# Share of the overall bar per stage (sums to 100).
WEIGHT = [2, 8, 15, 75]


def emit(line: str) -> None:
    print(line, flush=True)


def overall(stage: int, pct: float) -> int:
    return int(sum(WEIGHT[:stage]) + WEIGHT[stage] * pct / 100)


def run_stage(i: int, cmd: list[str], parse=None) -> None:
    emit(f"@stage {i + 1} {STAGES[i]}")
    emit(f"@progress {overall(i, 0)} {STAGES[i]}")
    proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    tail: list[str] = []
    for line in proc.stdout:
        tail = (tail + [line])[-40:]
        m = re.match(r"@progress (\d+) (.*)", line)
        pct = int(m.group(1)) if m else (parse(line) if parse else None)
        if pct is not None:
            emit(f"@progress {overall(i, pct)} {STAGES[i]}")
    if proc.wait():
        raise RuntimeError(f"{STAGES[i]} failed:\n" + "".join(tail))
    emit(f"@progress {overall(i, 100)} {STAGES[i]}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--dtk")
    a = ap.parse_args()
    dtk = ["--dtk", a.dtk] if a.dtk else []
    build = REPO / "build" / "game"
    try:
        run_stage(0, [PY, "tools/extract_disc.py", a.image, "--verify-only"])
        run_stage(1, [PY, "tools/extract_disc.py", a.image, "--force", *dtk],
                  parse=lambda l: (lambda m: int(100 * int(m[1]) / int(m[2])) if m else None)(re.search(r"(\d+)/(\d+) files", l)))

        run_stage(2, [PY, "tools/recomp.py"])

        run_stage(3, [PY, "tools/build_game.py"])

        exe = next((p for p in (build / "Wonderpark.exe", build / "Release" / "Wonderpark.exe") if p.is_file()), None)
        if not exe:
            raise RuntimeError("build finished but Wonderpark.exe is missing")
        emit(f"@done {exe}")
        return 0
    except (RuntimeError, subprocess.CalledProcessError) as e:
        msg = str(e).strip().replace("\n", " | ")
        emit(f"@error {msg}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
