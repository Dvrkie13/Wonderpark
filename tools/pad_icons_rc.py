"""Writes runtime/host/pad_icons.rc: the controller icons
(assets/ui/pad-icons/*.png) as resources of the game and the launcher, named
PADICON_<NAME>_<DPI> (xb_a.300dpi.png -> PADICON_XB_A_300). Run it again
after adding or removing icons. The pack's *_super icons (platform logos)
are never listed, even if present.

    python tools/pad_icons_rc.py
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ICONS = REPO / "assets" / "ui" / "pad-icons"
OUT = REPO / "runtime" / "host" / "pad_icons.rc"


def main():
    lines = [
        "// Controller icons (pad_icons.cpp; the launcher too): Flat Gamepad Icons by",
        "// RobTheFiveNine, CC BY-SA 4.0 (assets/ui/pad-icons/LICENSE.txt), and our",
        "// home icon in the same style. Written by tools/pad_icons_rc.py.",
    ]
    for p in sorted(ICONS.glob("*.png")):
        m = re.fullmatch(r"([a-z0-9_]+)\.(\d+)dpi\.png", p.name)
        if not m or m.group(1).endswith("_super"):
            continue
        lines.append(f'PADICON_{m.group(1).upper()}_{m.group(2)} RCDATA "../../assets/ui/pad-icons/{p.name}"')
    OUT.write_text("\n".join(lines) + "\n", encoding="ascii")
    print(OUT, len(lines) - 3, "icons")


if __name__ == "__main__":
    main()
