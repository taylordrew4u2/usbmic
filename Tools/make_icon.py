#!/usr/bin/env python3
"""Generate the application icon from the app's own §9.2 palette.

The app's identity is the skull level meter it draws for every microphone, so
the icon is a skull in the same bone white over the same warm near-black.

It is *designed* here rather than traced from
SkullMeterComponent::buildSkullSilhouette. That silhouette is a cranium ellipse
over a tapered trapezoid, and its own comment says real artwork would replace
it; rendered at icon size with no nasal cavity and no teeth it reads as a
flowerpot rather than a skull. The palette is shared, the geometry is not.

Run:  python3 Tools/make_icon.py
Out:  Resources/AppIcon.png (1024x1024, transparent rounded corners)
"""

import os
from PIL import Image, ImageDraw

# §9.2 palette, from Source/UI/SkullMeterComponent.cpp.
BONE = (237, 228, 211)
BONE_SHADE = (198, 187, 170)
SOCKET = (18, 14, 12)
BACKDROP_TOP = (44, 35, 31)
BACKDROP_BOTTOM = (18, 14, 12)

SIZE = 1024
SS = 3
W = SIZE * SS
CORNER = int(W * 0.2237)          # macOS squircle corner radius


def cubic(p0, c1, c2, p3, steps=90):
    out = []
    for i in range(steps + 1):
        t = i / steps
        u = 1 - t
        out.append((
            u**3 * p0[0] + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t**3 * p3[0],
            u**3 * p0[1] + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t**3 * p3[1],
        ))
    return out


# Right half of the skull outline, top of the cranium down to the chin, in a
# unit box. Mirrored to make the left half, so the skull is exactly symmetric.
HALF = [
    ((0.500, 0.028), (0.762, 0.028), (0.936, 0.212), (0.936, 0.404)),  # dome -> temple
    ((0.936, 0.404), (0.936, 0.536), (0.884, 0.596), (0.812, 0.639)),  # temple -> cheekbone
    ((0.812, 0.639), (0.772, 0.663), (0.748, 0.696), (0.736, 0.744)),  # cheek -> jaw corner
    ((0.736, 0.744), (0.722, 0.868), (0.638, 0.958), (0.500, 0.972)),  # jaw -> chin
]


def skull_outline(box):
    """Unit-space outline scaled into (x, y, w, h)."""
    bx, by, bw, bh = box
    pts = []
    for seg in HALF:
        pts += cubic(*seg)
    # Mirror back up the left side.
    pts += [(1.0 - x, y) for x, y in reversed(pts)]
    return [(bx + x * bw, by + y * bh) for x, y in pts]


def vertical_gradient(top, bottom, height):
    g = Image.new("RGB", (1, height))
    px = g.load()
    for y in range(height):
        t = y / (height - 1)
        px[0, y] = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))
    return g.resize((W, W))


def main():
    icon = Image.new("RGBA", (W, W), (0, 0, 0, 0))

    # Backdrop: warm near-black squircle.
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], CORNER, fill=255)
    icon.paste(vertical_gradient(BACKDROP_TOP, BACKDROP_BOTTOM, W), (0, 0), mask)

    # Skull, centred with margin so it stays clear of the squircle corners.
    bw = W * 0.615
    bh = W * 0.680
    box = ((W - bw) / 2.0, W * 0.150, bw, bh)
    bx, by, _, _ = box

    skull = Image.new("L", (W, W), 0)
    ImageDraw.Draw(skull).polygon(skull_outline(box), fill=255)

    # Bone with a soft top-down shade, so it has form rather than reading flat.
    icon.paste(vertical_gradient(BONE, BONE_SHADE, W), (0, 0), skull)

    cut = Image.new("L", (W, W), 0)
    d = ImageDraw.Draw(cut)

    def ux(v):
        return bx + v * bw

    def uy(v):
        return by + v * bh

    # Eye sockets: large and slightly tilted inward. The single strongest cue
    # that this is a skull, so they get the most area.
    for cx, tilt in ((0.310, -1), (0.690, 1)):
        socket = Image.new("L", (W, W), 0)
        rx, ry = bw * 0.163, bh * 0.132
        ImageDraw.Draw(socket).ellipse(
            [ux(cx) - rx, uy(0.408) - ry, ux(cx) + rx, uy(0.408) + ry], fill=255)
        socket = socket.rotate(tilt * 9, center=(ux(cx), uy(0.408)), resample=Image.BILINEAR)
        cut.paste(socket, (0, 0), socket)

    # Nasal cavity: inverted teardrop. Without it the face reads as a mask.
    d.polygon([(ux(0.500), uy(0.512)),
               (ux(0.573), uy(0.646)),
               (ux(0.500), uy(0.668)),
               (ux(0.427), uy(0.646))], fill=255)

    # Mouth: one band of teeth, not two. A second row plus a dividing line
    # turns into a waffle grid once this is scaled to 32px.
    left, right = ux(0.300), ux(0.700)
    top_y, bot_y = uy(0.744), uy(0.862)
    d.rounded_rectangle([left, top_y, right, bot_y], radius=bw * 0.026, fill=255)

    # Everything cut from the face is clipped to the silhouette, so the mouth
    # band cannot bleed past the jaw and leave dark tabs floating outside it.
    cut = Image.composite(cut, Image.new("L", (W, W), 0), skull)
    icon.paste(Image.new("RGB", (W, W), SOCKET), (0, 0), cut)

    # Put the teeth back as bone bars dividing that band.
    teeth = Image.new("L", (W, W), 0)
    td = ImageDraw.Draw(teeth)
    n = 5
    for i in range(1, n):
        x = left + (right - left) * i / n
        td.rectangle([x - bw * 0.013, top_y, x + bw * 0.013, bot_y], fill=255)
    teeth = Image.composite(skull, Image.new("L", (W, W), 0), teeth)
    icon.paste(vertical_gradient(BONE, BONE_SHADE, W), (0, 0), teeth)

    icon = icon.resize((SIZE, SIZE), Image.LANCZOS)

    out_dir = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Resources")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "AppIcon.png")
    icon.save(out)
    print(f"wrote {out} ({icon.size[0]}x{icon.size[1]})")


if __name__ == "__main__":
    main()
