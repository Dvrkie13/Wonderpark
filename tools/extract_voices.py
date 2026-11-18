#!/usr/bin/env python3
"""Japanese voices: copy the Pokémon voice banks out of the player's own
Japanese PokéPark 2 disc (PokéPark 2: Beyond the World, S2LJ01) for the
Voice language setting (Audio > Voice language: Japanese).

Usage:
    python tools/extract_voices.py <Japanese disc image> [--game game] [--dtk path]

The disc is extracted with decomp-toolkit (dtk) into a temporary folder
under build/ (on the same drive as the repo), and only these files are kept,
in <game>/voices/ja/ at the disc's own paths (git-ignored, like game/):

- every Sound/*.acb whose bytes differ from the US disc's AND whose cue
  table (names, indexes, IDs) is the same, so the game finds every cue
  where it expects it, with its _streamfiles.awb if it has one;
- not the others: a bank whose cues differ (the other Pokémon's shared
  voices, S_POKE_VOICE: 1,990 cues in the US, 1,096 in Japan) stays
  English, and so do files that aren't sound banks.

Then the temporary folder is deleted. The runtime serves these files in
place of the US ones from the next start (runtime/ios/dvd.c). Nothing from
the Japanese disc goes in git.
"""
from __future__ import annotations

import argparse
import filecmp
import shutil
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_disc import DiscError, extract_with_dtk, find_dtk, read_header  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
JP_ID = b"S2LJ01"


# --------------------------------------------------------------------------- CRI @UTF tables

def utf_table(b: bytes, off: int = 0):
    """A CRI @UTF table at off: its rows (dicts; data columns as (offset, size))."""
    if b[off:off + 4] != b"@UTF":
        raise ValueError("not a @UTF table")
    base = off + 8
    _, rows_off, str_off, data_off, _name, ncol, row_w, nrows = struct.unpack(">HHIIIHHI", b[base:base + 24])
    sizes = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 8, 7: 8, 8: 4, 9: 8, 0xA: 4, 0xB: 8}
    fmts = {0: ">B", 1: ">b", 2: ">H", 3: ">h", 4: ">I", 5: ">i", 6: ">Q", 7: ">q", 8: ">f", 9: ">d"}

    def s(o):
        a = base + str_off + o
        return b[a:b.index(b"\0", a)].decode("latin-1")

    def val(t, p):
        if t == 0xA:
            return s(struct.unpack(">I", b[p:p + 4])[0])
        if t == 0xB:
            o, n = struct.unpack(">II", b[p:p + 8])
            return (base + data_off + o, n)
        return struct.unpack(fmts[t], b[p:p + sizes[t]])[0]

    p = base + 24
    cols = []
    for _ in range(ncol):
        fl = b[p]
        nm = s(struct.unpack(">I", b[p + 1:p + 5])[0])
        p += 5
        st, t = fl & 0xF0, fl & 0x0F
        const = None
        if st == 0x30:
            const = val(t, p)
            p += sizes[t]
        cols.append((nm, st, t, const))
    rows = []
    for r in range(nrows):
        q = base + rows_off + r * row_w
        row = {}
        for nm, st, t, const in cols:
            if st == 0x50:
                row[nm] = val(t, q)
                q += sizes[t]
            elif st == 0x30:
                row[nm] = const
        rows.append(row)
    return rows


def cue_signature(path: Path):
    """What the game looks a bank's cues up by: names to indexes, and IDs."""
    b = path.read_bytes()
    header = utf_table(b)[0]
    names, ids = {}, []
    for col, want in (("CueNameTable", "names"), ("CueTable", "ids")):
        v = header.get(col)
        if not isinstance(v, tuple) or v[1] < 4 or b[v[0]:v[0] + 4] != b"@UTF":
            continue
        for row in utf_table(b, v[0]):
            if want == "names":
                names[row.get("CueName")] = row.get("CueIndex")
            else:
                ids.append(row.get("CueId"))
    return names, ids


# --------------------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path, help="the Japanese disc image (S2LJ01)")
    ap.add_argument("--game", type=Path, default=REPO / "game", help="the extracted US disc (default game/)")
    ap.add_argument("--dtk", help="path to dtk (else $WONDERPARK_DTK, tools/bin/, PATH)")
    ap.add_argument("--keep-temp", action="store_true", help="keep the extracted Japanese disc (tests)")
    ap.add_argument("--extracted", type=Path, help="an already extracted Japanese disc folder (skips dtk)")
    args = ap.parse_args()

    try:
        game_id, _rev, kind = read_header(args.image)
    except (OSError, DiscError) as e:
        print(f"error: {e}")
        return 1
    if game_id != JP_ID:
        print(f"error: wrong disc: game ID is {game_id.decode('ascii', 'replace')!r}; "
              f"Japanese voices need the Japanese disc S2LJ01")
        return 1
    us_sound = args.game / "files" / "Sound"
    if not us_sound.is_dir():
        print(f"error: no extracted US disc at {args.game} (build the game first)")
        return 1
    dtk = find_dtk(args.dtk) if not args.extracted else "-"
    if not dtk:
        print("error: dtk not found (set WONDERPARK_DTK or put dtk.exe in tools/bin)")
        return 1

    temp = args.extracted or REPO / "build" / "voices-ja-temp"
    if temp.exists() and not args.extracted:
        shutil.rmtree(temp)
    if not args.extracted:
        print(f"@progress 5 Extracting the Japanese disc ({kind})", flush=True)
        try:
            extract_with_dtk(dtk, args.image, temp)
        except DiscError as e:
            print(f"error: {e}")
            return 1
    jp_sound = temp / "files" / "Sound"
    out = args.game / "voices" / "ja"
    if out.exists():
        shutil.rmtree(out)
    (out / "Sound").mkdir(parents=True)

    kept, english, total = [], [], 0
    for jp in sorted(jp_sound.glob("*.acb")):
        us = us_sound / jp.name
        if not us.is_file() or filecmp.cmp(us, jp, shallow=False):
            continue
        try:
            same = cue_signature(us) == cue_signature(jp)
        except (ValueError, IndexError, struct.error):
            same = False
        if not same:
            english.append(jp.name)
            continue
        names = [jp.name]
        awb = jp.with_name(jp.stem + "_streamfiles.awb")
        if awb.is_file():
            names.append(awb.name)
        for n in names:
            shutil.copyfile(jp_sound / n, out / "Sound" / n)
            total += (jp_sound / n).stat().st_size
        kept.append(jp.name)
    (out / "voices.txt").write_text(
        "Japanese sound banks used for Voice language: Japanese (tools/extract_voices.py).\n"
        "Kept (same cues as the US banks):\n" + "".join(f"  {n}\n" for n in kept) +
        "Left English (cues differ):\n" + "".join(f"  {n}\n" for n in english), encoding="utf-8")
    if not args.keep_temp and not args.extracted:
        shutil.rmtree(temp, ignore_errors=True)
    print(f"Japanese voices: {len(kept)} banks ({total / 1048576:.0f} MB) in {out}; "
          f"left English: {', '.join(english) or 'none'}")
    print("@progress 100 Japanese voices ready: choose Japanese in F1 > Audio (from the next start)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
