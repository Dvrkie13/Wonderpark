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


def find_llvm_mingw() -> Path | None:
    """llvm-mingw's bin folder: $WONDERPARK_LLVM_MINGW, the one the launcher
    fetched into tools/, or D:/Tools/llvm-mingw-*."""
    cands = [os.environ.get("WONDERPARK_LLVM_MINGW", "")]
    cands += sorted(glob.glob(str(REPO / "tools" / "llvm-mingw*")), reverse=True)
    cands += sorted(glob.glob(r"D:\Tools\llvm-mingw-*"), reverse=True)
    for c in cands:
        if c and (Path(c) / "bin" / "x86_64-w64-mingw32-clang.exe").is_file():
            return Path(c) / "bin"
    return None


def find_ninja(env: dict[str, str]) -> str | None:
    bundled = REPO / "tools" / "bin" / "ninja.exe"
    if bundled.is_file():
        return str(bundled)
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


# SDL3 with only the parts the game uses (as in CMakeLists.txt).
SDL_OPTS = ["-DSDL_STATIC=ON", "-DSDL_SHARED=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF",
            "-DSDL_EXAMPLES=OFF", "-DSDL_INSTALL=OFF",
            *(f"-DSDL_{x}=OFF" for x in ("AUDIO", "VIDEO", "GPU", "RENDER", "CAMERA", "POWER", "DIALOG", "TRAY")),
            *(f"-DSDL_{x}=ON" for x in ("JOYSTICK", "HAPTIC", "HIDAPI", "SENSOR"))]
# Ready-built SDL3 for llvm-mingw, shipped in the release zip (tools/make_release.py)
# so players need no CMake: libSDL3.a and include-revision/SDL3/SDL_revision.h.
SDL_PREBUILT = REPO / "tools" / "prebuilt" / "sdl3-mingw"


def sdl3_mingw(bin_dir: Path, env: dict[str, str], ninja: str) -> tuple[Path, Path]:
    """(libSDL3.a, its include-revision folder): the ready-built one, else
    built once with CMake into build/sdl3-mingw (developers)."""
    for d in (SDL_PREBUILT, REPO / "build" / "sdl3-mingw"):
        if (d / "libSDL3.a").is_file() and (d / "include-revision").is_dir():
            return d / "libSDL3.a", d / "include-revision"
    out = REPO / "build" / "sdl3-mingw"
    if not shutil.which("cmake", path=env.get("PATH")):
        raise BuildError("no ready-built SDL3 in tools/prebuilt/sdl3-mingw, and no CMake to build it")
    progress(1, "Building SDL3")
    work = REPO / "build" / "sdl3-mingw-work"
    cfg = ["cmake", "-S", str(REPO / "tools" / "sdl3"), "-B", str(work), "-G", "Ninja",
           "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}",
           f"-DCMAKE_C_COMPILER={(bin_dir / 'x86_64-w64-mingw32-clang.exe').as_posix()}",
           f"-DCMAKE_CXX_COMPILER={(bin_dir / 'x86_64-w64-mingw32-clang++.exe').as_posix()}",
           f"-DCMAKE_RC_COMPILER={(bin_dir / 'x86_64-w64-mingw32-windres.exe').as_posix()}",
           f"-DCMAKE_AR={(bin_dir / 'llvm-ar.exe').as_posix()}", *SDL_OPTS]
    for cmd in (cfg, ["cmake", "--build", str(work)]):
        p = subprocess.run(cmd, cwd=REPO, env=env, capture_output=True, text=True)
        if p.returncode:
            raise BuildError("building SDL3 failed:\n" + (p.stdout + p.stderr)[-3000:])
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(work / "libSDL3.a", out / "libSDL3.a")
    shutil.copytree(work / "include-revision", out / "include-revision", dirs_exist_ok=True)
    return out / "libSDL3.a", out / "include-revision"


def write_ninja(a, build: Path, bin_dir: Path, flags: list[str], link_flags: list[str],
                sdl_lib: Path, sdl_rev: Path) -> Path:
    """build/<dir>/build.ninja for the llvm-mingw build, without CMake. Paths
    are relative to the repo (ninja runs there), so spaces and drive colons in
    the Wonderpark folder's own path never reach a build line."""
    gen = REPO / "generated" / "S2LE01_generated"
    if not (gen / "S2LE01.h").is_file():
        raise BuildError("no recompiled code in generated/: run tools/recomp.py")

    def rel(p: Path) -> str:
        return os.path.relpath(p, REPO).replace("\\", "/")

    def q(s: str) -> str:
        return f'"{s}"' if " " in s else s

    b = rel(build)
    tool = lambda n: q((bin_dir / n).as_posix())
    gen_r, dr = rel(gen), "tools/dolrecomp/src"
    common = " ".join(q(f) for f in flags)
    gen_flags = f"-std=gnu11 -w -I{gen_r} -Iruntime/core -I{dr} -DDOLRECOMP_CPU_HEADER=\\\"wp_cpu_fast.h\\\" -DWPF_FAST_FP"
    if a.chunk_opt:
        # The quick first build (and the PGO training build): the recompiled
        # chunks at -O1 (after the -O2 above, so it wins). Was missing here
        # until 2026-11-20, so players' quick builds compiled at -O2.
        gen_flags += f" -O{a.chunk_opt}"
    if a.watch:
        gen_flags += " -DWPF_WATCH"
    if a.debug_info:
        gen_flags += " -g -gcodeview"
    inc = f"-Iruntime/core -Iruntime/gpu -Iruntime/host -I{gen_r} -I{dr} -I{q(rel(sdl_rev))} -Itools/sdl3/include"
    defs = "-DNDEBUG -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_CRT_SECURE_NO_WARNINGS"
    # Debug info for the runtime as CodeView, in a PDB beside the exe: the
    # crash reports and the host profiler read it with dbghelp, as with clang-cl.
    rt = f"-g -gcodeview {inc} {defs}"
    libs = ("-lopengl32 -lgdi32 -luser32 -lwinmm -lshell32 -lole32 -ldwmapi -ld2d1 -ldwrite -lwindowscodecs "
            "-ldbghelp -ld3d11 -ldxgi -ldxguid -lsetupapi -lpdh -lpsapi -limm32 -loleaut32 -lversion -luuid -ladvapi32")
    L = [f"builddir = {b}",
         f"rule cc\n  command = {tool('x86_64-w64-mingw32-clang.exe')} {common} $extra -MD -MF $out.d -c $in -o $out\n"
         "  depfile = $out.d\n  deps = gcc",
         f"rule cxx\n  command = {tool('x86_64-w64-mingw32-clang++.exe')} {common} -std=gnu++17 $extra -MD -MF $out.d -c $in -o $out\n"
         "  depfile = $out.d\n  deps = gcc",
         f"rule rc\n  command = {tool('x86_64-w64-mingw32-windres.exe')} {inc} {defs} -O coff $in -o $out",
         f"rule ar\n  command = {tool('llvm-ar.exe')} rcs $out $in",
         f"rule link\n  command = {tool('x86_64-w64-mingw32-clang++.exe')} {common} {' '.join(q(f) for f in link_flags)} "
         f"-static -g -Wl,--pdb=$pdb $in -o $out {libs}"]

    def objs(sources: list[Path], extra: str, sub: str) -> list[str]:
        out = []
        for s in sources:
            o = f"{b}/obj/{sub}/{rel(s).replace('/', '_')}.o"
            rule = "cxx" if s.suffix == ".cpp" else "cc"
            L.append(f"build {o}: {rule} {rel(s)}\n  extra = {extra}")
            out.append(o)
        return out

    chunks = sorted((gen / "chunks").glob("*.c"))
    gen_objs = objs(chunks, gen_flags, "gen")
    cpu_obj = objs([REPO / dr / "cpu" / "cpu.c"], f"-std=gnu11 -I{dr} -D_CRT_SECURE_NO_WARNINGS", "cpu")
    rt_src = [p for d in ("core", "hw", "hle", "ios", "gpu") for p in sorted((REPO / "runtime" / d).glob("*.c"))]
    rt_src += sorted((REPO / "runtime" / "host").glob("*.c")) + sorted((REPO / "runtime" / "host").glob("*.cpp"))
    rt_src += sorted((REPO / "config" / "patches").glob("*.c")) + [gen / "wp_hooks.c"]
    rt_objs = objs([p for p in rt_src if p.suffix == ".c"], f"-std=gnu11 {rt}", "rt")
    rt_objs += objs([p for p in rt_src if p.suffix == ".cpp"], rt, "rt")
    res = []
    for rc in ("wonderpark.rc", "pad_icons.rc"):
        o = f"{b}/obj/{rc}.res"
        L.append(f"build {o}: rc runtime/host/{rc}")
        res.append(o)
    L.append(f"build {b}/libwp_generated.a: ar {' '.join(gen_objs)}")
    L.append(f"build {b}/Wonderpark.exe: link {' '.join(rt_objs + res)} {b}/libwp_generated.a "
             f"{q(rel(sdl_lib))} {' '.join(cpu_obj)}\n  pdb = {b}/Wonderpark.pdb")
    L.append(f"default {b}/Wonderpark.exe")
    path = build / "build.ninja"
    text = "\n".join(L) + "\n"
    if not path.is_file() or path.read_text() != text:
        path.write_text(text)
    return path


def build_mingw(a, build: Path, bin_dir: Path) -> int:
    """The build players get: llvm-mingw's clang for the MinGW target, which
    brings its own Windows headers and libraries (no Visual Studio)."""
    env = dict(os.environ)
    env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
    ninja = find_ninja(env)
    if not ninja:
        raise BuildError("ninja.exe not found (tools/bin/ninja.exe)")
    bin_dir = Path(bin_dir.as_posix())
    cc, cxx = (bin_dir / "x86_64-w64-mingw32-clang.exe").as_posix(), (bin_dir / "x86_64-w64-mingw32-clang++.exe").as_posix()
    print(f"compiler: llvm-mingw ({bin_dir.parent})", flush=True)
    # -O2 as clang-cl's /O2; -fms-extensions for the MSVC-style bits the
    # runtime shares with the clang-cl build.
    flags = ["-O2", "-fms-extensions", "-Wno-microsoft", "-Wno-pragma-pack"] + a.cflags.split()
    link_flags: list[str] = []
    if a.pgo_generate:
        flags += ["-fprofile-instr-generate", "-DWP_PGO_GENERATE"]
        link_flags += ["-fprofile-instr-generate"]
    if a.pgo_use:
        prof = Path(a.pgo_use).resolve()
        if not prof.is_file():
            raise BuildError(f"--pgo-use: no profile at {prof}")
        print(f"PGO: using {prof}", flush=True)
        flags += [f"-fprofile-instr-use={prof}", "-Wno-profile-instr-out-of-date",
                  "-Wno-profile-instr-unprofiled", "-Wno-backend-plugin"]
    if not a.no_lto:
        flags += ["-flto=thin"]
        link_flags += ["-flto=thin", "-Wl,--thinlto-cache-dir=" + str(build / "lto-cache")]
        print("ThinLTO: on", flush=True)
    if a.clean and build.exists():
        shutil.rmtree(build)
    marker = build / "wonderpark_toolchain.txt"
    want = "\n".join(["mingw", str(bin_dir), *flags, *link_flags]) + "\n"
    if build.exists() and (not marker.is_file() or marker.read_text() != want):
        print("compiler settings changed: building from scratch", flush=True)
        shutil.rmtree(build)
    progress(0, "Configuring")
    build.mkdir(parents=True, exist_ok=True)
    sdl_lib, sdl_rev = sdl3_mingw(bin_dir, env, ninja)
    ninja_file = write_ninja(a, build, bin_dir, flags, link_flags, sdl_lib, sdl_rev)
    marker.write_text(want)
    run_build([ninja, "-f", ninja_file.relative_to(REPO).as_posix()], env, 2, 99)
    exe = build / "Wonderpark.exe"
    if not exe.is_file():
        raise BuildError("build finished but Wonderpark.exe is missing")
    progress(100, f"Built {exe}")
    print(f"@exe {exe}", flush=True)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--msvc", action="store_true", help="build with MSVC instead of clang-cl")
    ap.add_argument("--mingw", action="store_true",
                    help="build with llvm-mingw (no Visual Studio needed; the default when it isn't installed)")
    ap.add_argument("--debug-info", action="store_true", help="PDBs for the recompiled code too (for profiling)")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--build-dir", default="game", help="folder under build/ (default game)")
    ap.add_argument("--cflags", default="", help="extra compiler flags, e.g. /arch:AVX2 (clang-cl)")
    ap.add_argument("--no-lto", action="store_true", help="no ThinLTO (faster to build, the game a little slower)")
    ap.add_argument("--chunk-opt", choices=["1", "2"], help="clang optimisation level for the recompiled chunks")
    ap.add_argument("--pgo-generate", action="store_true",
                    help="an instrumented build that writes a profile when it exits (LLVM_PROFILE_FILE)")
    ap.add_argument("--pgo-use", metavar="PROFDATA",
                    help="optimise with a merged profile (default: build/pgo/wonderpark.profdata when it exists)")
    ap.add_argument("--no-pgo", action="store_true", help="don't use a profile even if there is one")
    ap.add_argument("--watch", action="store_true",
                    help="a debugging build: loads and stores in WONDERPARK_WATCH=lo-hi are reported (slower)")
    a = ap.parse_args()
    build = REPO / "build" / a.build_dir
    default_prof = REPO / "build" / "pgo" / "wonderpark.profdata"
    if not a.pgo_use and not a.no_pgo and not a.pgo_generate and default_prof.is_file():
        # Made on this PC from this PC's recompiled code (tools/pgo_train.py):
        # game-derived, so never in git.
        a.pgo_use = str(default_prof)
    try:
        mingw = find_llvm_mingw() if not a.msvc else None
        if a.mingw or (mingw and not a.msvc and not VSWHERE.is_file()):
            if not mingw:
                raise BuildError("llvm-mingw not found (set WONDERPARK_LLVM_MINGW to its folder)")
            return build_mingw(a, build, mingw)
        env = vs_env()
        clang = None if a.msvc else find_clang_cl(env)
        ninja = find_ninja(env)
        if a.clean and build.exists():
            shutil.rmtree(build)
        progress(0, "Configuring")
        opts = [f"-DWP_DEBUG_INFO_GENERATED={'ON' if a.debug_info else 'OFF'}",
                f"-DWP_STORE_WATCH={'ON' if a.watch else 'OFF'}",
                f"-DWP_CHUNK_OPT={a.chunk_opt or ''}"]
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
