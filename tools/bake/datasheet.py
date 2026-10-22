"""Read curve data straight out of a manufacturer datasheet PDF.

Kodak and Fujifilm publish their sensitometry as *vector* plots, so the curves
are exact paths rather than pictures of curves. Extracting the path geometry and
mapping it through the plot's axes recovers the published numbers to within the
line width — far better than digitising a rendered image, and with no hand
clicking.

The only inputs that need a human are the axis ranges, because those live in the
tick labels rather than the drawing.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pymupdf


@dataclass(frozen=True)
class Axes:
    """Plot frame in PDF points, and what its corners mean.

    PDF y grows downward, so `y0` is the *top* edge and normally carries the
    larger data value.
    """
    x0: float
    y0: float
    x1: float
    y1: float
    x_min: float
    x_max: float
    y_at_top: float
    y_at_bottom: float

    def to_data(self, points: np.ndarray) -> np.ndarray:
        tx = (points[:, 0] - self.x0) / (self.x1 - self.x0)
        ty = (points[:, 1] - self.y0) / (self.y1 - self.y0)
        return np.stack([
            self.x_min + tx * (self.x_max - self.x_min),
            self.y_at_top + ty * (self.y_at_bottom - self.y_at_top),
        ], axis=-1)


def _flatten_path(items, steps: int = 16) -> np.ndarray:
    """Path items to a polyline, subdividing cubic segments."""
    pts: list[tuple[float, float]] = []

    def push(p):
        t = (float(p.x), float(p.y))
        if not pts or abs(t[0] - pts[-1][0]) > 1e-9 or abs(t[1] - pts[-1][1]) > 1e-9:
            pts.append(t)

    for item in items:
        kind = item[0]
        if kind == "l":
            push(item[1]); push(item[2])
        elif kind == "c":
            p0, p1, p2, p3 = item[1], item[2], item[3], item[4]
            push(p0)
            for i in range(1, steps + 1):
                t = i / steps
                u = 1 - t
                x = (u ** 3 * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t ** 3 * p3.x)
                y = (u ** 3 * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t ** 3 * p3.y)
                pts.append((x, y))
    return np.asarray(pts, dtype=float)


def curves_in(pdf_path: str, page_index: int, axes: Axes,
              min_points: int = 8, inset: float = 2.0) -> list[np.ndarray]:
    """Every plotted curve inside `axes`, in data coordinates.

    A path counts as a curve when it has enough segments and lies inside the
    frame; that rejects the axis rules, tick marks and leader lines without
    needing to know anything about the specific plot.
    """
    page = pymupdf.open(pdf_path)[page_index]
    out: list[np.ndarray] = []
    for path in page.get_drawings():
        segments = [i for i in path["items"] if i[0] in ("l", "c")]
        if len(segments) < min_points:
            continue
        rect = path["rect"]
        if not (axes.x0 - inset <= rect.x0 and rect.x1 <= axes.x1 + inset
                and axes.y0 - inset <= rect.y0 and rect.y1 <= axes.y1 + inset):
            continue
        # A curve spans a meaningful part of the frame; leader lines do not.
        if rect.width < 0.15 * (axes.x1 - axes.x0):
            continue
        polyline = _flatten_path(segments)
        if polyline.shape[0] < 4:
            continue
        out.append(axes.to_data(polyline))
    return out


def resample(curve: np.ndarray, x_grid: np.ndarray, monotonic_x: bool = True) -> np.ndarray:
    """Sample a extracted polyline onto a regular grid.

    Datasheet paths are drawn left to right but can double back by a hair at the
    ends; sorting and averaging duplicates keeps `np.interp` well defined.
    """
    xs, ys = curve[:, 0], curve[:, 1]
    order = np.argsort(xs, kind="stable")
    xs, ys = xs[order], ys[order]
    if monotonic_x:
        unique_x, index = np.unique(np.round(xs, 6), return_inverse=True)
        summed = np.bincount(index, weights=ys)
        counts = np.bincount(index)
        xs, ys = unique_x, summed / counts
    return np.interp(x_grid, xs, ys, left=ys[0], right=ys[-1])


def order_curves(curves: list[np.ndarray], at_x: float, descending: bool = True) -> list[np.ndarray]:
    """Sort curves by their value at a probe abscissa.

    Datasheets label their curves with leader lines, not with the path order, so
    identifying which is which means evaluating them somewhere they separate
    cleanly.
    """
    def value(c: np.ndarray) -> float:
        return float(resample(c, np.array([at_x]))[0])
    return sorted(curves, key=value, reverse=descending)


def _numeric_ticks(page, *, horizontal: bool, frame, band: float = 26.0):
    """Tick labels along one edge of a plot frame, as (centre, magnitude)."""
    out = []
    for w in page.get_text("words"):
        x0, y0, x1, y1, text = w[0], w[1], w[2], w[3], w[4]
        # Fujifilm sets its minus signs as an en dash ("–3.0"), and some
        # sheets use the true minus sign; float() takes neither.
        text = text.replace("–", "-").replace("−", "-").replace("‒", "-")
        try:
            value = float(text)
        except ValueError:
            continue
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        if horizontal:
            if frame.y1 < y0 <= frame.y1 + band and frame.x0 - band <= cx <= frame.x1 + band:
                out.append((cx, value))
        else:
            if frame.x0 - band <= x1 < frame.x0 and frame.y0 - band <= cy <= frame.y1 + band:
                out.append((cy, value))
    out.sort(key=lambda t: t[0])
    return out


def _restore_signs(ticks, ascending: bool):
    """Kodak draws minus signs as a separate overbar glyph, so the text of a
    negative tick is just its magnitude. Recover the signs by requiring the
    axis to be monotonic: the leading run that would otherwise decrease is the
    negative half.
    """
    values = [v for _, v in ticks]
    n = len(values)
    best = None
    for split in range(n + 1):
        candidate = [-v for v in values[:split]] + values[split:]
        ok = all((b > a) if ascending else (b < a)
                 for a, b in zip(candidate, candidate[1:]))
        if ok:
            best = candidate
            break
    if best is None:
        best = values
    return [(pos, val) for (pos, _), val in zip(ticks, best)]


def calibrate(pdf_path: str, page_index: int, frame,
              x_ascending: bool = True, y_ascending_upward: bool = True) -> Axes:
    """Build an `Axes` from the plot's own tick labels.

    Reading the axis off the page rather than hard-coding it means the same
    code handles every sheet — and it caught that Kodachrome 25 and 64 are
    plotted over different exposure ranges, which a hard-coded axis would have
    silently mangled.
    """
    page = pymupdf.open(pdf_path)[page_index]
    x_ticks = _restore_signs(_numeric_ticks(page, horizontal=True, frame=frame), x_ascending)
    y_ticks = _restore_signs(_numeric_ticks(page, horizontal=False, frame=frame),
                             not y_ascending_upward)
    if len(x_ticks) < 2 or len(y_ticks) < 2:
        raise ValueError(f"not enough tick labels: x={x_ticks} y={y_ticks}")

    def fit(ticks):
        pos = np.array([p for p, _ in ticks])
        val = np.array([v for _, v in ticks])
        slope, intercept = np.polyfit(pos, val, 1)
        return slope, intercept

    xs, xi = fit(x_ticks)
    ys, yi = fit(y_ticks)
    return Axes(x0=frame.x0, y0=frame.y0, x1=frame.x1, y1=frame.y1,
                x_min=xs * frame.x0 + xi, x_max=xs * frame.x1 + xi,
                y_at_top=ys * frame.y0 + yi, y_at_bottom=ys * frame.y1 + yi)


def frames(pdf_path: str, page_index: int, min_size: float = 80.0) -> list[pymupdf.Rect]:
    """Candidate plot frames, largest first — useful when exploring a new sheet."""
    page = pymupdf.open(pdf_path)[page_index]
    rects = [p["rect"] for p in page.get_drawings() if any(i[0] == "re" for i in p["items"])]
    rects = [r for r in rects if r.width > min_size and r.height > min_size]
    return sorted(rects, key=lambda r: -r.width * r.height)
