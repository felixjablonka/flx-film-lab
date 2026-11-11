"""Polaroid's instant peel-apart print films, read off Polaroid Corporation's
data sheets of 1999 (`film spec sheets/553fds.pdf`, `554fds.pdf`,
`559fds.pdf`): Type 553 (T-53, T-553, T-803), Polapan Pro 100 (T-54, T-554,
T-664, T-804) and Polacolor 559, whose sheet covers the Polacolor ER family
(T-88, T-108, T-669, T-669S, T-559, T-559S, T-59, T-809).

Each sheet is a chart drawing: every curve a coloured stroke drawn in pieces
(a dashed curve as many short ones), some running on past the plot's frame
under a clipping path, with a short legend sample of each colour inside the
frame. A curve is every stroke of its colour inside the frame, the legend's
corner left out, clipped to the frame and sorted along x.

The frames and the inner ticks of the large charts do not quite agree: the
frame's edges, labelled 0 and 3 (density), sit up to 8 points (Polapan's)
from where the ticks at 1 and 2 put them on a uniform scale; the small
charts' agree to a fifth of a point. Each axis is read on a straight line
fitted to its frame's edges and its ticks together, within 0.04 in density
of either reading. Nothing on the sheets settles it: Polapan draws its 45
second, 70 F curve twice, in its temperature plot and its development-time
plot, and the two differ by up to 0.26 in density, as a shift of about 0.2
decades, whichever way either is read; and Type 553's stated D-max and slope
at 71 F (1.75 and 1.64) are below what its curve plots at 70 F (1.88 and
1.80) on any reading.

The characteristic curves plot the print's density ("Sample Density") against
relative log exposure, at 70 F and at the cold and hot temperatures the
sheets name (55 F and 95 F; 13 and 35 C); the spectral sensitivities, "the
equivalent energy needed at each wavelength ... to produce a neutral density
of .75", on a log scale whose bottom rule, labelled 0, is 0.1. The two
black-and-white sheets plot the print's MTF up to 4 cycles/mm, on a log
frequency axis. Every function returns arrays of (x, y) in data units.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pymupdf

from datasheet import _flatten_path
from vector_plots import _subpaths

SHEETS = Path(__file__).resolve().parents[2] / "film spec sheets"
T553 = SHEETS / "553fds.pdf"
T554 = SHEETS / "554fds.pdf"
T559 = SHEETS / "559fds.pdf"

RED, GREEN, BLUE = (1.0, 0.0, 0.0), (0.0, 0.5, 0.0), (0.0, 0.0, 1.0)

# The development temperatures the sheets plot, F and C.
COLD_F, NORMAL_F, HOT_F = 55.0, 70.0, 95.0


def f_to_c(f: float) -> float:
    return (f - 32.0) * 5.0 / 9.0


class Axis:
    """A linear map from a page coordinate to a value, fitted to known (position, value) pairs."""

    def __init__(self, pairs, log: bool = False):
        pos = np.array([p for p, _ in pairs], float)
        val = np.array([v for _, v in pairs], float)
        self.log = log
        self.k, self.c = np.polyfit(pos, np.log10(val) if log else val, 1)

    def __call__(self, p):
        v = self.k * np.asarray(p, float) + self.c
        return 10 ** v if self.log else v


def _colour_is(colour, want, tolerance=0.08) -> bool:
    return colour is not None and len(colour) == 3 and max(abs(a - b) for a, b in zip(colour, want)) < tolerance


def _strokes(pdf: Path, page: int, frame, colour, exclude=()) -> np.ndarray:
    """Every stroke of `colour` inside `frame` (x0, y0, x1, y1), its points
    clipped to the frame and sorted along x; `exclude` boxes (the legend)
    left out."""
    # A curve's last point can sit a few hundredths of a point past the frame
    # it ends on: half a point's slack keeps it.
    x0, y0, x1, y1 = frame[0] - 0.5, frame[1] - 0.5, frame[2] + 0.5, frame[3] + 0.5
    points = []
    for path in pymupdf.open(str(pdf))[page].get_drawings():
        if path.get("type") != "s" or not _colour_is(path.get("color"), colour):
            continue
        r = path["rect"]
        if r.x1 < x0 or r.x0 > x1 or r.y1 < y0 or r.y0 > y1:
            continue
        for run in _subpaths([i for i in path["items"] if i[0] in ("l", "c")]):
            pts = _flatten_path(run, steps=16)
            keep = (pts[:, 0] >= x0) & (pts[:, 0] <= x1) & (pts[:, 1] >= y0) & (pts[:, 1] <= y1)
            for bx0, by0, bx1, by1 in exclude:
                keep &= ~((pts[:, 0] >= bx0) & (pts[:, 0] <= bx1) & (pts[:, 1] >= by0) & (pts[:, 1] <= by1))
            if keep.sum() >= 2:
                points.append(pts[keep])
    if not points:
        raise ValueError(f"{pdf.name} page {page + 1}: no stroke coloured {colour} in {frame}")
    pts = np.concatenate(points)
    return pts[np.argsort(pts[:, 0], kind="stable")]


def _curve(pdf, page, frame, colour, x: Axis, y: Axis, exclude=()) -> np.ndarray:
    pts = _strokes(pdf, page, frame, colour, exclude)
    data = np.stack([x(pts[:, 0]), y(pts[:, 1])], axis=-1)
    # One value per abscissa: strokes drawn twice over the same stretch, or a
    # dash's two ends, are averaged.
    xs, inverse = np.unique(np.round(data[:, 0], 4), return_inverse=True)
    ys = np.bincount(inverse, weights=data[:, 1]) / np.bincount(inverse)
    return np.stack([xs, ys], axis=-1)


# --- the characteristic curves -------------------------------------------------
# Frames, inner ticks and legend corners, in PDF points, read off each page.

T553_CURVES = dict(page=1, frame=(254.8, 205.6, 531.5, 482.0), legend=((393.0, 222.0, 498.5, 296.0),),
                   x=Axis([(254.8, -3.0), (345.72, -2.0), (440.88, -1.0), (531.5, 0.0)]),
                   y=Axis([(482.0, 0.0), (391.32, 1.0), (296.28, 2.0), (205.6, 3.0)]),
                   colours={COLD_F: RED, NORMAL_F: GREEN, HOT_F: BLUE})
T554_CURVES = dict(page=1, frame=(248.0, 245.9, 543.4, 528.5), legend=((423.0, 275.0, 518.5, 339.5),),
                   x=Axis([(248.0, -3.0), (346.2, -2.0), (445.32, -1.0), (543.4, 0.0)]),
                   y=Axis([(528.5, 0.0), (436.68, 1.0), (336.84, 2.0), (245.9, 3.0)]),
                   colours={COLD_F: BLUE, NORMAL_F: GREEN, HOT_F: RED})
# Polapan's development-time plot at 70 F: 30, 45 and 180 seconds.
T554_TIMES = dict(page=1, frame=(61.7, 596.6, 179.5, 714.2), legend=((100.0, 600.0, 160.0, 635.0),),
                  colours={30.0: BLUE, 45.0: GREEN, 180.0: RED})
# Polacolor: one plot per temperature, each with its blue, green and red.
T559_CURVES = {
    NORMAL_F: dict(page=1, frame=(265.2, 218.5, 527.3, 472.7), legend=((420.0, 240.0, 480.0, 275.0),),
                   x=Axis([(265.2, -3.0), (351.96, -2.0), (438.72, -1.0), (527.3, 0.0)]),
                   y=Axis([(472.7, 0.0), (387.48, 1.0), (304.68, 2.0), (218.5, 3.0)])),
    COLD_F: dict(page=1, frame=(239.4, 588.8, 366.0, 712.1), legend=((315.0, 597.0, 345.0, 617.0),),
                 x=Axis([(239.4, -3.0), (281.04, -2.0), (322.68, -1.0), (366.0, 0.0)]),
                 y=Axis([(712.1, 0.0), (671.04, 1.0), (630.0, 2.0), (588.8, 3.0)])),
    HOT_F: dict(page=1, frame=(422.8, 588.6, 550.2, 711.7), legend=((500.0, 598.0, 530.0, 618.0),),
                x=Axis([(422.8, -3.0), (465.12, -2.0), (507.24, -1.0), (550.2, 0.0)]),
                y=Axis([(711.7, 0.0), (670.68, 1.0), (629.64, 2.0), (588.6, 3.0)])),
}


def _temperature_curves(spec) -> dict[float, np.ndarray]:
    return {t: _curve(T553 if spec is T553_CURVES else T554, spec["page"], spec["frame"], colour,
                      spec["x"], spec["y"], spec["legend"])
            for t, colour in spec["colours"].items()}


def type_553_curves() -> dict[float, np.ndarray]:
    """Type 553's print density against relative log exposure, by temperature (F)."""
    return _temperature_curves(T553_CURVES)


def polapan_100_curves() -> dict[float, np.ndarray]:
    """Polapan Pro 100's, by temperature (F)."""
    return _temperature_curves(T554_CURVES)


def polapan_100_times(x: Axis, y: Axis) -> dict[float, np.ndarray]:
    """Polapan's development-time plot at 70 F, by seconds, on the axes given
    (the check of which calibration these charts take)."""
    spec = T554_TIMES
    return {t: _curve(T554, spec["page"], spec["frame"], colour, x, y, spec["legend"])
            for t, colour in spec["colours"].items()}


def polacolor_curves() -> dict[float, dict[str, np.ndarray]]:
    """Polacolor's blue, green and red print densities, by temperature (F)."""
    return {t: {name: _curve(T559, s["page"], s["frame"], colour, s["x"], s["y"], s["legend"])
                for name, colour in (("R", RED), ("G", GREEN), ("B", BLUE))}
            for t, s in T559_CURVES.items()}


# --- spectral sensitivity ---------------------------------------------------------
# Log sensitivity, the bottom rule 0.1; wavelengths on their ticks.

def _sensitivity(pdf, page, frame, colour, x: Axis, y: Axis, exclude=()) -> np.ndarray:
    """(nm, log10 relative sensitivity)."""
    c = _curve(pdf, page, frame, colour, x, y, exclude)
    return np.stack([c[:, 0], np.log10(c[:, 1])], axis=-1)


T553_SENSITIVITY = dict(page=2, frame=(334.8, 247.3, 540.7, 361.4),
                        x=Axis([(365.52, 400), (394.8, 450), (425.52, 500), (454.8, 550), (485.76, 600),
                                (514.92, 650)]),
                        y=Axis([(247.3, 1000.0), (285.24, 100.0), (324.48, 10.0), (361.4, 1.0)], log=True))
T554_SENSITIVITY = dict(page=2, frame=(70.9, 437.4, 233.8, 553.4),
                        x=Axis([(94.92, 400), (118.92, 450), (140.4, 500), (162.0, 550), (187.08, 600),
                                (209.76, 650)]),
                        y=Axis([(437.4, 1000.0), (465.96, 100.0), (495.36, 10.0), (523.56, 1.0), (553.4, 0.1)],
                               log=True))
T559_SENSITIVITY = dict(page=2, frame=(348.2, 234.2, 538.9, 378.7), legend=((505.0, 225.0, 545.0, 262.0),),
                        x=Axis([(375.6, 400), (402.72, 450), (429.96, 500), (457.08, 550), (484.44, 600),
                                (511.56, 650)]),
                        y=Axis([(234.2, 1000.0), (270.36, 100.0), (306.48, 10.0), (342.6, 1.0), (378.7, 0.1)],
                               log=True))


def type_553_sensitivity() -> np.ndarray:
    s = T553_SENSITIVITY
    return _sensitivity(T553, s["page"], s["frame"], BLUE, s["x"], s["y"])


def polapan_100_sensitivity() -> np.ndarray:
    s = T554_SENSITIVITY
    return _sensitivity(T554, s["page"], s["frame"], BLUE, s["x"], s["y"])


def polacolor_sensitivity() -> dict[str, np.ndarray]:
    s = T559_SENSITIVITY
    return {name: _sensitivity(T559, s["page"], s["frame"], colour, s["x"], s["y"], s["legend"])
            for name, colour in (("R", RED), ("G", GREEN), ("B", BLUE))}


# --- MTF: % modulation, linear, against cycles/mm on a log axis -----------------------
# Ticks at 1, 2 and 4 cycles/mm (the frame's right side); the one labelled 3
# sits three points off the log scale the others make, and is left out.

T553_MTF = dict(page=2, frame=(76.0, 413.8, 220.2, 548.8),
                x=Axis([(148.08, 1.0), (184.2, 2.0), (220.2, 4.0)], log=True),
                y=Axis([(413.8, 100.0), (440.88, 80.0), (467.64, 60.0), (494.76, 40.0), (521.64, 20.0),
                        (548.8, 0.0)]))
T554_MTF = dict(page=2, frame=(386.4, 257.2, 539.5, 395.6),
                x=Axis([(462.96, 1.0), (501.24, 2.0), (539.5, 4.0)], log=True),
                y=Axis([(257.2, 100.0), (285.96, 80.0), (312.12, 60.0), (340.68, 40.0), (369.48, 20.0),
                        (395.6, 0.0)]))


def _mtf(pdf, spec) -> np.ndarray:
    """(cycles/mm, response as a fraction)."""
    c = _curve(pdf, spec["page"], spec["frame"], BLUE, spec["x"], spec["y"])
    return np.stack([c[:, 0], c[:, 1] / 100.0], axis=-1)


def type_553_mtf() -> np.ndarray:
    return _mtf(T553, T553_MTF)


def polapan_100_mtf() -> np.ndarray:
    return _mtf(T554, T554_MTF)


# The sheets' resolving powers at 1000:1, line pairs/mm: Type 553's 12-15,
# Polapan's 20-25, Polacolor's 9.
RESOLVING_POWER = {"553": 13.5, "554": 22.5, "559": 9.0}


def mtf_stretch_per_resolution() -> float:
    """How far a print's MTF moves along frequency with its resolving power, by
    the two black-and-white sheets: Polapan's MTF is Type 553's on frequencies
    stretched by k, for resolving powers in the ratio 22.5/13.5; the exponent
    log k / log (22.5/13.5)."""
    from scipy.optimize import minimize_scalar
    a, b = type_553_mtf(), polapan_100_mtf()

    def misfit(k):
        pred = np.interp(np.log(b[:, 0] / k), np.log(a[:, 0]), a[:, 1], left=np.nan, right=np.nan)
        ok = np.isfinite(pred)
        return float(np.sqrt(np.mean((pred[ok] - b[ok, 1]) ** 2)))

    k = minimize_scalar(misfit, bounds=(0.8, 2.0), method="bounded").x
    return float(np.log(k) / np.log(RESOLVING_POWER["554"] / RESOLVING_POWER["553"]))


def polacolor_mtf() -> np.ndarray:
    """An estimate: Polacolor's sheet gives only its resolving power, 9 line
    pairs/mm against Type 553's 12-15. Type 553's MTF, its frequencies scaled
    by (9 / 13.5) to the power the black-and-white sheets give between
    resolving power and MTF (`mtf_stretch_per_resolution`: Polapan's MTF is
    Type 553's at 1.33 times the frequency for 1.67 times the resolution)."""
    m = type_553_mtf()
    stretch = (RESOLVING_POWER["559"] / RESOLVING_POWER["553"]) ** mtf_stretch_per_resolution()
    return np.stack([m[:, 0] * stretch, m[:, 1]], axis=-1)
