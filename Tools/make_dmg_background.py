#!/usr/bin/env python3
"""Generate the macOS disk-image background.

The window a Mac user opens is the whole install experience, so it says what to
do rather than leaving them to guess: the app on the left, the Applications
folder on the right, an arrow between them, and the one first-launch step
underneath. Same §9.2 palette as the app and the icon.

Run:  python3 Tools/make_dmg_background.py
Out:  Resources/DmgBackground.png  (660x420 @1x)
      Resources/DmgBackground@2x.png
"""

import os
from PIL import Image, ImageDraw, ImageFont

BONE = (237, 228, 211)
SECONDARY = (140, 129, 119)
TERTIARY = (94, 85, 77)
BACKDROP_TOP = (34, 27, 24)
BACKDROP_BOTTOM = (18, 14, 12)

W, H = 660, 520
SS = 2


def load_font(size, bold=False):
    candidates = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold
        else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf" if bold
        else "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    ]
    for path in candidates:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def centred(d, y, text, font, fill):
    left, top, right, bottom = d.textbbox((0, 0), text, font=font)
    d.text(((W * SS - (right - left)) / 2 - left, y), text, font=font, fill=fill)


def main():
    img = Image.new("RGB", (W * SS, H * SS))
    px = img.load()
    for y in range(H * SS):
        t = y / (H * SS - 1)
        row = tuple(round(a + (b - a) * t) for a, b in zip(BACKDROP_TOP, BACKDROP_BOTTOM))
        for x in range(W * SS):
            px[x, y] = row

    d = ImageDraw.Draw(img)

    centred(d, 40 * SS, "Multi-Mic Aggregator", load_font(25 * SS, bold=True), BONE)
    centred(d, 78 * SS, "Drag the skull onto Applications", load_font(14 * SS), SECONDARY)

    # Arrow between the two icon positions the AppleScript sets (165 and 495).
    y = 205 * SS
    x0, x1 = 268 * SS, 392 * SS
    d.line([(x0, y), (x1 - 16 * SS, y)], fill=TERTIARY, width=3 * SS)
    d.polygon([(x1, y), (x1 - 19 * SS, y - 11 * SS), (x1 - 19 * SS, y + 11 * SS)],
              fill=TERTIARY)

    # The one step that is not obvious, said here rather than only in a README
    # nobody opens.
    centred(d, 322 * SS, "First launch: Control-click the app, then choose Open.",
            load_font(13 * SS), SECONDARY)
    centred(d, 348 * SS, "macOS asks once because this build has no paid Apple certificate.",
            load_font(11 * SS), TERTIARY)
    centred(d, 370 * SS, "If it says the app is damaged, see Troubleshooting in the README.",
            load_font(11 * SS), TERTIARY)

    out_dir = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Resources")
    os.makedirs(out_dir, exist_ok=True)

    img.resize((W, H), Image.LANCZOS).save(os.path.join(out_dir, "DmgBackground.png"))
    img.save(os.path.join(out_dir, "DmgBackground@2x.png"))
    print(f"wrote {out_dir}/DmgBackground.png and @2x")


if __name__ == "__main__":
    main()
