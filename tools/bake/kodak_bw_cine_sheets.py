"""Kodak's black-and-white motion picture stocks, read off their technical data
sheets in `film spec sheets/`: EASTMAN DOUBLE-X Negative Film 5222 and its
release print film, KODAK Black-and-White Print Film 2302.

Double-X comes in two editions of H-1-5222, and both are read:

- 10/2003 ("5222-Double-X_v1.pdf"), vector plots. Its producer outlined every
  glyph, so the tick labels are shapes, not text; each plot is calibrated on
  its own gridlines (every 0.2 in log exposure and density, every 10 nm,
  every decade's steps on the MTF's logarithmic axes). It plots one
  characteristic curve, labelled 5 min; the spectral sensitivity at D = 0.3
  over gross fog (a solid stroke) and D = 1.0 (red dashes, each dash a filled
  shape, traced off a 1600 dpi render, column by column); and the MTF.
- 3-22 ("DOUBLEX-Technical-Data-EN_v2.pdf"), the same spectral sensitivity
  and MTF as bitmaps: laid over each other, the two editions' agree to the
  width of a line. It plots the characteristic curves for five development
  times in D-96 at 21 C, each labelled with its gamma: 4 min 0.50, 5 min
  0.56, 6 1/2 min 0.66, 9 min 0.84, 12 min 1.05. Their exposure index, EI
  250 daylight and 200 tungsten, is "for development to a gamma of 0.65",
  and the processing table develops "to recommended control gamma of 0.65 to
  0.70": the 6 1/2 minute curve, which measures 0.66 as traced.

  The 2003 edition's one curve follows the 3-22 edition's 5 minute curve
  through the toe and lower straight line, within 0.02 up to log H -1.4, and
  climbs above it from there, 0.11 denser at its top (log H -0.5). Which is
  the 2003 film's and which a redrawing cannot be told; the 3-22 edition's
  curves are taken, being the ones the exposure index refers to.

The 3-22 bitmap draws the five curves in one black, and they run together in
the toe. They never cross, though: top to bottom they are always 12, 9, 6 1/2,
5 and 4 minutes. Each column's ink runs are shared out among the five in that
order, every run taking at least one; of the possible ways, the one that
moves the curves least from the column to its right is taken, and a run
holding several curves spaces them across its width (`double_x_curves`).

2302 (H-1-2302, 3-22) draws its curves in colour (2, 3 1/2, 5, 7 and 9
minutes in D-97 at 21 C, diffuse visual), so each is traced by its colour.
Two of its figures are transparent images whose labels a direct extraction
loses, the spectral sensitivity and the net fog; those are read off a 300 dpi
render of the page. Its characteristic curves start at densities of 0.005 to
0.016, which no film on a polyester base reads gross: they are plotted net of
the base, and their minimum is the sheet's net fog (0.013 to 0.016 at those
times, on its net-fog plot). The base comes from the sheet's granularity
plot, which draws the 3 1/2 minute film's density curve gross (Status A
green): its toe less the net fog at 3 1/2 minutes.
"""

from __future__ import annotations

import itertools
from functools import lru_cache
from pathlib import Path

import numpy as np
import pymupdf

import datasheet as ds

SHEETS = Path(__file__).resolve().parents[2] / "film spec sheets"
DOUBLE_X_2003 = SHEETS / "5222-Double-X_v1.pdf"
DOUBLE_X_2022 = SHEETS / "DOUBLEX-Technical-Data-EN_v2.pdf"
PRINT_2302 = SHEETS / "Kodak 2302.pdf"


def _line(p0: float, v0: float, p1: float, v1: float):
    """Page or pixel coordinate to value, through two known points."""
    return lambda p: v0 + (np.asarray(p, float) - p0) * (v1 - v0) / (p1 - p0)


def _pixels(pdf: Path, page: int, index: int) -> np.ndarray:
    """An embedded image as RGB, 0-255."""
    doc = pymupdf.open(str(pdf))
    xref = doc[page].get_images(full=True)[index][0]
    pix = pymupdf.Pixmap(doc, xref)
    if pix.alpha or pix.n != 3:
        pix = pymupdf.Pixmap(pymupdf.csRGB, pix)
    return np.frombuffer(pix.samples, np.uint8).reshape(pix.height, pix.width, pix.n)[..., :3].astype(int)


def _render(pdf: Path, page: int, clip: tuple[float, float, float, float], dpi: int) -> np.ndarray:
    pix = pymupdf.open(str(pdf))[page].get_pixmap(dpi=dpi, clip=pymupdf.Rect(*clip))
    return np.frombuffer(pix.samples, np.uint8).reshape(pix.height, pix.width, pix.n)[..., :3].astype(int)


# --------------------------------------------------------------------------
# Double-X, 2003 edition: page 3, vector
# --------------------------------------------------------------------------

# Each plot's gridlines, in page points: first and last, and what they mean.
_CURVE_X = _line(53.04, -4.0, 276.53, 2.0)         # log H, lux-seconds, every 0.2
_CURVE_Y = _line(305.48, 0.0, 178.05, 3.0)         # density, every 0.2
_SPECTRAL_X = _line(325.87, 250.0, 547.80, 750.0)  # nm, every 10
_SPECTRAL_Y = _line(314.75, -1.0, 169.99, 3.0)     # log sensitivity, every 0.2
_MTF_X = _line(53.36, 0.0, 276.14, 3.0)            # log10 cycles/mm
_MTF_Y = _line(563.05, 0.0, 416.56, 3.0)           # log10 response %


def _stroke(near: tuple[float, float], width: float) -> np.ndarray:
    """The stroked path of `width` whose bounding box starts at `near`."""
    page = pymupdf.open(str(DOUBLE_X_2003))[2]
    for d in page.get_drawings():
        r = d["rect"]
        if (d["type"] == "s" and abs((d.get("width") or 0) - width) < 0.02
                and abs(r.x0 - near[0]) < 0.6 and abs(r.y0 - near[1]) < 0.6):
            return ds._flatten_path([i for i in d["items"] if i[0] in ("l", "c")], steps=48)
    raise ValueError(f"no stroke of width {width} at {near} on page 3 of {DOUBLE_X_2003.name}")


def _by_x(points: np.ndarray) -> np.ndarray:
    points = points[np.argsort(points[:, 0], kind="stable")]
    return points[np.concatenate([[True], np.diff(points[:, 0]) > 1e-9])]


def _rising(points: np.ndarray, window: int = 5) -> np.ndarray:
    """A traced characteristic curve with stray columns taken out: a running
    median over `window` samples, then the closest non-decreasing curve (pool
    adjacent violators). Where two coloured strokes cross, anti-aliasing mixes
    their inks and a column or two lands on the wrong one."""
    from scipy.ndimage import median_filter
    y = median_filter(points[:, 1], size=window, mode="nearest")
    values, weights = [], []
    for v in y:
        values.append(float(v)); weights.append(1)
        while len(values) > 1 and values[-2] > values[-1]:
            w = weights[-2] + weights[-1]
            values[-2:] = [(values[-2] * weights[-2] + values[-1] * weights[-1]) / w]
            weights[-2:] = [w]
    return np.stack([points[:, 0], np.repeat(values, weights)], axis=-1)


@lru_cache(maxsize=None)
def double_x_2003_curve() -> np.ndarray:
    """The 2003 edition's characteristic curve, labelled 5 min: (log H, Status M
    blue density), base density 0.20 by the plot's own note."""
    p = _stroke((79.4, 241.5), 0.74)
    return _by_x(np.stack([_CURVE_X(p[:, 0]), _CURVE_Y(p[:, 1])], axis=-1))


@lru_cache(maxsize=None)
def double_x_spectral_d03() -> np.ndarray:
    """(nm, log sensitivity) at D = 0.3 over gross fog: a solid stroke."""
    p = _stroke((392.6, 197.7), 0.80)
    return _by_x(np.stack([_SPECTRAL_X(p[:, 0]), _SPECTRAL_Y(p[:, 1])], axis=-1))


@lru_cache(maxsize=None)
def double_x_spectral_d10() -> np.ndarray:
    """(nm, log sensitivity) at D = 1.0 over gross fog: red dashes, each a
    filled shape, so traced off a 1600 dpi render, one point a column."""
    clip, dpi = (398.0, 242.0, 512.0, 306.0), 1600
    a = _render(DOUBLE_X_2003, 2, clip, dpi)
    red = (a[..., 0] > 150) & (a[..., 1] < 110) & (a[..., 2] < 110)
    s = dpi / 72
    cols = np.where(red.any(axis=0))[0]
    x = clip[0] + (cols + 0.5) / s
    y = np.array([clip[1] + (np.where(red[:, c])[0].mean() + 0.5) / s for c in cols])
    return _by_x(np.stack([_SPECTRAL_X(x), _SPECTRAL_Y(y)], axis=-1))


def double_x_spectral() -> np.ndarray:
    """The D = 1.0 curve, the one Kodak's black-and-white profiles here take,
    drawn from 420 to 660 nm, continued to the D = 0.3 curve's ends (400 and
    670 nm) by that curve's own shape, joined where the D = 1.0 curve stops: the
    sheet's own data for the sensitivity it does not draw at D = 1.0. Nothing
    is plotted further out."""
    d10, d03 = double_x_spectral_d10(), double_x_spectral_d03()
    lo, hi = d10[0], d10[-1]
    below = d03[d03[:, 0] < lo[0]].copy()
    below[:, 1] += lo[1] - np.interp(lo[0], d03[:, 0], d03[:, 1])
    above = d03[d03[:, 0] > hi[0]].copy()
    above[:, 1] += hi[1] - np.interp(hi[0], d03[:, 0], d03[:, 1])
    return np.concatenate([below, d10, above])


@lru_cache(maxsize=None)
def double_x_mtf() -> np.ndarray:
    """(cycles/mm, response as a fraction): tungsten, D-96 to the recommended
    control gamma, diffuse visual."""
    p = _stroke((82.8, 458.1), 0.80)
    return _by_x(np.stack([10 ** _MTF_X(p[:, 0]), 10 ** _MTF_Y(p[:, 1]) / 100], axis=-1))


# --------------------------------------------------------------------------
# Double-X, 3-22 edition: page 3, the development series, a bitmap
# --------------------------------------------------------------------------

DOUBLE_X_TIMES = (12.0, 9.0, 6.5, 5.0, 4.0)       # top to bottom, as drawn
DOUBLE_X_GAMMAS = {4.0: 0.50, 5.0: 0.56, 6.5: 0.66, 9.0: 0.84, 12.0: 1.05}


@lru_cache(maxsize=None)
def double_x_curves() -> dict[float, np.ndarray]:
    """{minutes: (log H in lux-seconds, Status M blue density)}.

    The frame (x 101-668, y 666-100 px) spans log H -3 to +1 and density 0 to
    4; its tick marks fall at log H -2, -1, 0 and density 1, 2, 3 within half a
    pixel of that. The inset (time-gamma, time-fog) and the legend are left
    out; so are the frame's own tick marks."""
    ink = 255.0 - _pixels(DOUBLE_X_2022, 2, 0).mean(axis=2)
    left, right, top, bottom = 101, 668, 100, 666
    x_of, y_of = _line(left, -3.0, right, 1.0), _line(bottom, 0.0, top, 4.0)
    ticks = (241, 383, 524)
    stroke = 2.0

    def runs(x: int, lo: int, hi: int) -> list[tuple[int, int, float]]:
        col, out, y = ink[:, x], [], lo
        while y < hi:
            if col[y] > 110:
                s = y
                while y < hi and col[y] > 110:
                    y += 1
                w = col[s:y]
                centre = float((np.arange(s, y) * w).sum() / w.sum())
                if not (x < left + 9 and any(abs(centre - t) < 3 for t in ticks)):
                    out.append((s, y - 1, centre))
            y += 1
        return out

    def splits(n: int, k: int):
        for cuts in itertools.combinations(range(1, n), k - 1):
            edges = (0,) + cuts + (n,)
            yield [edges[i + 1] - edges[i] for i in range(k)]

    def placed(found, split) -> np.ndarray:
        ys = []
        for (s, e, centre), k in zip(found, split):
            ys += [centre] if k == 1 else list(np.linspace(s + (stroke - 1) / 2, e - (stroke - 1) / 2, k))
        return np.asarray(ys)

    track: dict[float, list[tuple[int, float]]] = {t: [] for t in DOUBLE_X_TIMES}
    previous = None
    for x in range(465, left, -1):
        lo, hi = (412 if 112 <= x <= 302 else 252), 662     # below the inset and the legend
        if x < 120:
            lo, hi = 600, 650                               # the far toe's band, density 0.11-0.47
        found = runs(x, lo, hi)
        if not 1 <= len(found) <= len(DOUBLE_X_TIMES):
            continue
        options = [placed(found, s) for s in splits(len(DOUBLE_X_TIMES), len(found))]
        best = options[0] if previous is None else min(options, key=lambda ys: float(((ys - previous) ** 2).sum()))
        previous = best
        for t, y in zip(DOUBLE_X_TIMES, best):
            track[t].append((x, y))
    return {t: _by_x(np.array([(x_of(x), y_of(y)) for x, y in track[t]])) for t in DOUBLE_X_TIMES}


def slope_gamma(curve: np.ndarray, lo: float = -2.0, hi: float = -1.0) -> float:
    """A negative's gamma as the straight line's slope (density a decade)."""
    return float((np.interp(hi, curve[:, 0], curve[:, 1]) - np.interp(lo, curve[:, 0], curve[:, 1])) / (hi - lo))


# --------------------------------------------------------------------------
# 2302
# --------------------------------------------------------------------------

PRINT_2302_TIMES = (2.0, 3.5, 5.0, 7.0)


def _time_plot(clip: tuple[float, float, float, float], n_y: int, y_range: tuple[float, float],
               name: str) -> np.ndarray:
    """(minutes, value): one of page 4's plots against development time, 2 to 10
    minutes, a transparent image read off a 300 dpi render. Its rules: five
    upright ones, every 2 minutes, and `n_y` level ones over `y_range`; the
    curve is the blue line."""
    a = _render(PRINT_2302, 3, clip, 300)
    blue = (a[..., 2] > 150) & (a[..., 0] < 130) & (a[..., 1] < 130)
    neutral = (a.mean(axis=2) < 200) & ((a.max(axis=2) - a.min(axis=2)) < 30)
    h, w = neutral.shape
    vx = [int(x) for x in np.where(neutral.sum(axis=0) > 0.3 * h)[0]]
    x_rules = [float(np.mean(g)) for g in np.split(np.array(vx), np.where(np.diff(vx) > 3)[0] + 1)]
    # Level rules run the frame's width, which the heading's lettering does not.
    inside = neutral[:, int(x_rules[0]) + 2:int(x_rules[-1]) - 1]
    hy = [int(y) for y in np.where(inside.mean(axis=1) > 0.9)[0]]
    y_rules = [float(np.mean(g)) for g in np.split(np.array(hy), np.where(np.diff(hy) > 3)[0] + 1)]
    if len(x_rules) != 5 or len(y_rules) != n_y:
        raise ValueError(f"{name} plot: expected 5 x and {n_y} y rules, found {len(x_rules)} and {len(y_rules)}")
    x_of = _line(x_rules[0], 2.0, x_rules[-1], 10.0)
    y_of = _line(y_rules[-1], y_range[0], y_rules[0], y_range[1])
    cols = np.where(blue.any(axis=0))[0]
    return _by_x(np.stack([x_of(cols), y_of(np.array([np.where(blue[:, c])[0].mean() for c in cols]))], axis=-1))


@lru_cache(maxsize=None)
def print_2302_gamma_plot() -> np.ndarray:
    """(minutes, gamma): the sheet's gamma against development time in D-97 (HQ
    and AA alike) at 21 C, page 4's first plot; rules every 0.5 from 1 to 4."""
    return _time_plot((30, 40, 290, 240), 7, (1.0, 4.0), "gamma")


def print_2302_minutes_for(gamma: float) -> float:
    """The development time the sheet's gamma plot gives a gamma."""
    plot = print_2302_gamma_plot()
    return float(np.interp(gamma, np.maximum.accumulate(plot[:, 1]), plot[:, 0]))


@lru_cache(maxsize=None)
def print_2302_curves() -> dict[float, np.ndarray]:
    """{minutes: (log H in lux-seconds, diffuse visual density over the base)}.

    Page 3's second image: calibrated on its gridlines, every 0.2 in log H
    (x 43 = -3.0 to 698 = +3.0 px) and in density (y 461.5 = 0 to 89 = 4.0);
    the gridlines fit a straight scale to 0.005. Each curve is taken by its
    colour; the legend is left out. 9 minutes, black and dashed among black
    rules, is not needed and not traced."""
    a = _pixels(PRINT_2302, 2, 1)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    x_cols = np.array([43, 108, 130, 152, 174, 217, 239, 261, 283, 305, 327, 349, 371, 392, 414, 436, 458, 480,
                       502, 567, 589, 611, 698])
    x_vals = np.array([-3.0, -2.4, -2.2, -2.0, -1.8, -1.4, -1.2, -1.0, -0.8, -0.6, -0.4, -0.2, 0.0, 0.2, 0.4,
                       0.6, 0.8, 1.0, 1.2, 1.8, 2.0, 2.2, 3.0])
    y_rows = np.array([89, 108, 126, 145, 164, 182, 201, 219, 238, 257, 275, 294, 312, 331, 350, 368, 387, 406,
                       424, 443, 461.5])
    y_vals = np.round(np.arange(4.0, -0.01, -0.2), 1)
    kx, ky = np.polyfit(x_cols, x_vals, 1), np.polyfit(y_rows, y_vals, 1)
    colours = {
        2.0: (r > 90) & (r < 180) & (g < 60) & (b < 60),             # dark red, dash-dot
        3.5: (r > 180) & (g < 110) & (b < 110),                      # red, dashed
        5.0: (b > 150) & (r < 130) & (g < 130),                      # blue, solid
        7.0: (r - b > 50) & (g - b > 50) & (np.abs(r - g) < 45) & (r < 210),   # olive, dash-dot
    }
    legend = np.zeros(r.shape, bool)
    legend[110:200, 70:175] = True
    out = {}
    for t, mask in colours.items():
        mask = mask & ~legend
        cols = np.where(mask.any(axis=0))[0]
        rows = np.array([np.median(np.where(mask[:, c])[0]) for c in cols])
        out[t] = _rising(_by_x(np.stack([np.polyval(kx, cols), np.polyval(ky, rows)], axis=-1)))
    return out


def kodak_print_gamma(curve: np.ndarray) -> float:
    """2302's control gamma as the sheet defines it: "D-min + 0.95 density to
    + 0.50 log E", the rise over the half decade from where the curve reaches
    D-min + 0.95."""
    e, d = curve[:, 0], curve[:, 1]
    d_min = float(d[e < e.min() + 0.6].min())
    e1 = float(np.interp(d_min + 0.95, np.maximum.accumulate(d), e))
    return float((np.interp(e1 + 0.5, e, d) - (d_min + 0.95)) / 0.5)


@lru_cache(maxsize=None)
def print_2302_spectral() -> np.ndarray:
    """(nm, log sensitivity) at D = 0.2 over D-min: 0.10 s tungsten exposure,
    D-97 to the recommended control gamma, visual density. Off a 300 dpi render
    of page 3 (the image is transparent); gridlines every 10 nm and 0.2."""
    a = _render(PRINT_2302, 2, (320, 540, 577, 738), 300)
    blue = (a[..., 2] > 150) & (a[..., 0] < 130) & (a[..., 1] < 130)
    blue[560:700, 150:420] = False                                   # the legend
    cols = np.where(blue.any(axis=0))[0]
    rows = np.array([np.where(blue[:, c])[0].mean() for c in cols])
    nm = _line(156.5, 250.0, 967.5, 750.0)(cols)
    log_s = _line(200.0, 1.0, 697.0, -3.0)(rows)
    return _by_x(np.stack([nm, log_s], axis=-1))


@lru_cache(maxsize=None)
def print_2302_net_fog(minutes: float) -> float:
    """The net fog (D-min less base) in D-97 (HQ) at 21 C, page 4's third plot;
    rules every 0.01 from 0.01 to 0.09."""
    plot = _time_plot((34, 330, 290, 528), 9, (0.01, 0.09), "net fog")
    return float(np.interp(minutes, plot[:, 0], plot[:, 1]))


@lru_cache(maxsize=None)
def print_2302_base() -> tuple[float, float, float]:
    """(base, the granularity plot's toe, the net fog it is less): the clear
    ESTAR base's density. The granularity plot (page 3, first image; Status A
    green, D-97 3 1/2 min) draws the film's density curve gross; its toe, less
    the net fog at 3 1/2 minutes, is the base. Its rules: density 0 at y 485,
    4.0 at y 97."""
    a = _pixels(PRINT_2302, 2, 0)
    blue = (a[..., 2] > 150) & (a[..., 0] < 120) & (a[..., 1] < 120)
    # Left of relative log E 0.8 the solid density curve is flat and lowest;
    # the dashed granularity curve lies above it there.
    lows = [np.where(blue[:, x])[0].max() for x in range(150, 200) if blue[:, x].any()]
    toe = (485.0 - float(np.median(lows))) / 97.0
    fog = print_2302_net_fog(3.5)
    return toe - fog, toe, fog
