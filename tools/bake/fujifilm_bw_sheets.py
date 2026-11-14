"""Fujifilm's NEOPAN 100 ACROS II, off its data sheet AF3-0258E (20.02).

Page 3 carries the plots, each an image with a transparency mask, read over
white at the image's own resolution:

- 13, characteristic curves, two plots: Microfine at 5, 10, 15 and 20 minutes
  and D-76 at 4, 7 and 10 minutes, small tank at 20 C, on Fujifilm's absolute
  axis of log H in lux-seconds (-4 to 1) and density (0 to 3);
- 14, spectral sensitivity: a "spectrogram to daylight (5400K)", relative log
  sensitivity, its only scale a bar marking one decade between two rules;
- 15, time-G curves: average gradient against development time, Microfine
  solid and D-76 dashed.

The text gives the rest: ISO 100, "orthopanchromatic"; EI 100 in Microfine
(10 min at 20 C) and in D-76 (7 1/4 min; EI 200 at 10 min); diffuse RMS
granularity 7 (Microfine, 48 um aperture, density 1.0 over minimum); resolving
power 60 lines/mm at 1.6:1 and 200 at 1000:1; filter factors for Wratten 8, 21
and 25 of 2, 4 and 8 in daylight, 1.5, 3 and 6 in tungsten light; no
reciprocity correction below 120 s, +1/2 stop from 120 to 1000 s; a grey
triacetate base. The sheet plots no MTF.

Each plot's frame and rules are found from its own lines (`_grid`); the
numbers along the axes are inside the image, so each rule is named by its
place in the sequence the labels give (checked: evenly spaced). Once the rules are
blanked, each curve is followed from where the curves stand apart (their
right ends, the time-G lines' left ends), column by column to the stroke
nearest where it was: the lettering beside the curves' ends lies further off
than that, and where the curves run together into one toe each follows the
shared stroke.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pymupdf

SHEET = Path(__file__).resolve().parents[2] / "film spec sheets" / "NEOPAN-100-ACROSII-135-3_data-sheet.pdf"
PAGE = 2
# (image xref, its mask's xref) on page 3
MICROFINE, D76, SPECTRAL, TIME_G = (18, 17), (24, 23), (22, 21), (20, 19)
MICROFINE_MINUTES = (20.0, 15.0, 10.0, 5.0)        # top to bottom at the curves' right ends
D76_MINUTES = (10.0, 7.0, 4.0)
# The developments Fujifilm gives for EI 100 (small tank, 20 C), its RMS figure's and its resolving power's.
MICROFINE_EI100_MINUTES = 10.0
D76_EI100_MINUTES = 7.25
RMS_GRANULARITY = 7.0
FILTER_FACTORS = {"daylight": {"W8": 2.0, "W21": 4.0, "W25": 8.0},
                  "tungsten": {"W8": 1.5, "W21": 3.0, "W25": 6.0}}


def ink(image: tuple[int, int]) -> np.ndarray:
    """The image's darkness, 0 (white) to 255 (black), composited over white
    through its mask."""
    doc = pymupdf.open(str(SHEET))
    pix = pymupdf.Pixmap(doc, image[0])
    if pix.colorspace is None or pix.colorspace.n != 3:
        pix = pymupdf.Pixmap(pymupdf.csRGB, pix)
    rgb = np.frombuffer(pix.samples, np.uint8).reshape(pix.height, pix.width, pix.n)[..., :3].astype(float)
    alpha = np.frombuffer(pymupdf.Pixmap(doc, image[1]).samples, np.uint8).reshape(pix.height, pix.width) / 255.0
    grey = (rgb.mean(axis=2) * alpha + 255.0 * (1 - alpha))
    return 255.0 - grey


def _bands(profile: np.ndarray, threshold: float) -> list[float]:
    idx = np.where(profile > threshold)[0]
    if idx.size == 0:
        return []
    groups = np.split(idx, np.where(np.diff(idx) > 2)[0] + 1)
    return [float(g.mean()) for g in groups]


def _grid(dark: np.ndarray, fill: float = 0.5) -> tuple[list[float], list[float]]:
    """Columns and rows that are dark over most of the plot: the frame and
    its rules. First the frame's extent from the longest lines, then the rules
    within it."""
    # the frame's edges run across 70-90% of the image; a line of lettering
    # is dark across a third of it at most
    cols = _bands(dark.sum(axis=0), 0.6 * dark.shape[0])
    rows = _bands(dark.sum(axis=1), 0.6 * dark.shape[1])
    left, right, top, bottom = cols[0], cols[-1], rows[0], rows[-1]
    inner = dark[int(top) + 3:int(bottom) - 2, int(left) + 3:int(right) - 2]
    xs = [left] + [c + int(left) + 3 for c in _bands(inner.sum(axis=0), fill * inner.shape[0])] + [right]
    ys = [top] + [r + int(top) + 3 for r in _bands(inner.sum(axis=1), fill * inner.shape[1])] + [bottom]
    return xs, ys


def _even(positions: list[float], values: np.ndarray):
    """The line through rules named in order, checked to be evenly spaced."""
    pos = np.asarray(positions, float)
    assert len(pos) == len(values), (positions, values)
    slope, intercept = np.polyfit(pos, values, 1)
    miss = np.abs(slope * pos + intercept - values).max()
    assert miss < 0.006 * np.ptp(values), f"rules not evenly spaced: {positions}"
    return lambda p: slope * np.asarray(p, float) + intercept


def _unruled(dark: np.ndarray, xs: list[float], ys: list[float], margin: int = 3) -> np.ndarray:
    """The ink inside the frame with its rules blanked, and `margin` pixels
    inside its edges (where a plot has tick marks)."""
    out = dark.copy()
    for x in xs:
        out[:, max(0, int(round(x)) - 2):int(round(x)) + 3] = False
    for y in ys:
        out[max(0, int(round(y)) - 2):int(round(y)) + 3, :] = False
    out[:int(ys[0]) + margin] = False
    out[int(ys[-1]) - margin + 1:] = False
    out[:, :int(xs[0]) + margin] = False
    out[:, int(xs[-1]) - margin + 1:] = False
    return out


def _runs(column: np.ndarray, weights: np.ndarray) -> list[tuple[int, int, float]]:
    """(start, end, ink-weighted centre) of each stroke in a column."""
    idx = np.where(column)[0]
    if idx.size == 0:
        return []
    out = []
    for g in np.split(idx, np.where(np.diff(idx) > 1)[0] + 1):
        w = weights[g]
        out.append((int(g[0]), int(g[-1]), float((g * w).sum() / w.sum())))
    return out


def _follow(ink: np.ndarray, darkness: np.ndarray, x0: int, y0: float, step: int, stop: int,
            window: float = 5.0, max_gap: int = 24) -> np.ndarray:
    """One curve, column by column from (x0, y0) in direction `step`: in
    each column the stroke nearest where the curve should be, if within
    `window` pixels of it (the stroke's own half-width added). Where it has
    none (a rule blanked, a dash's gap) the curve is carried on along its
    recent slope and the window widens with the gap; more than `max_gap`
    such columns in a row end it."""
    pts, y, gap = [], y0, 0
    for x in range(x0, stop, step):
        if len(pts) >= 6:
            recent = np.array(pts[-6:])
            slope = np.polyfit(recent[:, 0], recent[:, 1], 1)[0]
            expected = pts[-1][1] + slope * (x - pts[-1][0])
        else:
            expected = y
        reach = window + 0.5 * gap
        near = [r for r in _runs(ink[:, x], darkness[:, x]) if abs(r[2] - expected) <= reach + (r[1] - r[0]) / 2]
        if near:
            y = min(near, key=lambda r: abs(r[2] - expected))[2]
            pts.append((x, y))
            gap = 0
        else:
            gap += 1
            if gap > max_gap:
                break
    return np.array(pts, dtype=float)


def characteristic_curves(which: str = "Microfine") -> dict[float, np.ndarray]:
    """{minutes: (log H in lux-seconds, density)} off one of the two plots,
    each curve followed leftwards from the rightmost column where all of them
    stand apart, ordered top to bottom as the minutes printed beside them."""
    image, minutes = (MICROFINE, MICROFINE_MINUTES) if which == "Microfine" else (D76, D76_MINUTES)
    darkness = ink(image)
    dark = darkness > 110
    xs, ys = _grid(dark)
    # the frame and rules: log H -4.0 to 1.0 every 0.5; density 3.0 to 0 every 0.5
    x_of = _even(xs, np.arange(-4.0, 1.01, 0.5))
    y_of = _even(ys, np.arange(3.0, -0.01, -0.5))
    clean = _unruled(dark, xs, ys)
    start = next(x for x in range(int(xs[-1]) - 3, int(xs[0]), -1)
                 if len(_runs(clean[:, x], darkness[:, x])) == len(minutes)
                 and min(np.diff([r[2] for r in _runs(clean[:, x], darkness[:, x])])) > 25)
    seeds = [r[2] for r in _runs(clean[:, start], darkness[:, start])]
    out = {}
    for t, y0 in zip(minutes, seeds):
        # rightwards to the curve's own end, then leftwards to its start
        right = _follow(clean, darkness, start, y0, +1, int(xs[-1]))
        left = _follow(clean, darkness, start, y0, -1, int(xs[0]))
        track = np.concatenate([left[::-1], right[1:]]) if len(right) else left[::-1]
        pts = np.stack([x_of(track[:, 0]), y_of(track[:, 1])], axis=-1)
        out[t] = pts[np.argsort(pts[:, 0])]
    return out


def spectrogram() -> np.ndarray:
    """(wavelength nm, relative log sensitivity) as plotted: the spectrogram
    to daylight, its zero at the lower of the two rules a decade apart. Along
    the curve column by column from 400 nm while it is no steeper than its
    stroke is wide; its red edge, nearly upright, row by row."""
    darkness = ink(SPECTRAL)
    dark = darkness > 110
    xs, ys = _grid(dark)
    # rules at 400, 500, 600 and 700 nm between the frame's edges; two
    # horizontal rules a decade apart between its top and bottom
    nm_of = _even(xs[1:-1], np.array([400.0, 500.0, 600.0, 700.0]))
    upper, lower = ys[1], ys[2]
    decade = lower - upper
    clean = _unruled(dark, xs, ys)
    x0 = int(np.where(clean.any(axis=0))[0].min())
    first = _runs(clean[:, x0], darkness[:, x0])
    track = _follow(clean, darkness, x0, first[0][2], +1, int(xs[-1]))
    # where the stroke stands upright, the column runs grow long: the edge
    # from there, row by row, below the last point along the columns
    pts = []
    for x, y in track:
        s, e, c = min(_runs(clean[:, int(x)], darkness[:, int(x)]), key=lambda r: abs(r[2] - y))
        if e - s > 8:
            break
        pts.append((x, y))
    edge_x = int(pts[-1][0]) - 2
    rows = np.where(clean[:, edge_x:].any(axis=1))[0]
    for y in range(int(pts[-1][1]) + 1, int(rows.max()) + 1):
        run = np.where(clean[y, edge_x:])[0]
        if run.size:
            w = darkness[y, edge_x + run]
            pts.append((float((run * w).sum() / w.sum()) + edge_x, float(y)))
    pts = np.array(pts, dtype=float)
    out = np.stack([nm_of(pts[:, 0]), (lower - pts[:, 1]) / decade], axis=-1)
    order = np.argsort(out[:, 0], kind="stable")
    out = out[order]
    return out[np.concatenate([[True], np.diff(out[:, 0]) > 1e-6])]


def time_gradients() -> dict[str, np.ndarray]:
    """{"Microfine", "D-76": (minutes, average gradient)}: each line followed
    rightwards from its left end, D-76's dashed one (from 4 minutes, the
    upper) across its gaps."""
    darkness = ink(TIME_G)
    dark = darkness > 110
    xs, ys = _grid(dark)
    t_of = _even(xs, np.arange(0.0, 20.01, 4.0))
    g_of = _even(ys, np.arange(1.2, -0.01, -0.2))
    clean = _unruled(dark, xs, ys, margin=10)       # its ticks stand 8 px into the frame
    # the legend's samples and words, lower right, out of the way
    clean[int(ys[-3]) + 4:, int(xs[3]):] = False
    first = int(np.where(clean.any(axis=0))[0].min())               # D-76's left end, 4 min
    (dashed,) = _runs(clean[:, first], darkness[:, first])[:1]
    upper = _follow(clean, darkness, first, dashed[2], +1, int(xs[-1]), max_gap=20)
    later = next(x for x in range(first + 1, int(xs[-1]))
                 if any(r[2] > dashed[2] + 20 for r in _runs(clean[:, x], darkness[:, x])))
    solid = max(_runs(clean[:, later], darkness[:, later]), key=lambda r: r[2])
    lower = _follow(clean, darkness, later, solid[2], +1, int(xs[-1]))
    to = lambda tr: np.stack([t_of(tr[:, 0]), g_of(tr[:, 1])], axis=-1)  # noqa: E731
    return {"D-76": to(upper), "Microfine": to(lower)}


def average_gradient(curve: np.ndarray) -> float:
    """G-bar: from 0.10 over base + fog to 1.5 log H further, the slope of the
    line between them (the reading time-G plots of this kind give)."""
    e = np.linspace(curve[0, 0], curve[-1, 0], 3000)
    d = np.interp(e, *curve.T)
    net = d - d.min()
    lo = float(np.interp(0.10, net, e))
    return float((np.interp(lo + 1.5, e, net) - 0.10) / 1.5)
