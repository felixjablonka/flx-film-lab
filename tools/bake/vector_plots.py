"""Curves out of a datasheet's vector plots, by an explicit description of each plot.

`datasheet.py` finds a plot frame and calibrates it from its tick labels, which
suits Kodak's sheets. Fujifilm's draw some plots so that does not work: all
three spectral curves as one path, a characteristic curve in two Bezier
segments, a sensitivity axis with no numbers on it, only a bar marking one
decade. So each plot here is described by what can be read off the page itself
— its frame, and gridlines whose values are known — and its curves are picked
by stroke colour, by dash pattern, or by being the plot's curve-shaped path.
Coordinates are PDF points, as `pymupdf` reports them.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pymupdf

from datasheet import _flatten_path


@dataclass(frozen=True)
class VectorPlot:
    pdf: str
    page: int                                      # 0-based
    frame: tuple[float, float, float, float]       # x0, y0, x1, y1
    x_rules: tuple[tuple[float, float], ...]       # (position, value) along x
    y_rules: tuple[tuple[float, float], ...]       # (position, value) along y


def _fit(rules):
    pos = np.array([r[0] for r in rules])
    val = np.array([r[1] for r in rules])
    slope, intercept = np.polyfit(pos, val, 1)
    return slope, intercept, float(np.abs(slope * pos + intercept - val).max())


def _to_data(plot: VectorPlot, points: np.ndarray) -> np.ndarray:
    ax, bx, _ = _fit(plot.x_rules)
    ay, by, _ = _fit(plot.y_rules)
    return np.stack([ax * points[:, 0] + bx, ay * points[:, 1] + by], axis=-1)


def _inside(plot: VectorPlot, rect, slack: float = 2.0) -> bool:
    x0, y0, x1, y1 = plot.frame
    return (x0 - slack <= rect.x0 and rect.x1 <= x1 + slack
            and y0 - slack <= rect.y0 and rect.y1 <= y1 + slack)


def _subpaths(items) -> list[list]:
    """A path's line and curve items split wherever the pen lifts."""
    runs, current, last = [], [], None
    for item in items:
        if item[0] not in ("l", "c"):
            continue
        start = item[1]
        if last is not None and (abs(start.x - last.x) > 0.05 or abs(start.y - last.y) > 0.05):
            runs.append(current)
            current = []
        current.append(item)
        last = item[-1]
    if current:
        runs.append(current)
    return runs


def _curve_paths(plot: VectorPlot, polylines: bool = False):
    """Stroked paths inside the plot that are curves: they bend (rules and
    ticks are straight lines), or, for a sheet that draws its curves as long
    polylines, they run a third of the plot's width in many segments."""
    page = pymupdf.open(plot.pdf)[plot.page]
    width = plot.frame[2] - plot.frame[0]
    for path in page.get_drawings():
        if path.get("type") != "s" or not _inside(plot, path["rect"]):
            continue
        items = [i for i in path["items"] if i[0] in ("l", "c")]
        if any(i[0] == "c" for i in items) or (polylines and len(items) >= 12
                                               and path["rect"].width > width / 3):
            yield path, items


def by_colour(plot: VectorPlot, colours: dict[str, tuple[float, float, float]],
              tolerance: float = 0.05) -> dict[str, np.ndarray]:
    """The curves stroked in each named colour, solid ones only."""
    out: dict[str, np.ndarray] = {}
    for path, items in _curve_paths(plot):
        if path.get("dashes") not in (None, "[] 0"):
            continue
        colour = path.get("color") or ()
        for name, want in colours.items():
            if len(colour) == 3 and max(abs(a - b) for a, b in zip(colour, want)) < tolerance:
                out[name] = _to_data(plot, _flatten_path(items, steps=32))
    missing = set(colours) - set(out)
    if missing:
        raise ValueError(f"{plot.pdf} page {plot.page + 1}: no curve stroked in {sorted(missing)}")
    return out


def by_position(plot: VectorPlot, names: tuple[str, ...], drop_neutral: bool = False,
                solid_only: bool = False) -> dict[str, np.ndarray]:
    """Curves drawn as sub-paths of one black path, named in order of where they peak along x.

    `drop_neutral` leaves out a visual neutral drawn among the dye curves, as
    Kodak's spectral dye density plots do: the one curve that never falls
    low across the middle of the plot. `solid_only` leaves out dashed curves,
    such as the sensitivity of Fujifilm's fourth, cyan-sensitive layer.
    """
    runs = []
    for path, items in _curve_paths(plot):
        if solid_only and path.get("dashes") not in (None, "[] 0"):
            continue
        for run in _subpaths(items):
            data = _to_data(plot, _flatten_path(run, steps=32))
            if np.ptp(data[:, 0]) > 0.05 * np.ptp([r[1] for r in plot.x_rules]):
                runs.append(data)
    if drop_neutral and len(runs) == len(names) + 1:
        lo, hi = np.percentile([r[1] for r in plot.x_rules], [25, 75])
        def floor(d):
            inside = (d[:, 0] >= lo) & (d[:, 0] <= hi)
            return d[inside, 1].min() if inside.any() else -np.inf
        runs.pop(int(np.argmax([floor(r) for r in runs])))
    if len(runs) != len(names):
        raise ValueError(f"{plot.pdf} page {plot.page + 1}: {len(runs)} curves, expected {len(names)}")
    runs.sort(key=lambda d: d[np.argmax(d[:, 1]), 0])
    return dict(zip(names, runs))


def calibration_report(plot: VectorPlot) -> str:
    _, _, ex = _fit(plot.x_rules)
    _, _, ey = _fit(plot.y_rules)
    return f"rules fit to {ex:.3g} (x) and {ey:.3g} (y) in data units"


def by_dashes(plot: VectorPlot, patterns: dict[str, tuple[float, ...]],
              polylines: bool = False) -> dict[str, np.ndarray]:
    """Curves drawn alike but for their dash pattern (`()` for solid); with
    `polylines`, also curves drawn as long chains of straight segments."""
    out: dict[str, np.ndarray] = {}
    for path, items in _curve_paths(plot, polylines):
        text = path.get("dashes") or "[] 0"
        dash = tuple(round(float(v), 1) for v in text[text.index("[") + 1:text.index("]")].split())
        for name, want in patterns.items():
            if dash == tuple(round(v, 1) for v in want):
                out[name] = _to_data(plot, _flatten_path(items, steps=32))
    missing = set(patterns) - set(out)
    if missing:
        raise ValueError(f"{plot.pdf} page {plot.page + 1}: no curve dashed as {sorted(missing)}")
    return out


def joined(plot: VectorPlot, tolerance: float = 0.3) -> list[np.ndarray]:
    """Every curve in the plot as data, its strokes joined end to end.

    Kodak's sheets of the 1990s draw a curve in several strokes — part of one
    in the same path as part of another, the steep end of a sensitivity as a
    path of its own — so strokes are collected wherever they lie and chained
    where one ends within `tolerance` points of another's end. Straight rules
    and ticks, which have no extent across one axis, are left out; so are
    dots.
    """
    page = pymupdf.open(plot.pdf)[plot.page]
    strokes = []
    for path in page.get_drawings():
        if path.get("type") != "s" or not _inside(plot, path["rect"]):
            continue
        for run in _subpaths([i for i in path["items"] if i[0] in ("l", "c")]):
            points = _flatten_path(run, steps=32)
            if np.ptp(points[:, 0]) >= 0.5 and np.ptp(points[:, 1]) >= 0.5:
                strokes.append(points)

    def near(p, q):
        return float(np.hypot(*(p - q))) < tolerance

    merged = True
    while merged:
        merged = False
        for i in range(len(strokes)):
            for j in range(len(strokes)):
                a, b = strokes[i], strokes[j]
                if i == j:
                    continue
                if near(a[-1], b[0]):
                    chain = np.concatenate([a, b[1:]])
                elif near(a[-1], b[-1]):
                    chain = np.concatenate([a, b[::-1][1:]])
                elif near(a[0], b[0]):
                    chain = np.concatenate([a[::-1], b[1:]])
                else:
                    continue
                strokes = [s for k, s in enumerate(strokes) if k not in (i, j)] + [chain]
                merged = True
                break
            if merged:
                break
    curves = [_to_data(plot, s) for s in strokes]
    return [c[np.argsort(c[:, 0], kind="stable")] for c in curves]


def joined_by_value(plot: VectorPlot, names: tuple[str, ...], at_x: float) -> dict[str, np.ndarray]:
    """Joined curves (`joined`) named top to bottom by their value at `at_x`."""
    curves = [c for c in joined(plot) if c[0, 0] <= at_x <= c[-1, 0]]
    if len(curves) != len(names):
        raise ValueError(f"{plot.pdf} page {plot.page + 1}: {len(curves)} curves at x={at_x}, expected {len(names)}")
    return dict(zip(names, sorted(curves, key=lambda c: float(np.interp(at_x, c[:, 0], c[:, 1])), reverse=True)))


def joined_by_peak(plot: VectorPlot, names: tuple[str, ...]) -> dict[str, np.ndarray]:
    """Joined curves (`joined`) named in order of where they peak along x."""
    curves = joined(plot)
    if len(curves) != len(names):
        raise ValueError(f"{plot.pdf} page {plot.page + 1}: {len(curves)} curves, expected {len(names)}")
    return dict(zip(names, sorted(curves, key=lambda c: c[np.argmax(c[:, 1]), 0])))


def by_order(plot: VectorPlot, names: tuple[str, ...], at_x: float, polylines: bool = False) -> dict[str, np.ndarray]:
    """Curves named top to bottom by their value at `at_x` (data units); a path
    the pen lifts within counts as one curve per stroke."""
    curves = [_to_data(plot, _flatten_path(run, steps=32))
              for _, items in _curve_paths(plot, polylines) for run in _subpaths(items)]
    curves = [c for c in curves if c[:, 0].min() <= at_x <= c[:, 0].max()]
    if len(curves) != len(names):
        raise ValueError(f"{plot.pdf} page {plot.page + 1}: {len(curves)} curves at x={at_x}, expected {len(names)}")
    value = lambda c: float(np.interp(at_x, *zip(*sorted(zip(c[:, 0], c[:, 1])))))   # noqa: E731
    return dict(zip(names, sorted(curves, key=value, reverse=True)))
