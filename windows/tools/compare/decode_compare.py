"""Compares the port's RAW decode with Apple's, both linear Rec.2020 dumps.

    python3 tools/compare/decode_compare.py <apple.f32> <port.f32> [--map out.png]

Prints how far the two are offset (phase correlation of their luminance),
how their colour differs (each channel's median ratio in the midtones, and
the 3x3 matrix that takes the port's RGB to Apple's best), and the colour
difference per pixel (CIEDE2000, Lab under D65, white at Y = 1).
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import ciede2000, lab, read_dump, write_png  # noqa: E402

REC2020_TO_XYZ = np.array([[0.6369580, 0.1446169, 0.1688810],
                           [0.2627002, 0.6779981, 0.0593017],
                           [0.0000000, 0.0280727, 1.0609851]])
D65 = np.array([0.95047, 1.0, 1.08883])


def offset(a, b):
    """Where b's content sits relative to a's, in pixels, by phase correlation."""
    h, w = a.shape
    s = min(h, w, 1024)
    y0, x0 = (h - s) // 2, (w - s) // 2
    fa = np.fft.fft2(np.log(np.maximum(a[y0:y0 + s, x0:x0 + s], 1e-4)))
    fb = np.fft.fft2(np.log(np.maximum(b[y0:y0 + s, x0:x0 + s], 1e-4)))
    r = fa * np.conj(fb)
    c = np.abs(np.fft.ifft2(r / np.maximum(np.abs(r), 1e-12)))
    y, x = np.unravel_index(np.argmax(c), c.shape)
    return (y - s if y > s // 2 else y), (x - s if x > s // 2 else x)


def main():
    args = sys.argv[1:]
    png = None
    if "--map" in args:
        i = args.index("--map")
        png = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        raise SystemExit(__doc__)
    apple = read_dump(args[0])[..., :3].astype(np.float64)
    port = read_dump(args[1])[..., :3].astype(np.float64)
    print(f"Apple {apple.shape[1]} x {apple.shape[0]}, port {port.shape[1]} x {port.shape[0]}")
    if apple.shape != port.shape:
        return
    ya, yp = apple @ REC2020_TO_XYZ[1], port @ REC2020_TO_XYZ[1]
    dy, dx = offset(ya, yp)
    print(f"offset: the port's picture sits {dx:+d} px across, {dy:+d} px down from Apple's")

    mid = (ya > 0.03) & (ya < 0.4) & np.all(apple > 0.005, axis=-1) & np.all(port > 0.005, axis=-1)
    ratios = [np.median(port[..., c][mid] / apple[..., c][mid]) for c in range(3)]
    print(f"midtones ({mid.mean() * 100:.0f}% of pixels): port / Apple R {ratios[0]:.4f} G {ratios[1]:.4f} "
          f"B {ratios[2]:.4f}  ({np.log2(ratios[1]):+.3f} EV in green)")
    p, a = port[mid], apple[mid]
    m, *_ = np.linalg.lstsq(p, a, rcond=None)
    print("the matrix that takes the port's RGB to Apple's best:")
    for row in m.T:
        print("   " + "  ".join(f"{v:8.4f}" for v in row))
    fitted = p @ m
    print(f"   leaves {np.median(np.abs(fitted - a) / np.maximum(a, 1e-6)) * 100:.2f}% median, "
          f"{np.quantile(np.abs(fitted - a) / np.maximum(a, 1e-6), 0.99) * 100:.1f}% at the 99th percentile")

    de = ciede2000(lab(apple @ REC2020_TO_XYZ.T, D65), lab(port @ REC2020_TO_XYZ.T, D65))
    print(f"ΔE2000: median {np.median(de):.3g}, mean {de.mean():.3g}, 99% {np.quantile(de, 0.99):.3g}, "
          f"max {de.max():.3g}; over 1: {np.mean(de > 1) * 100:.1f}% of pixels")
    # what is left with the colour taken out: the port's pixels through the fitted matrix
    de_fit = ciede2000(lab(apple @ REC2020_TO_XYZ.T, D65), lab((port @ m) @ REC2020_TO_XYZ.T, D65))
    print(f"   through the fitted matrix: median {np.median(de_fit):.3g}, 99% {np.quantile(de_fit, 0.99):.3g}, "
          f"max {de_fit.max():.3g}; over 1: {np.mean(de_fit > 1) * 100:.2f}% of pixels")
    bright = ya > 0.9
    if bright.any():
        print(f"   where Apple's luminance is over 0.9 ({bright.mean() * 100:.2f}% of pixels): "
              f"median {np.median(de_fit[bright]):.3g}, 99% {np.quantile(de_fit[bright], 0.99):.3g}")
    # fine detail: ΔE against how much the pixel differs from its neighbours
    gy, gx = np.gradient(np.log(np.maximum(ya, 1e-4)))
    edge = np.hypot(gx, gy)
    flat, busy = edge < np.quantile(edge, 0.5), edge > np.quantile(edge, 0.95)
    print(f"   in flat areas: median {np.median(de_fit[flat]):.3g}; on the busiest 5%: median {np.median(de_fit[busy]):.3g}")
    if png:
        write_png(png, np.clip(de_fit / 2 * 255, 0, 255).astype(np.uint8))


if __name__ == "__main__":
    main()
