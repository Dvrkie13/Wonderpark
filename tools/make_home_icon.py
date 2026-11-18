"""Draws the controller "home" (guide) button icon, home.<dpi>.png, in the
style of the Flat Gamepad Icons pack (assets/ui/pad-icons/): a rounded
square key like the pack's ps_l2, with a house on it. Like the pack it's CC BY-SA
4.0 (it's derived from the pack's style). We draw our own instead of
shipping the pack's *_super icons, which carry the platforms' logos.

Measures the key's shape and colours from the pack's ps_l2 at each size, so
it matches. Pure Python (zlib only): no imaging library needed.

    python tools/make_home_icon.py [--measure]
"""
import struct
import sys
import zlib
from pathlib import Path

ICONS = Path(__file__).resolve().parent.parent / "assets" / "ui" / "pad-icons"
DPIS = (50, 96, 150, 300)


def read_png(path):
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, lace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and ctype == 6 and lace == 0, (path, depth, ctype, lace)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw, bpp, stride = zlib.decompress(idat), 4, w * 4
    px, prev, at = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        px.append([tuple(line[x * 4:x * 4 + 4]) for x in range(w)])
        prev = line
    return w, h, px


def write_png(path, w, h, px):
    raw = b"".join(b"\x00" + bytes(v for p in row for v in p) for row in px)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def measure():
    """Prints ps_l2's middle column and row at 300 dpi, colour runs."""
    w, h, px = read_png(ICONS / "ps_l2.300dpi.png")
    for name, line in (("column", [px[y][w // 2] for y in range(h)]), ("row", px[h // 2])):
        runs, start = [], 0
        for i in range(1, len(line) + 1):
            if i == len(line) or max(abs(a - b) for a, b in zip(line[i], line[start])) > 6:
                runs.append((start, i - 1, line[start]))
                start = i
        print(name, w, h, [r for r in runs if r[1] - r[0] >= 2])


# The key, in the 264-unit box of the pack's 300 dpi icons (measured from
# ps_l2 with --measure): the grey rim, the face, and its darker bottom.
RIM = (216, 216, 216)
FACE = (56, 65, 98)
FACE_DARK = (44, 51, 78)
GLYPH = (211, 225, 231)


def rounded(x, y, x0, y0, x1, y1, r):
    """Whether (x, y) is inside the rounded rectangle."""
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def in_house(x, y):
    """The house: a roof (triangle with eaves), a body, a door cut out."""
    # roof: apex (132, 62), eaves to (68, 124) and (196, 124), 18 thick band
    if 62 <= y <= 128:
        half = (y - 62) * 64 / 62 + 8
        if abs(x - 132) <= half and not (abs(x - 132) <= half - 26 and y >= 84):
            return True
    # body
    if 92 <= x <= 172 and 118 <= y <= 194:
        if 118 <= x <= 146 and 146 <= y <= 194:  # the door
            return False
        return True
    return False


def colour_at(x, y):
    if not rounded(x, y, 0, 0, 263, 263, 44):
        return None
    if not rounded(x, y, 31, 31, 232, 232, 26):
        return RIM
    if in_house(x, y):
        return GLYPH
    return FACE_DARK if y > 202 else FACE


def draw(size):
    ss = 4
    px = []
    for py in range(size):
        row = []
        for qx in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    u = (qx + (sx + .5) / ss) * 264 / size
                    v = (py + (sy + .5) / ss) * 264 / size
                    c = colour_at(u, v)
                    if c:
                        acc[0] += c[0]
                        acc[1] += c[1]
                        acc[2] += c[2]
                        acc[3] += 1
            n = acc[3]
            row.append((acc[0] // n, acc[1] // n, acc[2] // n, n * 255 // (ss * ss)) if n else (0, 0, 0, 0))
        px.append(row)
    return px


def main():
    if "--measure" in sys.argv:
        measure()
        return
    for dpi in DPIS:
        w, h, _ = read_png(ICONS / f"ps_l2.{dpi}dpi.png")
        out = ICONS / f"home.{dpi}dpi.png"
        write_png(out, w, h, draw(w))
        print(out, w, h)


if __name__ == "__main__":
    main()
