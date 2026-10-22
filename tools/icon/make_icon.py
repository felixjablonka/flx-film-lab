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

Run:  conda activate flx-bake && python tools/icon/make_icon.py
"""

from __future__ import annotations

import subprocess
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[2]
ARTWORK = Path(__file__).resolve().parent / "AppIcon-artwork.png"
OUT = REPO / "FLXFilmLab" / "Resources" / "AppIcon.icns"
SIZE = 1024


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


if __name__ == "__main__":
    build()
