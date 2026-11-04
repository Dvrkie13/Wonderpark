#!/usr/bin/env python3
"""Verify a PokePark 2 (US, S2LE01) disc image and extract it.

Usage:
    python tools/extract_disc.py <disc image> [--out game] [--dtk path/to/dtk.exe]

Output layout (same as `dtk disc extract`):
    <out>/sys/main.dol, boot.bin, bi2.bin, apploader.img, fst.bin
    <out>/files/...            the disc file tree
    <out>/manifest.json        game ID, sizes, .rel list (for later pipeline stages)

Extraction normally uses decomp-toolkit (dtk). NKit-processed .iso images
(hashes stripped, partition stored decrypted) are not readable by dtk, so for
those a small built-in reader is used instead.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

EXPECTED_ID = b"S2LE01"
REPO = Path(__file__).resolve().parent.parent

WII_MAGIC = 0x5D1C9EA3
NKIT_MAGIC_OFFSET = 0x200
CHUNK = 1 << 20


class DiscError(Exception):
    pass


# --------------------------------------------------------------------------- ID check

def read_game_id(path: Path) -> tuple[bytes, str]:
    """Return (game ID, container kind). Plain/NKit ISOs carry the ID in the first 6 bytes."""
    with path.open("rb") as f:
        head = f.read(0x220)
    if len(head) < 0x20:
        raise DiscError("file is too small to be a disc image")
    if head[:4] == b"WBFS":
        return head[0x200:0x206], "wbfs"
    if head[:4] in (b"RVZ\x01", b"WIA\x01"):
        # The disc header is stored inside the WIA/RVZ header (disc_header at 0x58).
        return head[0x58:0x5E], "rvz"
    kind = "nkit" if head[NKIT_MAGIC_OFFSET:NKIT_MAGIC_OFFSET + 4] == b"NKIT" else "iso"
    return head[:6], kind


def verify(path: Path) -> str:
    gid, kind = read_game_id(path)
    if gid != EXPECTED_ID:
        shown = gid.decode("ascii", "replace")
        hint = {"S2LJ01": " (that's the Japanese disc)", "S2LP01": " (that's the European disc)"}.get(shown, "")
        raise DiscError(f"wrong disc: game ID is {shown!r}{hint}; Wonderpark needs the US disc {EXPECTED_ID.decode()}")
    return kind


# --------------------------------------------------------------------------- dtk path

def find_dtk(explicit: str | None) -> str | None:
    for cand in (explicit, os.environ.get("WONDERPARK_DTK"),
                 REPO / "tools" / "bin" / "dtk.exe", REPO / "tools" / "bin" / "dtk"):
        if cand and Path(cand).is_file():
            return str(cand)
    return shutil.which("dtk")


def extract_with_dtk(dtk: str, image: Path, out: Path) -> None:
    proc = subprocess.run([dtk, "disc", "extract", str(image), str(out)],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise DiscError("dtk failed:\n" + (proc.stderr or proc.stdout).strip())


# --------------------------------------------------------------------------- NKit reader

def be32(b: bytes, o: int) -> int:
    return struct.unpack_from(">I", b, o)[0]


def extract_nkit(image: Path, out: Path, progress=print) -> None:
    """Extract an NKit Wii ISO whose data partition is stored decrypted with hash blocks removed."""
    with image.open("rb") as f:
        hdr = f.read(0x50000)
        if be32(hdr, 0x18) != WII_MAGIC:
            raise DiscError("NKit image is not a Wii disc")
        # Partition table: first group, first entry.
        f.seek(0x40000)
        count, table = struct.unpack(">II", f.read(8))
        if count == 0:
            raise DiscError("no partitions")
        f.seek(table << 2)
        parts = [struct.unpack(">II", f.read(8)) for _ in range(count)]
        data = [p for p in parts if p[1] == 0]
        if not data:
            raise DiscError("no data partition")
        poff = data[0][0] << 2
        f.seek(poff + 0x2B8)
        data_off = be32(f.read(4), 0) << 2
        base = poff + data_off

        def rd(o: int, n: int) -> bytes:
            f.seek(base + o)
            buf = f.read(n)
            if len(buf) != n:
                raise DiscError(f"read past end of image at partition offset {o:#x}")
            return buf

        boot = rd(0, 0x440)
        if boot[:6] != EXPECTED_ID or be32(boot, 0x18) != WII_MAGIC:
            raise DiscError("partition data is not readable as decrypted, unhashed data "
                            "(unsupported NKit variant; convert it back to a full ISO first)")
        dol_off, fst_off, fst_size = (be32(boot, 0x420) << 2, be32(boot, 0x424) << 2, be32(boot, 0x428) << 2)

        sysd = out / "sys"
        sysd.mkdir(parents=True, exist_ok=True)
        (sysd / "boot.bin").write_bytes(boot)
        (sysd / "bi2.bin").write_bytes(rd(0x440, 0x2000))
        ah = rd(0x2440, 0x20)
        (sysd / "apploader.img").write_bytes(rd(0x2440, 0x20 + be32(ah, 0x14) + be32(ah, 0x18)))

        dh = rd(dol_off, 0x100)
        dol_size = max(be32(dh, i * 4) + be32(dh, 0x90 + i * 4) for i in range(18))
        (sysd / "main.dol").write_bytes(rd(dol_off, dol_size))

        fst = rd(fst_off, fst_size)
        (sysd / "fst.bin").write_bytes(fst)

        n = be32(fst, 8)
        strtab = n * 12

        def name(i: int) -> str:
            s = strtab + (be32(fst, i * 12) & 0xFFFFFF)
            return fst[s:fst.index(b"\0", s)].decode("shift_jis")

        files = out / "files"
        files.mkdir(exist_ok=True)
        stack: list[tuple[int, Path]] = [(n, files)]  # (end index, dir)
        done = 0
        total_files = sum(1 for i in range(1, n) if not (fst[i * 12] & 1))
        for i in range(1, n):
            while i >= stack[-1][0]:
                stack.pop()
            parent = stack[-1][1]
            ent = name(i)
            if fst[i * 12] & 1:
                d = parent / ent
                d.mkdir(exist_ok=True)
                stack.append((be32(fst, i * 12 + 8), d))
                continue
            off, size = be32(fst, i * 12 + 4) << 2, be32(fst, i * 12 + 8)
            f.seek(base + off)
            with (parent / ent).open("wb") as o:
                left = size
                while left:
                    buf = f.read(min(CHUNK, left))
                    if not buf:
                        raise DiscError(f"truncated image while reading {ent}")
                    o.write(buf)
                    left -= len(buf)
            done += 1
            if done % 100 == 0 or done == total_files:
                progress(f"  {done}/{total_files} files")


# --------------------------------------------------------------------------- manifest

def dol_sections(dol: bytes) -> list[dict]:
    secs = []
    for i in range(18):
        off, addr, size = be32(dol, i * 4), be32(dol, 0x48 + i * 4), be32(dol, 0x90 + i * 4)
        if size:
            secs.append({"name": f"{'text' if i < 7 else 'data'}{i if i < 7 else i - 7}",
                         "offset": off, "address": addr, "size": size})
    return secs


def write_manifest(out: Path, image: Path, kind: str, method: str) -> dict:
    dol = (out / "sys" / "main.dol").read_bytes()
    files = out / "files"
    rels = sorted(str(p.relative_to(out)).replace("\\", "/") for p in out.rglob("*") if p.suffix.lower() in (".rel", ".rso", ".sel"))
    top = sorted(p.name + ("/" if p.is_dir() else "") for p in files.iterdir())
    man = {
        "game_id": EXPECTED_ID.decode(),
        "source_image": image.name,
        "image_kind": kind,
        "extracted_with": method,
        "main_dol": {"size": len(dol), "entry": be32(dol, 0xE0),
                     "bss": {"address": be32(dol, 0xD8), "size": be32(dol, 0xDC)},
                     "sections": dol_sections(dol)},
        "modules": rels,
        "top_level": top,
        "file_count": sum(1 for p in files.rglob("*") if p.is_file()),
    }
    (out / "manifest.json").write_text(json.dumps(man, indent=2))
    return man


# --------------------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path)
    ap.add_argument("--out", type=Path, default=REPO / "game")
    ap.add_argument("--dtk", help="path to dtk (else $WONDERPARK_DTK, tools/bin/, PATH)")
    ap.add_argument("--verify-only", action="store_true")
    ap.add_argument("--force", action="store_true", help="overwrite an existing output folder")
    a = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)  # keep progress and errors in order

    try:
        if not a.image.is_file():
            raise DiscError(f"not found: {a.image}")
        print("[1/2] Verifying disc")
        kind = verify(a.image)
        print(f"  game ID {EXPECTED_ID.decode()} OK ({kind})")
        if a.verify_only:
            return 0

        if a.out.exists() and any(a.out.iterdir()):
            if not a.force:
                raise DiscError(f"{a.out} is not empty (use --force to replace it)")
            shutil.rmtree(a.out)
        a.out.mkdir(parents=True, exist_ok=True)

        print("[2/2] Extracting main.dol and files")
        dtk = find_dtk(a.dtk)
        method = None
        if kind != "nkit":
            if not dtk:
                raise DiscError("dtk not found: pass --dtk, set WONDERPARK_DTK, or put dtk.exe in tools/bin/")
            extract_with_dtk(dtk, a.image, a.out)
            method = "dtk"
        else:
            if dtk:
                try:
                    extract_with_dtk(dtk, a.image, a.out)
                    method = "dtk"
                except DiscError:
                    print("  dtk can't read this NKit image; using built-in reader")
                    shutil.rmtree(a.out)
                    a.out.mkdir(parents=True)
            if method is None:
                extract_nkit(a.image, a.out)
                method = "builtin-nkit"

        man = write_manifest(a.out, a.image, kind, method)
        print(f"  main.dol {man['main_dol']['size']:,} bytes, {man['file_count']} files, "
              f"{len(man['modules'])} code modules -> {a.out}")
        return 0
    except DiscError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
