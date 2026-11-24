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

It also writes the icon's layered form, an Icon Composer document,
`FLXFilmLab/Resources/AppIcon.icon`: the artwork cut from its white ground as
one layer, on a white fill. Where Xcode is installed, build.sh compiles it
with actool into the app's Assets.car, and macOS 26 draws it in Liquid Glass,
with its dark, clear and tinted forms; without Xcode the app carries the
`.icns`, which macOS 26 masks and leaves flat. The layer carries the icon's
reservation too.

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

import json

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from scipy import ndimage

REPO = Path(__file__).resolve().parents[2]
ARTWORK = Path(__file__).resolve().parent / "AppIcon-artwork.png"
OUT = REPO / "FLXFilmLab" / "Resources" / "AppIcon.icns"
LAYERED = REPO / "FLXFilmLab" / "Resources" / "AppIcon.icon"
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


def layered() -> Path:
    """The Icon Composer document: the artwork's peaks as one layer, cut from
    its white ground, on a white fill, glass opaque so the colours stay the
    artwork's, and a shadow in the layer's own colours.

    The cut un-blends the artwork's own anti-aliasing: an edge pixel is the
    nearest solid colour laid over the white at some coverage, and that
    coverage becomes its alpha, so no white fringe shows on the dark form."""
    art = np.asarray(Image.open(ARTWORK).convert("RGB")).astype(np.float64) / 255
    border = np.concatenate([art[0], art[-1], art[:, 0], art[:, -1]])
    white = np.median(border, axis=0)
    away = np.abs(art - white).max(axis=2)
    solid = ndimage.binary_erosion(ndimage.binary_fill_holes(away > 0.25), iterations=2)
    _, (iy, ix) = ndimage.distance_transform_edt(~solid, return_indices=True)
    colour = art[iy, ix]                                  # the nearest solid colour
    span = colour - white
    alpha = ((art - white) * span).sum(axis=2) / np.maximum((span * span).sum(axis=2), 1e-6)
    alpha = np.where(solid, 1.0, np.clip(alpha, 0, 1))
    near = ndimage.distance_transform_edt(~solid) <= 4
    alpha = np.where(near, alpha, 0.0)
    rgba = np.dstack([colour, alpha])
    layer = Image.fromarray((rgba * 255 + 0.5).astype(np.uint8), "RGBA")
    layer = layer.convert("RGBa").resize((SIZE, SIZE), Image.LANCZOS).convert("RGBA")

    assets = LAYERED / "Assets"
    assets.mkdir(parents=True, exist_ok=True)
    for stale in assets.iterdir():
        stale.unlink()
    layer.save(assets / "artwork.png", optimize=True)
    icon = {
        "fill": {"solid": "extended-srgb:1.00000,1.00000,1.00000,1.00000"},
        "groups": [{
            "layers": [{"image-name": "artwork.png", "name": "artwork"}],
            "shadow": {"kind": "layer-color", "opacity": 0.5},
            "translucency": {"enabled": False, "value": 0.5},
        }],
        "supported-platforms": {"squares": "shared"},
    }
    (LAYERED / "icon.json").write_text(json.dumps(icon, indent=2) + "\n")
    print(f"wrote {LAYERED}")
    return LAYERED


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
        ("A Mac and Windows app for shooting", "Regular", 36, (199, 201, 207), 26),
        ("digital photos as if they were film.", "Regular", 36, (199, 201, 207), 6),
        ("60 film stocks · RAW photos · log video · LUTs", "Medium", 26, (138, 141, 150), 30),
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
    layered()
    readme_icon()
    social_preview()
