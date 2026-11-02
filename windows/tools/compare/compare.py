"""Compares the port's render of a setup with the Mac engine's, stage by stage.

    python3 tools/compare/compare.py <dumpdir> [port-<backend> ...]

<dumpdir> is what flxref (tools/mac-reference) wrote; each port-<backend>
in it is what flxport-render made from it, all of them when none is named.
For each stage it prints the largest and the mean difference and where the
largest is. For the print, the colour difference: CIEDE2000, in Lab under
the viewing illuminant, from the linear print and the setup's own output
matrix. For the screen's 8-bit image, how many values differ, and by how
much. It writes each port's ΔE map, de_map.png, black at 0 and white at
0.5 and over.
"""

import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np


def read_dump(path):
    raw = Path(path).read_bytes()
    magic, version, w, h, c, kind = struct.unpack("<4s5I", raw[:24])
    if magic != b"FLXD" or version != 1:
        raise SystemExit(f"{path}: not an FLXD v1 dump")
    dtype = np.float32 if kind == 0 else np.uint8
    return np.frombuffer(raw, dtype=dtype, offset=32).reshape(h, w, c)


def write_png(path, gray):
    """An 8-bit greyscale PNG, with nothing but the standard library."""
    h, w = gray.shape
    rows = b"".join(b"\x00" + gray[y].tobytes() for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def lab(xyz, white):
    t = xyz / white
    f = np.where(t > (6 / 29) ** 3, np.cbrt(t), t / (3 * (6 / 29) ** 2) + 4 / 29)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def ciede2000(lab1, lab2):
    L1, a1, b1 = np.moveaxis(lab1, -1, 0)
    L2, a2, b2 = np.moveaxis(lab2, -1, 0)
    C1, C2 = np.hypot(a1, b1), np.hypot(a2, b2)
    Cm = (C1 + C2) / 2
    G = 0.5 * (1 - np.sqrt(Cm**7 / (Cm**7 + 25.0**7)))
    a1p, a2p = (1 + G) * a1, (1 + G) * a2
    C1p, C2p = np.hypot(a1p, b1), np.hypot(a2p, b2)
    h1p = np.degrees(np.arctan2(b1, a1p)) % 360
    h2p = np.degrees(np.arctan2(b2, a2p)) % 360
    dLp, dCp = L2 - L1, C2p - C1p
    dh = h2p - h1p
    dh = np.where(dh > 180, dh - 360, np.where(dh < -180, dh + 360, dh))
    dh = np.where(C1p * C2p == 0, 0, dh)
    dHp = 2 * np.sqrt(C1p * C2p) * np.sin(np.radians(dh / 2))
    Lpm, Cpm = (L1 + L2) / 2, (C1p + C2p) / 2
    hs = h1p + h2p
    hpm = np.where(C1p * C2p == 0, hs,
                   np.where(np.abs(h1p - h2p) <= 180, hs / 2, np.where(hs < 360, (hs + 360) / 2, (hs - 360) / 2)))
    T = (1 - 0.17 * np.cos(np.radians(hpm - 30)) + 0.24 * np.cos(np.radians(2 * hpm))
         + 0.32 * np.cos(np.radians(3 * hpm + 6)) - 0.20 * np.cos(np.radians(4 * hpm - 63)))
    Sl = 1 + 0.015 * (Lpm - 50) ** 2 / np.sqrt(20 + (Lpm - 50) ** 2)
    Sc, Sh = 1 + 0.045 * Cpm, 1 + 0.015 * Cpm * T
    Rt = (-2 * np.sqrt(Cpm**7 / (Cpm**7 + 25.0**7))
          * np.sin(np.radians(60 * np.exp(-(((hpm - 275) / 25) ** 2)))))
    return np.sqrt((dLp / Sl) ** 2 + (dCp / Sc) ** 2 + (dHp / Sh) ** 2 + Rt * (dCp / Sc) * (dHp / Sh))


def stage_line(name, ref, port):
    d = np.abs(port[..., :3].astype(np.float64) - ref[..., :3])
    y, x, c = np.unravel_index(np.argmax(d), d.shape)
    return (f"  {name:11s} max {d.max():.3g} at ({x}, {y}) {'RGB'[c]}: {ref[y, x, c]:.6g} vs {port[y, x, c]:.6g}"
            f"   mean {d.mean():.3g}   99.9% {np.quantile(d, 0.999):.3g}")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    root = Path(sys.argv[1])
    setup = json.loads((root / "setup.json").read_text())
    ports = [root / p for p in sys.argv[2:]] or sorted(root.glob("port-*"))
    if not ports:
        raise SystemExit(f"{root}: no port-<backend> renders")

    p = setup["printUniforms"]
    xyz_to_rgb = np.array(p["xyzToRGB"], dtype=np.float64).reshape(3, 3).T   # written as columns
    rgb_to_xyz = np.linalg.inv(xyz_to_rgb)
    white = np.array(p["illuminantXYZ"], dtype=np.float64)
    ref = {t: read_dump(root / f"{t}.f32") for t in ("log_e_film", "cmy_film", "rgb_out")}
    ref_lab = lab(ref["rgb_out"][..., :3].astype(np.float64) @ rgb_to_xyz.T, white)
    ref_display = read_dump(root / "display.u8")

    print(f"{setup['input']}: {setup['width']} x {setup['height']}, {setup['film']} on {setup['paper']}, "
          f"Mac engine {setup['macRelease']}, render {setup['renderSeconds'] * 1000:.1f} ms")
    for port in ports:
        report = json.loads((port / "report.json").read_text())
        print(f"{port.name} — {report['device']}, render {report['frameSeconds'] * 1000:.1f} ms")
        for t in ("log_e_film", "cmy_film", "rgb_out"):
            print(stage_line(t, ref[t], read_dump(port / f"{t}.f32")))
        port_lab = lab(read_dump(port / "rgb_out.f32")[..., :3].astype(np.float64) @ rgb_to_xyz.T, white)
        de = ciede2000(ref_lab, port_lab)
        y, x = np.unravel_index(np.argmax(de), de.shape)
        print(f"  ΔE2000      max {de.max():.3g} at ({x}, {y})   mean {de.mean():.3g}   "
              f"99.9% {np.quantile(de, 0.999):.3g}   over 0.1: {np.mean(de > 0.1) * 100:.3f}% of pixels")
        diff = np.abs(read_dump(port / "display.u8")[..., :3].astype(int) - ref_display[..., :3])
        counts = np.bincount(diff.ravel(), minlength=4)
        print(f"  display     {np.mean(diff > 0) * 100:.3f}% of 8-bit values differ; "
              f"by 1: {counts[1]}, by 2: {counts[2]}, by more: {int(diff.size - counts[:3].sum())}, max {diff.max()}")
        write_png(port / "de_map.png", np.clip(de / 0.5 * 255, 0, 255).astype(np.uint8))


if __name__ == "__main__":
    main()
