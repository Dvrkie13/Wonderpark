#!/usr/bin/env python3
"""Makes a release zip (docs/RELEASING.md): the launcher, built here, and
what a player's PC needs to build the game from their own disc, as source.
Nothing from the game.

    python tools/make_release.py [--out build/release] [--no-launcher-build]

Only committed files go in (the working tree must be clean), taken from
git's own list, submodules included (DolRecomp, SDL3). The folder and the
zip are both run through tools/check_release.py; the zip's SHA-256 is
written beside it. The version is runtime/host/version.h's
WONDERPARK_VERSION; the zip is Wonderpark-<version>.zip.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# What goes in, by the committed paths' first part (RELEASING.md): the
# source the build needs, our assets and the papers. Not: docs/ (notes for
# developers), .github/, CLAUDE.md.
TOP_DIRS = {"runtime", "tools", "builder", "config", "assets"}
TOP_FILES = {"CMakeLists.txt", "README.md", "LICENSE", "THIRD_PARTY_NOTICES.md", "CONTRIBUTING.md", ".gitmodules"}


def git(*args: str, cwd: Path = REPO) -> str:
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, check=True).stdout


def version() -> str:
    m = re.search(r'#define WONDERPARK_VERSION "([^"]+)"', (REPO / "runtime/host/version.h").read_text(encoding="utf-8"))
    if not m:
        sys.exit("error: no WONDERPARK_VERSION in runtime/host/version.h")
    return m.group(1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=str(REPO / "build" / "release"))
    ap.add_argument("--no-launcher-build", action="store_true", help="use build/launcher's exe as it is")
    a = ap.parse_args()

    if git("status", "--porcelain", "--ignore-submodules=none").strip():
        sys.exit("error: commit everything first (a release is made only from committed files)")
    ver = version()
    name = f"Wonderpark-{ver}"
    out = Path(a.out)
    folder = out / name
    zpath = out / f"{name}.zip"

    if not a.no_launcher_build:
        if subprocess.run([sys.executable, str(REPO / "tools" / "build_launcher.py")]).returncode:
            sys.exit("error: the launcher didn't build")
    exe = REPO / "build" / "launcher" / "WonderparkLauncher.exe"
    if not exe.is_file():
        sys.exit("error: build/launcher/WonderparkLauncher.exe is missing")

    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    files = [f for f in git("ls-files", "--recurse-submodules", "-z").split("\0") if f]
    taken = 0
    for f in files:
        top = f.split("/", 1)[0]
        if not (top in TOP_DIRS and "/" in f) and f not in TOP_FILES:
            continue
        src = REPO / f
        if not src.is_file():
            continue
        dst = folder / f
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        taken += 1
    shutil.copy2(exe, folder / "WonderparkLauncher.exe")
    commit = git("rev-parse", "HEAD").strip()
    (folder / "RELEASE.txt").write_text(
        f"Wonderpark {ver}\r\nMade from commit {commit} of https://github.com/Dvrkie13/Wonderpark\r\n"
        "Start WonderparkLauncher.exe. README.md says what to install first.\r\n"
        "Never share your built Wonderpark folder: once built, it contains the game.\r\n",
        encoding="utf-8")
    print(f"{taken} files + the launcher in {folder}")

    check = [sys.executable, str(REPO / "tools" / "check_release.py")]
    if subprocess.run([*check, str(folder)]).returncode:
        sys.exit("error: the release folder isn't clean (above)")

    if zpath.exists():
        zpath.unlink()
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(folder.rglob("*")):
            if p.is_file():
                z.write(p, f"{name}/{p.relative_to(folder).as_posix()}")
    if subprocess.run([*check, str(zpath)]).returncode:
        sys.exit("error: the release zip isn't clean (above)")
    sha = hashlib.sha256(zpath.read_bytes()).hexdigest()
    (out / f"{name}.zip.sha256.txt").write_text(f"{sha}  {name}.zip\n", encoding="utf-8")
    print(f"{zpath} ({zpath.stat().st_size / 1e6:.1f} MB)\nSHA-256 {sha}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
