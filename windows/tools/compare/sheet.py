"""A comparison sheet: the Mac engine's render and the port's, side by side,
with their colour difference as a map, and full-size crops where they differ
most.

    python3 tools/compare/sheet.py <dumpdir> port-<backend> <out.png> [--scale DE]

Top row: the two screen images and the ΔE2000 map, each at a third of the
size; bottom row: 1:1 crops around the largest ΔE. The map is black at 0
and white at --scale (0.05 unless given): far below anything visible, so the
map shows where the two differ at all.
"""

import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import ciede2000, lab, read_dump  # noqa: E402


def write_png(path, rgb):
    h, w, _ = rgb.shape
    rows = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def shrink(img, k):
    h, w = img.shape[0] // k * k, img.shape[1] // k * k
    return img[:h, :w].reshape(h // k, k, w // k, k, -1).mean(axis=(1, 3))


def main():
    args = sys.argv[1:]
    scale = 0.05
    if "--scale" in args:
        i = args.index("--scale")
        scale = float(args[i + 1])
        del args[i:i + 2]
    if len(args) != 3:
        raise SystemExit(__doc__)
    root, port, out = Path(args[0]), Path(args[0]) / args[1], args[2]
    setup = json.loads((root / "setup.json").read_text())
    p = setup["printUniforms"]
    rgb_to_xyz = np.linalg.inv(np.array(p["xyzToRGB"], dtype=np.float64).reshape(3, 3).T)
    white = np.array(p["illuminantXYZ"], dtype=np.float64)

    mac = read_dump(root / "display.u8")[..., :3]
    ported = read_dump(port / "display.u8")[..., :3]
    de = ciede2000(lab(read_dump(root / "rgb_out.f32")[..., :3].astype(np.float64) @ rgb_to_xyz.T, white),
                   lab(read_dump(port / "rgb_out.f32")[..., :3].astype(np.float64) @ rgb_to_xyz.T, white))
    de_map = np.repeat(np.clip(de / scale, 0, 1)[..., None] * 255, 3, axis=2)

    k = 3
    top = [shrink(a.astype(np.float64), k) for a in (mac, ported, de_map)]
    gap = 12
    th, tw = top[0].shape[:2]
    y, x = np.unravel_index(np.argmax(de), de.shape)
    c = min(tw, th, 600) // 2
    y0, x0 = int(np.clip(y - c, 0, de.shape[0] - 2 * c)), int(np.clip(x - c, 0, de.shape[1] - 2 * c))
    crops = [a[y0:y0 + 2 * c, x0:x0 + 2 * c].astype(np.float64) for a in (mac, ported, de_map)]

    width = 3 * tw + 2 * gap
    sheet = np.full((th + gap + 2 * c, width, 3), 40.0)
    for i, t in enumerate(top):
        sheet[:th, i * (tw + gap):i * (tw + gap) + tw] = t
    cx = (width - (3 * 2 * c + 2 * gap)) // 2
    for i, t in enumerate(crops):
        sheet[th + gap:, cx + i * (2 * c + gap):cx + i * (2 * c + gap) + 2 * c] = t
    write_png(out, np.clip(sheet, 0, 255).astype(np.uint8))
    report = json.loads((port / "report.json").read_text())
    print(f"{out}: Mac engine | port on {report['device']} ({report['backend']}) | ΔE2000, white at {scale}; "
          f"crops 1:1 around ({x}, {y}), ΔE {de[y, x]:.3g} there")


if __name__ == "__main__":
    main()
