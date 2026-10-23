#!/usr/bin/env python
"""Build the FLX Film Lab app icon (.icns) from Felix's artwork.

The artwork is © Felix Jablonka, all rights reserved, and is not part of the
public source; the .icns it builds is, under the same reservation (NOTICE.md).

`AppIcon-artwork.png` is the mark on its own square: three dye layers'
spectral sensitivities, overlapping where film sees colour.

Apple's Human Interface Guidelines (App icons, June 2026) have macOS mask
every app icon to its rounded rectangle itself: an icon is supplied as an
unmasked, opaque 1024 px square, with no drop shadow, highlight or glow of
its own, since the system adds those. macOS 26 and later also shrink an icon
whose edges are not opaque onto a grey rounded square, which is what a
pre-shaped Big Sur-style icon with transparent corners and a shadow now gets.
So the artwork goes in as it is, full-bleed and opaque.

The layered Liquid Glass form (an Icon Composer `.icon`, compiled into
Assets.car) needs Xcode's actool, which this build does not use; the `.icns`
is what every macOS from 14 reads. macOS 14 and 15 do not mask icons, so
there the square shows as a square.

It also writes the two pictures made from the icon, both masked to its
rounded square as macOS draws it: `docs/images/icon.png`, at the top of the
README, and `tools/icon/social-preview.png`, which GitHub shows when the
repository is shared (uploaded by hand, under Settings, General, Social
preview). They carry the icon's reservation too.

Run:  conda activate flx-bake && python tools/icon/make_icon.py
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

REPO = Path(__file__).resolve().parents[2]
ARTWORK = Path(__file__).resolve().parent / "AppIcon-artwork.png"
OUT = REPO / "FLXFilmLab" / "Resources" / "AppIcon.icns"
SIZE = 1024
README_ICON = REPO / "docs" / "images" / "icon.png"
SOCIAL_PREVIEW = Path(__file__).resolve().parent / "social-preview.png"
FONT = "/System/Library/Fonts/Avenir Next.ttc"


def build() -> Path:
    icon = Image.open(ARTWORK).convert("RGB").resize((SIZE, SIZE), Image.LANCZOS)

    iconset = REPO / "build" / "AppIcon.iconset"
    iconset.mkdir(parents=True, exist_ok=True)
    for stale in iconset.glob("*.png"):
        stale.unlink()
    # iconutil's canonical names: 16 to 512 points, each at 1x and 2x. RGB,
    # so every pixel is opaque.
    for points in (16, 32, 128, 256, 512):
        icon.resize((points, points), Image.LANCZOS).save(iconset / f"icon_{points}x{points}.png")
        icon.resize((points * 2, points * 2), Image.LANCZOS).save(iconset / f"icon_{points}x{points}@2x.png")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(OUT)], check=True)
    print(f"wrote {OUT} ({OUT.stat().st_size / 1024:.0f} KB)")
    return OUT


def rounded(size: int) -> Image.Image:
    """The icon as macOS draws it: masked to a rounded square with continuous
    corners, here a superellipse of exponent 5, drawn at 4x and reduced."""
    big = size * 4
    t = np.abs((np.arange(big) + 0.5) / big * 2 - 1)
    u, v = np.meshgrid(t, t)
    mask = Image.fromarray(((u ** 5 + v ** 5 <= 1) * 255).astype(np.uint8), "L")
    icon = Image.open(ARTWORK).convert("RGB").resize((size, size), Image.LANCZOS).convert("RGBA")
    icon.putalpha(mask.resize((size, size), Image.LANCZOS))
    return icon


def shadow(icon: Image.Image, canvas: tuple[int, int], at: tuple[int, int],
           blur: float, offset: int, opacity: float) -> Image.Image:
    """A soft drop shadow of the icon's shape, as macOS gives icons."""
    layer = Image.new("RGBA", canvas, (0, 0, 0, 0))
    alpha = icon.getchannel("A").point(lambda a: round(a * opacity))
    layer.paste((0, 0, 0, 255), (at[0], at[1] + offset), alpha)
    return layer.filter(ImageFilter.GaussianBlur(blur))


def readme_icon(size: int = 512) -> Path:
    """The icon for the top of the README, with the faint shadow macOS gives
    it, so its edge holds on GitHub's white page as well as on its dark one."""
    body = round(size * 0.89)
    at = ((size - body) // 2, (size - body) // 2 - round(size * 0.008))
    icon = rounded(body)
    out = shadow(icon, (size, size), at, blur=size * 0.012, offset=round(size * 0.008), opacity=0.35)
    out.alpha_composite(icon, at)
    README_ICON.parent.mkdir(parents=True, exist_ok=True)
    out.save(README_ICON, optimize=True)
    print(f"wrote {README_ICON} ({README_ICON.stat().st_size / 1024:.0f} KB)")
    return README_ICON


def font(style: str, size: int) -> ImageFont.FreeTypeFont:
    for index in range(32):
        try:
            face = ImageFont.truetype(FONT, size, index=index)
        except OSError:
            break
        if face.getname()[1] == style:
            return face
    raise LookupError(f"no Avenir Next {style} in {FONT}")


def social_preview() -> Path:
    """The picture GitHub shows when the repository is shared: 1280 x 640, the
    icon, the app's name and its line on a darkroom-dark ground."""
    width, height = 1280, 640
    out = Image.new("RGBA", (width, height), (19, 20, 23, 255))
    body = 340
    at = (120, (height - body) // 2)
    icon = rounded(body)
    out.alpha_composite(shadow(icon, (width, height), at, blur=18, offset=10, opacity=0.6))
    out.alpha_composite(icon, at)

    # Text, style, size, colour, and the space above it.
    lines = [
        ("FLX Film Lab", "Demi Bold", 92, (245, 245, 247), 0),
        ("A Mac app for shooting digital photos", "Regular", 36, (199, 201, 207), 26),
        ("as if they were film.", "Regular", 36, (199, 201, 207), 6),
        ("34 film stocks · RAW photos · log video · LUTs", "Medium", 26, (138, 141, 150), 30),
    ]
    faces = [font(style, size) for _, style, size, _, _ in lines]
    heights = [sum(face.getmetrics()) for face in faces]
    x = at[0] + body + 72
    y = (height - sum(heights) - sum(line[4] for line in lines)) / 2
    draw = ImageDraw.Draw(out)
    for (text, _, _, colour, above), face, line_height in zip(lines, faces, heights):
        y += above
        assert x + face.getlength(text) <= width - 60, f"too wide: {text}"
        draw.text((x, y), text, font=face, fill=colour, anchor="la")
        y += line_height
    out.convert("RGB").save(SOCIAL_PREVIEW, optimize=True)
    print(f"wrote {SOCIAL_PREVIEW} ({SOCIAL_PREVIEW.stat().st_size / 1024:.0f} KB)")
    return SOCIAL_PREVIEW


if __name__ == "__main__":
    build()
    readme_icon()
    social_preview()
