#!/usr/bin/env python3
"""Profile-guided optimisation, on this PC: builds an instrumented
Wonderpark.exe (build/game-pgogen), plays each given save headless for a
while (loads save slot 1 and walks), and merges the profiles into
build/pgo/wonderpark.profdata, which tools/build_game.py then uses.

The profile is made from the recompiled game code, so it is game-derived:
it stays in build/ (git-ignored) and is never shared.

Usage:
    python tools/pgo_train.py [--save NAND_DIR ...] [--seconds 150]

Each --save is a NAND folder (like %APPDATA%\\Wonderpark\\nand) whose save
slot 1 is somewhere busy (a park area). Default: the player's own NAND.
Train on more than one area if you can (the park and a beach, say).
Takes about 10 minutes to build plus the play time; then run
tools/build_game.py (about 20 minutes) to build with the profile.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import build_game  # noqa: E402

# Press 2 through the title and the save menu (slot 1), then walk about.
PAD_SCRIPT = ",".join(f"{t}:2,{t + 0.3:.1f}:" for t in range(25, 46, 3)) + \
    ",60:up,72:,73:right,76:up,90:left,93:up,110:,112:right,125:up,140:"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--save", action="append", help="NAND folder to train on (repeatable)")
    ap.add_argument("--seconds", type=int, default=150, help="play time per save (default 150)")
    a = ap.parse_args()
    saves = [Path(s) for s in a.save] if a.save else \
        [Path(os.environ.get("APPDATA", "")) / "Wonderpark" / "nand"]
    for s in saves:
        if not (s / "title").is_dir():
            print(f"error: {s} is not a NAND folder (no title\\ inside)", file=sys.stderr)
            return 1

    print("Building the instrumented executable", flush=True)
    if subprocess.run([sys.executable, str(REPO / "tools/build_game.py"), "--build-dir", "game-pgogen",
                       "--no-lto", "--pgo-generate"], cwd=REPO).returncode:
        return 1
    exe = REPO / "build/game-pgogen/Wonderpark.exe"
    out = REPO / "build/pgo"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    data = REPO / "build/pgo-train-data"
    for s in saves:
        print(f"Training on {s} ({a.seconds} s)", flush=True)
        shutil.rmtree(data, ignore_errors=True)
        shutil.copytree(s, data / "nand")
        env = dict(os.environ, LLVM_PROFILE_FILE=str(out / "%p.profraw"), WONDERPARK_DATA_DIR=str(data),
                   WONDERPARK_PAD_SCRIPT=PAD_SCRIPT)
        subprocess.run([str(exe), "--headless", "--exit-after", str(a.seconds)], cwd=REPO, env=env)
    shutil.rmtree(data, ignore_errors=True)
    raws = sorted(out.glob("*.profraw"))
    if not raws:
        print("error: no profile was written", file=sys.stderr)
        return 1

    clang = build_game.find_clang_cl(build_game.vs_env())
    profdata = Path(clang[0]).parent / "llvm-profdata.exe" if clang else None
    if not profdata or not profdata.is_file():
        print("error: no llvm-profdata.exe beside clang-cl", file=sys.stderr)
        return 1
    if subprocess.run([str(profdata), "merge", "-o", str(out / "wonderpark.profdata"), *map(str, raws)]).returncode:
        return 1
    for r in raws:
        r.unlink()
    print(f"Profile: {out / 'wonderpark.profdata'}. Now run: python tools/build_game.py", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
