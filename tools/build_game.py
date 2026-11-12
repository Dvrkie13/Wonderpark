#!/usr/bin/env python3
"""Compile Wonderpark.exe from the recompiled code (generated/) and the runtime.

Usage:
    python tools/build_game.py [--msvc] [--no-lto] [--debug-info] [--clean]

Uses clang-cl when one is found (the recompiled code runs about twice as fast
built with clang as with MSVC), with Ninja and the Visual Studio Build Tools'
headers, libraries and linker. Where clang-cl comes from, in order:
  $WONDERPARK_CLANG_CL, Visual Studio's own (the "C++ Clang tools" component),
  clang-cl on PATH, or an llvm-mingw install (D:/Tools/llvm-mingw-*), whose
  clang-23.exe needs a clang-cl.exe hard link beside it.
--msvc builds with MSVC instead. The executable is build/game/Wonderpark.exe.

With a profile at build/pgo/wonderpark.profdata (made on this PC by
tools/pgo_train.py; game-derived, never in git), the build is also
profile-guided: the game code takes about 35% less time. --no-pgo skips it.

With clang-cl the whole program is optimised together (ThinLTO: the game
runs 3-5% faster; the build takes ~18 min instead of ~13), when
lld-link.exe and llvm-lib.exe are beside clang-cl; else without, with a
warning. llvm-mingw has no lld-link.exe: a hard link to its ld.lld.exe
named lld-link.exe works. --no-lto builds without.

Progress lines for the launcher: @progress <0-100> <message>
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VSWHERE = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"


class BuildError(Exception):
    pass


def progress(pct: int, msg: str) -> None:
    print(f"@progress {pct} {msg}", flush=True)


def vs_env() -> dict[str, str]:
    if not VSWHERE.is_file():
        raise BuildError("Visual Studio Build Tools not found (install them with the C++ workload)")
    path = subprocess.run([str(VSWHERE), "-latest", "-products", "*", "-requires",
                           "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                          capture_output=True, text=True).stdout.strip()
    vcvars = Path(path) / "VC/Auxiliary/Build/vcvars64.bat"
    if not vcvars.is_file():
        raise BuildError("Visual Studio has no C++ tools (vcvars64.bat missing)")
    out = subprocess.run(f'cmd /c ""{vcvars}" >nul 2>&1 && set"', capture_output=True, text=True, shell=True).stdout
    env = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    env["WP_VS_PATH"] = path
    return env


def find_clang_cl(env: dict[str, str]) -> tuple[str, list[str]] | None:
    """Returns (path, extra flags) or None."""
    cand = os.environ.get("WONDERPARK_CLANG_CL")
    if cand and Path(cand).is_file():
        return cand, []
    vs = Path(env.get("WP_VS_PATH", "")) / "VC/Tools/Llvm/x64/bin/clang-cl.exe"
    if vs.is_file():
        return str(vs), []
    onpath = shutil.which("clang-cl", path=env.get("PATH") or env.get("Path"))
    if onpath and "llvm-mingw" not in onpath.lower():
        return onpath, []
    for d in sorted(glob.glob(r"D:\Tools\llvm-mingw-*\bin"), reverse=True):
        exe = Path(d) / "clang-cl.exe"
        if exe.is_file():
            # llvm-mingw's clang defaults to the MinGW target and its config files.
            return str(exe), ["--no-default-config", "--target=x86_64-pc-windows-msvc"]
    return None


def find_ninja(env: dict[str, str]) -> str | None:
    n = shutil.which("ninja", path=env.get("PATH") or env.get("Path"))
    if n:
        return n
    vs = Path(env.get("WP_VS_PATH", "")) / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
    return str(vs) if vs.is_file() else None


def run_build(cmd: list[str], env: dict[str, str], lo: int, hi: int) -> None:
    proc = subprocess.Popen(cmd, cwd=REPO, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    tail: list[str] = []
    for line in proc.stdout:
        tail = (tail + [line])[-60:]
        m = re.match(r"\[(\d+)/(\d+)\]", line)
        if m:
            progress(lo + (hi - lo) * int(m.group(1)) // max(1, int(m.group(2))), "Compiling Wonderpark.exe")
    if proc.wait():
        raise BuildError("build failed:\n" + "".join(l for l in tail if "warning" not in l.lower())[-4000:])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--msvc", action="store_true", help="build with MSVC instead of clang-cl")
    ap.add_argument("--debug-info", action="store_true", help="PDBs for the recompiled code too (for profiling)")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--build-dir", default="game", help="folder under build/ (default game)")
    ap.add_argument("--cflags", default="", help="extra compiler flags, e.g. /arch:AVX2 (clang-cl)")
    ap.add_argument("--no-lto", action="store_true", help="no ThinLTO (faster to build, the game a little slower)")
    ap.add_argument("--pgo-generate", action="store_true",
                    help="an instrumented build that writes a profile when it exits (LLVM_PROFILE_FILE)")
    ap.add_argument("--pgo-use", metavar="PROFDATA",
                    help="optimise with a merged profile (default: build/pgo/wonderpark.profdata when it exists)")
    ap.add_argument("--no-pgo", action="store_true", help="don't use a profile even if there is one")
    a = ap.parse_args()
    build = REPO / "build" / a.build_dir
    default_prof = REPO / "build" / "pgo" / "wonderpark.profdata"
    if not a.pgo_use and not a.no_pgo and not a.pgo_generate and default_prof.is_file():
        # Made on this PC from this PC's recompiled code (tools/pgo_train.py):
        # game-derived, so never in git.
        a.pgo_use = str(default_prof)
    try:
        env = vs_env()
        clang = None if a.msvc else find_clang_cl(env)
        ninja = find_ninja(env)
        if a.clean and build.exists():
            shutil.rmtree(build)
        progress(0, "Configuring")
        opts = [f"-DWP_DEBUG_INFO_GENERATED={'ON' if a.debug_info else 'OFF'}"]
        if clang and ninja:
            path, extra = clang
            print(f"compiler: clang-cl ({path})", flush=True)
            cache = build / "CMakeCache.txt"
            if cache.is_file() and "CMAKE_GENERATOR:INTERNAL=Ninja" not in cache.read_text(errors="replace"):
                shutil.rmtree(build)  # switching generators needs a fresh build folder
            flags = extra + a.cflags.split()
            linker = ["-DCMAKE_LINKER=link.exe"]
            link_flags = []
            if a.pgo_generate:
                # Profile-guided optimisation, step 1. The profile runtime
                # comes from the clang install (llvm-mingw has it only as a
                # MinGW-style .a, which lld-link and link.exe read too).
                rt = sorted(glob.glob(str(Path(path).parent.parent / "lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib"))
                            + glob.glob(str(Path(path).parent.parent / "lib/clang/*/lib/windows/libclang_rt.profile-x86_64.a")))
                if not rt:
                    raise BuildError("--pgo-generate: no clang_rt.profile-x86_64 library in the clang install")
                flags += ["-fprofile-instr-generate", "-DWP_PGO_GENERATE"]
                # The compiler asks the linker for clang_rt.profile.lib by name.
                libdir = REPO / "build" / "pgo-lib"
                libdir.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(rt[-1], libdir / "clang_rt.profile.lib")
                # Built for MinGW, it also needs printf-family functions (in
                # legacy_stdio_definitions.lib with the UCRT) and MinGW's
                # stack probe, which does what __chkstk does on x64.
                link_flags += [f"/LIBPATH:{libdir}", "legacy_stdio_definitions.lib",
                               "/alternatename:___chkstk_ms=__chkstk"]
            if a.pgo_use:
                prof = Path(a.pgo_use).resolve()
                if not prof.is_file():
                    raise BuildError(f"--pgo-use: no profile at {prof}")
                print(f"PGO: using {prof}", flush=True)
                flags += [f"-fprofile-instr-use={prof}", "-Wno-profile-instr-out-of-date",
                          "-Wno-profile-instr-unprofiled", "-Wno-backend-plugin"]
            if not a.no_lto:
                tools = Path(path).parent
                lld, lib = tools / "lld-link.exe", tools / "llvm-lib.exe"
                if lld.is_file() and lib.is_file():
                    flags += ["-flto=thin", "-fuse-ld=lld"]
                    linker = [f"-DCMAKE_LINKER={lld}", f"-DCMAKE_AR={lib}"]
                    print("ThinLTO: on (lld-link)", flush=True)
                else:
                    print(f"warning: no lld-link.exe/llvm-lib.exe beside {path}: building without ThinLTO "
                          "(the game runs 3-5% slower)", flush=True)
            cfg = ["cmake", "-S", str(REPO), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                   f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DCMAKE_C_COMPILER={path}",
                   f"-DCMAKE_C_FLAGS_INIT={' '.join(flags)}",
                   # The F1 settings tab (settings_ui.cpp) is C++: the same compiler and flags.
                   f"-DCMAKE_CXX_COMPILER={path}", f"-DCMAKE_CXX_FLAGS_INIT={' '.join(flags)}",
                   *linker, *opts]
            if link_flags:
                cfg.append(f"-DCMAKE_EXE_LINKER_FLAGS_INIT={' '.join(link_flags)}")
            # CMake takes the flags and linker only when a build folder is
            # first configured: when they change, start the folder again.
            marker = build / "wonderpark_toolchain.txt"
            want = "\n".join([path, *flags, *linker, *link_flags]) + "\n"
            if marker.is_file() and marker.read_text() != want or not marker.is_file() and cache.is_file():
                print("compiler settings changed: building from scratch", flush=True)
                shutil.rmtree(build)
            build_cmd = ["cmake", "--build", str(build)]
        else:
            if not a.msvc:
                print("warning: no clang-cl found; building with MSVC (the game will run about half as fast)", flush=True)
            cache = build / "CMakeCache.txt"
            if cache.is_file() and "CMAKE_GENERATOR:INTERNAL=Ninja" in cache.read_text(errors="replace"):
                shutil.rmtree(build)
            cfg = ["cmake", "-S", str(REPO), "-B", str(build), *opts]
            build_cmd = ["cmake", "--build", str(build), "--config", "Release", "-j"]
        p = subprocess.run(cfg, cwd=REPO, env=env, capture_output=True, text=True)
        if p.returncode:
            raise BuildError("cmake configure failed:\n" + (p.stdout + p.stderr)[-3000:])
        if clang and ninja:
            marker.write_text(want)
        run_build(build_cmd, env, 2, 99)
        exe = next((x for x in (build / "Wonderpark.exe", build / "Release" / "Wonderpark.exe") if x.is_file()), None)
        if not exe:
            raise BuildError("build finished but Wonderpark.exe is missing")
        progress(100, f"Built {exe}")
        print(f"@exe {exe}", flush=True)
        return 0
    except BuildError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
