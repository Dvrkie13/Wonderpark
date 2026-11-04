#!/usr/bin/env python3
"""Recompile the extracted main.dol to C with DolRecomp.

Usage:
    python tools/recomp.py [--game game] [--out generated] [-j N]

Steps:
  1. Build DolRecomp from the pinned submodule (tools/dolrecomp) into
     build/dolrecomp, unless --dolrecomp points at an existing binary.
  2. Run it on <game>/sys/main.dol as a Wii (Broadway) title, S2LE01.
     (The disc has no .rel modules; if any ever appear, the folder is passed
     instead so DolRecomp resolves imports between them.)
  3. Check every self-modifying-code flag against config/S2LE01/smc_review.toml
     and stop if an unreviewed one shows up.

Progress lines for the launcher are printed as:  @progress <0-100> <message>
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GAME_ID = "S2LE01"
EXE = ".exe" if os.name == "nt" else ""


class RecompError(Exception):
    pass


def progress(pct: int, msg: str) -> None:
    print(f"@progress {pct} {msg}", flush=True)


def build_dolrecomp(build_dir: Path) -> Path:
    src = REPO / "tools" / "dolrecomp"
    if not (src / "CMakeLists.txt").is_file():
        raise RecompError("tools/dolrecomp is empty: run  git submodule update --init --recursive")
    for cand in (build_dir / "Release" / f"dolrecomp{EXE}", build_dir / f"dolrecomp{EXE}"):
        if cand.is_file():
            return cand
    progress(2, "Building DolRecomp")
    run(["cmake", "-S", str(src), "-B", str(build_dir)])
    run(["cmake", "--build", str(build_dir), "--config", "Release", "--target", "dolrecomp", "-j"])
    for cand in (build_dir / "Release" / f"dolrecomp{EXE}", build_dir / f"dolrecomp{EXE}"):
        if cand.is_file():
            return cand
    raise RecompError("DolRecomp built, but dolrecomp executable not found")


def run(cmd: list[str], **kw) -> None:
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if p.returncode:
        raise RecompError(f"{Path(cmd[0]).name} failed:\n{(p.stdout + p.stderr)[-4000:]}")


def reviewed_flags() -> set[int]:
    path = REPO / "config" / GAME_ID / "smc_review.toml"
    if not path.is_file():
        return set()
    with path.open("rb") as f:
        return {int(e["address"]) for e in tomllib.load(f).get("flag", [])}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", type=Path, default=REPO / "game")
    ap.add_argument("--out", type=Path, default=REPO / "generated")
    ap.add_argument("--dolrecomp", type=Path, help="use this dolrecomp binary instead of building one")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    try:
        dol = a.game / "sys" / "main.dol"
        if not dol.is_file():
            raise RecompError(f"{dol} not found: run tools/extract_disc.py first")
        rels = sorted(p for p in a.game.rglob("*.rel"))
        if rels:
            raise RecompError(f"{len(rels)} .rel modules found; REL support isn't wired up yet")

        tool = a.dolrecomp or build_dolrecomp(REPO / "build" / "dolrecomp")

        # DolRecomp falls back to GameCube mode when database/titles.txt is
        # missing; the file only provides display names, so an empty one is enough.
        work = REPO / "build" / "recomp-work"
        (work / "database").mkdir(parents=True, exist_ok=True)
        (work / "database" / "titles.txt").touch()

        out_dir = a.out / f"{GAME_ID}_generated"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        a.out.mkdir(parents=True, exist_ok=True)

        progress(5, "Recompiling PowerPC to C")
        log_lines: list[str] = []
        proc = subprocess.Popen([str(tool), f"-j{a.j}", str(dol.resolve()), GAME_ID, str(a.out.resolve())],
                                cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        total = done = 0
        for line in proc.stdout:
            log_lines.append(line)
            m = re.match(r"decoding text\[\d+\]: (\d+) instructions", line)
            if m:
                total += int(m.group(1))
            m = re.match(r"\s+(\d+) decoded, (\d+) known, (\d+) embedded data, (\d+) unknown", line)
            if m:
                done += int(m.group(1))
                if int(m.group(4)):
                    print(f"warning: {m.group(4)} unknown instructions", file=sys.stderr)
                progress(5 + int(90 * done / max(total, 1057160 + 2504)), "Recompiling PowerPC to C")
        proc.wait()
        (a.out / "dolrecomp.log").write_text("".join(log_lines))
        if proc.returncode or not (out_dir / f"{GAME_ID}.h").is_file():
            raise RecompError("dolrecomp failed:\n" + "".join(log_lines[-30:]))
        if not any("Broadway" in l for l in log_lines):
            raise RecompError("dolrecomp did not run in Wii (Broadway) mode")

        smc = out_dir / f"{GAME_ID}_smc.txt"
        flagged = {int(m, 16) for m in re.findall(r"^0x([0-9A-F]{8})-", smc.read_text(), re.M)} if smc.is_file() else set()
        new = sorted(flagged - reviewed_flags())
        if new:
            raise RecompError("unreviewed self-modifying-code flags (add them to config/S2LE01/smc_review.toml): "
                              + ", ".join(f"{x:08X}" for x in new))

        chunks = len(list((out_dir / "chunks").glob("*.c")))
        progress(100, f"Recompiled {done} instructions into {chunks} chunks")
        return 0
    except RecompError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
