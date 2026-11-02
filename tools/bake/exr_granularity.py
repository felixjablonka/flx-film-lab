"""Kodak EXR diffuse RMS granularity, per layer, off Kodak's H-1 sheets of 1999.

Each sheet's third page plots, against one relative log exposure, the three
characteristic curves and over them the RMS granularity of each layer, read
on a log sigma-D scale on the right, as Kodak's VISION3 brochures do
(`vision3_granularity.py`). Kodak's instruction is the same: from a density
across to its characteristic curve, then up or down to the granularity curve.
This returns each layer's granularity against its own net density.

Every curve is black. The characteristic curves are the three that rise
across the plot, named B, G and R from the top. The granularity curves keep
that order everywhere, blue over green over red, and never cross: where all
three are drawn at one exposure they are named by their order there, and a
piece drawn alone (EXR 50D's are dashed; EXR 200T's red is broken where its
label sits) goes to the curve it lies nearest. The density axis is
calibrated on its ticks, the sigma-D axis on its thirteen ticks, labelled
.001 to .100.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pymupdf

from datasheet import _flatten_path
from vector_plots import _subpaths

SHEETS_DIR = Path(__file__).resolve().parents[2] / "film spec sheets"

# stock: (sheet, page, frame, density ticks as (y, density))
SHEETS = {
    "kodak_exr_50d": ("Kodak EXR 50D 5245-1999.pdf", 2, (85.2, 513.8, 260.3, 689.1),
                      ((557.61, 3.0), (601.43, 2.0), (645.25, 1.0))),
    "kodak_exr_200t": ("Kodak EXR 200T 5293-1999.pdf", 2, (95.7, 510.5, 270.7, 685.6),
                       ((553.14, 3.0), (596.56, 2.0))),
}
# the sigma-D scale's labelled ticks, top to bottom
_SIGMA_TICKS = (0.100, 0.050, 0.040, 0.030, 0.020, 0.010, 0.008, 0.006, 0.005, 0.004, 0.003, 0.002, 0.001)


def _strokes(page, frame) -> list[np.ndarray]:
    """Every stroke inside the plot, as points sorted along x, but its axes'
    ticks (level strokes at the frame's sides, short ones on its foot)."""
    x0, y0, x1, y1 = frame
    out = []
    for d in page.get_drawings():
        r = d["rect"]
        if not (x0 - 1 <= r.x0 and r.x1 <= x1 + 1 and y0 - 1 <= r.y0 and r.y1 <= y1 + 1):
            continue
        if r.width > (x1 - x0) - 2 and r.height > (y1 - y0) - 2:
            continue                                    # the frame
        for run in _subpaths(d["items"]):
            if not run:
                continue
            p = _flatten_path(run, steps=6)
            if np.ptp(p[:, 0]) < 0.3:
                continue
            level = np.ptp(p[:, 1]) < 0.05
            if level and (p[:, 0].min() < x0 + 6 or p[:, 0].max() > x1 - 6):
                continue
            if p[:, 1].min() > y1 - 6 and np.ptp(p[:, 0]) < 1:
                continue
            out.append(p[np.argsort(p[:, 0])])
    return out


def _by_order(pieces: list[np.ndarray], step: float = 0.25) -> list[np.ndarray]:
    """Three curves that never cross, top to bottom, out of their pieces: named
    by their order wherever all three are drawn, and each other piece given to
    the curve it lies nearest, as interpolated from those."""
    lo, hi = min(p[0, 0] for p in pieces), max(p[-1, 0] for p in pieces)
    full_x, full_y = [], []
    for x in np.arange(lo, hi + step, step):
        ys = sorted(float(np.interp(x, p[:, 0], p[:, 1])) for p in pieces if p[0, 0] <= x <= p[-1, 0])
        if len(ys) == 3:
            full_x.append(x)
            full_y.append(ys)
    if len(full_x) < 20:
        raise ValueError("the granularity curves are not drawn together anywhere")
    full_x, full_y = np.array(full_x), np.array(full_y)
    curves = [[], [], []]
    for p in pieces:
        guess = np.array([np.interp(p[:, 0], full_x, full_y[:, k]) for k in range(3)])
        curves[int(np.argmin(np.abs(guess - p[:, 1][None, :]).mean(axis=1)))].append(p)
    joined = []
    for c in curves:
        points = np.concatenate(c)
        joined.append(points[np.argsort(points[:, 0])])
    return joined


def _sigma_ticks(page, frame) -> list[float]:
    x0, y0, x1, y1 = frame
    ticks = set()
    for d in page.get_drawings():
        for item in d["items"]:
            if item[0] == "l":
                a, b = item[1], item[2]
                if abs(a.y - b.y) < 0.2 and min(a.x, b.x) >= x1 - 0.5 and abs(a.x - b.x) < 6 \
                        and y0 - 1 < a.y < y1 + 1:
                    ticks.add(round(a.y, 2))
    return sorted(ticks)


def read(stock: str) -> dict[int, dict[str, np.ndarray]]:
    """Per layer (0 red, 1 green, 2 blue): the characteristic and granularity
    curves against the plot's x, and the granularity against the layer's own
    net density."""
    pdf, page_no, frame, density_ticks = SHEETS[stock]
    page = pymupdf.open(SHEETS_DIR / pdf)[page_no]
    ticks = _sigma_ticks(page, frame)
    if len(ticks) != len(_SIGMA_TICKS):
        raise ValueError(f"{stock}: {len(ticks)} sigma-D ticks, expected {len(_SIGMA_TICKS)}")
    sigma_fit = np.polyfit(ticks, np.log10(_SIGMA_TICKS), 1)
    density_fit = np.polyfit([y for y, _ in density_ticks], [v for _, v in density_ticks], 1)

    strokes = _strokes(page, frame)
    rising = sorted((s for s in strokes if np.ptp(s[:, 1]) > 30), key=lambda s: s[:, 1].mean())
    if len(rising) != 3:
        raise ValueError(f"{stock}: {len(rising)} characteristic curves, expected 3")
    granularity = _by_order([s for s in strokes if np.ptp(s[:, 1]) <= 30])
    out = {}
    for layer, (curve, grain) in enumerate(zip(reversed(rising), reversed(granularity))):
        density = np.polyval(density_fit, curve[:, 1])
        sigma = 10 ** np.polyval(sigma_fit, grain[:, 1])
        net = density - density[:5].mean()
        out[layer] = {"x": curve[:, 0], "density": density, "granularityX": grain[:, 0], "sigma": sigma,
                      "netDensity": net, "sigmaAtNet": np.interp(curve[:, 0], grain[:, 0], sigma)}
    return out


if __name__ == "__main__":
    for stock in SHEETS:
        layers = read(stock)
        for layer, c in sorted(layers.items()):
            net, sig = c["netDensity"], c["sigmaAtNet"]
            picks = [f"{d:.1f}:{1000 * np.interp(d, net, sig):.1f}" for d in (0.2, 0.5, 1.0, 1.5) if d < net.max()]
            print(f"{stock:16s} {'RGB'[layer]}: RMS x1000 at net density " + "  ".join(picks))
