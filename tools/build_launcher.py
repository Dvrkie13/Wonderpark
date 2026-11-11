#!/usr/bin/env python3
"""Build the Wonderpark launcher (builder/launcher) with the Visual Studio
Build Tools: build/launcher/WonderparkLauncher.exe.

Usage:
    python tools/build_launcher.py [--clean]

The launcher holds no game code or data (only our own code and the
Fredoka and Nunito fonts, SIL OFL), so the executable can be shipped in a
release. It finds the Wonderpark folder by looking upwards from where it
is (or --root / WONDERPARK_ROOT).
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import build_game  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clean", action="store_true")
    a = ap.parse_args()
    out = REPO / "build" / "launcher"
    if a.clean and out.exists():
        shutil.rmtree(out)
    try:
        env = build_game.vs_env()
    except build_game.BuildError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    ninja = build_game.find_ninja(env)
    gen = ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"] if ninja else []
    cfg = ["cmake", "-S", str(REPO / "builder" / "launcher"), "-B", str(out), *gen,
           "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=cl", "-DCMAKE_RC_COMPILER=rc"]
    if subprocess.run(cfg, env=env).returncode:
        return 1
    if subprocess.run(["cmake", "--build", str(out), "--config", "Release"], env=env).returncode:
        return 1
    exe = next((p for p in (out / "WonderparkLauncher.exe", out / "Release" / "WonderparkLauncher.exe") if p.is_file()), None)
    if not exe:
        print("error: WonderparkLauncher.exe is missing after the build", file=sys.stderr)
        return 1
    print(f"Built {exe}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
