"""Kodak's 1962 camera negative, Eastman Color Negative 5251 (50T), and its
companion print film, Eastman Color Print 5385, read off the scanned figures in
"Kodak 50T_5251_and 5385 print film.pdf": W. I. Kisner's two papers introducing
them (Journal of the SMPTE 71(10), 1962, pp. 776-779 and 779-781) and the
film-only MTF the BBC measured (C. B. B. Wood, BBC Research Department Report
1966/31, Fig. 1).

Every figure is a scan of a printed page, monochrome, each film's curves drawn
solid and its predecessor's (5250, 5382) dashed over them. A scan is turned a
little in the page; it is levelled first, by the tilt its frame shows. The axes
are calibrated on their own tick marks, found as dark marks along the frame and
counted against the printed labels. The dashed curves are taken out as what
they are, short separate strokes: ink in pieces smaller than a stroke of the
solid lines. Lettering inside a plot is blanked. Each solid curve is then
followed from a point on it (`trace_raster_plot.trace_curves`, or column by
column for the characteristic curves, which never cross).

The characteristic curves have no numbered exposure ticks. Their log exposure
is read at the scale of their density, one decade drawn as long as one unit of
density: Kodak's technical sheets draw them so (H-1-5293 and H-1-5245, 1999,
and E-26, 1997: 46.2 points to a decade and to a density unit), and Kodak's
1968 paper on 5254, "structurally the same" as 5251, marks its exposure axis
in decades as long as its density units, to 3% (Journal of the SMPTE 77(9),
p. 990, Fig. 3).
"""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

import numpy as np
import pymupdf
from PIL import Image
from scipy import ndimage

import trace_raster_plot as raster

SHEET = Path(__file__).resolve().parents[2] / "film spec sheets" / "Kodak 50T_5251_and 5385 print film.pdf"


@dataclass(frozen=True)
class Figure:
    page: int                       # in the compiled PDF, from 0
    image: int                      # the scan's index among its page's images
    tilt: float                     # degrees the scan is turned; levelled first
    ink: float                      # grey below which a pixel is ink
    frame: tuple[float, float, float, float]          # left, right, top, bottom, levelled pixels
    x: tuple[float, float, float, float]              # two calibration points: pixel, value, pixel, value
    y: tuple[float, float, float, float]
    exclude: tuple[tuple[int, int, int, int], ...] = ()   # lettering, x0 y0 x1 y1
    stroke: int = 40                # ink pieces smaller than this are dashes
    rules: int = 2                  # half-width the frame and rules are taken out at


def _value(cal, px):
    p0, v0, p1, v1 = cal
    return v0 + (np.asarray(px, float) - p0) * (v1 - v0) / (p1 - p0)


@lru_cache(maxsize=None)
def _levelled(page: int, image: int, tilt: float) -> np.ndarray:
    doc = pymupdf.open(str(SHEET))
    xref = doc[page].get_images(full=True)[image][0]
    pix = pymupdf.Pixmap(doc, xref)
    im = Image.frombytes("RGB" if pix.n >= 3 else "L", (pix.width, pix.height), pix.samples).convert("L")
    # PIL turns counter-clockwise for a positive angle; the frame's tilt is undone
    return np.asarray(im.rotate(-tilt, resample=Image.BICUBIC, fillcolor=255), float)


def _mask(fig: Figure, drop_dashes: bool = True) -> np.ndarray:
    ink = _levelled(fig.page, fig.image, fig.tilt) < fig.ink
    for x0, y0, x1, y1 in fig.exclude:
        ink[y0:y1, x0:x1] = False
    if drop_dashes:
        labels, n = ndimage.label(ink, structure=np.ones((3, 3)))
        sizes = ndimage.sum(ink, labels, index=np.arange(1, n + 1))
        keep = np.zeros(n + 1, bool)
        keep[1:] = sizes >= fig.stroke
        ink = keep[labels]
    return ink


def _plot_frame(fig: Figure) -> raster.PlotFrame:
    left, right, top, bottom = fig.frame
    return raster.PlotFrame(left=left, right=right, top=top, bottom=bottom,
                            x_min=float(_value(fig.x, left)), x_max=float(_value(fig.x, right)),
                            y_at_top=float(_value(fig.y, top)), y_at_bottom=float(_value(fig.y, bottom)))


def _follow(ink: np.ndarray, fig: Figure, seeds: dict[str, tuple[int, int]], window: int = 5) -> dict[str, np.ndarray]:
    """Curves that never cross, followed column by column from a pixel on each:
    in every column the run of ink nearest where the curve was heading."""
    left, right, top, bottom = (int(round(v)) for v in fig.frame)
    runs = {}
    for col in range(left + 3, right - 2):
        ys = np.nonzero(ink[top + 3:bottom - 2, col])[0] + top + 3
        groups = np.split(ys, np.nonzero(np.diff(ys) > 1)[0] + 1) if ys.size else []
        runs[col] = [float(g.mean()) for g in groups if g.size]
    out = {}
    for name, (sx, sy) in seeds.items():
        points = {sx: float(sy)}
        for step in (1, -1):
            path = [(sx, float(sy))]
            col = sx + step
            while left + 3 <= col < right - 2:
                if len(path) >= 6:
                    (x0, y0), (x1, y1) = path[-6], path[-1]
                    predicted = y1 + (y1 - y0) / (x1 - x0) * (col - x1)
                else:
                    predicted = path[-1][1]
                near = [y for y in runs.get(col, []) if abs(y - predicted) <= window]
                if near:
                    y = min(near, key=lambda v: abs(v - predicted))
                    path.append((col, y))
                    points[col] = y
                elif col - path[-1][0] > 12:      # a gap longer than a break in the line: its end
                    break
                col += step
        cols = np.array(sorted(points))
        out[name] = np.stack([_value(fig.x, cols), _value(fig.y, [points[c] for c in cols])], axis=-1)
    return out


def _extend(main: np.ndarray, below: np.ndarray | None = None, above: np.ndarray | None = None) -> np.ndarray:
    """A curve walked in pieces, where a walk ends at a crossing: `main`, and
    of the others only what lies below or above its range."""
    parts = [main]
    if below is not None:
        parts.append(below[below[:, 0] < main[0, 0]])
    if above is not None:
        parts.append(above[above[:, 0] > main[-1, 0]])
    out = np.concatenate(parts)
    return out[np.argsort(out[:, 0], kind="stable")]


def _walk(fig: Figure, seeds: dict[str, tuple[float, float]]) -> dict[str, np.ndarray]:
    """Crossing curves (spectral), each walked from a point on it, in data units."""
    # the scans' rules are a couple of pixels wide: taken out narrowly, so the
    # curves are followed to within a few nanometres of the frame
    return raster.trace_curves(_mask(fig), _plot_frame(fig), seeds, exclude=list(fig.exclude), gap=40,
                               rule_halfwidth=fig.rules)


# --- Eastman Color Negative 5251 (Kisner 1962) ------------------------------
# Fig. 2, D-log E, integral density: ticks every 0.2 from 3.0 (y 63.5) to 0.2
# (y 488.5), 151.8 pixels to a density unit, and so to a decade of exposure
# (counted from the frame's left edge; the sheet gives no absolute exposure).
NEG_CURVES = Figure(0, 0, tilt=1.20, ink=220, frame=(66.0, 518.5, 34.5, 518.0),
                    x=(66.0, 0.0, 66.0 + 151.79, 1.0), y=(63.5, 3.0, 488.5, 0.2),
                    exclude=((198, 118, 330, 168),), stroke=70)
# Fig. 1, spectral sensitivity, log scale: 100 at y 58.6, 10 at 127.0, 1.0 at
# 195.0 (68.2 to a decade); 340 nm at the frame's left edge, the labels 400-700
# centred 136.1 pixels to 100 nm.
NEG_SENSITIVITY = Figure(0, 1, tilt=-0.06, ink=240, frame=(47.0, 573.0, 32.0, 332.0),
                         x=(132.7, 400.0, 404.8, 600.0), y=(58.6, 2.0, 195.0, 0.0),
                         exclude=((330, 62, 425, 95), (80, 108, 156, 122), (311, 152, 372, 170),
                                  (438, 162, 488, 182)),
                         stroke=60)
# Fig. 3, equivalent dyes: ticks every 0.1 from 1.2 (y 52.0) to -0.1 (y 452.0);
# 400 nm at x 73.0, 700 nm at 390.0. The zero line is a rule and comes out with
# the frame, taken out wider: the cyan dye crosses it twice.
NEG_DYES = Figure(1, 1, tilt=0.86, ink=215, frame=(72.0, 389.5, 22.0, 484.0),
                  x=(73.0, 400.0, 390.0, 700.0), y=(52.0, 1.2, 452.0, -0.1),
                  exclude=((170, 28, 380, 84),), stroke=70, rules=4)
# BBC, film-only MTF: a full grid, 0-120% every 10 (y 637.5 to 41.5) and 0-6
# MHz on the 16 mm scale every 0.5 (x 112.5 to 729.5).
MTF = Figure(1, 0, tilt=0.11, ink=150, frame=(112.5, 729.5, 41.5, 637.5),
             x=(112.5, 0.0, 729.5, 6.0), y=(637.5, 0.0, 41.5, 120.0))
# 625-line television: an active line of 52 us, so f MHz is 52 f cycles across
# the picture; the 16 mm television transmitted width, 0.368 in (SMPTE). The
# figure's own two scales put 35 mm's 6 MHz at 2.78 MHz on the 16 mm scale, the
# ratio of the transmitted widths (0.792 / 0.368 = 2.15).
MHZ_TO_CYCLES_PER_MM_16 = 52.0 / (0.368 * 25.4)

# --- Eastman Color Print 5385 (Kisner 1962) ---------------------------------
# Fig. 3, integral density, the curves moved apart along the exposure axis:
# ticks every 0.2 from 3.4 (y 58.5) to 0.2 (y 564.5), 158.1 pixels a unit.
PRINT_CURVES = Figure(2, 0, tilt=0.11, ink=220, frame=(70.0, 543.0, 27.0, 595.5),
                      x=(70.0, 0.0, 70.0 + 158.13, 1.0), y=(58.5, 3.4, 564.5, 0.2),
                      exclude=((128, 72, 258, 122),), stroke=70)
# Fig. 1, spectral sensitivity, read on the scale labelled for 5385 (the outer
# one: 100, 10, 1.0, 0.1 at y 75, 158, 240.5, 322.5); 340 nm at the inner axis
# (x 93.5), 400 at 193.5, 500 at 361.5, 600 at 528.5.
PRINT_SENSITIVITY = Figure(2, 2, tilt=1.03, ink=228, frame=(93.5, 760.5, 42.5, 406.5),
                           x=(193.5, 400.0, 528.5, 600.0), y=(75.0, 2.0, 322.5, -1.0),
                           exclude=((340, 88, 440, 122), (205, 90, 246, 112), (205, 112, 229, 151),
                                    (174, 186, 272, 212)),
                           stroke=60)
# Fig. 2, spectral density of a neutral and of the three dyes: ticks every 0.1
# from 1.2 (y 97.0) to 0.1 (y 501.5); 400 nm at x 84, 700 at 462.5.
PRINT_DYES = Figure(2, 1, tilt=0.29, ink=215, frame=(84.0, 462.5, 32.0, 540.0),
                    x=(84.0, 400.0, 462.5, 700.0), y=(97.0, 1.2, 501.5, 0.1),
                    exclude=((222, 52, 350, 102),), stroke=70)


def negative() -> dict:
    """5251's characteristic curves (relative log exposure, integral density),
    spectral sensitivities (nm, log10 S) and equivalent dyes (nm, density)."""
    curves = _walk(NEG_CURVES, {"B": (2.200, 1.926), "G": (2.200, 1.656), "R": (2.200, 1.142)})
    # Each layer from a point on its main lobe; the green layer's blue hump
    # and steep fall, and the red layer's rise and last fall, from points of
    # their own, where a walk ends at a crossing.
    s = _walk(NEG_SENSITIVITY, {"B": (450.0, np.log10(20.0)), "G": (560.0, np.log10(9.5)),
                                "R": (640.0, np.log10(6.0)), "g<": (400.0, -0.955), "g>": (600.0, -1.261),
                                "r<": (560.0, -0.697), "r>": (710.0, -1.115)})
    sensitivity = {"B": s["B"], "G": _extend(s["G"], s["g<"], s["g>"]), "R": _extend(s["R"], s["r<"], s["r>"])}
    dyes = _walk(NEG_DYES, {"Y": (440.0, 0.99), "M": (548.0, 1.0), "C": (690.0, 0.99)})
    return {"curves": curves, "sensitivity": sensitivity, "dyes": dyes}


def negative_mtf() -> np.ndarray:
    """5251's film-only MTF, cycles/mm against response (1 = 100%): its
    dash-dot curve on the BBC's plot, the lowest of the three, followed with
    the grid taken out from a point at 3 MHz where it runs clear of the others,
    over 0.5-6 MHz; at 0 all three start from 100%."""
    ink = _levelled(MTF.page, MTF.image, MTF.tilt) < MTF.ink
    rows, cols = raster.grid_lines(ink)
    for r in rows:
        ink[max(int(r) - 2, 0):int(r) + 3, :] = False
    for c in cols:
        ink[:, max(int(c) - 2, 0):int(c) + 3] = False
    traced = _follow(ink, MTF, {"5251": (421, 311)}, window=6)["5251"]
    traced = traced[traced[:, 0] >= 0.5]
    mhz = np.concatenate([[0.0], traced[:, 0]])
    response = np.concatenate([[100.0], traced[:, 1]]) / 100
    return np.stack([mhz * MHZ_TO_CYCLES_PER_MM_16, response], axis=-1)


def print_film() -> dict:
    """5385's characteristic curves (relative log exposure, integral density;
    each moved along the axis by the paper), spectral sensitivities (nm, log10
    S on 5385's scale, each layer as its strokes) and the spectral density of a
    neutral and its three dyes (nm, density)."""
    curves = _walk(PRINT_CURVES, {"B": (0.930, 1.556), "G": (1.420, 1.556), "R": (2.112, 1.556),
                                  "b": (0.150, 0.190)})
    curves["B"] = _extend(curves["B"], below=curves.pop("b"))      # its toe, where 5382's runs into it
    # The green layer is one curve, its blue lobe through the dip at 440 nm
    # into its green one; the red layer two strokes, its blue lobe and its red.
    s = _walk(PRINT_SENSITIVITY, {"B": (390.0, 1.08), "G": (555.0, 0.0), "g": (380.0, 0.31),
                                  "R": (690.0, -0.95), "r": (380.0, -0.07)})
    sensitivity = {"B": [s["B"]], "G": [_extend(s["G"], below=s["g"])], "R": [s["r"], s["R"]]}
    dyes = _walk(PRINT_DYES, {"neutral": (655.0, 1.13), "n": (440.0, 1.09), "Y": (445.0, 0.83),
                              "M": (535.0, 0.89), "C": (655.0, 1.07)})
    neutral = _extend(dyes["neutral"], below=dyes["n"])
    return {"curves": curves, "sensitivity": sensitivity, "dyes": {k: dyes[k] for k in "CMY"},
            "neutral": neutral}


def _overlay(fig: Figure, traces: dict[str, np.ndarray], path: Path) -> None:
    from PIL import ImageDraw
    im = Image.fromarray(_levelled(fig.page, fig.image, fig.tilt).astype(np.uint8)).convert("RGB")
    s = 2
    im = im.resize((im.width * s, im.height * s), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    colours = [(230, 30, 30), (20, 160, 40), (30, 60, 230), (200, 0, 200), (230, 140, 0)]
    flat = {}
    for name, t in traces.items():
        for i, part in enumerate(t if isinstance(t, list) else [t]):
            flat[f"{name}{i}"] = part
    for (name, t), colour in zip(flat.items(), colours * 3):
        px = (t[:, 0] - fig.x[1]) * (fig.x[2] - fig.x[0]) / (fig.x[3] - fig.x[1]) + fig.x[0]
        py = (t[:, 1] - fig.y[1]) * (fig.y[2] - fig.y[0]) / (fig.y[3] - fig.y[1]) + fig.y[0]
        for x, y in zip(px[::2], py[::2]):
            d.point((x * s, y * s), fill=colour)
            d.point((x * s + 1, y * s), fill=colour)
    im.save(path)


if __name__ == "__main__":
    import sys
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    neg = negative()
    _overlay(NEG_CURVES, neg["curves"], out / "5251_curves.png")
    _overlay(NEG_SENSITIVITY, neg["sensitivity"], out / "5251_sensitivity.png")
    _overlay(NEG_DYES, neg["dyes"], out / "5251_dyes.png")
    prt = print_film()
    _overlay(PRINT_CURVES, prt["curves"], out / "5385_curves.png")
    _overlay(PRINT_SENSITIVITY, prt["sensitivity"], out / "5385_sensitivity.png")
    _overlay(PRINT_DYES, {**prt["dyes"], "neutral": prt["neutral"]}, out / "5385_dyes.png")
    mtf = negative_mtf()
    for f in (5, 10, 20, 30):
        print(f"5251 MTF at {f} cycles/mm: {np.interp(f, mtf[:, 0], mtf[:, 1]):.3f}")
    for name, t in neg["curves"].items():
        print(f"5251 {name}: log E {t[0, 0]:.2f}..{t[-1, 0]:.2f}, D {t[:, 1].min():.3f}..{t[:, 1].max():.3f}, {len(t)} points")
    for name, t in prt["curves"].items():
        print(f"5385 {name}: log E {t[0, 0]:.2f}..{t[-1, 0]:.2f}, D {t[:, 1].min():.3f}..{t[:, 1].max():.3f}, {len(t)} points")
