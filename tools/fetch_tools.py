#!/usr/bin/env python3
"""Get the build tools a player's PC is missing, once, into the Wonderpark
folder's tools/ (never anywhere else, no install, no admin, no PATH):

  llvm-mingw   the C compiler (Clang, with its own Windows headers and
               libraries: no Visual Studio), from github.com/mstorsjo/llvm-mingw
  dtk          decomp-toolkit, which reads .iso/.wbfs/.rvz disc images, from
               github.com/encounter/decomp-toolkit

Usage:
    python tools/fetch_tools.py [--status] [--from FILE ...]

Each download comes from the tool's official release page and is checked
against the SHA-256 pinned below; a file that doesn't match is deleted, never
used. Downloads resume where they stopped after a lost connection.
--from takes the same files downloaded on another PC (the launcher's "no
internet" help lists the links), checked the same way.

Progress lines for the launcher: @progress <0-100> <message>, and
@size <bytes still to download> before anything starts.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import ssl
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / "tools"
DOWNLOADS = TOOLS / "downloads"


class Tool:
    def __init__(self, name: str, url: str, sha256: str, size: int, done: Path):
        self.name, self.url, self.sha256, self.size, self.done = name, url, sha256, size, done
        self.file = url.rsplit("/", 1)[1]


LLVM_MINGW = "llvm-mingw-20261117-ucrt-x86_64"
TOOLSET = [
    Tool("the compiler (llvm-mingw)",
         f"https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/{LLVM_MINGW}.zip",
         "4d002e330a0f703625aa5ccbc37e96f8f824413e163bfc7336774aabd2c3a412", 190_000_000,
         TOOLS / LLVM_MINGW / "bin" / "x86_64-w64-mingw32-clang.exe"),
    Tool("the disc reader (dtk)",
         "https://github.com/encounter/decomp-toolkit/releases/download/v1.9.1/dtk-windows-x86_64.exe",
         "1782c628bbd13dc4ecd831ab33d96d5c818e7331942a89a7d344bfd148c78f6a", 8_700_000,
         TOOLS / "bin" / "dtk.exe"),
]
# llvm-mingw also targets 32-bit and ARM Windows: only the 64-bit PC parts are kept.
# (Also the debugger, lldb, and the Python it brings.)
LLVM_SKIP = ("i686-w64-mingw32/", "armv7-w64-mingw32/", "aarch64-w64-mingw32/", "arm64ec-w64-mingw32/", "python/")
LLVM_SKIP_BIN = ("i686-", "armv7-", "aarch64-", "arm64ec-", "lldb", "liblldb")


class FetchError(Exception):
    pass


def progress(pct: int, msg: str) -> None:
    print(f"@progress {pct} {msg}", flush=True)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(t: Tool, lo: int, hi: int) -> Path:
    """Into tools/downloads/, resuming a part-file; checked against the pin."""
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    dst, part = DOWNLOADS / t.file, DOWNLOADS / (t.file + ".part")
    if dst.is_file() and sha256(dst) == t.sha256:
        return dst
    ctx = ssl.create_default_context()
    tries = 0
    while True:
        have = part.stat().st_size if part.is_file() else 0
        req = urllib.request.Request(t.url, headers={"User-Agent": "Wonderpark-launcher"})
        if have:
            req.add_header("Range", f"bytes={have}-")
        try:
            with urllib.request.urlopen(req, timeout=60, context=ctx) as r:
                if have and r.status != 206:
                    have = 0  # the server sent the whole file again
                total = have + int(r.headers.get("Content-Length") or 0) or t.size
                with part.open("ab" if have else "wb") as f:
                    got, last = have, 0.0
                    while chunk := r.read(1 << 16):
                        f.write(chunk)
                        got += len(chunk)
                        if time.monotonic() - last > 0.5:
                            last = time.monotonic()
                            progress(lo + (hi - lo) * got // max(total, 1),
                                     f"Getting {t.name}: {got / 1e6:.0f} of {total / 1e6:.0f} MB")
            break
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
            tries += 1
            if tries > 8:
                raise FetchError(f"couldn't download {t.file}: {e}. Check the connection and press Build again; "
                                 "it carries on where it stopped.")
            time.sleep(min(30, 2 ** tries))
    if sha256(part) != t.sha256:
        part.unlink()
        raise FetchError(f"{t.file} didn't match its checksum, so it was deleted. Press Build to try again.")
    os.replace(part, dst)
    return dst


def install(t: Tool, src: Path) -> None:
    if t.file.endswith(".exe"):
        t.done.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, t.done)
        return
    # The compiler: unzipped beside a temporary name, then renamed, so a
    # half-unzipped folder never counts as installed.
    tmp = TOOLS / (LLVM_MINGW + ".unzipping")
    if tmp.exists():
        shutil.rmtree(tmp)
    with zipfile.ZipFile(src) as z:
        for m in z.infolist():
            name = m.filename.split("/", 1)[1] if "/" in m.filename else ""
            if not name or name.startswith(LLVM_SKIP) or (name.startswith("bin/") and name[4:].startswith(LLVM_SKIP_BIN)):
                continue
            if ".." in Path(name).parts:
                raise FetchError("unexpected path in the compiler's zip")
            out = tmp / name
            if m.is_dir():
                out.mkdir(parents=True, exist_ok=True)
                continue
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(m) as i, out.open("wb") as o:
                shutil.copyfileobj(i, o, 1 << 20)
    final = TOOLS / LLVM_MINGW
    if final.exists():
        shutil.rmtree(final)
    os.replace(tmp, final)


def missing() -> list[Tool]:
    return [t for t in TOOLSET if not t.done.is_file()]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--status", action="store_true", help="say what's missing and its download size")
    ap.add_argument("--from", dest="files", nargs="*", default=[], help="tool files downloaded on another PC")
    a = ap.parse_args()
    try:
        for f in a.files:
            p = Path(f)
            digest = sha256(p)
            t = next((t for t in TOOLSET if t.sha256 == digest), None)
            if not t:
                raise FetchError(f"{p.name} isn't one of the files Wonderpark needs (its checksum doesn't match)")
            DOWNLOADS.mkdir(parents=True, exist_ok=True)
            if p.resolve() != (DOWNLOADS / t.file).resolve():
                shutil.copyfile(p, DOWNLOADS / t.file)
            print(f"ok: {t.name}", flush=True)
        todo = missing()
        need = sum(t.size for t in todo if not (DOWNLOADS / t.file).is_file())
        print(f"@size {need}", flush=True)
        if a.status:
            for t in todo:
                print(f"missing: {t.name} ({t.size / 1e6:.0f} MB) {t.url}")
            return 0
        for i, t in enumerate(todo):
            lo, hi = 100 * i // len(todo), 100 * (i + 1) // len(todo)
            src = download(t, lo, lo + (hi - lo) * 4 // 5)
            progress(lo + (hi - lo) * 4 // 5, f"Unpacking {t.name}")
            install(t, src)
            (DOWNLOADS / t.file).unlink(missing_ok=True)
        progress(100, "Build tools ready")
        return 0
    except FetchError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
