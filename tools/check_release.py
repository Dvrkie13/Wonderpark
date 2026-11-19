"""Checks a release folder (or .zip) for anything from the game. Fails on any.

    python tools/check_release.py <folder or .zip>

A Wonderpark release may hold only the launcher, our runtime and tools
(source), our own assets and their licenses (docs/RELEASING.md). Never the
recompiled game, the built game program, or anything from the disc. This
looks at every file's name, folder and first bytes:

- disc images, the game's executable and modules, and game file formats (by
  extension and by their magic numbers: Wii discs, U8 archives, Yaz0,
  textures, layouts, fonts, message files, CRI sound, movies);
- recompiled code (generated C from DolRecomp: the S2LE01 chunks, or any file
  that includes DOLRECOMP_CPU_HEADER), the git-ignored game/, generated/ and
  build/game/ folders, and the built game program (Wonderpark.exe, .pdb);
- saves (NAND title folders), settings and logs from someone's PC.

Exit status 0: clean. 1: something was found (each listed).
"""
import os
import sys
import zipfile

BAD_EXT = {
    ".iso", ".gcm", ".rvz", ".wia", ".wbfs", ".ciso", ".gcz", ".wad", ".nkit",
    ".dol", ".rel", ".elf", ".map",
    ".dac", ".arc", ".szs", ".u8", ".brlyt", ".brlan", ".brres", ".tpl", ".bti", ".brfnt", ".msbt", ".msbp",
    ".acb", ".awb", ".acf", ".brstm", ".mo", ".thp", ".rlb",
    ".sav", ".log", ".bmp", ".dmp", ".pdb",
}
BAD_NAMES = {"wonderpark.exe", "main.dol", "bi2.bin", "boot.bin", "fst.bin", "apploader.img", "settings.ini",
             "bindings.ini", "dolrecomp.log"}
BAD_DIRS = {"game", "generated", "nand", "s2le01_generated", "backups", "voices",
            "hd", "hd-pack"}  # hd: the HD Pack (upscaled game art) and its downloaded upscaler
MAGICS = [
    (0x18, b"\x5d\x1c\x9e\xa3", "a Wii disc image"),
    (0x1C, b"\xc2\x33\x9f\x3d", "a GameCube disc image"),
    (0, b"WBFS", "a WBFS disc image"),
    (0, b"RVZ\x01", "an RVZ disc image"),
    (0, b"WIA\x01", "a WIA disc image"),
    (0, b"\x55\xaa\x38\x2d", "a U8 archive"),
    (0, b"Yaz0", "a Yaz0-compressed file"),
    (0, b"\x00\x20\xaf\x30", "a TPL texture"),
    (0, b"RLYT", "a layout"),
    (0, b"RFNT", "a game font"),
    (0, b"MsgStdBn", "a message (text) file"),
    (0, b"@UTF", "a CRI sound bank"),
    (0, b"AFS2", "a CRI sound archive"),
    (0, b"RSTM", "a sound stream"),
    (0, b"MOC5", "a Mobiclip movie"),
]


def check_bytes(name, head, text_sample):
    found = []
    for off, magic, what in MAGICS:
        if head[off:off + len(magic)] == magic:
            found.append(what)
    # The generated code includes the CPU header by this macro (split here so
    # this file doesn't match itself), and its chunks are named by address.
    if b"#include " + b"DOLRECOMP_CPU_HEADER" in text_sample or b"_text1_80" in name.encode("utf-8", "replace").lower():
        found.append("recompiled game code")
    return found


def check_name(path):
    parts = [p.lower() for p in path.replace("\\", "/").split("/") if p]
    name = parts[-1] if parts else ""
    found = []
    if os.path.splitext(name)[1] in BAD_EXT:
        found.append("a game, save, dump or log file type (%s)" % os.path.splitext(name)[1])
    if name in BAD_NAMES:
        found.append("a file that's never shipped (%s)" % name)
    if any(p in BAD_DIRS for p in parts[:-1]):
        found.append("inside a game, generated, save or HD Pack folder")
    if "build" in parts[:-1] and "game" in parts[:-1]:
        found.append("inside build/game (the built game)")
    if name.startswith("s2le01") and name.endswith((".c", ".h", ".cpp")):
        found.append("recompiled game code")
    return found


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    target = sys.argv[1]
    problems = []
    if zipfile.is_zipfile(target):
        with zipfile.ZipFile(target) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                with z.open(info) as f:
                    data = f.read(1 << 20)
                why = check_name(info.filename) + check_bytes(info.filename, data[:64], data)
                if why:
                    problems.append((info.filename, why))
    else:
        for root, dirs, files in os.walk(target):
            for fn in files:
                path = os.path.join(root, fn)
                rel = os.path.relpath(path, target)
                try:
                    with open(path, "rb") as f:
                        data = f.read(1 << 20)
                except OSError as e:
                    problems.append((rel, ["can't be read: %s" % e]))
                    continue
                why = check_name(rel) + check_bytes(rel, data[:64], data)
                if why:
                    problems.append((rel, why))
    for rel, why in problems:
        print("NOT ALLOWED: %s: %s" % (rel, "; ".join(why)))
    if problems:
        print("%d file(s) must not be in a release (docs/RELEASING.md)." % len(problems))
        sys.exit(1)
    print("Clean: nothing from the game found.")


if __name__ == "__main__":
    main()
