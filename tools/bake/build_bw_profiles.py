#!/usr/bin/env python
"""Build black-and-white film and paper profiles from manufacturer datasheets.

Five stocks, each from its maker's own sheet in `film spec sheets/`:

- Kodak Tri-X 400 (F-4017): spectral sensitivity and the D-76 characteristic
  curves, both vector plots, read exactly. Also pushed two stops: Kodak's D-76
  time for EI 1600, interpolated between the sheet's 10 and 12 minute curves.
- Ilford HP5 Plus: spectral sensitivity and the characteristic curve, vector
  plots, calibrated on their own tick marks and grid.
- Fujifilm Neopan 1600 (AF3-608E): spectral sensitivity and the D-76
  characteristic curves, scanned line art (CCITT image masks), traced.
- Agfa APX 25 (Agfa Professional range sheet): spectral sensitivity and the
  characteristic curve, vector plots labelled in text.
- Ilford Multigrade RC Deluxe: spectral sensitivity and the curves for each
  filter grade, bitmaps, traced.

Unlike `build_profiles.py`, nothing here is inherited from a donor: a
black-and-white emulsion is one silver layer with a flat spectral density, so
the datasheet is the whole profile. The engine is built for three dye layers,
so each profile carries that one layer three times, each with a third of the
silver's density: identical exposures develop identical densities and the
picture stays neutral by construction. `channel_model` says "bw" so the bake
and the app know.

Choices the datasheets leave open, and how they are made here:

- Where mid-grey sits on the film curve. The ISO B&W speed point is 0.10 over
  base + fog, and a meter calibrated to ISO 2720 puts the metered mid-tone
  log10(0.65 x 12.5 / 0.8) = 1.007 decades above it. Kodak's absolute axis puts
  the same point within a few hundredths of where ISO 400 says it should be.
- Ilford's spectral plots are "wedge spectrograms to tungsten light" on an
  unlabelled scale. Read literally as tungsten-weighted they give HP5 Plus a
  red-filter factor of 12-16x at any plausible scale, against the 8x
  photographers use and the 9x Kodak's data gives Tri-X. Read as the film's own
  relative log sensitivity they give 6x, so that is how they are read.
- Neopan 1600 is rated EI 1600, but its D-76 curve for that rating (7 1/2
  min) puts the ISO speed point at about ISO 750: the rating is a push of
  about a stop, as Fujifilm's development table implies. Mid-grey goes where
  a meter set to EI 1600 puts it on Fujifilm's absolute lux-second axis, so
  the film renders as it is shot. Its spectral plot is a "spectrogram to
  daylight (5400K)", read literally: divided by CIE daylight at 5400 K, which
  gives filter factors of 2.6x yellow, 4.3x orange (No. 22) and 7.5x red
  against Fujifilm's 2, 4 (No. 21) and 8 for daylight. Read as it stands the
  red comes out 6.6x.
- Multigrade's native curve is grade 1 (ISO R110 on Ilford's table): the
  app's Contrast is relative to a paper's own curve with 115 as neutral, so the
  slider then reads close to Multigrade's real ISO-R grades.

Run:  PYTHONPATH=<spektrafilm>/src python tools/bake/build_bw_profiles.py
"""

from __future__ import annotations

import io
import json
import sys
from dataclasses import dataclass
from pathlib import Path

import colour
import numpy as np
import pymupdf
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import datasheet as ds  # noqa: E402
from build_profiles import fit_cdf_model, layer_curves  # noqa: E402

from spektrafilm.profiles.io import (  # noqa: E402
    DensityCurvesModel, Profile, ProfileData, ProfileInfo, ProfileMetadata,
    _json_safe, profile_to_dict,
)
from spektrafilm.utils.spectral_upsampling import (  # noqa: E402
    compute_hanatos2025_tc_lut, rgb_to_raw_hanatos2025,
)
from spektrafilm.profiles.io import Hanatos2025SensitivityAdaptation  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SHEETS = REPO / "film spec sheets"
OUT_DIR = REPO / "profiles"
PACKS = REPO / "FLXFilmLab" / "Resources" / "packs"

WAVELENGTHS = np.arange(380.0, 781.0, 5.0)      # spektrafilm's SPECTRAL_SHAPE
LOG_EXPOSURE = np.linspace(-3.0, 4.0, 256)      # every bundled profile's axis
WORKING_SPACE = "ITU-R BT.2020"                 # bake_packs.WORKING_COLOR_SPACE

# ISO 6 speed point and the ISO 2720 metered mid-tone above it.
SPEED_POINT_NET_DENSITY = 0.10
MID_TONE_ABOVE_SPEED_POINT = float(np.log10(0.65 * 12.5 / 0.8))   # 1.007
MID_GREY_REFLECTION_DENSITY = float(-np.log10(0.18))               # 0.745

TRIX_DEVELOPMENT_MINUTES = 8      # D-76 stock, large tank, 20 C: Kodak's standard time
TRIX_PUSH2_MINUTES = 10.75        # D-76 stock, large tank, 20 C: Kodak's time for EI 1600 (2-stop push)
TRIX_PUSH2_EI = 1600
NEOPAN_EI = 1600                  # D-76 stock, small tank, 7 1/2 min at 20 C: Fujifilm's time for EI 1600
FILM_CURVE_EXTENSION = 0.8        # decades past each sheet's last plotted exposure (see characteristic)
MULTIGRADE_NATIVE_GRADE = "1"     # ISO R110 on Ilford's table


@dataclass(frozen=True)
class Film:
    stock: str
    name: str
    iso: int
    notes: str


TRIX = Film("kodak_tri_x_400", "Kodak Tri-X 400", 400,
            "Kodak F-4017 (2021), KODAK PROFESSIONAL TRI-X 400 Film / 400TX, 35 mm, "
            f"D-76 developer, large tank, {TRIX_DEVELOPMENT_MINUTES} min at 20 C, diffuse visual density.")
TRIX_PUSH2 = Film("kodak_tri_x_400_push2", "Kodak Tri-X 400 (Push 2)", TRIX_PUSH2_EI,
                  "Kodak F-4017 (2021), KODAK PROFESSIONAL TRI-X 400 Film / 400TX, 35 mm, "
                  f"D-76 developer, large tank, {TRIX_PUSH2_MINUTES:g} min at 20 C, Kodak's time for EI 1600 "
                  "(2-stop push), interpolated between the published 10 and 12 minute curves along "
                  "exposure at each density; diffuse visual density.")
HP5 = Film("ilford_hp5_plus", "Ilford HP5 Plus", 400,
           "HARMAN technology, ILFORD HP5 PLUS technical information, 35 mm, "
           "ILFOTEC HC (1+31) 6.5 min at 20 C.")
APX25 = Film("agfa_apx_25", "Agfa APX 25", 25,
             "Agfa-Gevaert, AGFAPAN APX 25 PROFESSIONAL (Agfa Professional range, Technical Data), 35 mm; "
             "the plot names no developer, the sheet's ISO 25 and RMS 7.0 are for REFINAL, 6 min at 20 C.")
NEOPAN = Film("fujifilm_neopan_1600", "Fujifilm Neopan 1600", NEOPAN_EI,
              "Fujifilm data sheet AF3-608E, NEOPAN 1600 Super Presto / Professional, 35 mm, "
              "D-76 developer, small tank, 7 1/2 min at 20 C (Fujifilm's time for EI 1600).")
PAPER_STOCK = "ilford_multigrade_rc_deluxe"
PAPER_NAME = "Ilford Multigrade RC Deluxe"


# --------------------------------------------------------------------------
# Reading the sheets
# --------------------------------------------------------------------------

def _vector_plot(pdf: str, page: int, x_range: tuple[float, float], n_curves: int | None = None):
    """The upper-left plot on `page` whose own tick labels span `x_range`."""
    span = abs(x_range[1] - x_range[0])
    hits = []
    for frame in ds.frames(pdf, page, min_size=100):
        try:
            axes = ds.calibrate(pdf, page, frame)
        except ValueError:
            continue
        if abs(axes.x_min - x_range[0]) > 0.1 * span or abs(axes.x_max - x_range[1]) > 0.1 * span:
            continue
        curves = ds.curves_in(pdf, page, axes)
        if n_curves is None or len(curves) == n_curves:
            hits.append((frame.y0, frame.x0, axes, curves))
    if not hits:
        raise ValueError(f"no plot spanning {x_range} on page {page + 1} of {pdf}")
    hits.sort(key=lambda h: (h[0], h[1]))
    return hits[0][2], hits[0][3]


def _tri_x_development_curves() -> dict[int, np.ndarray]:
    """Page 9, upper left: 400TX 35 mm in D-76, large tank, at 6, 8, 10 and 12
    minutes. They separate cleanly by density at log H = 0."""
    pdf = str(SHEETS / "Kodak Tri-X 320 and 400 F-4017.pdf")
    _, curves = _vector_plot(pdf, 8, (-4, 1), n_curves=4)
    return dict(zip((12, 10, 8, 6), ds.order_curves(curves, at_x=0.0, descending=True)))


def tri_x_sheet() -> tuple[np.ndarray, np.ndarray]:
    """(wavelength, log sensitivity) and (log H in lux-seconds, density)."""
    pdf = str(SHEETS / "Kodak Tri-X 320 and 400 F-4017.pdf")
    # Page 8, "Spectral Sensitivity Curves": the upper curve is D = 1.0 over
    # gross fog, the lower one D = 0.3. Same shape; 1.0 is the conventional one.
    _, curves = _vector_plot(pdf, 7, (250, 750))
    spectral = max(curves, key=lambda c: c[:, 1].max())
    return spectral, _tri_x_development_curves()[TRIX_DEVELOPMENT_MINUTES]


def tri_x_push_curve(minutes: float) -> np.ndarray:
    """Tri-X in D-76 at a development time between two of Kodak's published ones.

    Interpolated along exposure at each net density, base + fog alike: the
    curves move steadily with time, the toe gaining speed and the slope
    rising (Kodak's contrast index 0.58 at 8 minutes, 0.68 at 10, 0.79 at 12),
    and a time between two of them sits between them.
    """
    by_time = _tri_x_development_curves()
    times = sorted(by_time)
    lo = max(t for t in times if t <= minutes)
    hi = min(t for t in times if t >= minutes)
    w = 0.0 if hi == lo else (minutes - lo) / (hi - lo)
    grid = np.linspace(-3.6, 0.6, 841)
    dens = {t: ds.resample(by_time[t], grid) for t in (lo, hi)}
    top = min((d - d.min()).max() for d in dens.values())
    levels = np.linspace(0.0, top, 400)

    def log_h(d):
        net = d - d.min()
        order = np.argsort(net)
        return np.interp(levels, net[order], grid[order])

    base = (1 - w) * dens[lo].min() + w * dens[hi].min()
    return np.stack([(1 - w) * log_h(dens[lo]) + w * log_h(dens[hi]), levels + base], axis=-1)


def _paths(page, width: float, min_items: int, inside: pymupdf.Rect):
    out = []
    for p in page.get_drawings():
        r = p["rect"]
        if (p.get("width") == width and len(p["items"]) >= min_items
                and r.x0 >= inside.x0 - 1 and r.x1 <= inside.x1 + 1
                and r.y0 >= inside.y0 - 1 and r.y1 <= inside.y1 + 1):
            out.append(ds._flatten_path([i for i in p["items"] if i[0] in ("l", "c")]))
    return out


def _frame_near(page, x0: float, y0: float) -> pymupdf.Rect:
    """The plot's own rectangle: the drawn 're' whose corner is nearest (x0, y0)."""
    rects = [p["rect"] for p in page.get_drawings() if any(i[0] == "re" for i in p["items"])]
    rects = [r for r in rects if r.width > 100 and r.height > 50]
    return min(rects, key=lambda r: abs(r.x0 - x0) + abs(r.y0 - y0))


def _short_lines(page, near: pymupdf.Rect, length: float = 5.0):
    """Tick marks: short straight segments just outside a plot frame."""
    xs, ys = [], []
    for p in page.get_drawings():
        for item in p["items"]:
            if item[0] != "l":
                continue
            a, b = item[1], item[2]
            if abs(a.x - b.x) < 0.01 and abs(a.y - b.y) <= length and near.x0 - 1 <= a.x <= near.x1 + 1 \
                    and min(a.y, b.y) >= near.y1 - 0.5 and max(a.y, b.y) <= near.y1 + length:
                xs.append(round(float(a.x), 2))
            if abs(a.y - b.y) < 0.01 and abs(a.x - b.x) <= length and near.y0 - 1 <= a.y <= near.y1 + 1 \
                    and min(a.x, b.x) >= near.x1 - 0.5 and max(a.x, b.x) <= near.x1 + length:
                ys.append(round(float(a.y), 2))
    return sorted(set(xs)), sorted(set(ys))


def _grid(page, inside: pymupdf.Rect):
    """Positions of the grid's vertical and horizontal lines inside a frame."""
    xs, ys = set(), set()
    for p in page.get_drawings():
        r = p["rect"]
        # the grid's path also carries the tick marks, a few points outside
        if not (r.x0 >= inside.x0 - 5 and r.x1 <= inside.x1 + 5 and r.y0 >= inside.y0 - 5 and r.y1 <= inside.y1 + 5):
            continue
        for item in p["items"]:
            if item[0] != "l":
                continue
            a, b = item[1], item[2]
            if abs(a.x - b.x) < 0.01 and abs(a.y - b.y) > 0.5 * inside.height:
                xs.add(round(float(a.x), 1))
            if abs(a.y - b.y) < 0.01 and abs(a.x - b.x) > 0.5 * inside.width:
                ys.add(round(float(a.y), 1))
    return sorted(xs), sorted(ys)


def _labels(page, inside: pymupdf.Rect, below: bool):
    """Numeric tick labels under (or right of) a frame, as (centre, value)."""
    out = []
    for w in page.get_text("words"):
        try:
            value = float(w[4].replace("·", "."))
        except ValueError:
            continue
        cx, cy = (w[0] + w[2]) / 2, (w[1] + w[3]) / 2
        if below and inside.y1 < cy < inside.y1 + 20 and inside.x0 - 10 < cx < inside.x1 + 10:
            out.append((cx, value))
        if not below and inside.x1 < w[0] < inside.x1 + 25 and inside.y0 - 10 < cy < inside.y1 + 10:
            out.append((cy, value))
    return out


def _snap(labels, lines):
    """Pair each label with the grid line it names."""
    return [(min(lines, key=lambda l: abs(l - pos)), value) for pos, value in labels]


def _fit_axis(pairs):
    pos = np.array([p for p, _ in pairs]); val = np.array([v for _, v in pairs])
    slope, intercept = np.polyfit(pos, val, 1)
    resid = np.abs(slope * pos + intercept - val).max()
    assert resid < 0.02 * (val.max() - val.min()), f"axis does not calibrate: {pairs}"
    return slope, intercept


def hp5_sheet() -> tuple[np.ndarray, np.ndarray]:
    """(wavelength, relative log sensitivity) and (relative log E, density)."""
    pdf = str(SHEETS / "Ilford HP5 Plus.pdf")
    doc = pymupdf.open(pdf)

    # Page 1, "Wedge spectrogram to tungsten light". The tick labels are drawn
    # as outlines rather than text, so the axis comes from the tick marks:
    # six along the bottom for 400-650 nm, two on the right for 1.0 and 0.5.
    page = doc[0]
    frame = _frame_near(page, 326, 410)
    xt, yt = _short_lines(page, frame)
    assert len(xt) == 6 and len(yt) == 2, (xt, yt)
    kx, bx = _fit_axis(list(zip(xt, np.arange(400.0, 651.0, 50.0))))
    ky, by = _fit_axis([(yt[0], 1.0), (yt[1], 0.5)])
    (curve,) = _paths(page, 1.5, 100, frame)
    spectral = np.stack([kx * curve[:, 0] + bx, ky * curve[:, 1] + by], axis=-1)

    # Page 5, "Characteristic curves": calibrated on the grid lines the
    # printed labels sit on.
    page = doc[4]
    frame = _frame_near(page, 326, 313)
    gx, gy = _grid(page, frame)
    kx, bx = _fit_axis(_snap(_labels(page, frame, below=True), gx + [frame.x0]))
    ky, by = _fit_axis(_snap(_labels(page, frame, below=False), gy + [frame.y1]))
    (curve,) = _paths(page, 1.5, 30, frame)
    characteristic = np.stack([kx * curve[:, 0] + bx, ky * curve[:, 1] + by], axis=-1)
    return spectral, characteristic


def apx25_sheet() -> tuple[np.ndarray, np.ndarray]:
    """(wavelength, relative log sensitivity) and (log H in lux-seconds, density),
    off page 10 of Agfa's range sheet: vector plots, one curve each, labelled
    in text on its rules."""
    from vector_plots import VectorPlot, by_position
    pdf = str(SHEETS / "agfa_films_data.pdf")
    sensitivity = VectorPlot(pdf, 9, (53.4, 80.2, 179.8, 175.0),
                             ((69.2, 400), (100.8, 500), (132.4, 600), (164.0, 700)),
                             ((80.2, 1.0), (111.8, 0.0), (143.4, -1.0), (175.0, -2.0)))
    curve = VectorPlot(pdf, 9, (53.7, 227.5, 179.8, 290.4),
                       tuple(zip((53.7, 64.3, 74.8, 85.4, 95.9, 106.4, 116.9, 127.4, 137.9, 148.4, 158.9, 169.4,
                                  179.8), np.arange(-4.0, 2.01, 0.5))),
                       ((290.4, 0.0), (279.9, 0.5), (269.5, 1.0), (259.0, 1.5), (248.5, 2.0), (238.0, 2.5),
                        (227.5, 3.0)))
    spectral = by_position(sensitivity, ("s",))["s"]
    characteristic = by_position(curve, ("d",))["d"]
    return spectral[np.argsort(spectral[:, 0])], characteristic[np.argsort(characteristic[:, 0])]


def _bitmap(pdf: str, page: int, index: int) -> np.ndarray:
    doc = pymupdf.open(pdf)
    xref = doc[page].get_images(full=True)[index][0]
    pix = pymupdf.Pixmap(doc, xref)
    if pix.n > 4 or pix.alpha:
        pix = pymupdf.Pixmap(pymupdf.csRGB, pix)
    rgb = np.frombuffer(pix.samples, dtype=np.uint8).reshape(pix.height, pix.width, pix.n)
    return np.asarray(Image.fromarray(rgb[..., :3]).convert("L"), dtype=float)


def _runs(mask_line: np.ndarray, gap: int = 1):
    idx = np.where(mask_line)[0]
    if idx.size == 0:
        return []
    return np.split(idx, np.where(np.diff(idx) > gap)[0] + 1)


def _lines(profile: np.ndarray, threshold: float):
    """Centres of the runs where a projection profile exceeds `threshold`."""
    return [float(g.mean()) for g in _runs(profile > threshold, gap=2)]


def _stencil(pdf: str, page: int, index: int) -> np.ndarray:
    """Ink of a scanned 1-bit image mask (CCITT), which has no colour space for
    a Pixmap; decoded, the ink is the bright value."""
    doc = pymupdf.open(pdf)
    xref = doc[page].get_images(full=True)[index][0]
    image = Image.open(io.BytesIO(doc.extract_image(xref)["image"]))
    return np.asarray(image.convert("L"), dtype=float) > 128


def neopan_sheet() -> tuple[np.ndarray, np.ndarray]:
    """(wavelength, log sensitivity) and (log H in lux-seconds, density)."""
    pdf = str(SHEETS / "datasheet_neopan1600superpresto_en_01.pdf")

    # Page 3, "Spectral Sensitivity Curve": one curve, grid lines at 400-700 nm,
    # and two horizontal lines a decade apart (the plot's only scale).
    ink = _stencil(pdf, 2, 0)
    vx = _lines(ink[45:500, :].sum(axis=0), 0.5 * 455)
    nm = [v for v in vx if 100 < v < 670]                       # 400, 500, 600, 700
    hy = [y + 45 for y in _lines(ink[45:500, 85:680].sum(axis=1), 0.3 * 595)]
    assert len(nm) == 4 and len(hy) == 3, (vx, hy)
    kx, bx = _fit_axis(list(zip(nm, [400.0, 500.0, 600.0, 700.0])))
    decade = hy[1] - hy[0]
    clean = ink.copy()
    for v in vx:
        clean[:, int(round(v)) - 1:int(round(v)) + 2] = False
    for y in hy:
        clean[int(round(y)) - 1:int(round(y)) + 2, :] = False
    pts = []
    for x in range(int(vx[1]) + 5, int(nm[2]) + 130):
        tops = [r.mean() + 150 for r in _runs(clean[150:420, x]) if len(r) < 14]
        if tops:
            pts.append((kx * x + bx, (hy[1] - min(tops)) / decade))
    spectral = np.array(pts)
    # A spectrogram to daylight records sensitivity times the source: take the
    # source back out (CIE daylight at 5400 K, on the plotted wavelengths).
    xy = colour.temperature.CCT_to_xy_CIE_D(5400 * 1.4388 / 1.4380)
    daylight = colour.sd_CIE_illuminant_D_series(xy)
    spectral[:, 1] -= np.log10(np.array([daylight[w] for w in spectral[:, 0]]))

    # Page 4, "D-76": three development times; 7 1/2 min, the highest, is
    # Fujifilm's time for EI 1600. Dashed grid every 0.5 in log H and density.
    ink = _stencil(pdf, 3, 1)
    vx = _lines(ink[40:512, :].sum(axis=0), 0.25 * 472)
    hy = [y for y in _lines(ink[:, 100:900].sum(axis=1), 0.25 * 800) if y < 520]
    assert len(vx) == 11 and len(hy) == 7, (vx, hy)
    kx, bx = _fit_axis(list(zip(vx, np.arange(-4.0, 1.01, 0.5))))
    ky, by = _fit_axis(list(zip(sorted(hy, reverse=True), np.arange(0.0, 3.01, 0.5))))
    clean = ink.copy()
    for v in vx:
        clean[:, int(round(v)) - 1:int(round(v)) + 2] = False
    for y in hy:
        clean[int(round(y)) - 1:int(round(y)) + 2, :] = False
    # From the shared toe, column by column, the highest stroke near the last
    # one: the labels beside the curves never come within reach.
    pts, last = [], (0.19 - by) / ky                           # the toe, at base + fog
    for x in range(int(vx[0]) + 12, int(vx[9]) - 46):
        near = [r.mean() + 45 for r in _runs(clean[45:506, x]) if len(r) < 14]
        near = [c for c in near if abs(c - last) <= 6]
        if near:
            last = min(near)
            pts.append((kx * x + bx, ky * last + by))
    return spectral, np.array(pts)


def multigrade_sheet() -> tuple[np.ndarray, dict[str, np.ndarray]]:
    """(wavelength, relative sensitivity) and {grade: (relative log E, density)}."""
    pdf = str(SHEETS / "Ilford Multigrade RC Deluxe.pdf")

    # Page 2, "Spectral Sensitivity": one black curve in a frame; six tick
    # marks under the axis for 400-650 nm. No vertical scale is printed.
    img = _bitmap(pdf, 1, 0)
    dark = img < 110
    cols = _lines(dark.sum(axis=0), 0.5 * img.shape[0])       # frame's left and right edges
    rows = _lines(dark.sum(axis=1), 0.5 * img.shape[1])       # frame's top and bottom edges
    left, right, top, bottom = cols[0], cols[-1], rows[0], rows[-1]
    under = dark[int(bottom) + 2:int(bottom) + 9, :].sum(axis=0) >= 3
    ticks = [float(g.mean()) for g in _runs(under)]
    assert len(ticks) == 6, ticks
    kx, bx = _fit_axis(list(zip(ticks, np.arange(400.0, 651.0, 50.0))))
    xs, ys = [], []
    for x in range(int(left) + 4, int(right) - 3):
        runs = _runs(dark[int(top) + 3:int(bottom) - 1, x])
        if runs:
            xs.append(x)
            ys.append(runs[0].mean() + int(top) + 3)
    xs, ys = np.array(xs, dtype=float), np.array(ys)
    height = bottom - ys
    spectral = np.stack([kx * xs + bx, height / height.max()], axis=-1)

    # Page 3, "NEW MULTIGRADE RC DELUXE", left plot: grades 00 to 3 through
    # filters. Grey grid at whole log-exposure units and half density units.
    img = _bitmap(pdf, 2, 2)
    grey = (img > 120) & (img < 225)
    vx = _lines(grey.sum(axis=0), 0.25 * img.shape[0])
    hy = _lines(grey.sum(axis=1), 0.25 * img.shape[1])
    # vertical lines: log E 0, 1, 2, 3, 4 and the frame at 4.5
    kx, bx = _fit_axis(list(zip(vx[:5], [0.0, 1.0, 2.0, 3.0, 4.0])))
    # horizontal lines bottom-up: density 0, 0.5, ... 2.5
    ky, by = _fit_axis(list(zip(sorted(hy, reverse=True), np.arange(0.0, 0.5 * len(hy), 0.5))))
    dark = img < 100
    # the "3" and "00" annotations sit inside the plot
    for x0, y0, x1, y1 in ((318, 100, 345, 130), (440, 110, 480, 140)):
        dark[y0:y1, x0:x1] = False
    x_left, x_right = int(vx[0]) + 2, int(vx[-1]) - 1
    y_top, y_bottom = int(min(hy)) + 2, int(max(hy)) - 2

    grades = ["3", "2", "1", "0", "00"]
    traced = {g: [] for g in grades}
    # Row by row, where the five curves are apart: one crossing each, hardest
    # (leftmost) first.
    for y in range(y_top, y_bottom):
        runs = _runs(dark[y, x_left:x_right])
        if len(runs) == len(grades):
            for g, r in zip(grades, runs):
                traced[g].append((kx * (r.mean() + x_left) + bx, ky * y + by))
    # Column by column for the shared toe and the plateau, where they merge
    # into one stroke and a row would run along the line instead of across it.
    toe, plateau = [], []
    for x in range(x_left, x_right):
        runs = _runs(dark[y_top:y_bottom, x])
        if len(runs) != 1:
            continue
        d = ky * (runs[0].mean() + y_top) + by
        e = kx * x + bx
        if d < 0.55:
            toe.append((e, d))
        elif d > 2.0 and e > 3.0:
            plateau.append((e, d))
    curves = {}
    for g in grades:
        pts = sorted(toe + traced[g] + plateau)
        curves[g] = np.array(pts)
    return spectral, curves


# --------------------------------------------------------------------------
# Profiles
# --------------------------------------------------------------------------

def on_grid(curve: np.ndarray, grid: np.ndarray) -> np.ndarray:
    """Resample, leaving NaN outside what the sheet actually plots."""
    values = ds.resample(curve, grid)
    values[(grid < curve[:, 0].min()) | (grid > curve[:, 0].max())] = np.nan
    return values


def neutral_mid_grey_raw(log_sensitivity: np.ndarray, reference_illuminant: str) -> float:
    """What an 18% grey exposes, the way the bake and the engine compute it."""
    sensitivity = np.repeat(np.nan_to_num(10 ** log_sensitivity)[:, None], 3, axis=1)
    adaptation =Hanatos2025SensitivityAdaptation(reference_illuminant=reference_illuminant,
                                                  apply_window=False, apply_surface=False)
    tc_lut = compute_hanatos2025_tc_lut(sensitivity, adaptation)
    grey = np.full((1, 1, 3), 0.184)
    raw = rgb_to_raw_hanatos2025(grey, sensitivity, color_space=WORKING_SPACE,
                                 apply_cctf_decoding=False,
                                 reference_illuminant=reference_illuminant, tc_lut=tc_lut)
    return float(np.ravel(raw)[0])


def speed_point(log_e: np.ndarray, net: np.ndarray) -> float:
    order = np.argsort(net)
    return float(np.interp(SPEED_POINT_NET_DENSITY, net[order], log_e[order]))


def characteristic(curve: np.ndarray, anchor, extend: float = 0.0) -> tuple[np.ndarray, float, float, dict]:
    """Fit agx's sum of normal CDFs to a sheet curve and lay it on agx's axis.

    `anchor(log_e, net)` returns the sheet log E that should become 0.
    `extend` continues the curve past the sheet's last plotted exposure at its
    final gradient, for that many decades: a black-and-white negative has no
    shoulder anywhere in the range a datasheet plots, and holding the last
    value instead would clip every highlight past it to one density. Returns
    the fitted net density (256, 3), the base + fog, the shift applied, and
    the fitted model.
    """
    sheet_e = np.linspace(curve[:, 0].min(), curve[:, 0].max(), 1024)
    density = ds.resample(curve, sheet_e)
    if extend > 0:
        tail = sheet_e > sheet_e[-1] - 0.4
        gradient = float(np.polyfit(sheet_e[tail], density[tail], 1)[0])
        more = np.linspace(sheet_e[-1], sheet_e[-1] + extend, 160)[1:]
        sheet_e = np.concatenate([sheet_e, more])
        density = np.concatenate([density, density[-1] + gradient * (more - more[0] + (more[1] - more[0]))])
    base = float(density.min())
    net = density - base
    shift = -anchor(sheet_e, net)
    e = sheet_e + shift
    # Hold the end values outside the plotted range, as agx's own interpolation does.
    net_on_axis = np.interp(LOG_EXPOSURE, e, net)
    three = np.repeat(net_on_axis[:, None], 3, axis=1)
    centers, amplitudes, sigmas = fit_cdf_model(LOG_EXPOSURE, three, positive=False)
    fitted = layer_curves(LOG_EXPOSURE, centers, amplitudes, sigmas, positive=False).sum(axis=1)
    rms = float(np.sqrt(np.mean((fitted - three) ** 2)))
    model = dict(centers=centers, amplitudes=amplitudes, sigmas=sigmas, rms=rms)
    return fitted, base, shift, model


def metadata(datasource: str) -> ProfileMetadata:
    return ProfileMetadata(
        version="0.3.4",
        copyright="FLX Film Lab. Profile data measured off the manufacturer's published datasheet.",
        created="2026-10-14",
        license="Profile format: spektrafilm (https://github.com/andreavolpato/spektrafilm), CC BY-SA 4.0. "
                "The values are the datasheet's own published measurements.",
        citation="Format and model: spektrafilm, https://github.com/andreavolpato/spektrafilm.",
        datasource=datasource,
    )


def assemble(info: ProfileInfo, log_sensitivity: np.ndarray, base_density: float,
             net: np.ndarray, model: dict, datasource: str) -> Profile:
    """One silver layer, carried as three identical channels a third each."""
    centers, amplitudes, sigmas = model["centers"], model["amplitudes"], model["sigmas"]
    data = ProfileData(
        wavelengths=WAVELENGTHS,
        log_sensitivity=np.repeat(log_sensitivity[:, None], 3, axis=1),
        channel_density=np.full((WAVELENGTHS.size, 3), 1.0 / 3.0),
        base_density=np.full(WAVELENGTHS.size, base_density),
        log_exposure=LOG_EXPOSURE,
        density_curves=net,
        density_curves_layers=layer_curves(LOG_EXPOSURE, centers, amplitudes, sigmas, positive=False),
        density_curves_model=DensityCurvesModel(model_type="cdfs", centers=centers,
                                                amplitudes=amplitudes, sigmas=sigmas),
    )
    return Profile(metadata=metadata(datasource), info=info, data=data)


def build_film(film: Film, spectral: np.ndarray, curve: np.ndarray, sensitivity_note: str,
               cross_check=None, metered_at: int | None = None) -> Profile:
    """`metered_at`: place mid-grey where a meter set to that exposure index
    puts it on the sheet's absolute lux-second axis, for a film whose rating
    is not its ISO speed. Otherwise mid-grey is 1.007 decades above the ISO
    speed point."""
    if metered_at is None:
        anchor = lambda e, net: speed_point(e, net) + MID_TONE_ABOVE_SPEED_POINT
        placement = (f"with the ISO speed point (0.10 over base + fog) {MID_TONE_ABOVE_SPEED_POINT:.3f} "
                     f"decades below mid-grey, box speed ISO {film.iso}")
    else:
        anchor = lambda e, net: float(np.log10(0.65 * 12.5 / metered_at))
        sheet = np.linspace(curve[:, 0].min(), curve[:, 0].max(), 1024)
        density = ds.resample(curve, sheet)
        iso = 0.8 / 10 ** speed_point(sheet, density - density.min())
        placement = (f"with mid-grey where a meter set to EI {metered_at} puts it on the sheet's absolute "
                     f"axis (log H {np.log10(0.65 * 12.5 / metered_at):.3f}); the curve's own ISO speed "
                     f"point is ISO {iso:.0f}")
        print(f"    {film.stock}: ISO speed point of the sheet curve ISO {iso:.0f}, rated EI {metered_at}")
    net, base, shift, model = characteristic(curve, anchor, extend=FILM_CURVE_EXTENSION)

    log_s = on_grid(spectral, WAVELENGTHS)
    log_s -= np.log10(neutral_mid_grey_raw(log_s, "D55"))     # an 18% grey exposes 1, as in agx

    info = ProfileInfo(stock=film.stock, name=film.name, type="negative", support="film",
                       stage="filming", use="still", antihalation="strong",
                       target_print=PAPER_STOCK, channel_model="bw", densitometer="diffuse_visual",
                       log_sensitivity_density_over_min=1.0, reference_illuminant="D55",
                       viewing_illuminant="D50")
    datasource = (
        f"Measured off {film.notes} Characteristic curve laid on agx's axis {placement}; "
        f"base + fog {base:.3f}. Continued at its final gradient for "
        f"{FILM_CURVE_EXTENSION} decades past the last plotted exposure. {sensitivity_note} "
        "One silver layer, carried as three identical channels."
    )
    profile = assemble(info, log_s, base, net, model, datasource)
    dmax = net[:, 0].max()
    msg = (f"  {film.stock:28s} base+fog {base:.3f}  D-max net {dmax:.2f}  "
           f"axis shift {shift:+.3f}  fit RMS {model['rms']:.4f}")
    if cross_check is not None:
        msg += f"  ({cross_check(shift)})"
    print(msg)
    return profile


def iso_range(e: np.ndarray, d: np.ndarray) -> float:
    """ISO 6846 paper range: log H from D-min + 0.04 to D-min + 0.9 (D-max - D-min)."""
    lo, hi = d.min(), d.max()
    order = np.argsort(d)
    toe = np.interp(lo + 0.04, d[order], e[order])
    shoulder = np.interp(lo + 0.9 * (hi - lo), d[order], e[order])
    return 100.0 * (shoulder - toe)


def build_paper(spectral: np.ndarray, curves: dict[str, np.ndarray]) -> Profile:
    for g, c in curves.items():
        grid = np.linspace(c[:, 0].min(), c[:, 0].max(), 1024)
        print(f"    grade {g:>2s}: traced ISO R{iso_range(grid, ds.resample(c, grid)):.0f}")
    curve = curves[MULTIGRADE_NATIVE_GRADE]
    # A print of a mid-grey negative is mid-grey: log E = 0 lands on 18% reflectance.
    base_guess = float(curve[:, 1].min())
    anchor = lambda e, net: float(np.interp(MID_GREY_REFLECTION_DENSITY - base_guess, net, e))
    net, base, shift, model = characteristic(curve, anchor)

    # Read like the films' spectrograms: relative log sensitivity, peak 1.0.
    log_s = on_grid(spectral, WAVELENGTHS)
    log_s += _paper_level(log_s)

    info = ProfileInfo(stock=PAPER_STOCK, name=PAPER_NAME, type="negative", support="paper",
                       stage="printing", use="still", antihalation="strong", target_print=None,
                       channel_model="bw", densitometer="diffuse_visual",
                       log_sensitivity_density_over_min=1.0, reference_illuminant="TH-KG3",
                       viewing_illuminant="D50")
    datasource = (
        "Traced off the bitmaps of HARMAN technology, 'New MGRC vs MGIVRC' technical information "
        f"(Mar 2020): MULTIGRADE RC DELUXE (new) through filter grade {MULTIGRADE_NATIVE_GRADE}, "
        "MULTIGRADE developer 1+9, 1 min at 20 C; spectral sensitivity read as relative log "
        "sensitivity, peak 1.0, level set so the paper sees as much of the enlarger lamp as "
        f"Kodak Portra Endura does through its neutral filtration. Base {base:.3f}; mid-grey "
        "(18% reflectance) at log E 0. One silver layer, carried as three identical channels."
    )
    profile = assemble(info, log_s, base, net, model, datasource)
    print(f"  {PAPER_STOCK:28s} base {base:.3f}  D-max net {net[:, 0].max():.2f}  "
          f"axis shift {shift:+.3f}  fit RMS {model['rms']:.4f}  "
          f"native ISO R{iso_range(LOG_EXPOSURE, net[:, 0] + base):.0f}")
    return profile


def _paper_level(log_s: np.ndarray) -> float:
    """Offset that makes the paper as sensitive to the lamp as Portra Endura.

    Pre-flash is added to the print exposure *after* the mid-grey
    normalisation, so its strength depends on the paper's absolute
    sensitivity. Matching what each paper collects from the unfiltered-for-it
    lamp keeps Highlight protection doing the same thing on either paper.
    """
    shared = json.loads((PACKS / "shared.json").read_text())
    endura = json.loads((PACKS / "kodak_portra_endura.json").read_text())
    lamp = np.asarray(shared["illuminants"]["TH-KG3"])
    dichroics = np.asarray(shared["dichroicFilters"]).reshape(-1, 3)
    cmy = shared["neutralPrintFilters"]["kodak_portra_endura"]["TH-KG3"]["kodak_portra_400"]
    t = 10 ** (-np.asarray(cmy) / 100)
    filtered = lamp * np.prod(1 - (1 - dichroics) * (1 - t), axis=1)
    s_endura = np.asarray(endura["sensitivity"]).reshape(-1, 3)
    endura_raw = np.exp(np.mean(np.log((filtered[:, None] * s_endura).sum(axis=0))))
    ours = float(np.sum(lamp * np.nan_to_num(10 ** log_s)))
    return float(np.log10(endura_raw / ours))


def write(profile: Profile) -> Path:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUT_DIR / f"{profile.info.stock}.json"
    path.write_text(json.dumps(_json_safe(profile_to_dict(profile)), indent=4, allow_nan=False))
    return path


def main() -> int:
    print(f"building black-and-white profiles from datasheets -> {OUT_DIR}")
    spectral, curve = tri_x_sheet()
    # Kodak plots against absolute lux-seconds: the ISO 400 metered mid-tone,
    # 0.65 x 12.5 / 400 lux-s, should land where the speed point puts it.
    absolute = float(np.log10(0.65 * 12.5 / TRIX.iso))
    write(build_film(TRIX, spectral, curve,
                     "Spectral sensitivity: the D = 1.0 over gross fog curve (equal-energy, "
                     "erg/cm^2, effective exposure 0.5 s).",
                     cross_check=lambda shift: f"Kodak's absolute axis puts mid-grey {(-shift) - absolute:+.3f} "
                                               f"decades from ISO {TRIX.iso}'s"))
    write(build_film(TRIX_PUSH2, spectral, tri_x_push_curve(TRIX_PUSH2_MINUTES),
                     "Spectral sensitivity: as Tri-X 400, the D = 1.0 over gross fog curve.",
                     metered_at=TRIX_PUSH2_EI))
    spectral, curve = hp5_sheet()
    write(build_film(HP5, spectral, curve,
                     "Spectral sensitivity: Ilford's wedge spectrogram read as relative log "
                     "sensitivity (read as tungsten-weighted it implies a 12-16x red filter factor)."))
    spectral, curve = apx25_sheet()
    # Agfa plots against absolute lux-seconds, as Kodak does.
    absolute = float(np.log10(0.65 * 12.5 / APX25.iso))
    write(build_film(APX25, spectral, curve,
                     "Spectral sensitivity: Agfa's curve as plotted (equal-energy spectrum, read at density "
                     "1.0 above minimum density).",
                     cross_check=lambda shift: f"Agfa's absolute axis puts mid-grey {(-shift) - absolute:+.3f} "
                                               f"decades from ISO {APX25.iso}'s"))
    spectral, curve = neopan_sheet()
    write(build_film(NEOPAN, spectral, curve,
                     "Spectral sensitivity: Fujifilm's spectrogram to daylight (5400K), divided by CIE "
                     "daylight at 5400 K (read as it stands it implies a 6.6x red filter factor, "
                     "against Fujifilm's 8x).",
                     metered_at=NEOPAN_EI))
    spectral, curves = multigrade_sheet()
    write(build_paper(spectral, curves))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
