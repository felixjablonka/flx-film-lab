"""Kodak VISION3 diffuse RMS granularity, per layer, off Kodak's 2025 brochures.

Each brochure's second page plots the three characteristic curves and, over
them, the RMS granularity of each layer against the same log exposure, read on
a log sigma-D scale on the right. Kodak's instruction is to go from a density
across to its characteristic curve and then up or down to the granularity
curve; this does the same, and returns each layer's granularity against its
own net density (over the curve's minimum).

The plots are vector paths: stroked curves on 250D and 500T, thin filled
outlines on 50D (whose middle is the curve). The axes are calibrated from the
page itself: the density and log-exposure tick labels, and the sigma-D tick
marks, one at each of 0.001 ... 0.009, 0.01 ... 0.05 and 0.10.

The 200T brochure (K-1167) repeats 500T's plots path for path, so it carries
no data of its own, and 500T's plot draws no red granularity curve.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pymupdf

from datasheet import _flatten_path

SHEETS_DIR = Path(__file__).resolve().parents[2] / "film spec sheets"

SHEETS = {
    "kodak_vision3_50d": "KODAK-VISION3-50D-5203-7203-brochure.pdf",
    "kodak_vision3_250d": "KODAK-VISION3-250D-5207-7207-brochure.pdf",
    "kodak_vision3_500t": "KODAK-VISION3-500T-5219-7219-brochure.pdf",
}

# Kodak's curve colours, red, green and blue layers in that order.
_COLOURS = ((0.88, 0.16, 0.15), (0.41, 0.74, 0.27), (0.23, 0.33, 0.65))
_SIGMA_TICKS = [0.001 * k for k in range(1, 10)] + [0.01 * k for k in range(1, 6)] + [0.10]


def _layer(colour) -> int | None:
    if colour is None:
        return None
    for i, ref in enumerate(_COLOURS):
        if max(abs(a - b) for a, b in zip(colour, ref)) < 0.06:
            return i
    return None


def _number(text: str) -> float | None:
    if len(text) == 3 and text[1] == ".":
        try:
            return float(text)
        except ValueError:
            return None
    return None


def _centreline(points: np.ndarray, filled: bool, step: float = 0.8) -> tuple[np.ndarray, np.ndarray]:
    """y against x along a curve: the stroke itself, or the middle of an outline."""
    xs, ys = [], []
    for a in np.arange(points[:, 0].min(), points[:, 0].max() + step, step):
        sel = (points[:, 0] >= a) & (points[:, 0] < a + step)
        if not sel.any():
            continue
        y = points[sel, 1]
        xs.append(a + step / 2)
        ys.append(0.5 * (y.min() + y.max()) if filled else float(np.median(y)))
    return np.array(xs), np.array(ys)


def read(stock: str) -> dict[int, dict[str, np.ndarray]]:
    """Per layer (0 red, 1 green, 2 blue): the characteristic curve and the
    granularity curve, both against log relative exposure, and the
    granularity against the layer's own net density."""
    page = pymupdf.open(SHEETS_DIR / SHEETS[stock])[1]
    width = page.rect.width
    # the granularity plot is the right-hand one of the three on the page
    paths = [d for d in page.get_drawings() if d["rect"].x0 > 0.62 * width and 470 < d["rect"].y0 < 640]
    spans = []
    for block in page.get_text("dict")["blocks"]:
        for line in block.get("lines", []):
            for s in line["spans"]:
                x0, y0, x1, y1 = s["bbox"]
                if x0 > 0.62 * width and 470 < y0 < 640:
                    spans.append((s["text"].strip(), (x0 + x1) / 2, (y0 + y1) / 2, x1))

    # sigma-D: tick marks at the frame's right edge, small filled rectangles or
    # one stroked path of them
    ticks: set[float] = set()
    for d in paths:
        r, fill, stroke = d["rect"], d.get("fill"), d.get("color")
        if fill is not None and max(fill) < 0.05 and r.height < 0.8 and r.width < 2 and r.x0 > 540:
            ticks.add(round((r.y0 + r.y1) / 2, 2))
        if stroke is not None and max(stroke) < 0.05 and r.x0 > 540 and r.width < 3:
            ticks.update(round((i[1].y0 + i[1].y1) / 2, 2) for i in d["items"] if i[0] == "re")
    ticks_y = sorted(ticks, reverse=True)
    if len(ticks_y) != len(_SIGMA_TICKS):
        raise ValueError(f"{stock}: {len(ticks_y)} sigma-D ticks, expected {len(_SIGMA_TICKS)}")
    sigma_fit = np.polyfit(ticks_y, np.log10(_SIGMA_TICKS), 1)

    curves = [d for d in paths if _layer(d.get("color")) is not None or _layer(d.get("fill")) is not None]
    frame_x0 = min(d["rect"].x0 for d in curves)
    density_labels = [(v, y) for t, x, y, x1 in spans if (v := _number(t)) is not None and x1 < frame_x0 - 0.5]
    zero_y = max(y for v, y in density_labels if v == 0.0)
    exposure_labels = [(v, x) for t, x, y, x1 in spans
                       if (v := _number(t)) is not None and y > zero_y + 3 and x1 >= frame_x0 - 0.5 and v <= 5.0]
    density_fit = np.polyfit([y for _, y in density_labels], [v for v, _ in density_labels], 1)
    exposure_fit = np.polyfit([x for _, x in exposure_labels], [v for v, _ in exposure_labels], 1)

    by_layer: dict[int, list] = {}
    for d in curves:
        filled = d.get("fill") is not None
        layer = _layer(d.get("fill") if filled else d.get("color"))
        x, y = _centreline(_flatten_path(d["items"], steps=24), filled)
        by_layer.setdefault(layer, []).append((d["rect"].height, np.polyval(exposure_fit, x), y))
    out: dict[int, dict[str, np.ndarray]] = {}
    for layer, found in by_layer.items():
        found.sort(key=lambda f: -f[0])            # the taller path is the characteristic curve
        _, e, y = found[0]
        density = np.polyval(density_fit, y)
        entry = {"logE": e, "density": density}
        if len(found) > 1:
            _, ge, gy = found[1]
            sigma = 10 ** np.polyval(sigma_fit, gy)
            net = density - density[:5].mean()
            # Kodak's reading: across to the characteristic curve, then to the
            # granularity curve at the same exposure
            order = np.argsort(ge)
            entry.update(granularityLogE=ge, sigma=sigma,
                         netDensity=net, sigmaAtNet=np.interp(e, ge[order], sigma[order]))
        out[layer] = entry
    return out


if __name__ == "__main__":
    for stock in SHEETS:
        layers = read(stock)
        for layer, c in sorted(layers.items()):
            if "sigma" not in c:
                print(f"{stock:20s} {'RGB'[layer]}: no granularity curve")
                continue
            net, sig = c["netDensity"], c["sigmaAtNet"]
            picks = [f"{d:.1f}:{1000 * np.interp(d, net, sig):.1f}" for d in (0.2, 0.5, 1.0, 1.5) if d < net.max()]
            print(f"{stock:20s} {'RGB'[layer]}: RMS x1000 at net density " + "  ".join(picks))
