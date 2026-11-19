"""Builds docs/banner.svg's embedded fonts and docs/banner.png (the social preview).

    python tools/make_banner.py

docs/banner.svg is our own art, drawn in SVG. Its text uses Fredoka (the
wordmark) and Nunito (the lines under it), the launcher's fonts (SIL OFL 1.1,
builder/launcher/fonts/): this script pins each to the weight used, keeps
only the letters the banner shows, and embeds them as base64 WOFF2
@font-face rules between the "fonts" markers, so the banner looks the same
on any machine (GitHub shows SVGs as images: no web fonts load).

Then it renders docs/banner.png at 1280x640 with Microsoft Edge (headless),
the 1280x400 banner centred on the same night sky.

Needs fontTools and brotli (pip install fonttools brotli).
"""
import base64
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG = os.path.join(ROOT, "docs", "banner.svg")
PNG = os.path.join(ROOT, "docs", "banner.png")
FONTS = os.path.join(ROOT, "builder", "launcher", "fonts")


def text_of(svg, family):
    """Every character in the <text> elements set in this family."""
    chars = set()
    for m in re.finditer(r"<text[^>]*>(.*?)</text>", svg, re.S):
        tag = m.group(0)
        ctx = svg[: m.start()]
        # A <text> takes its font-family from itself or the nearest <g> before it.
        fam = re.search(r'font-family="([^"]*)"', tag)
        if not fam:
            g = list(re.finditer(r'<g[^>]*font-family="([^"]*)"', ctx))
            fam = g[-1] if g else None
        if fam and fam.group(1).startswith(family):
            chars |= set(m.group(1))
    return "".join(sorted(chars))


def woff2(path, axes, text):
    font = TTFont(path)
    font = instancer.instantiateVariableFont(font, axes)
    opts = subset.Options()
    opts.flavor = "woff2"
    opts.layout_features = ["kern", "liga"]
    opts.name_IDs = ["*"]  # keep the names, with the copyright and license lines
    sub = subset.Subsetter(opts)
    sub.populate(text=text)
    sub.subset(font)
    out = io.BytesIO()
    font.flavor = "woff2"
    font.save(out)
    return out.getvalue()


def main():
    svg = open(SVG, encoding="utf-8").read()
    rules = []
    fredoka = text_of(svg, "Fredoka")
    nunito = text_of(svg, "Nunito")
    for family, file, weight, text, extra in (
        ("Fredoka", "Fredoka.ttf", 700, fredoka, {"wdth": 100}),
        ("Nunito", "Nunito.ttf", 800, nunito, {}),
        ("Nunito", "Nunito.ttf", 700, nunito, {}),
    ):
        data = woff2(os.path.join(FONTS, file), dict(wght=weight, **extra), text)
        rules.append(
            "@font-face{font-family:'%s';font-weight:%d;src:url(data:font/woff2;base64,%s) format('woff2')}"
            % (family, weight, base64.b64encode(data).decode())
        )
        print(f"{family} {weight}: {len(text)} characters, {len(data)} bytes")
    block = "/* fonts (tools/make_banner.py: Fredoka and Nunito, SIL OFL 1.1, subset) */\n      " + "\n      ".join(rules) + "\n      /* end fonts */"
    if "/* fonts" in svg:
        svg = re.sub(r"/\* fonts.*?/\* end fonts \*/", lambda _: block, svg, flags=re.S)
    else:
        svg = svg.replace("<style>", "<style>\n      " + block, 1)
    open(SVG, "w", encoding="utf-8", newline="\n").write(svg)
    print(f"{SVG}: {os.path.getsize(SVG)} bytes")

    # The social preview: 1280x640, the banner in the middle of the sky.
    edge = None
    for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")):
        if base and os.path.exists(os.path.join(base, r"Microsoft\Edge\Application\msedge.exe")):
            edge = os.path.join(base, r"Microsoft\Edge\Application\msedge.exe")
    if not edge:
        sys.exit("Microsoft Edge wasn't found: docs/banner.png not made")
    tmp = tempfile.mkdtemp(dir=os.path.join(ROOT, "build"))
    page = os.path.join(tmp, "preview.html")
    open(page, "w", encoding="utf-8").write(
        "<!doctype html><html><body style='margin:0;width:1280px;height:640px;overflow:hidden;"
        "background:radial-gradient(ellipse at 30% 50%,#2e4388 0,#16224f 45%,#0a1029 100%)'>"
        "<img src='" + SVG.replace("\\", "/") + "' style='position:absolute;left:0;top:120px;width:1280px;height:400px'>"
        "</body></html>"
    )
    subprocess.run([edge, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--user-data-dir=" + os.path.join(tmp, "profile"),
                    "--window-size=1280,640", "--screenshot=" + PNG, "file:///" + page.replace("\\", "/")],
                   check=True, timeout=120)
    # Edge's first process can return before the screenshot is written.
    for _ in range(120):
        if os.path.exists(PNG) and os.path.getsize(PNG) > 0:
            break
        time.sleep(.5)
    time.sleep(2)
    shutil.rmtree(tmp, ignore_errors=True)  # the page and Edge's profile, made just now
    print(f"{PNG}: {os.path.getsize(PNG)} bytes")


if __name__ == "__main__":
    main()
