#!/usr/bin/env python3
"""One-time build pipeline: disc image -> Wonderpark.exe, all on this PC.

Usage:
    python builder/pipeline.py <disc image> [--dtk path]   build (the launcher's Build)
    python builder/pipeline.py --rebuild                   build again from the extracted disc in game/
    python builder/pipeline.py --status                    is there a build, and is it current?
    python builder/pipeline.py --optimise                  the fully optimised build (after a build)

Building is quick first (the recompiled code at -O1: playable in minutes);
the launcher then runs --optimise in the background while it's idle (stopped
when the game starts, picked up again later) and plays build/game-opt once
that's done.

Stages (the launcher relights one star point per finished stage):
  1. Verifying disc                  tools/extract_disc.py (game ID, revision, image hash)
  2. Extracting main.dol and files   tools/extract_disc.py
  3. Recompiling PowerPC to C++      tools/recomp.py
  4. Compiling Wonderpark.exe        tools/build_game.py (clang-cl when available),
     ending with "Preparing shaders…" (Wonderpark.exe --prepare-shaders, a few seconds)
Stages 1-2 are skipped (shown as done) when game/ already holds this disc.

Machine-readable lines for the launcher, one each (everything else is log):
    @stage <1-4> <name>
    @progress <overall 0-100> <stage name>
    @done <path to exe>
    @error <short plain-language message>
    @status ok|missing|outdated            (--status)
    @image <path of the disc image last built from>   (--status, when known)
    @exe <path of the exe to play>          (--status, when ok: the optimised one once it's current)
    @optimise needed|done                  (--status, when ok)

After a build, build/game/wonderpark_build.json records a hash of the
sources it was built from; --status compares it with the sources now
("outdated" when they differ, as after an update).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PY = sys.executable
STAGES = ["Verifying disc", "Extracting main.dol and files", "Recompiling PowerPC to C++", "Compiling Wonderpark.exe"]
# Share of the overall bar per stage (sums to 100).
WEIGHT = [3, 7, 15, 75]
GAME = REPO / "game"
BUILD = REPO / "build" / "game"
STAMP = BUILD / "wonderpark_build.json"
# The optimised build (--optimise): the same game compiled with full
# optimisation, made after the quick first build while the launcher sits
# idle; used instead of build/game once it's done and current.
OPT = REPO / "build" / "game-opt"
OPT_STAMP = OPT / "wonderpark_build.json"
# The first build compiles the recompiled code at -O1 without ThinLTO:
# about 4 minutes instead of 16 on a 16-thread PC (measured 2026-11-19),
# the game about 7% busier per frame than -O2. -O2 (--optimise) takes
# about 11; ThinLTO adds 5 more for about 2%, so neither build uses it.
QUICK_FLAGS = ["--no-lto", "--chunk-opt", "1"]
OPT_FLAGS = ["--no-lto", "--build-dir", "game-opt"]
# What the build is made from: a change to any of these makes a build out of date.
SOURCES = ["CMakeLists.txt", "runtime", "config", "tools/recomp.py", "tools/build_game.py",
           "tools/extract_disc.py", "tools/dolrecomp/src", "tools/dolrecomp/CMakeLists.txt"]
SOURCE_EXT = {".c", ".h", ".cpp", ".hpp", ".cc", ".inc", ".py", ".txt", ".toml", ".json", ".cmake", ".def"}


class StageError(Exception):
    """A failure with a short message for the player (details are in the log)."""


def emit(line: str) -> None:
    print(line, flush=True)


def overall(stage: int, pct: float) -> int:
    return int(sum(WEIGHT[:stage]) + WEIGHT[stage] * max(0.0, min(100.0, pct)) / 100)


def source_stamp() -> str:
    h = hashlib.sha1()
    for rel in SOURCES:
        p = REPO / rel
        files = [p] if p.is_file() else sorted(f for f in p.rglob("*") if f.is_file() and f.suffix.lower() in SOURCE_EXT)
        for f in files:
            h.update(str(f.relative_to(REPO)).replace("\\", "/").encode())
            h.update(f.read_bytes().replace(b"\r\n", b"\n"))
    return h.hexdigest()


def find_exe(folder: Path = BUILD) -> Path | None:
    return next((p for p in (folder / "Wonderpark.exe", folder / "Release" / "Wonderpark.exe") if p.is_file()), None)


def read_stamp(path: Path) -> dict:
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return {}


def opt_current(sources: str) -> bool:
    """The optimised build is there and made from the same sources."""
    return bool(find_exe(OPT)) and read_stamp(OPT_STAMP).get("sources") == sources


def status() -> int:
    stamp = read_stamp(STAMP)
    if stamp.get("image"):
        emit(f"@image {stamp['image']}")
    sources = source_stamp()
    if not find_exe() or not stamp:
        emit("@status missing")
    elif stamp.get("sources") != sources or not (GAME / "manifest.json").is_file():
        emit("@status outdated")
    else:
        emit("@status ok")
        if opt_current(sources):
            emit(f"@exe {find_exe(OPT)}")
            emit("@optimise done")
        else:
            emit(f"@exe {find_exe()}")
            emit("@optimise needed")
    return 0


def optimise() -> int:
    """Builds build/game-opt with full optimisation from the recompiled code
    already in generated/ (the launcher runs this in the background, at a
    low priority, and stops it when the game starts; ninja goes on where it
    stopped next time)."""
    sources = source_stamp()
    if read_stamp(STAMP).get("sources") != sources:
        emit("@error Build the game first.")
        return 1
    if opt_current(sources):
        emit(f"@done {find_exe(OPT)}")
        return 0
    t0 = time.time()
    emit(f"Optimising, {time.strftime('%Y-%m-%d %H:%M:%S')}")
    try:
        run_stage(3, [PY, "-u", "tools/build_game.py", *OPT_FLAGS], "Optimising failed. The log has the details.")
    except StageError as e:
        emit(f"@error {e}")
        return 1
    exe = find_exe(OPT)
    if not exe:
        emit("@error The optimised build is missing Wonderpark.exe.")
        return 1
    OPT_STAMP.write_text(json.dumps({"sources": sources, "built": time.strftime("%Y-%m-%d %H:%M:%S")}, indent=2))
    emit(f"Optimised in {(time.time() - t0) / 60:.1f} min")
    emit(f"@done {exe}")
    return 0


def preflight() -> None:
    """What the build needs on this PC, checked before anything starts."""
    sys.path.insert(0, str(REPO / "tools"))
    import build_game  # noqa: E402
    try:
        env = build_game.vs_env()
    except build_game.BuildError:
        raise StageError("Visual Studio Build Tools (with the C++ workload) aren't installed. "
                         "The README says how to get them.")
    if not shutil.which("cmake", path=env.get("PATH") or env.get("Path")):
        raise StageError("CMake wasn't found. Install the Visual Studio C++ workload, which includes it.")
    if not (REPO / "tools" / "dolrecomp" / "CMakeLists.txt").is_file():
        raise StageError("The DolRecomp recompiler is missing from tools/dolrecomp. Download the full release again.")


def run_stage(i: int, cmd: list[str], fail: str, phases: dict[str, int] | None = None) -> None:
    """Runs one stage's script; its @progress lines become overall progress.
    phases maps a script's "@phase <name>" lines to stages (extract_disc.py
    does stages 1 and 2 in one run)."""
    stage = i
    emit(f"@stage {stage + 1} {STAGES[stage]}")
    emit(f"@progress {overall(stage, 0)} {STAGES[stage]}")
    env = dict(os.environ, PYTHONUNBUFFERED="1")
    proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", env=env)
    tail: list[str] = []
    last = -1
    for line in proc.stdout:
        line = line.rstrip("\n")
        m = re.match(r"@phase (\w+)", line)
        if m and phases and m.group(1) in phases and phases[m.group(1)] != stage:
            emit(f"@progress {overall(stage, 100)} {STAGES[stage]}")
            stage = phases[m.group(1)]
            emit(f"@stage {stage + 1} {STAGES[stage]}")
            last = -1
            continue
        m = re.match(r"@progress (\d+)", line)
        if m:
            pct = overall(stage, int(m.group(1)))
            if pct != last:
                last = pct
                emit(f"@progress {pct} {STAGES[stage]}")
            continue
        print(line, flush=True)  # the log
        tail = (tail + [line])[-40:]
    if proc.wait():
        err = next((l for l in reversed(tail) if l.lower().startswith(("error", "fatal"))), "")
        raise StageError(fail(err) if callable(fail) else fail)
    emit(f"@progress {overall(stage, 100)} {STAGES[stage]}")


def prepare_shaders(exe: Path) -> None:
    """The end of stage 4, "Preparing shaders…": runs the new build's boot
    hidden for a few seconds (Wonderpark.exe --prepare-shaders), so the
    graphics driver compiles and caches its shaders now instead of pausing
    the player's first start. Not needed for the game to work: a failure is
    only noted in the log."""
    emit(f"@progress {overall(3, 100)} Preparing shaders…")
    t = time.time()
    try:
        p = subprocess.run([str(exe), "--prepare-shaders"], cwd=REPO, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=120)
        print(p.stdout + p.stderr, flush=True)  # the log
        if p.returncode:
            emit(f"Preparing shaders ended with code {p.returncode}; the first start may pause briefly")
        else:
            emit(f"Shaders prepared in {time.time() - t:.1f} s")
    except (OSError, subprocess.TimeoutExpired) as e:
        emit(f"Preparing shaders didn't finish ({type(e).__name__}); the first start may pause briefly")


def disc_failure(err: str) -> str:
    low = err.lower()
    if "wrong disc" in low:
        return "That isn't the US PokéPark 2 disc (S2LE01)."
    if "revision" in low:
        return "This disc revision hasn't been tested with Wonderpark yet."
    if "dtk not found" in low:
        return ("Reading this disc image needs decomp-toolkit (dtk.exe). Put dtk.exe in tools\\bin "
                "or on your PATH; the README has the link.")
    if "not found" in low:
        return "The disc image wasn't found. Place it again."
    return "The disc image couldn't be read. It may be damaged or in a format that isn't supported."


def already_extracted(image: Path) -> bool:
    """game/ holds this disc already (same file name and size, fully extracted)."""
    try:
        man = json.loads((GAME / "manifest.json").read_text())
    except (OSError, ValueError):
        return False
    return (man.get("game_id") == "S2LE01" and man.get("source_image") == image.name
            and man.get("image_size") == image.stat().st_size and (GAME / "sys" / "main.dol").is_file())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?", type=Path)
    ap.add_argument("--dtk")
    ap.add_argument("--status", action="store_true", help="report whether a current build exists")
    ap.add_argument("--rebuild", action="store_true", help="build from the already extracted disc in game/")
    ap.add_argument("--optimise", action="store_true", help="build the fully optimised game (after a build)")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    if a.status:
        return status()
    if a.optimise:
        return optimise()
    if not a.image and not a.rebuild:
        ap.error("give a disc image, or --rebuild")
    t0 = time.time()
    try:
        emit(f"Wonderpark build, {time.strftime('%Y-%m-%d %H:%M:%S')}, Python {sys.version.split()[0]}")
        preflight()
        image = a.image.resolve() if a.image else None
        if image and not image.is_file():
            raise StageError("The disc image wasn't found. Place it again.")
        if image is None or already_extracted(image):
            if not (GAME / "sys" / "main.dol").is_file():
                raise StageError("There's no extracted disc to rebuild from. Place your disc image again.")
            emit("The disc is already extracted in game/: skipping stages 1 and 2")
            for i in (0, 1):
                emit(f"@stage {i + 1} {STAGES[i]}")
                emit(f"@progress {overall(i, 100)} {STAGES[i]}")
            if image is None:
                image = Path(json.loads((GAME / "manifest.json").read_text()).get("source_image", ""))
        else:
            dtk = ["--dtk", a.dtk] if a.dtk else []
            run_stage(0, [PY, "-u", "tools/extract_disc.py", str(image), "--force", "--progress", *dtk],
                      disc_failure, phases={"verify": 0, "extract": 1})
            man_path = GAME / "manifest.json"
            man = json.loads(man_path.read_text())
            man["image_size"] = image.stat().st_size
            man_path.write_text(json.dumps(man, indent=2))

        # The optimised build is of the code being replaced: not used again
        # until --optimise makes it anew.
        if OPT_STAMP.is_file():
            OPT_STAMP.unlink()
        run_stage(2, [PY, "-u", "tools/recomp.py"], "Recompiling the game's code failed. The log has the details.")
        run_stage(3, [PY, "-u", "tools/build_game.py", *QUICK_FLAGS],
                  "Compiling Wonderpark.exe failed. The log has the details.")

        exe = find_exe()
        if not exe:
            raise StageError("The build finished but Wonderpark.exe is missing. The log has the details.")
        prepare_shaders(exe)
        STAMP.write_text(json.dumps({"sources": source_stamp(), "image": str(image),
                                     "built": time.strftime("%Y-%m-%d %H:%M:%S")}, indent=2))
        emit(f"Done in {(time.time() - t0) / 60:.1f} min")
        emit(f"@done {exe}")
        return 0
    except StageError as e:
        emit(f"@error {e}")
        return 1
    except Exception as e:  # anything unexpected: still a message for the player
        import traceback
        traceback.print_exc(file=sys.stdout)
        emit(f"@error Something went wrong in the builder ({type(e).__name__}). The log has the details.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
