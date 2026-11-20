#!/usr/bin/env python3
"""Make the ready-built tools that go in a release zip, so a player's PC needs
nothing installed (tools/make_release.py takes them from here):

  tools/python/                 the official embeddable Python (python.org)
  tools/bin/ninja.exe           Ninja (github.com/ninja-build/ninja)
  tools/bin/dolrecomp.exe       DolRecomp from the tools/dolrecomp submodule,
                                built self-contained with llvm-mingw
  tools/prebuilt/sdl3-mingw/    SDL3 from the tools/sdl3 submodule, built with
                                llvm-mingw (libSDL3.a + its revision header)

Python and Ninja are downloaded from their official pages and checked against
the SHA-256 pinned here. All four are git-ignored. Needs llvm-mingw
(tools/fetch_tools.py gets it) and CMake.

    python tools/make_tools.py [--force]
"""
from __future__ import annotations

import argparse
import hashlib
import io
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import build_game  # noqa: E402

PYTHON = ("https://www.python.org/ftp/python/3.15.2/python-3.15.2-embed-amd64.zip",
          "d012f3aa29cf07436dae8aea1fec0fd195c2612a0b059b1f915ae0afb9ad1f62")
NINJA = ("https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
         "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65")


def fetch(url: str, sha: str) -> bytes:
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "Wonderpark-tools"}),
                                timeout=120) as r:
        data = r.read()
    if hashlib.sha256(data).hexdigest() != sha:
        sys.exit(f"error: {url} didn't match its pinned SHA-256")
    return data


def run(cmd: list[str], env: dict[str, str]) -> None:
    p = subprocess.run(cmd, cwd=REPO, env=env, capture_output=True, text=True)
    if p.returncode:
        sys.exit(f"error: {Path(cmd[0]).name} failed:\n{(p.stdout + p.stderr)[-3000:]}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--force", action="store_true", help="make them again")
    a = ap.parse_args()
    bindir = REPO / "tools" / "bin"
    bindir.mkdir(parents=True, exist_ok=True)

    py = REPO / "tools" / "python"
    if a.force or not (py / "python.exe").is_file():
        print("Python (embeddable)", flush=True)
        if py.exists():
            shutil.rmtree(py)
        zipfile.ZipFile(io.BytesIO(fetch(*PYTHON))).extractall(py)

    if a.force or not (bindir / "ninja.exe").is_file():
        print("Ninja", flush=True)
        with zipfile.ZipFile(io.BytesIO(fetch(*NINJA))) as z:
            (bindir / "ninja.exe").write_bytes(z.read("ninja.exe"))

    mingw = build_game.find_llvm_mingw()
    if not mingw:
        sys.exit("error: no llvm-mingw (python tools/fetch_tools.py)")
    mingw = Path(mingw.as_posix())
    env = dict(build_game.os.environ)
    env["PATH"] = str(mingw) + build_game.os.pathsep + env.get("PATH", "")
    ninja = str(bindir / "ninja.exe")
    cc = [f"-DCMAKE_C_COMPILER={(mingw / 'x86_64-w64-mingw32-clang.exe').as_posix()}",
          f"-DCMAKE_CXX_COMPILER={(mingw / 'x86_64-w64-mingw32-clang++.exe').as_posix()}",
          f"-DCMAKE_RC_COMPILER={(mingw / 'x86_64-w64-mingw32-windres.exe').as_posix()}",
          f"-DCMAKE_AR={(mingw / 'llvm-ar.exe').as_posix()}", f"-DCMAKE_MAKE_PROGRAM={Path(ninja).as_posix()}"]

    dr = bindir / "dolrecomp.exe"
    if a.force or not dr.is_file():
        print("DolRecomp (llvm-mingw, self-contained)", flush=True)
        work = REPO / "build" / "dolrecomp-mingw"
        run(["cmake", "-S", str(REPO / "tools" / "dolrecomp"), "-B", str(work), "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXE_LINKER_FLAGS=-static", *cc], env)
        run(["cmake", "--build", str(work), "--target", "dolrecomp"], env)
        shutil.copyfile(work / "dolrecomp.exe", dr)

    sdl = build_game.SDL_PREBUILT
    if a.force or not (sdl / "libSDL3.a").is_file():
        print("SDL3 (llvm-mingw)", flush=True)
        if a.force:
            shutil.rmtree(REPO / "build" / "sdl3-mingw", ignore_errors=True)
        lib, rev = build_game.sdl3_mingw(mingw, env, ninja)
        if lib.parent != sdl:
            sdl.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(lib, sdl / "libSDL3.a")
            shutil.copytree(rev, sdl / "include-revision", dirs_exist_ok=True)

    # The licenses of what's built into dolrecomp.exe from the compiler's
    # runtime (mingw-w64, winpthreads, LLVM's libc++), and Ninja's (Apache
    # 2.0, whose text is the first part of LLVM's LICENSE.TXT).
    lic = bindir / "licenses"
    lic.mkdir(exist_ok=True)
    share = mingw.parent / "x86_64-w64-mingw32" / "share" / "mingw32"
    for src, name in ((share / "COPYING.MinGW-w64-runtime.txt", "mingw-w64-runtime.txt"),
                      (share / "COPYING.winpthreads.txt", "winpthreads.txt"),
                      (mingw.parent / "LICENSE.TXT", "LLVM-and-Apache-2.0.txt")):
        if not src.is_file():
            sys.exit(f"error: {src} is missing from llvm-mingw")
        shutil.copyfile(src, lic / name)
    (lic / "README.txt").write_text(
        "dolrecomp.exe: DolRecomp (GPL-3.0-or-later, source in tools/dolrecomp), built with llvm-mingw;\r\n"
        "  it contains parts of the mingw-w64 runtime and winpthreads (mingw-w64-runtime.txt,\r\n"
        "  winpthreads.txt) and LLVM's libc++ (LLVM-and-Apache-2.0.txt).\r\n"
        "ninja.exe: Ninja (https://github.com/ninja-build/ninja), Apache License 2.0\r\n"
        "  (the license text is the first part of LLVM-and-Apache-2.0.txt).\r\n"
        "tools/python: the official Python embeddable package; its own LICENSE.txt is beside python.exe.\r\n"
        "tools/prebuilt/sdl3-mingw: SDL3 (zlib license, tools/sdl3/LICENSE.txt).\r\n", encoding="utf-8")

    # Nothing here may need a DLL a clean Windows doesn't have.
    objdump = mingw / "llvm-objdump.exe"
    for exe in (dr, bindir / "ninja.exe"):
        out = subprocess.run([str(objdump), "-p", str(exe)], capture_output=True, text=True).stdout
        dlls = {l.split(":", 1)[1].strip().lower() for l in out.splitlines() if "DLL Name" in l}
        odd = {d for d in dlls if not d.startswith("api-ms-win-") and d not in
               {"kernel32.dll", "advapi32.dll", "user32.dll", "shell32.dll", "ole32.dll", "ws2_32.dll",
                "bcrypt.dll", "ntdll.dll", "shlwapi.dll", "oleaut32.dll", "userenv.dll"}}
        if odd:
            sys.exit(f"error: {exe.name} needs {', '.join(sorted(odd))}, which a clean PC may not have")
    print("ready: tools/python, tools/bin/ninja.exe, tools/bin/dolrecomp.exe, tools/prebuilt/sdl3-mingw")
    return 0


if __name__ == "__main__":
    sys.exit(main())
