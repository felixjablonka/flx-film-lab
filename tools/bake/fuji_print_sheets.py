"""Fujifilm's motion picture print films, read off their own datasheets.

Three Fujicolor positive films, each from its own two-page sheet:

- F-CP 3519 (KB-918E, 1999), the normal-contrast release print stock;
- F-CP 3510 (KB-301E, 2003), "Super F-CP", which followed it;
- ETERNA-CP 3513DI (KB-505E, 2005), the high-contrast stock for digital
  intermediates.

Every sheet plots the same four things, as vector paths: the spectral density
of the three dyes and of a grey ("Gray"), 400-700 nm; the characteristic
curves in Status A, exposed with 2854 K tungsten through CC-90Y, CC-60M and a
Fuji SC-41 ultraviolet filter, against relative log H with no numbers on it,
only a bar marking half a decade; the spectral sensitivities, log, at D-min +
1.0; and the contrast transfer function. Their labels are outlines, so each
plot is described by its frame and the gridlines whose values its labels give,
and its curves are told apart by stroke colour. The 3519 sheet's pages are
stored rotated a quarter turn; its paths are read as the page is displayed.

The 2008 motion picture manual (076_fujifilm_motion_picture_film_manual.pdf,
pages 25-28) repeats the 3510 and 3513DI curves, the aim print densities and
the printer settings; its plots are read as a check on the sheets'.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pymupdf

from datasheet import _flatten_path
from vector_plots import _subpaths

SHEETS_DIR = Path(__file__).resolve().parents[2] / "film spec sheets"
LOG = np.log10


@dataclass(frozen=True)
class Plot:
    frame: tuple[float, float, float, float]       # x0, y0, x1, y1, as the page is displayed
    x_rules: tuple[tuple[float, float], ...]       # (position, value) along x
    y_rules: tuple[tuple[float, float], ...]       # (position, value) along y


@dataclass(frozen=True)
class Sheet:
    pdf: str
    page: int
    curves: Plot            # x: relative log H, the frame's left edge 0
    sensitivity: Plot       # x: nm, y: log sensitivity
    dyes: Plot              # x: nm, y: diffuse spectral density
    ctf: Plot               # x: log10 cycles/mm, y: log10 response
    layers: dict[str, tuple[float, float, float]]   # B, G, R stroke colours
    dye_colours: dict[str, tuple[float, float, float]]   # C, M, Y and Gray
    # What the sheet prints in text: Status A densities a grey card's image
    # should print at, projected with xenon, and the printer's ND filters on
    # a Bell & Howell Model C at trimmer 15 (13 for 3513DI), tape 25.
    aim: tuple[float, float, float]                 # R, G, B
    printer_nd: tuple[float, float, float]          # R, G, B


def _half_decades(left: float, step: float, count: int) -> tuple[tuple[float, float], ...]:
    return tuple((left + i * step, 0.5 * i) for i in range(count))


def _ctf_x(positions) -> tuple[tuple[float, float], ...]:
    return tuple(zip(positions, LOG([1, 2, 5, 10, 20, 30, 40, 50, 100])))


def _ctf_y(positions) -> tuple[tuple[float, float], ...]:
    return tuple(zip(positions, LOG([1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1])))


_DENSITY_4 = tuple((v,) for v in np.arange(0.0, 4.01, 0.5))
_FUJI_2003 = {"B": (0.0, 0.443, 0.737), "G": (0.0, 0.651, 0.314), "R": (0.929, 0.11, 0.141)}
_FUJI_2003_DYES = {"C": (0.0, 0.678, 0.937), "M": (0.925, 0.0, 0.549), "Y": (1.0, 0.949, 0.0),
                   "Gray": (0.502, 0.51, 0.518)}

SHEETS = {
    # Each frame's left edge is a gridline: the curves' x starts there at 0.
    "fujifilm_fcp_3519": Sheet(
        "fcp_type3519.pdf", 0,
        curves=Plot((349.5, 113.9, 605.3, 340.3), _half_decades(349.48, 28.44, 10),
                    tuple(zip((340.3, 312.04, 283.65, 255.27, 226.89, 198.5, 170.31, 142.11, 113.9),
                              np.arange(0.0, 4.01, 0.5)))),
        sensitivity=Plot((372.5, 485.3, 604.3, 615.3),
                         tuple(zip((389.38, 422.95, 455.55, 489.12, 521.72, 555.29, 587.89), range(400, 701, 50))),
                         ((615.3, 0.0), (571.44, 1.0), (528.65, 2.0), (485.3, 3.0))),
        dyes=Plot((95.9, 102.4, 279.1, 341.4), ((95.9, 400), (156.58, 500), (217.86, 600), (279.1, 700)),
                  ((341.4, 0.0), (281.54, 0.5), (221.78, 1.0), (162.04, 1.5), (102.4, 2.0))),
        ctf=Plot((95.9, 417.6, 293.4, 555.7),
                 _ctf_x((95.9, 126.49, 164.22, 194.86, 222.61, 240.23, 252.9, 263.09, 293.4)),
                 _ctf_y((423.92, 428.91, 435.4, 440.47, 446.89, 455.67, 464.72, 477.49, 495.43, 524.86))),
        layers={"B": (0.0, 0.451, 0.733), "G": (0.0, 0.624, 0.235), "R": (0.875, 0.0, 0.141)},
        dye_colours={"C": (0.0, 0.659, 0.925), "M": (0.89, 0.0, 0.482), "Y": (0.973, 0.957, 0.0),
                     "Gray": (0.565, 0.565, 0.565)},
        aim=(1.10, 1.05, 1.05), printer_nd=(0.45, 0.50, 0.90)),
    "fujifilm_fcp_3510": Sheet(
        "fcp_type3510.pdf", 0,
        curves=Plot((306.8, 71.7, 562.3, 276.4), _half_decades(306.83, 25.547, 11),
                    tuple(zip((276.4, 250.77, 225.19, 199.6, 174.01, 148.42, 122.84, 97.25, 71.7),
                              np.arange(0.0, 4.01, 0.5)))),
        sensitivity=Plot((329.3, 421.1, 561.6, 551.4),
                         tuple(zip((351.39, 379.03, 406.66, 434.29, 461.93, 489.56, 517.2, 544.83),
                                   range(400, 751, 50))),
                         ((551.4, 0.0), (507.97, 1.0), (464.55, 2.0), (421.1, 3.0))),
        dyes=Plot((52.7, 60.4, 235.9, 299.4), ((52.7, 400), (113.38, 500), (174.66, 600), (235.9, 700)),
                  ((299.4, 0.0), (239.5, 0.5), (179.75, 1.0), (120.01, 1.5), (60.4, 2.0))),
        ctf=Plot((52.7, 375.5, 250.2, 513.7),
                 _ctf_x((52.7, 83.29, 121.02, 151.66, 179.42, 197.03, 209.7, 219.89, 250.2)),
                 _ctf_y((381.89, 386.88, 393.37, 398.44, 404.86, 413.64, 422.69, 435.46, 453.4, 482.83))),
        layers=_FUJI_2003, dye_colours=_FUJI_2003_DYES,
        aim=(1.10, 1.05, 1.05), printer_nd=(0.50, 0.55, 0.90)),
    "fujifilm_eterna_cp_3513di": Sheet(
        "eterna_cp_type3513di.pdf", 0,
        curves=Plot((331.83, 104.08, 541.76, 271.96), _half_decades(331.69, 20.99, 11),
                    tuple(zip((271.96, 251.14, 230.12, 209.1, 188.08, 167.06, 146.04, 125.02, 104.08),
                              np.arange(0.0, 4.01, 0.5)))),
        sensitivity=Plot((332.0, 438.9, 541.3, 556.2),
                         tuple(zip((351.93, 376.82, 401.71, 426.61, 451.5, 476.4, 501.29, 526.19),
                                   range(400, 751, 50))),
                         ((556.2, 0.0), (517.12, 1.0), (478.01, 2.0), (438.9, 3.0))),
        dyes=Plot((70.6, 104.1, 239.8, 322.4), ((70.6, 400), (126.98, 500), (183.36, 600), (239.8, 700)),
                  ((322.4, 0.0), (267.82, 0.5), (213.24, 1.0), (158.66, 1.5), (104.1, 2.0))),
        ctf=Plot((70.5, 438.2, 239.9, 556.7),
                 _ctf_x((70.5, 96.74, 129.1, 155.38, 179.18, 194.29, 205.16, 213.9, 239.9)),
                 _ctf_y((443.63, 447.91, 452.65, 457.82, 463.33, 470.87, 478.62, 489.58, 504.96, 530.2))),
        layers=_FUJI_2003, dye_colours=_FUJI_2003_DYES,
        aim=(1.10, 1.05, 1.05), printer_nd=(0.50, 0.55, 0.90)),
}

# The manual's plots of the same two stocks' curves (page index 25: 3510, 27: 3513DI).
MANUAL = "076_fujifilm_motion_picture_film_manual.pdf"
MANUAL_CURVES = Plot((279.5, 164.1, 419.6, 274.9), _half_decades(279.5, 14.012, 11),
                     tuple(zip((274.9, 261.03, 247.18, 233.33, 219.48, 205.63, 191.78, 177.93, 164.1),
                               np.arange(0.0, 4.01, 0.5))))
MANUAL_PAGES = {"fujifilm_fcp_3510": 25, "fujifilm_eterna_cp_3513di": 27}
MANUAL_LAYERS = {"B": (0.0, 0.446, 0.736), "G": (0.0, 0.65, 0.315), "R": (0.93, 0.111, 0.141)}


def _axis(rules):
    pos = np.array([r[0] for r in rules])
    val = np.array([r[1] for r in rules])
    slope, intercept = np.polyfit(pos, val, 1)
    return slope, intercept, float(np.abs(slope * pos + intercept - val).max())


def _strokes(pdf: str, page_no: int, plot: Plot, colour, tolerance: float = 0.03,
             curved: bool = False) -> list[np.ndarray]:
    """Every stroke of one colour inside the plot, in data units, each sorted
    along x; the page's rotation undone, so coordinates are as displayed.
    `curved` keeps only paths that bend, leaving out a black plot's rules."""
    page = pymupdf.open(SHEETS_DIR / pdf)[page_no]
    m = page.rotation_matrix
    x0, y0, x1, y1 = plot.frame
    ax, bx, _ = _axis(plot.x_rules)
    ay, by, _ = _axis(plot.y_rules)
    out = []
    for path in page.get_drawings():
        if path.get("type") != "s" or len(path.get("color") or ()) != 3:
            continue
        if max(abs(a - b) for a, b in zip(path["color"], colour)) > tolerance:
            continue
        if curved and not any(i[0] == "c" for i in path["items"]):
            continue
        rect = pymupdf.Rect(path["rect"]) * m
        rect.normalize()
        if not (x0 - 2 <= rect.x0 and rect.x1 <= x1 + 2 and y0 - 2 <= rect.y0 and rect.y1 <= y1 + 2):
            continue
        for run in _subpaths([i for i in path["items"] if i[0] in ("l", "c")]):
            p = _flatten_path(run, steps=32)
            shown = np.stack([m.a * p[:, 0] + m.c * p[:, 1] + m.e, m.b * p[:, 0] + m.d * p[:, 1] + m.f], axis=-1)
            data = np.stack([ax * shown[:, 0] + bx, ay * shown[:, 1] + by], axis=-1)
            if np.ptp(data[:, 0]) > 0:
                out.append(data[np.argsort(data[:, 0], kind="stable")])
    return out


def _one(pdf, page_no, plot, colour, name) -> np.ndarray:
    strokes = _strokes(pdf, page_no, plot, colour)
    if not strokes:
        raise ValueError(f"{pdf} page {page_no + 1}: no curve stroked as {name} {colour}")
    points = np.concatenate(strokes)
    return points[np.argsort(points[:, 0], kind="stable")]


def read(stock: str) -> dict:
    """The sheet's plots, in data units:

    - "curves": {B, G, R}: (relative log H, Status A density), as drawn, the
      green and blue curves still shifted for legibility;
    - "sensitivity": {B, G, R}: a list of strokes (nm, log sensitivity), the
      red layer's in two, its blue-light tail and its own band;
    - "dyes": {C, M, Y}: (nm, diffuse spectral density); "neutral": the grey;
    - "ctf": (cycles/mm, response).
    """
    s = SHEETS[stock]
    out = {"curves": {n: _one(s.pdf, s.page, s.curves, c, n) for n, c in s.layers.items()},
           "sensitivity": {n: _strokes(s.pdf, s.page, s.sensitivity, c) for n, c in s.layers.items()},
           "dyes": {n: _one(s.pdf, s.page, s.dyes, c, n) for n, c in s.dye_colours.items() if n != "Gray"},
           "neutral": _one(s.pdf, s.page, s.dyes, s.dye_colours["Gray"], "Gray")}
    # one black path: a straight run along the top, then the fall
    ctf = np.concatenate(_strokes(s.pdf, s.page, s.ctf, (0.0, 0.0, 0.0), tolerance=0.2, curved=True))
    ctf = ctf[np.argsort(ctf[:, 0], kind="stable")]
    out["ctf"] = np.stack([10 ** ctf[:, 0], 10 ** ctf[:, 1]], axis=-1)
    return out


def manual_curves(stock: str) -> dict[str, np.ndarray]:
    """The manual's plot of the stock's curves, as `read` returns the sheet's."""
    return {n: _one(MANUAL, MANUAL_PAGES[stock], MANUAL_CURVES, c, n) for n, c in MANUAL_LAYERS.items()}


def calibration_report(stock: str) -> str:
    s = SHEETS[stock]
    parts = []
    for name in ("curves", "sensitivity", "dyes", "ctf"):
        plot = getattr(s, name)
        parts.append(f"{name} {_axis(plot.x_rules)[2]:.3g}/{_axis(plot.y_rules)[2]:.3g}")
    return f"{stock}: rules fit (x/y, data units) " + ", ".join(parts)


if __name__ == "__main__":
    for stock in SHEETS:
        print(calibration_report(stock))
        r = read(stock)
        for n, c in r["curves"].items():
            print(f"  curve {n}: x {c[0, 0]:.2f}..{c[-1, 0]:.2f}  D {c[:, 1].min():.3f}..{c[:, 1].max():.3f}")
        for n, strokes in r["sensitivity"].items():
            print(f"  sens {n}: " + ", ".join(f"{st[0, 0]:.0f}-{st[-1, 0]:.0f} nm peak {st[:, 1].max():.2f} "
                                              f"at {st[np.argmax(st[:, 1]), 0]:.0f}" for st in strokes))
        for n, c in r["dyes"].items():
            print(f"  dye {n}: {c[0, 0]:.0f}-{c[-1, 0]:.0f} nm peak {c[:, 1].max():.3f} at {c[np.argmax(c[:, 1]), 0]:.0f}")
        g = r["neutral"]
        print(f"  grey: {g[0, 0]:.0f}-{g[-1, 0]:.0f} nm, {g[:, 1].min():.3f}..{g[:, 1].max():.3f}")
        print(f"  ctf: {r['ctf'][0, 0]:.2f}-{r['ctf'][-1, 0]:.1f} c/mm, response at 10/20/40: "
              + " ".join(f"{np.interp(np.log10(f), np.log10(r['ctf'][:, 0]), r['ctf'][:, 1]):.3f}" for f in (10, 20, 40)))
