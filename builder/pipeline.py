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

Stages (the build screen ticks each one off):
  1. Checking your disc              tools/extract_disc.py --verify-only (game ID, revision, image hash)
  2. Getting the build tools         tools/fetch_tools.py: the compiler and the disc reader,
                                     downloaded once (skipped when they're already in tools/)
  3. Reading the game                tools/extract_disc.py (main.dol and the files)
  4. Recompiling                     tools/recomp.py
  5. Compiling                       tools/build_game.py --mingw (llvm-mingw: no Visual Studio),
     ending with "Testing this PC's speed…" (Wonderpark.exe --prepare-shaders, 10 s: the
     driver caches its shaders, and the time per frame suggests a graphics preset: @suggest)
Stages 1 and 3 are skipped (shown as done) when game/ already holds this disc.

Machine-readable lines for the launcher, one each (everything else is log):
    @stage <1-5> <name>
    @size <bytes>                          (stage 2: what's left to download; 0 when nothing)
    @detail <text>                         (stage 2: "Compiler: 97 of 191 MB", "Compiler: unpacking")
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
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PY = sys.executable
STAGES = ["Checking your disc", "Getting the build tools", "Reading the game", "Recompiling", "Compiling"]
# Share of the overall bar per stage (sums to 100).
WEIGHT = [4, 8, 6, 14, 68]
TOOLS_STAGE, COMPILE_STAGE = 1, 4
HASHES = REPO / "build" / "disc_hashes.json"
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
QUICK_FLAGS = ["--mingw", "--no-lto", "--chunk-opt", "1"]
OPT_FLAGS = ["--mingw", "--no-lto", "--build-dir", "game-opt"]
# The profile the optimised build is guided by (made on this PC; game-derived).
PGO_PROFILE = REPO / "build" / "pgo" / "wonderpark.profdata"
# What the build is made from: a change to any of these makes a build out of date.
SOURCES = ["runtime", "config", "tools/recomp.py", "tools/build_game.py",
           "tools/extract_disc.py", "tools/dolrecomp/src", "tools/dolrecomp/CMakeLists.txt"]
SOURCE_EXT = {".c", ".h", ".cpp", ".hpp", ".cc", ".inc", ".py", ".txt", ".toml", ".json", ".cmake", ".def"}


class StageError(Exception):
    """A failure with a short message for the player (details are in the log)."""


# A build runs on its own, not tied to the launcher: closing the launcher
# leaves it running, and a launcher started later finds it (--status says
# "building" and where its log is) and shows it again. RUNNING says which
# process is building; the launcher asks it to stop by creating CANCEL.
RUNNING = REPO / "build" / "build-running.json"
CANCEL = REPO / "build" / "build.cancel"
_child: subprocess.Popen | None = None
_cancelled = threading.Event()


def pid_alive(pid: int) -> bool:
    """A live Python process with this ID (not a later process reusing it)."""
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.OpenProcess.restype = wintypes.HANDLE
    h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    try:
        code = wintypes.DWORD()
        if not k.GetExitCodeProcess(h, ctypes.byref(code)) or code.value != 259:  # STILL_ACTIVE
            return False
        buf = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(1024)
        if not k.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            return False
        return "python" in Path(buf.value).name.lower()
    finally:
        k.CloseHandle(h)


def running_build() -> dict | None:
    try:
        r = json.loads(RUNNING.read_text())
    except (OSError, ValueError):
        return None
    return r if r.get("pid") != os.getpid() and pid_alive(int(r.get("pid", 0))) else None


def watch_cancel() -> None:
    """Stops the build when the launcher asks (CANCEL appears): the running
    step and everything it started (ninja, the compilers) end at once."""
    while not _cancelled.is_set():
        if CANCEL.exists():
            try:
                CANCEL.unlink()
            except OSError:
                pass
            _cancelled.set()
            c = _child
            if c and c.poll() is None:
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(c.pid)], capture_output=True,
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            return
        time.sleep(0.5)


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
    """The optimised build is there, made from the same sources, and no older
    than the quick build (a quick build made since, by hand or by Rebuild, is
    the one to play)."""
    opt, quick = find_exe(OPT), find_exe()
    st = read_stamp(OPT_STAMP)
    # (One from before the profile-guided build has no "pgo": made again.)
    if not opt or st.get("sources") != sources or "pgo" not in st:
        return False
    return not quick or opt.stat().st_mtime >= quick.stat().st_mtime


def status() -> int:
    r = running_build()
    if r:
        emit("@status building")
        emit(f"@pid {r['pid']}")
        if r.get("log"):
            emit(f"@log {r['log']}")
        return 0
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
    # Profile-guided: a training copy plays a fresh game (and the player's
    # save, if any) hidden and silent, and the build uses what it counted
    # (tools/pgo_train.py): the game's own code takes about 38% less time.
    # Without a profile (training failed) it's the plain optimised build.
    pgo = False
    if not os.environ.get("WONDERPARK_NO_PGO"):
        try:
            run_stage(COMPILE_STAGE, [PY, "-u", "tools/pgo_train.py", "--mingw"],
                      "Training for the optimised build failed.")
            pgo = PGO_PROFILE.is_file()
        except StageError as e:
            emit(f"{e} Optimising without it.")
        shutil.rmtree(REPO / "build" / "game-pgogen", ignore_errors=True)  # its 1+ GB, no longer needed
    try:
        run_stage(COMPILE_STAGE, [PY, "-u", "tools/build_game.py", *OPT_FLAGS,
                                  *(["--pgo-use", str(PGO_PROFILE)] if pgo else ["--no-pgo"])],
                  "Optimising failed. The log has the details.")
    except StageError as e:
        emit(f"@error {e}")
        return 1
    exe = find_exe(OPT)
    if not exe:
        emit("@error The optimised build is missing Wonderpark.exe.")
        return 1
    OPT_STAMP.write_text(json.dumps({"sources": sources, "built": time.strftime("%Y-%m-%d %H:%M:%S"), "pgo": pgo},
                                    indent=2))
    emit(f"Optimised in {(time.time() - t0) / 60:.1f} min{' (profile-guided)' if pgo else ''}")
    emit(f"@done {exe}")
    return 0


def preflight() -> None:
    """What the build needs on this PC, checked before anything starts."""
    # Nothing to install: the release zip ships DolRecomp, SDL3, Ninja and
    # Python ready-built (tools/make_tools.py); the compiler comes in stage 2.
    # A source checkout without them builds them with CMake instead.
    tools = REPO / "tools"
    ready = (tools / "bin" / "dolrecomp.exe").is_file() and (tools / "prebuilt" / "sdl3-mingw" / "libSDL3.a").is_file()
    if not ready and not shutil.which("cmake"):
        raise StageError("Some of Wonderpark's files are missing from the tools folder. Download the full release again.")
    if not ready and not (tools / "dolrecomp" / "CMakeLists.txt").is_file():
        raise StageError("The DolRecomp recompiler is missing from tools/dolrecomp. Download the full release again.")


def free_space_check(need: int) -> None:
    free = shutil.disk_usage(REPO).free
    if free < need:
        raise StageError(f"The drive with the Wonderpark folder needs about {need / 1e9:.1f} GB free "
                         f"for the build; it has {free / 1e9:.1f} GB.")


def run_stage(i: int, cmd: list[str], fail: str, phases: dict[str, int] | None = None) -> None:
    """Runs one stage's script; its @progress lines become overall progress.
    phases maps a script's "@phase <name>" lines to stages (extract_disc.py
    does stages 1 and 2 in one run)."""
    stage = i
    emit(f"@stage {stage + 1} {STAGES[stage]}")
    emit(f"@progress {overall(stage, 0)} {STAGES[stage]}")
    global _child
    if _cancelled.is_set():
        raise StageError("cancelled")
    env = dict(os.environ, PYTHONUNBUFFERED="1")
    proc = _child = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     encoding="utf-8", errors="replace", env=env,
                                     creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
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
        if line.startswith("@size "):
            emit(line)
            continue
        m = re.match(r"@progress (\d+)", line)
        if m and stage == TOOLS_STAGE:
            emit("@detail " + line[m.end():].strip())  # "Compiler: 97 of 191 MB", for the build screen
        if m:
            pct = overall(stage, int(m.group(1)))
            if pct != last:
                last = pct
                emit(f"@progress {pct} {STAGES[stage]}")
            continue
        print(line, flush=True)  # the log
        tail = (tail + [line])[-40:]
    if proc.wait():
        if _cancelled.is_set():
            raise StageError("cancelled")
        err = next((l for l in reversed(tail) if l.lower().startswith(("error", "fatal"))), "")
        raise StageError(fail(err) if callable(fail) else fail)
    emit(f"@progress {overall(stage, 100)} {STAGES[stage]}")


def done_stage(i: int) -> None:
    emit(f"@stage {i + 1} {STAGES[i]}")
    emit(f"@progress {overall(i, 100)} {STAGES[i]}")


# The speed test (the same run as "Testing this PC's speed": Wonderpark.exe
# prints "@perf cpu_ms=.. gpu_ms=.." for 5 s of the title screen at the
# game's own speed, at 2x the Wii's resolution on every PC; gx_frames.c
# perf_test_add). It runs the quick first build; the player then plays the
# profile-guided optimised one. These factors turn the title's figures into
# the park's in that build. Measured 2026-11-21 (16 threads, RX 9060 XT;
# docs/research.md "Speed test"): title 16.0 ms CPU a frame (two cores
# 17.6), 1.65 ms GPU; the Wish Park hub, game + GX per frame, 12.9 ms at 30
# fps and 10.5 at 60 (two cores 14.9 and 13.2), GPU 6.2 ms at 2x and 8.5 at
# 3x. The two-core ratios (the larger) are used.
PARK_CPU_FACTOR_30 = 0.85
PARK_CPU_FACTOR_60 = 0.75
PARK_GPU_FACTOR = 3.8


def gpu_scale(res: int) -> float:
    """The GPU's time at res x the Wii's resolution, over its time at 2x:
    between the measured (8.5 / 6.2 = 1.37 for 3x: the fixed cost counts)
    and the pixel count (2.25), as a weak GPU's time follows its pixels more."""
    return 0.5 + 0.125 * res * res
PERF = REPO / "build" / "perf-test.json"
# The graphics presets (F1 > Graphics, Part 5 of the plan): the frame rate
# each aims for and its internal resolution (times the Wii's).
PRESETS = [("Ultra", 60, 4), ("High", 60, 3), ("Medium", 60, 2), ("Low", 30, 1)]


def suggest_preset(cpu_ms: float, gpu_ms: float) -> tuple[str, dict]:
    """The best preset this PC keeps up with, with a fifth spare: the game
    thread's time per frame in the park within the frame time, and the GPU's
    (scaled from 2x by the preset's pixel count) too. Potato when even Low
    doesn't."""
    est_gpu = gpu_ms * PARK_GPU_FACTOR
    est = {"park_cpu_ms_30": round(cpu_ms * PARK_CPU_FACTOR_30, 2), "park_cpu_ms_60": round(cpu_ms * PARK_CPU_FACTOR_60, 2),
           "park_gpu_ms_2x": round(est_gpu, 2)}
    for name, fps, res in PRESETS:
        budget = 0.8 * 1000 / fps
        est_cpu = cpu_ms * (PARK_CPU_FACTOR_60 if fps >= 60 else PARK_CPU_FACTOR_30)
        if est_cpu <= budget and est_gpu * gpu_scale(res) <= budget:
            return name, est
    return "Potato", est


def speed_test(output: str) -> None:
    m = re.search(r"@perf cpu_ms=([\d.]+) gpu_ms=(-?[\d.]+) frames=(\d+)", output)
    if not m or float(m.group(1)) <= 0 or int(m.group(3)) < 30:
        emit("Speed test: not enough frames to judge; no preset suggested")
        return
    cpu_ms, gpu_ms = float(m.group(1)), max(0.0, float(m.group(2)))
    name, est = suggest_preset(cpu_ms, gpu_ms)
    PERF.write_text(json.dumps({"title_cpu_ms": cpu_ms, "title_gpu_ms": gpu_ms, **est, "suggested": name,
                                "tested": time.strftime("%Y-%m-%d %H:%M:%S")}, indent=2))
    emit(f"Speed test: {cpu_ms:.2f} ms CPU, {gpu_ms:.2f} ms GPU per frame at the title -> {name}")
    emit(f"@suggest {name}")


def prepare_shaders(exe: Path) -> None:
    """The end of stage 4, "Preparing shaders…": runs the new build's boot
    hidden for a few seconds (Wonderpark.exe --prepare-shaders), so the
    graphics driver compiles and caches its shaders now instead of pausing
    the player's first start. Not needed for the game to work: a failure is
    only noted in the log."""
    emit(f"@progress {overall(COMPILE_STAGE, 100)} Testing this PC's speed…")
    t = time.time()
    try:
        p = subprocess.run([str(exe), "--prepare-shaders"], cwd=REPO, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=120)
        print(p.stdout + p.stderr, flush=True)  # the log
        speed_test(p.stdout + p.stderr)
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
        return "The disc reader (dtk) is missing. Press Build again to download it."
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
    ap.add_argument("--log", help="where this build's output goes (the launcher names it; kept in build-running.json)")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    if a.status:
        return status()
    if a.optimise:
        return optimise()
    if not a.image and not a.rebuild:
        ap.error("give a disc image, or --rebuild")
    if running_build():
        emit("@error A build is already running.")
        return 1
    t0 = time.time()
    RUNNING.parent.mkdir(parents=True, exist_ok=True)
    CANCEL.unlink(missing_ok=True)
    RUNNING.write_text(json.dumps({"pid": os.getpid(), "log": a.log or "",
                                   "started": time.strftime("%Y-%m-%d %H:%M:%S")}))
    threading.Thread(target=watch_cancel, daemon=True).start()
    try:
        return build(a, t0)
    finally:
        _cancelled.set()  # ends the watcher
        RUNNING.unlink(missing_ok=True)
        CANCEL.unlink(missing_ok=True)


def build(a, t0: float) -> int:
    try:
        emit(f"Wonderpark build, {time.strftime('%Y-%m-%d %H:%M:%S')}, Python {sys.version.split()[0]}")
        preflight()
        image = a.image.resolve() if a.image else None
        if image and not image.is_file():
            raise StageError("The disc image wasn't found. Place it again.")
        extracted = image is None or already_extracted(image)
        if extracted:
            if not (GAME / "sys" / "main.dol").is_file():
                raise StageError("There's no extracted disc to rebuild from. Place your disc image again.")
            emit("The disc is already extracted in game/: skipping the disc stages")
            done_stage(0)
            if image is None:
                image = Path(json.loads((GAME / "manifest.json").read_text()).get("source_image", ""))
        else:
            # About 2.2 GB in all: the tools, the game's files, the recompiled code and the build.
            free_space_check(int(2.4e9))
            run_stage(0, [PY, "-u", "tools/extract_disc.py", str(image), "--verify-only", "--progress",
                          "--hashes", str(HASHES)], disc_failure)

        run_stage(TOOLS_STAGE, [PY, "-u", "tools/fetch_tools.py"],
                  lambda err: (err.split(":", 1)[1].strip() if err.lower().startswith("error:") else
                               "Getting the build tools failed. The log has the details."))

        if extracted:
            done_stage(2)
        else:
            dtk = ["--dtk", a.dtk] if a.dtk else []
            run_stage(2, [PY, "-u", "tools/extract_disc.py", str(image), "--force", "--progress",
                          "--hashes", str(HASHES), *dtk], disc_failure, phases={"verify": 2, "extract": 2})
            man_path = GAME / "manifest.json"
            man = json.loads(man_path.read_text())
            man["image_size"] = image.stat().st_size
            man_path.write_text(json.dumps(man, indent=2))

        # The optimised build is of the code being replaced: not used again
        # until --optimise makes it anew.
        if OPT_STAMP.is_file():
            OPT_STAMP.unlink()
        run_stage(3, [PY, "-u", "tools/recomp.py"], "Recompiling the game's code failed. The log has the details.")
        run_stage(COMPILE_STAGE, [PY, "-u", "tools/build_game.py", *QUICK_FLAGS],
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
        if _cancelled.is_set():
            emit("Build stopped")
            emit("@cancelled")
            return 2
        emit(f"@error {e}")
        return 1
    except Exception as e:  # anything unexpected: still a message for the player
        import traceback
        traceback.print_exc(file=sys.stdout)
        emit(f"@error Something went wrong in the builder ({type(e).__name__}). The log has the details.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
