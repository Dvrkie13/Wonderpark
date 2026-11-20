#!/usr/bin/env python3
"""Verify a PokePark 2 (US, S2LE01) disc image and extract it.

Usage:
    python tools/extract_disc.py <disc image> [--out game] [--dtk path/to/dtk.exe]

Output layout (same as `dtk disc extract`):
    <out>/sys/main.dol, boot.bin, bi2.bin, apploader.img, fst.bin
    <out>/files/...            the disc file tree
    <out>/manifest.json        game ID, revision, image hashes, sizes, .rel list

The disc must be S2LE01 and a revision Wonderpark has been tested with
(TESTED_REVISIONS; --allow-untested-revision overrides). The whole image is
hashed and compared with GameTDB's clean dump; a mismatch is only a warning,
since NKit, WBFS and RVZ images hash differently from a plain ISO.

Extraction normally uses decomp-toolkit (dtk). NKit-processed .iso images
(hashes stripped, partition stored decrypted) are not readable by dtk, so for
those a small built-in reader is used instead.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

EXPECTED_ID = b"S2LE01"
# Disc revisions (header byte 7) Wonderpark has been tested with.
TESTED_REVISIONS = {0}
# The clean US v1.00 ISO, per GameTDB.
CLEAN_SHA1 = "e429a232431ea7075ae959b030a7675846357c19"
CLEAN_MD5 = "4b087f066230fa72db05362186532899"
REPO = Path(__file__).resolve().parent.parent

PROGRESS = False  # --progress
WII_MAGIC = 0x5D1C9EA3
NKIT_MAGIC_OFFSET = 0x200
CHUNK = 1 << 20


class DiscError(Exception):
    pass


# --------------------------------------------------------------------------- ID check

def read_header(path: Path) -> tuple[bytes, int, str]:
    """Return (game ID, revision, container kind). The disc header holds the
    6-byte ID at 0 and the revision at 7; WBFS and WIA/RVZ embed it."""
    with path.open("rb") as f:
        head = f.read(0x220)
    if len(head) < 0x20:
        raise DiscError("file is too small to be a disc image")
    if head[:4] == b"WBFS":
        return head[0x200:0x206], head[0x207], "wbfs"
    if head[:4] in (b"RVZ", b"WIA"):
        # The disc header is stored inside the WIA/RVZ header (disc_header at 0x58).
        return head[0x58:0x5E], head[0x5F], "rvz"
    kind = "nkit" if head[NKIT_MAGIC_OFFSET:NKIT_MAGIC_OFFSET + 4] == b"NKIT" else "iso"
    return head[:6], head[7], kind


def progress_line(pct: float, msg: str) -> None:
    """For the launcher (builder/pipeline.py): @progress <0-100> <message>."""
    if PROGRESS:
        print(f"@progress {int(pct)} {msg}", flush=True)


def image_hashes(path: Path) -> tuple[str, str]:
    """SHA-1 and MD5 of the whole image file."""
    sha1, md5 = hashlib.sha1(), hashlib.md5()
    total, done, last = max(1, path.stat().st_size), 0, -1
    with path.open("rb") as f:
        while chunk := f.read(CHUNK * 8):
            sha1.update(chunk)
            md5.update(chunk)
            done += len(chunk)
            if (pct := 100 * done // total) != last:
                last = pct
                progress_line(pct, "Hashing the disc image")
    return sha1.hexdigest(), md5.hexdigest()


def verify(path: Path, allow_untested: bool = False) -> tuple[str, int]:
    """Check the game ID and revision; return (container kind, revision)."""
    gid, rev, kind = read_header(path)
    if gid != EXPECTED_ID:
        shown = gid.decode("ascii", "replace")
        hint = {"S2LJ01": " (that's the Japanese disc)", "S2LP01": " (that's the European disc)"}.get(shown, "")
        raise DiscError(f"wrong disc: game ID is {shown!r}{hint}; Wonderpark needs the US disc {EXPECTED_ID.decode()}")
    if rev not in TESTED_REVISIONS and not allow_untested:
        raise DiscError(f"disc revision {rev} hasn't been tested with Wonderpark (tested: "
                        f"{sorted(TESTED_REVISIONS)}); use --allow-untested-revision to try it anyway")
    return kind, rev


def check_hashes(path: Path, kind: str) -> dict:
    """Hash the image and compare with the clean dump. Returns the manifest entry."""
    sha1, md5 = image_hashes(path)
    clean = sha1 == CLEAN_SHA1 and md5 == CLEAN_MD5
    if clean:
        print("  image hash matches the clean US v1.00 dump (GameTDB)")
    else:
        why = {"nkit": "NKit images", "wbfs": "WBFS images", "rvz": "RVZ/WIA images"}.get(kind)
        print(f"  warning: image hash doesn't match the clean dump"
              + (f" ({why} always hash differently)" if why else " (the disc may be modified or a bad dump)"))
    return {"sha1": sha1, "md5": md5, "matches_clean_dump": clean}


# --------------------------------------------------------------------------- dtk path

def find_dtk(explicit: str | None) -> str | None:
    for cand in (explicit, os.environ.get("WONDERPARK_DTK"),
                 REPO / "tools" / "bin" / "dtk.exe", REPO / "tools" / "bin" / "dtk"):
        if cand and Path(cand).is_file():
            return str(cand)
    return shutil.which("dtk")


# What extracting the US disc writes, in bytes (game/ after extraction), for
# progress while dtk works: dtk shows none when its output isn't a console.
EXTRACTED_BYTES = 916_992_770


def process_bytes_written(handle: int) -> int | None:
    """Bytes a child process has written so far (Windows I/O counters)."""
    if os.name != "nt":
        return None
    import ctypes

    class IoCounters(ctypes.Structure):
        _fields_ = [(n, ctypes.c_ulonglong) for n in ("ReadOps", "WriteOps", "OtherOps", "ReadBytes", "WriteBytes",
                                                      "OtherBytes")]
    c = IoCounters()
    if not ctypes.windll.kernel32.GetProcessIoCounters(ctypes.c_void_p(handle), ctypes.byref(c)):
        return None
    return c.WriteBytes


def extract_with_dtk(dtk: str, image: Path, out: Path) -> None:
    proc = subprocess.Popen([dtk, "disc", "extract", str(image), str(out)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    while True:
        try:
            output, _ = proc.communicate(timeout=1.0)
            break
        except subprocess.TimeoutExpired:
            written = process_bytes_written(int(proc._handle)) if hasattr(proc, "_handle") else None
            if written is not None:
                progress_line(min(99, 100 * written / EXTRACTED_BYTES), "Extracting files")
    if proc.returncode != 0:
        raise DiscError("dtk failed:\n" + (output or "").strip())


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

        # Partition ticket, TMD and certificates, as dtk writes them.
        f.seek(poff)
        phdr = f.read(0x2C0)
        (out / "ticket.bin").write_bytes(phdr[:0x2A4])
        for name, size_off, ptr_off in (("tmd.bin", 0x2A4, 0x2A8), ("cert.bin", 0x2AC, 0x2B0)):
            size, ptr = be32(phdr, size_off), be32(phdr, ptr_off) << 2
            f.seek(poff + ptr)
            (out / name).write_bytes(f.read(size))

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
            if done % 10 == 0 or done == total_files:
                progress_line(100 * done / max(1, total_files), "Extracting files")


# --------------------------------------------------------------------------- manifest

def dol_sections(dol: bytes) -> list[dict]:
    secs = []
    for i in range(18):
        off, addr, size = be32(dol, i * 4), be32(dol, 0x48 + i * 4), be32(dol, 0x90 + i * 4)
        if size:
            secs.append({"name": f"{'text' if i < 7 else 'data'}{i if i < 7 else i - 7}",
                         "offset": off, "address": addr, "size": size})
    return secs


def write_manifest(out: Path, image: Path, kind: str, method: str, rev: int, hashes: dict) -> dict:
    dol = (out / "sys" / "main.dol").read_bytes()
    files = out / "files"
    rels = sorted(str(p.relative_to(out)).replace("\\", "/") for p in out.rglob("*") if p.suffix.lower() in (".rel", ".rso", ".sel"))
    top = sorted(p.name + ("/" if p.is_dir() else "") for p in files.iterdir())
    man = {
        "game_id": EXPECTED_ID.decode(),
        "revision": rev,
        "source_image": image.name,
        "image_hash": hashes,
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
    ap.add_argument("--hashes", type=Path, help="keep (--verify-only) or reuse the image's hashes in this file")
    ap.add_argument("--force", action="store_true", help="overwrite an existing output folder")
    ap.add_argument("--allow-untested-revision", action="store_true", help="accept a disc revision not yet tested")
    ap.add_argument("--progress", action="store_true",
                    help="for the launcher: @phase verify|extract and @progress <0-100> lines")
    a = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)  # keep progress and errors in order
    global PROGRESS
    PROGRESS = a.progress

    try:
        if not a.image.is_file():
            raise DiscError(f"not found: {a.image}")
        if PROGRESS:
            print("@phase verify", flush=True)
        print("[1/2] Verifying disc")
        kind, rev = verify(a.image, a.allow_untested_revision)
        print(f"  game ID {EXPECTED_ID.decode()}, revision {rev} OK ({kind})")
        # --hashes: the image's hashes from a --verify-only run, so the
        # extract run doesn't read the whole image a second time.
        st = a.image.stat()
        key = {"image": str(a.image.resolve()), "size": st.st_size, "mtime": st.st_mtime}
        saved = None
        if a.hashes and not a.verify_only and a.hashes.is_file():
            try:
                j = json.loads(a.hashes.read_text())
                saved = j["hashes"] if j.get("key") == key else None
            except (OSError, ValueError, KeyError):
                saved = None
        hashes = saved if saved is not None else check_hashes(a.image, kind)
        if a.verify_only:
            if a.hashes:
                a.hashes.parent.mkdir(parents=True, exist_ok=True)
                a.hashes.write_text(json.dumps({"key": key, "hashes": hashes}))
            return 0

        if a.out.exists() and any(a.out.iterdir()):
            if not a.force:
                raise DiscError(f"{a.out} is not empty (use --force to replace it)")
            shutil.rmtree(a.out)
        a.out.mkdir(parents=True, exist_ok=True)

        if PROGRESS:
            print("@phase extract", flush=True)
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

        man = write_manifest(a.out, a.image, kind, method, rev, hashes)
        print(f"  main.dol {man['main_dol']['size']:,} bytes, {man['file_count']} files, "
              f"{len(man['modules'])} code modules -> {a.out}")
        return 0
    except DiscError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
