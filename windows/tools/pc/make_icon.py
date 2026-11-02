#!/usr/bin/env python
"""Build the Windows app's icon (app/flx.ico) from Felix's artwork.

The artwork (tools/icon/AppIcon-artwork.png in the Mac app's tree) is
© Felix Jablonka, all rights reserved, and so is the icon made from it (the
Mac app's NOTICE.md); it is not in the public repository.

macOS masks an icon to its rounded square itself; Windows shows an icon as
it is, so here the artwork is masked to that rounded square, its corners
transparent, a little in from the edge as Windows' own icons sit. Each size
is drawn from the full artwork, so the small ones stay sharp.

Run:  conda run -n flx-bake python tools/pc/make_icon.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
# the Mac app's tree: mac/ in the port's own repository, the parent beside
# windows/ in the public one
MAC = ROOT / "mac" if (ROOT / "mac" / "build.sh").exists() else ROOT.parent
ARTWORK = MAC / "tools" / "icon" / "AppIcon-artwork.png"
OUT = ROOT / "app" / "flx.ico"
SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
MASTER = 1024
MARGIN = 0.04       # of the side, all round
RADIUS = 0.2237     # of the tile's side: macOS's icon corner


def tile() -> Image.Image:
    art = Image.open(ARTWORK).convert("RGB")
    inner = round(MASTER * (1 - 2 * MARGIN))
    art = art.resize((inner, inner), Image.LANCZOS)
    # the mask drawn four times larger and brought down, for a smooth edge
    big = inner * 4
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, big - 1, big - 1), radius=round(big * RADIUS), fill=255)
    mask = mask.resize((inner, inner), Image.LANCZOS)
    icon = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    offset = (MASTER - inner) // 2
    icon.paste(art, (offset, offset), mask)
    return icon


def main() -> None:
    master = tile()
    images = [master.resize((s, s), Image.LANCZOS) for s in SIZES]
    images[-1].save(OUT, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    print(f"wrote {OUT.relative_to(ROOT)}: {', '.join(str(s) for s in SIZES)} px")


if __name__ == "__main__":
    main()
