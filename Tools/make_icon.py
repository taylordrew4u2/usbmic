#!/usr/bin/env python3
"""Generate the SobStage application icon from the app's own §9.2 palette.

The mark is a sob face: a ring, two eyes, a downturned mouth, and one tear.

It replaces the skull that was here before. The skull came from the level
meters, which is a good reason for a meter and a poor one for an icon -- at
32px in a dock, among other icons, a skull with a fill line in it read as a
flowerpot, and it said "recorder" to nobody. The sob face says the app's name.
It survives the shrink because it is four shapes with daylight between them,
and it is legible at a glance in a way a silhouette with internal detail is not.

The tear is the accent, and it is the only saturated thing in the mark. That is
deliberate: it is the smallest element, so it needs the most contrast to survive
scaling, and it lands where the eye already goes.

Run:  python3 Tools/make_icon.py
Out:  Resources/AppIcon.png (1024x1024, transparent rounded corners)
"""

import os
from PIL import Image, ImageDraw

# §9.2 palette, from the `palette` namespace in Source/UI/AppLookAndFeel.h.
BONE = (227, 234, 242)          # palette::bone -- the face itself
ACCENT = (34, 211, 238)         # palette::accent -- the tear, and only the tear
BACKDROP_TOP = (28, 39, 51)     # palette::surfaceHigh
BACKDROP_BOTTOM = (10, 14, 19)  # palette::background

SIZE = 1024
SS = 3  # supersample factor: drawn large, resized down, so every curve is clean
W = SIZE * SS

CORNER = 0.2246 * SIZE  # macOS squircle-ish corner radius at 1024


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def backdrop(img):
    """A vertical gradient, so the mark sits on something with depth rather
    than on a flat rectangle that reads as a placeholder at large sizes."""
    d = ImageDraw.Draw(img)
    for y in range(W):
        d.line([(0, y), (W, y)], fill=lerp(BACKDROP_TOP, BACKDROP_BOTTOM, y / W))


def rounded_mask():
    """Transparent corners. Every platform draws its own mask over an icon, but
    a square PNG shows its corners in file listings and in the DMG window."""
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1],
                                           radius=CORNER * SS, fill=255)
    return mask


def draw_face(d):
    cx = cy = W / 2

    # The ring. Sized so the face fills the icon without touching the corners,
    # which is where a platform mask bites hardest.
    r = W * 0.335
    stroke = W * 0.052
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=BONE, width=round(stroke))

    # Eyes. Round, and set wide and high -- close-set eyes read as a scowl, and
    # the expression has to be unhappy rather than angry.
    eye_r = W * 0.042
    eye_dx = W * 0.125
    eye_y = cy - W * 0.075
    for sx in (-1, 1):
        ex = cx + sx * eye_dx
        d.ellipse([ex - eye_r, eye_y - eye_r, ex + eye_r, eye_y + eye_r], fill=BONE)

    # The mouth: an arc of a circle centred *below* the face, so what shows is
    # the top of that circle -- a frown. Drawing the bottom of a circle centred
    # above would give the same curve upside down, which is the classic way this
    # mark comes out smiling.
    mouth_r = W * 0.15
    mouth_cy = cy + W * 0.235
    d.arc([cx - mouth_r, mouth_cy - mouth_r, cx + mouth_r, mouth_cy + mouth_r],
          start=200, end=340, fill=BONE, width=round(W * 0.042))

    # The tear, under the left eye. A circle and a triangle sharing an edge:
    # round at the bottom, pointed at the top, which is the shape a falling drop
    # actually has.
    # Set low enough to clear the eye. At ty = eye_y + 0.135W the drop's point
    # landed inside the eye it falls from, and the two merged into one blob the
    # moment the icon was scaled down.
    tx = cx - eye_dx
    ty = eye_y + W * 0.180
    drop_r = W * 0.043
    d.ellipse([tx - drop_r, ty - drop_r, tx + drop_r, ty + drop_r], fill=ACCENT)
    d.polygon([(tx, ty - drop_r * 2.5), (tx - drop_r, ty), (tx + drop_r, ty)],
              fill=ACCENT)


def main():
    img = Image.new("RGB", (W, W))
    backdrop(img)
    draw_face(ImageDraw.Draw(img))

    out = img.convert("RGBA")
    out.putalpha(rounded_mask())
    out = out.resize((SIZE, SIZE), Image.LANCZOS)

    path = os.path.join(os.path.dirname(__file__), "..", "Resources", "AppIcon.png")
    out.save(os.path.normpath(path))
    print("wrote", os.path.normpath(path))


if __name__ == "__main__":
    main()
