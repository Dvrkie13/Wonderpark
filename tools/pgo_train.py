#!/usr/bin/env python3
"""Profile-guided optimisation, on this PC: builds an instrumented
Wonderpark.exe (build/game-pgogen), plays it hidden and silent for a few
minutes, and merges what it counted into build/pgo/wonderpark.profdata,
which tools/build_game.py then uses (about 38% less time in the game's own
code: docs/research.md, "Weak-PC measurements").

What it plays:
  - a fresh game (an empty save folder): pressing 2 takes it through the
    opening story into the first area, where it walks and jumps about;
  - and each save folder given (--save), or the player's own
    (%APPDATA%\\Wonderpark\\nand) if it has a save: slot 1, walking.
Nothing is written to the player's own saves or settings: each run gets a
data folder of its own in build/.

The profile is made from the recompiled game code, so it is game-derived:
it stays in build/ (git-ignored) and is never shared.

Usage:
    python tools/pgo_train.py [--mingw] [--save NAND_DIR ...] [--no-own-save]
                              [--fresh-seconds 200] [--seconds 150]

The pipeline's background optimised build (builder/pipeline.py --optimise)
runs this with --mingw, then builds with the profile. By hand: run this,
then tools/build_game.py.
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

# A save: press 2 through the title and the save menu (slot 1), then walk.
SAVE_SCRIPT = ",".join(f"s6+{t}:2,s6+{t}.3:" for t in range(4, 60, 2)) + \
    ",s0+5:up,s0+17:,s0+18:right,s0+21:up,s0+35:left,s0+38:up,s0+55:,s0+57:right,s0+70:up,s0+85:"
# A fresh game: press 2 (it moves the story's talk on) until the first area
# (about 130 s), then walk, turn and jump.
FRESH_SCRIPT = ",".join(f"{t}:2,{t}.3:" for t in range(6, 132, 2)) + "," + ",".join(
    f"{t}:{d},{t + 3}:,{t + 3}.2:2,{t + 3}.5:" for t, d in
    zip(range(134, 400, 4), ["up", "left", "up", "right", "down", "up", "left", "right"] * 9))


def own_save() -> Path | None:
    """The player's own NAND folder, if it has a Wonderpark save in it."""
    nand = Path(os.environ.get("APPDATA", "")) / "Wonderpark" / "nand"
    data = os.environ.get("WONDERPARK_DATA_DIR")
    if data:
        nand = Path(data) / "nand"
    return nand if (nand / "title/00010000/53324c45/data/GD0").is_file() else None


def profdata_tool(mingw: bool) -> Path | None:
    if mingw:
        b = build_game.find_llvm_mingw()
        return b / "llvm-profdata.exe" if b and (b / "llvm-profdata.exe").is_file() else None
    clang = build_game.find_clang_cl(build_game.vs_env())
    p = Path(clang[0]).parent / "llvm-profdata.exe" if clang else None
    return p if p and p.is_file() else None


def play(exe: Path, out: Path, nand: Path | None, script: str, seconds: int) -> None:
    """One hidden, silent run; its counts land in out/<pid>.profraw."""
    data = REPO / "build" / "pgo-train-data"
    shutil.rmtree(data, ignore_errors=True)
    (data / "nand").mkdir(parents=True)
    if nand:
        shutil.copytree(nand, data / "nand", dirs_exist_ok=True)
    env = dict(os.environ, LLVM_PROFILE_FILE=str(out / "%p.profraw"), WONDERPARK_DATA_DIR=str(data),
               WONDERPARK_PAD_SCRIPT=script, RECOMP_AUDIO="0", WONDERPARK_NO_MODS="1",
               WONDERPARK_PAUSE_BACKGROUND="0", WONDERPARK_SKIP_LOGOS="1")
    # Under another name: the launcher takes a running Wonderpark.exe for the
    # player's game, and stops the background build when it sees one.
    runner = exe.with_name("WonderparkTraining.exe")
    shutil.copyfile(exe, runner)
    subprocess.run([str(runner), "--headless", "--exit-after", str(seconds)], cwd=REPO, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    shutil.rmtree(data, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--mingw", action="store_true", help="build with llvm-mingw (the build players get)")
    ap.add_argument("--save", action="append", default=[], help="NAND folder to train on too (repeatable)")
    ap.add_argument("--no-own-save", action="store_true", help="leave out the player's own save")
    ap.add_argument("--fresh-seconds", type=int, default=200, help="fresh game play time (default 200; 0: none)")
    ap.add_argument("--seconds", type=int, default=150, help="play time per save (default 150)")
    a = ap.parse_args()
    saves = [Path(s) for s in a.save]
    if not a.no_own_save and own_save():
        saves.append(own_save())
    for s in saves:
        if not (s / "title").is_dir():
            print(f"error: {s} is not a NAND folder (no title\\ inside)", file=sys.stderr)
            return 1
    tool = profdata_tool(a.mingw)
    if not tool:
        print("error: no llvm-profdata.exe beside the compiler", file=sys.stderr)
        return 1

    print("@progress 0 Building the training copy", flush=True)
    # Counted per function as the compiler first sees it, so the training
    # copy's chunks can be -O1 (built in about a third of the time).
    cmd = [sys.executable, "-u", str(REPO / "tools/build_game.py"), "--build-dir", "game-pgogen", "--no-lto",
           "--pgo-generate", "--chunk-opt", "1"] + (["--mingw"] if a.mingw else [])
    if subprocess.run(cmd, cwd=REPO).returncode:
        return 1
    exe = REPO / "build/game-pgogen/Wonderpark.exe"
    out = REPO / "build/pgo"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    runs = ([(None, FRESH_SCRIPT, a.fresh_seconds)] if a.fresh_seconds > 0 else []) + \
        [(s, SAVE_SCRIPT, a.seconds) for s in saves]
    for i, (nand, script, secs) in enumerate(runs):
        print(f"@progress {60 + 35 * i // len(runs)} Training: {'your save' if nand else 'a fresh game'} ({secs} s)",
              flush=True)
        play(exe, out, nand, script, secs)
    raws = sorted(out.glob("*.profraw"))
    if not raws:
        print("error: no profile was written", file=sys.stderr)
        return 1
    if subprocess.run([str(tool), "merge", "-o", str(out / "wonderpark.profdata"), *map(str, raws)]).returncode:
        return 1
    for r in raws:
        r.unlink()
    print(f"@progress 100 Profile: {out / 'wonderpark.profdata'}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
