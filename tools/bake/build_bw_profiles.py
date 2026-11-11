#!/usr/bin/env python
"""Build black-and-white film and paper profiles from manufacturer datasheets.

Six stocks, each from its maker's own sheet in `film spec sheets/`:

- Kodak Tri-X 400 (F-4017): spectral sensitivity and the D-76 characteristic
  curves, both vector plots, read exactly. Also pushed two stops: Kodak's D-76
  time for EI 1600, interpolated between the sheet's 10 and 12 minute curves.
- Kodak T-Max 100 (F-4016): spectral sensitivity and the D-76 characteristic
  curves, vector plots. Kodak's small-tank time, 6 1/2 minutes, lies between
  the sheet's 6 and 7 1/2 minute curves and is interpolated like Tri-X's push.
- Ilford HP5 Plus: spectral sensitivity and the characteristic curve, vector
  plots, calibrated on their own tick marks and grid.
- Fujifilm Neopan 1600 (AF3-608E): spectral sensitivity and the D-76
  characteristic curves, scanned line art (CCITT image masks), traced.
- Agfa APX 25 (Agfa Professional range sheet): spectral sensitivity and the
  characteristic curve, vector plots labelled in text.
- Ilford Multigrade RC Deluxe: spectral sensitivity and the curves for each
  filter grade, bitmaps, traced.
- Ilford Delta 100, Delta 400, FP4 Plus, Pan F Plus and Ortho Plus (HARMAN
  technology's technical information, 2018-19): the wedge spectrogram and the
  characteristic curve for the developer and time each sheet names, bitmaps
  on a ruled grid, traced (`harman_sheet`).
- Kodak Double-X 5222, Kodak's black-and-white motion picture negative, and
  Kodak 2302, the release print film it is printed on (H-1-5222 in its
  editions of 2003 and 2022, H-1-2302): `kodak_bw_cine_sheets`. 2302 is the
  first black-and-white print film here, projected like agx's 2383.

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
  relative log sensitivity they give 6x, so that is how they are read. Ortho
  Plus's sheet bears this out with numbers of its own: it rates the film ISO 80
  in daylight and 40 in tungsten light, and its spectrogram read this way puts
  daylight 1.92 times as effective as tungsten at equal illuminance, where read
  as tungsten-weighted it gives 2.45; its daylight factor for a yellow filter,
  2.5, comes out 2.7 through Kodak's Wratten 8, against 5.3.
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
- Double-X's mid-grey goes at Kodak's normal exposure, 10/EI, as the colour
  motion picture films' does: its own exposure table, worked through for an
  18% grey card, gives exactly that, not the 8.1/EI of a reflected-light
  meter's mid-tone.
- 2302's sheet disagrees with itself about its contrast. Its control gamma is
  2.4 to 2.6, and its gamma-against-time plot reaches the middle, 2.5, at 6.52
  minutes, which is the development taken (interpolated between its 5 and 7
  minute curves). Measured by the formula the sheet states for its control
  gamma, though, every plotted curve is 0.23 to 0.47 above that plot, and this
  one 2.78; the 5 minute curve, 2.53 so measured, would have been the other
  reading, a print about a tenth softer.

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
TMAX_DEVELOPMENT_MINUTES = 6.5    # D-76 stock, small tank, 20 C: Kodak's time
NEOPAN_EI = 1600                 # D-76 stock, small tank, 7 1/2 min at 20 C: Fujifilm's time for EI 1600
FILM_CURVE_EXTENSION = 0.8        # decades past each sheet's last plotted exposure (see characteristic)
MULTIGRADE_NATIVE_GRADE = "1"     # ISO R110 on Ilford's table
DOUBLE_X_MINUTES = 6.5            # D-96 at 21 C: the curve the exposure index's "gamma of 0.65" names
DOUBLE_X_EI = 250                 # daylight, the light its curves are exposed to; 200 to tungsten
PRINT_2302_GAMMA = 2.5            # the middle of 2302's control gamma, 2.4 to 2.6, on its gamma plot
# A projected print's mid-grey, agx's rule for every print film: 18.4% of the
# projector's light through it.
MID_GREY_TRANSMISSION_DENSITY = float(-np.log10(0.184))           # 0.735


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
TMAX = Film("kodak_t_max_100", "Kodak T-Max 100", 100,
            "Kodak Alaris F-4016 (rev. 6-18), KODAK PROFESSIONAL T-MAX 100 Film / 100TMX, D-76 developer, "
            f"small tank, {TMAX_DEVELOPMENT_MINUTES:g} min at 20 C (Kodak's time), interpolated between the "
            "published 6 and 7 1/2 minute curves along exposure at each density; diffuse visual density.")
HP5 = Film("ilford_hp5_plus", "Ilford HP5 Plus", 400,
           "HARMAN technology, ILFORD HP5 PLUS technical information, 35 mm, "
           "ILFOTEC HC (1+31) 6.5 min at 20 C.")
APX25 = Film("agfa_apx_25", "Agfa APX 25", 25,
             "Agfa-Gevaert, AGFAPAN APX 25 PROFESSIONAL (Agfa Professional range, Technical Data), 35 mm; "
             "the plot names no developer, the sheet's ISO 25 and RMS 7.0 are for REFINAL, 6 min at 20 C.")
NEOPAN = Film("fujifilm_neopan_1600", "Fujifilm Neopan 1600", NEOPAN_EI,
              "Fujifilm data sheet AF3-608E, NEOPAN 1600 Super Presto / Professional, 35 mm, "
              "D-76 developer, small tank, 7 1/2 min at 20 C (Fujifilm's time for EI 1600).")
# HARMAN technology's current sheets, each with the characteristic curve for
# the developer and time it names. Their curves come out at about the normal
# contrast Ilford sets its bold times for (G-bar 0.62): Delta 100 0.65, Delta
# 400 0.64, FP4 Plus 0.68, Pan F Plus 0.71.
DELTA100 = Film("ilford_delta_100", "Ilford Delta 100", 100,
                "HARMAN technology, ILFORD DELTA 100 PROFESSIONAL technical information (Nov 2018), roll film "
                "(as representative of 35 mm and sheet film), ID-11 stock 8 1/2 min at 20 C, intermittent "
                "agitation.")
DELTA400 = Film("ilford_delta_400", "Ilford Delta 400", 400,
                "HARMAN technology, ILFORD DELTA 400 PROFESSIONAL technical information (Nov 2018), ID-11 stock "
                "8 min at 24 C, intermittent agitation: the sheet's only characteristic curve, at Ilford's "
                "normal contrast (G-bar 0.62; the curve's own 0.64).")
FP4 = Film("ilford_fp4_plus", "Ilford FP4 Plus", 125,
           "HARMAN technology, ILFORD FP4 PLUS technical information (Nov 2018), roll film (as representative "
           "of 35 mm and sheet film), ILFOTEC HC (1+31) 8 min at 20 C, intermittent agitation.")
PANF = Film("ilford_pan_f_plus", "Ilford Pan F Plus", 50,
            "HARMAN technology, ILFORD PAN F PLUS technical information (Dec 2018), roll film (as representative "
            "of 35 mm), ILFOTEC HC (1+31) 4 min at 20 C, intermittent agitation.")
ORTHO = Film("ilford_ortho_plus", "Ilford Ortho Plus", 80,
             "HARMAN technology, ILFORD ORTHO PLUS technical information (Oct 2019), ID-11 stock 8 min at 20 C, "
             "intermittent agitation: the lower of the sheet's two pictorial curves, G-bar 0.62 (Ilford calls "
             "0.62-0.70 normal for camera use, and rates the film ISO 80 to daylight in ID-11 at normal contrast).")
DOUBLEX = Film("kodak_double_x_5222", "Kodak Double-X 5222", DOUBLE_X_EI,
                "Kodak H-1-5222 (3-22), EASTMAN DOUBLE-X Negative Film 5222 / 7222: its characteristic curve "
                f"for D-96 at 21 C, {DOUBLE_X_MINUTES:g} min (gamma 0.66), the development its exposure index "
                "names ('for development to a gamma of 0.65'), daylight exposure, Status M (blue) density; its "
                "spectral sensitivity and MTF off the vector plots of the edition of 10/2003 (H-1-5222), which "
                "the 3-22 edition's bitmaps repeat.")
PAPER_STOCK = "ilford_multigrade_rc_deluxe"
PAPER_NAME = "Ilford Multigrade RC Deluxe"
PRINT_2302_STOCK = "kodak_2302"
PRINT_2302_NAME = "Kodak 2302"


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
    """Tri-X in D-76 at a development time between two of Kodak's published
    ones (Kodak's contrast index 0.58 at 8 minutes, 0.68 at 10, 0.79 at 12)."""
    return between_times(_tri_x_development_curves(), minutes, np.linspace(-3.6, 0.6, 841))


def _t_max_development_curves() -> dict[float, np.ndarray]:
    """Page 8, upper right: T-MAX 100 in D-76, small tank, at 6, 7 1/2 and 10
    minutes. They separate cleanly by density at log H = 0."""
    pdf = str(SHEETS / "f4016_tmax_100-2018.pdf")
    _, curves = _vector_plot(pdf, 7, (-4, 1), n_curves=3)
    return dict(zip((10.0, 7.5, 6.0), ds.order_curves(curves, at_x=0.0, descending=True)))


def t_max_sheet() -> tuple[np.ndarray, np.ndarray]:
    """(wavelength, log sensitivity) and (log H in lux-seconds, density).

    Kodak's contrast index for T-MAX 100 in D-76 (page 9, small tank) runs
    from 0.56 at 6.3 minutes to 0.82 at 9.1, about 0.58 at its 6 1/2 minutes;
    the curve interpolated here measures 0.585, and the sheet's 6 and 7 1/2
    minute curves 0.54 and 0.69.
    """
    pdf = str(SHEETS / "f4016_tmax_100-2018.pdf")
    # Page 8, "Spectral Sensitivity Curves", each curve labelled on the plot:
    # the upper one D = 0.3 over D-min, the lower D = 1.0, the conventional
    # one. It stops at 670 nm, two decades down.
    _, curves = _vector_plot(pdf, 7, (250, 750))
    spectral = min(curves, key=lambda c: c[:, 1].max())
    curve = between_times(_t_max_development_curves(), TMAX_DEVELOPMENT_MINUTES, np.linspace(-3.15, 0.65, 761))
    return spectral, curve


def between_times(by_time: dict[float, np.ndarray], minutes: float, grid: np.ndarray) -> np.ndarray:
    """A development time between two of a sheet's published curves.

    Interpolated along exposure at each net density, base + fog alike: the
    curves move steadily with time, the toe gaining speed and the slope
    rising, and a time between two of them sits between them.
    """
    times = sorted(by_time)
    lo = max(t for t in times if t <= minutes)
    hi = min(t for t in times if t >= minutes)
    w = 0.0 if hi == lo else (minutes - lo) / (hi - lo)
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


def harman_spectrogram(pdf: str, page: int, index: int) -> np.ndarray:
    """(wavelength, value as plotted) off one of HARMAN's 2018-19 wedge
    spectrograms: one black curve in a frame (light grey on Ortho Plus), a tick
    for 1.0 and one for 0.5 on its right edge, and 400 to 650 nm lettered under
    it, all drawn into the bitmap."""
    img = _bitmap(str(SHEETS / pdf), page, index)
    dark, ruled = img < 120, img < 200
    h, w = dark.shape
    cols, rows = _lines(ruled.sum(axis=0), 0.5 * h), _lines(ruled.sum(axis=1), 0.5 * w)
    left, right, top, bottom = cols[0], cols[-1], rows[0], rows[-1]
    strip = ruled[int(top) + 2:int(bottom) - 1, int(right) + 1:int(right) + 11]
    ticks = [g.mean() + int(top) + 2 for g in _runs(strip.sum(axis=1) >= 3, gap=2)]
    assert len(ticks) == 2, (pdf, "ticks", ticks)
    ky, by = _fit_axis([(ticks[0], 1.0), (ticks[1], 0.5)])
    # each label's glyphs, as a group of inked columns under the frame
    under = dark[int(bottom) + 4:int(bottom) + 40, int(left):int(right) + 30]
    labels = [g.mean() + int(left) for g in _runs(under.sum(axis=0) > 0, gap=9) if len(g) > 8]
    assert len(labels) == 6, (pdf, "labels", labels)
    kx, bx = _fit_axis(list(zip(labels, np.arange(400.0, 651.0, 50.0))))
    xs, ys = [], []
    for x in range(int(left) + 4, int(right) - 3):
        runs = _runs(dark[int(top) + 4:int(bottom) - 3, x])
        if runs:
            xs.append(x)
            ys.append(runs[0].mean() + int(top) + 4)
    return np.stack([kx * np.asarray(xs, float) + bx, ky * np.asarray(ys) + by], axis=-1)


def harman_curves(pdf: str, page: int, index: int, x_values, y_values, n: int = 1,
                  exclude=()) -> list[np.ndarray]:
    """(relative log E, density) for each of `n` curves, densest first, off a
    HARMAN characteristic-curve bitmap ruled every 0.5 in both: `x_values` and
    `y_values` name its rules left to right and top to bottom (a rule too faint
    to find, Ortho Plus's density 0, left out). `exclude` blanks labels."""
    img = _bitmap(str(SHEETS / pdf), page, index)
    dark = img < 120
    h, w = dark.shape
    vx, hy = _lines(dark.sum(axis=0), 0.40 * h), _lines(dark.sum(axis=1), 0.40 * w)
    assert len(vx) == len(x_values) and len(hy) == len(y_values), (pdf, vx, hy)
    kx, bx = _fit_axis(list(zip(vx, x_values)))
    ky, by = _fit_axis(list(zip(hy, y_values)))
    clean = dark.copy()
    for v in vx:
        clean[:, int(round(v)) - 2:int(round(v)) + 3] = False
    for y in hy:
        clean[int(round(y)) - 2:int(round(y)) + 3, :] = False
    for x0, y0, x1, y1 in exclude:
        clean[y0:y1, x0:x1] = False
    top = int(hy[0]) - 3
    bottom = int(round(hy[-1] + (hy[-1] - hy[-2] if y_values[-1] > 0 else 0))) + 4
    out, last = [[] for _ in range(n)], []
    for x in range(int(vx[0]) + 3, int(vx[-1]) - 2):
        # One curve: a stroke is joined across the band a rule was erased from.
        # Several, thin and close: strokes are kept apart, as drawn.
        runs = [r for r in _runs(clean[top:bottom, x], gap=6 if n == 1 else 1) if 2 <= len(r) <= 16]
        centres = [r.mean() + top for r in runs]
        if n == 1:
            if len(centres) == 1:
                out[0].append((x, centres[0]))
            continue
        # Curves sharing a toe split from one stroke; apart, they are taken top
        # to bottom; where a stroke is missing, each curve takes only one where
        # its own course puts it, so it neither swaps nor reaches a label.
        if not last:
            if len(centres) == 1:
                last = centres * n
            continue
        near = sorted(c for c in centres if min(abs(c - l) for l in last) <= 8)
        if len(near) == n:
            last, moved = near, range(n)
        else:
            moved = []
            for k in range(n):
                if len(out[k]) >= 2:
                    (x1, y1), (x2, y2) = out[k][-2:]
                    expect, reach = y2 + (y2 - y1) / max(x2 - x1, 1e-6) * (x - x2), 4 + 0.5 * (x - x2)
                else:
                    expect, reach = last[k], 6
                own = [c for c in centres if abs(c - expect) <= reach]
                if own:
                    last[k] = min(own, key=lambda c: abs(c - expect))
                    moved.append(k)
        for k in moved:
            out[k].append((x, last[k]))
    return [np.array([(kx * x + bx, ky * y + by) for x, y in c]) for c in out]


# Each HARMAN sheet: its file, the spectrogram's (page, image), and the curve
# plot's (page, image, rules across, rules down, curves, labels to blank, which
# curve), pages and images counted from 0.
_HARMAN_GRID = (np.arange(0.0, 4.51, 0.5), np.arange(3.0, -0.01, -0.5))
HARMAN_SHEETS = {
    "ilford_delta_100": ("Delta-100_201811.pdf", (0, 2), (3, 0, *_HARMAN_GRID, 1, (), 0)),
    "ilford_delta_400": ("Delta-400_201811.pdf", (0, 1), (5, 0, *_HARMAN_GRID, 1, (), 0)),
    "ilford_fp4_plus": ("FP4-Plus_201811.pdf", (0, 2), (4, 0, *_HARMAN_GRID, 1, (), 0)),
    "ilford_pan_f_plus": ("Pan-F-Plus_201812.pdf", (0, 2), (3, 0, *_HARMAN_GRID, 1, (), 0)),
    # ID-11 at 10 and 8 min, labelled "G 0.70" and "G 0.62" beside them: the 8.
    "ilford_ortho_plus": ("Ortho-Plus_201910.pdf", (0, 2),
                          (2, 2, np.arange(0.0, 4.01, 0.5), np.arange(3.5, 0.49, -0.5), 2,
                           ((395, 172, 462, 194), (450, 216, 522, 242)), 1)),
}


def harman_sheet(film: Film) -> tuple[np.ndarray, np.ndarray]:
    """(wavelength, relative log sensitivity) and (relative log E, density)."""
    pdf, (sp, si), (cp, ci, xv, yv, n, exclude, pick) = HARMAN_SHEETS[film.stock]
    spectral = harman_spectrogram(pdf, sp, si)
    curve = harman_curves(pdf, cp, ci, xv, yv, n=n, exclude=exclude)[pick]
    return spectral, curve[np.argsort(curve[:, 0])]


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


def metadata(datasource: str, created: str = "2026-10-14") -> ProfileMetadata:
    return ProfileMetadata(
        version="0.3.4",
        copyright="FLX Film Lab. Profile data measured off the manufacturer's published datasheet.",
        created=created,
        license="Profile format: spektrafilm (https://github.com/andreavolpato/spektrafilm), CC BY-SA 4.0. "
                "The values are the datasheet's own published measurements.",
        citation="Format and model: spektrafilm, https://github.com/andreavolpato/spektrafilm.",
        datasource=datasource,
    )


def assemble(info: ProfileInfo, log_sensitivity: np.ndarray, base_density: float,
             net: np.ndarray, model: dict, datasource: str, created: str = "2026-10-14") -> Profile:
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
    return Profile(metadata=metadata(datasource, created), info=info, data=data)


def build_film(film: Film, spectral: np.ndarray, curve: np.ndarray, sensitivity_note: str,
               cross_check=None, metered_at: int | None = None, created: str = "2026-10-14",
               normal_log_h: float | None = None, normal_note: str = "", use: str = "still",
               target_print: str = PAPER_STOCK, densitometer: str = "diffuse_visual") -> Profile:
    """`metered_at`: place mid-grey where a meter set to that exposure index
    puts it on the sheet's absolute lux-second axis, for a film whose rating
    is not its ISO speed. `normal_log_h`: mid-grey at that log H on the
    sheet's axis, the maker's own normal exposure (`normal_note` says whose
    and why). Otherwise mid-grey is 1.007 decades above the ISO speed point."""
    if normal_log_h is not None:
        anchor = lambda e, net: normal_log_h
        sheet = np.linspace(curve[:, 0].min(), curve[:, 0].max(), 1024)
        density = ds.resample(curve, sheet)
        iso = 0.8 / 10 ** speed_point(sheet, density - density.min())
        placement = (f"with mid-grey at log H {normal_log_h:.3f} on the sheet's absolute axis, {normal_note} "
                     f"The curve's own ISO speed point is ISO {iso:.0f}")
        print(f"    {film.stock}: ISO speed point of the sheet curve ISO {iso:.0f}, rated EI {film.iso}")
    elif metered_at is None:
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
                       stage="filming", use=use, antihalation="strong",
                       target_print=target_print, channel_model="bw", densitometer=densitometer,
                       log_sensitivity_density_over_min=1.0, reference_illuminant="D55",
                       viewing_illuminant="D50")
    datasource = (
        f"Measured off {film.notes} Characteristic curve laid on agx's axis {placement}; "
        f"base + fog {base:.3f}. Continued at its final gradient for "
        f"{FILM_CURVE_EXTENSION} decades past the last plotted exposure. {sensitivity_note} "
        "One silver layer, carried as three identical channels."
    )
    profile = assemble(info, log_s, base, net, model, datasource, created)
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


def build_print_film() -> Profile:
    """Kodak 2302, the black-and-white release print film a Double-X negative
    is printed on, projected as agx projects 2383.

    The sheet develops it to a control gamma of 2.4 to 2.6, and its own plot of
    gamma against development time in D-97 reaches the middle of that, 2.5, at
    6.52 minutes: the curve is interpolated there between the sheet's 5 and 7
    minute curves, along exposure at each density (`between_times`), plotted
    net of the base (the base, 0.048, from its granularity plot,
    `kodak_bw_cine_sheets`). The sheet disagrees with itself here: measured as
    it defines its control gamma ("D-min + 0.95 density to + 0.50 log E"), its
    curves come out 0.23 to 0.47 above that plot at every time, this one 2.78,
    and its plotted 5 minute curve, 2.53, would meet the control gamma so
    measured. Felix's pick (2026-11-08): the plot's time, the harder print of
    the two. Mid-grey prints at 18.4%
    transmission (log H 0), agx's rule for its print films; the spectral
    sensitivity is the sheet's (D-min + 0.2, 380-500 nm, blue-sensitive), at
    the level that collects as much of agx's printer lamp as 2383 does through
    its filtration for Vision3 250D, so Highlight protection's pre-flash acts
    the same on both. The sheet's MTF and granularity are read but not
    modelled: the app blurs and grains the negative, not the print."""
    import kodak_bw_cine_sheets as cine
    import spektrafilm
    from build_print_profiles import LEVEL_NEGATIVE, TEMPLATE, lamp_collected
    from spektrafilm.profiles.io import load_profile

    curves = cine.print_2302_curves()
    plot = cine.print_2302_gamma_plot()
    on_plot = lambda t: float(np.interp(t, plot[:, 0], plot[:, 1]))
    for t, c in curves.items():
        print(f"    2302 {t:g} min: gamma {cine.kodak_print_gamma(c):.2f} by the sheet's formula, "
              f"{on_plot(t):.2f} on its gamma plot")
    base_plate, toe, fog = cine.print_2302_base()
    minutes = cine.print_2302_minutes_for(PRINT_2302_GAMMA)
    curve = between_times({5.0: curves[5.0], 7.0: curves[7.0]}, minutes,
                          np.linspace(curves[5.0][0, 0], curves[5.0][-1, 0], 800))
    gamma = cine.kodak_print_gamma(curve)
    print(f"    2302 at {minutes:.2f} min, gamma {on_plot(minutes):.2f} on its plot: {gamma:.2f} by the sheet's formula "
          f"(5 min {cine.kodak_print_gamma(curves[5.0]):.2f})")
    curve[:, 1] += base_plate                       # plotted net of the base
    gross_min = float(curve[:, 1].min())
    anchor = lambda e, net: float(np.interp(MID_GREY_TRANSMISSION_DENSITY - gross_min, net, e))
    net, base, shift, model = characteristic(curve, anchor)

    log_s = on_grid(cine.print_2302_spectral(), WAVELENGTHS)
    template = load_profile(TEMPLATE)
    table = json.loads((Path(spektrafilm.__file__).parent / "data" / "filters" / "neutral_print_filters.json")
                       .read_text())
    reference = table[TEMPLATE]["TH-KG3"][LEVEL_NEGATIVE]
    level = (lamp_collected(np.asarray(template.data.log_sensitivity, float), reference)
             - lamp_collected(np.repeat(log_s[:, None], 3, axis=1), (0.0, 0.0, 0.0)))
    log_s = log_s + level

    info = ProfileInfo(stock=PRINT_2302_STOCK, name=PRINT_2302_NAME, type="negative", support="film",
                       stage="printing", use="cine", antihalation="strong", target_print=None,
                       channel_model="bw", densitometer="diffuse_visual",
                       log_sensitivity_density_over_min=0.2, reference_illuminant="TH-KG3",
                       viewing_illuminant="K75P")
    datasource = (
        "Traced off the bitmaps of Kodak H-1-2302 (3-22), KODAK Black-and-White Print Film 2302 / 3302: its "
        f"characteristic curve for D-97 at 21 C, {minutes:.2f} min, where the sheet's gamma-against-time plot "
        f"reaches {PRINT_2302_GAMMA:g}, the middle of the control gamma it develops to (2.4 to 2.6), interpolated "
        "between its 5 and 7 minute curves along exposure at each density (tungsten 1/50 s, diffuse visual). "
        "The sheet disagrees with itself: measured as it defines its control gamma (D-min + 0.95 to + 0.50 log "
        f"E), its curves come out 0.23 to 0.47 above that plot at every time, this one {gamma:.2f}, and its 5 "
        "minute curve (2.53) would meet the control gamma so measured; the plot's time is taken, the harder "
        "print of the two. The curves are plotted net of the base: "
        f"base {base_plate:.3f}, the toe of the sheet's granularity plot ({toe:.3f}, Status A green, 3 1/2 "
        f"min) less its net fog at 3 1/2 minutes ({fog:.3f}); D-min {base:.3f}. Mid-grey (18.4% "
        "transmission) at log E 0, agx's rule for its print films. Spectral sensitivity at D-min + 0.2, "
        "380-500 nm (blue-sensitive; nothing beyond), at the level that collects as much of agx's printer "
        "lamp (TH-KG3) as 2383 does through its filtration for Kodak Vision3 250D. Printing lamp TH-KG3 and "
        "projector K75P as agx's 2383. Its MTF and granularity are read, not modelled. One silver layer, "
        "carried as three identical channels."
    )
    profile = assemble(info, log_s, base, net, model, datasource, created="2026-11-08")
    print(f"  {PRINT_2302_STOCK:28s} D-min {base:.3f}  D-max net {net[:, 0].max():.2f}  axis shift {shift:+.3f}  "
          f"fit RMS {model['rms']:.4f}  lamp level {level:+.3f}")
    return profile


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
    spectral, curve = t_max_sheet()
    absolute = float(np.log10(0.65 * 12.5 / TMAX.iso))
    write(build_film(TMAX, spectral, curve,
                     "Spectral sensitivity: the D = 1.0 over D-min curve (equal-energy, erg/cm^2, effective "
                     "exposure 1.4 s), which stops at 670 nm.",
                     cross_check=lambda shift: f"Kodak's absolute axis puts mid-grey {(-shift) - absolute:+.3f} "
                                               f"decades from ISO {TMAX.iso}'s",
                     created="2026-10-25"))
    spectral, curve = hp5_sheet()
    write(build_film(HP5, spectral, curve,
                     "Spectral sensitivity: Ilford's wedge spectrogram read as relative log "
                     "sensitivity (read as tungsten-weighted it implies a 12-16x red filter factor)."))
    for film in (DELTA100, DELTA400, FP4, PANF, ORTHO):
        spectral, curve = harman_sheet(film)
        note = ("Spectral sensitivity: Ilford's wedge spectrogram to tungsten light, read as relative log "
                "sensitivity, as HP5 Plus's; Ortho Plus's own ratings, ISO 80 to daylight and 40 to tungsten, "
                "bear the reading out.")
        if film is ORTHO:
            note += " Orthochromatic: the spectrogram ends at 565 nm, and the film sees nothing redder."
        write(build_film(film, spectral, curve, note, created="2026-11-06"))
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

    import kodak_bw_cine_sheets as cine
    write(build_print_film())
    curves = cine.double_x_curves()
    for t, c in sorted(curves.items()):
        print(f"    Double-X {t:g} min: gamma {cine.slope_gamma(c):.2f} as traced, "
              f"{cine.DOUBLE_X_GAMMAS[t]:.2f} labelled")
    early, late = cine.double_x_2003_curve(), curves[5.0]
    for e in (-2.0, -1.4, -0.5):
        print(f"    Double-X at log H {e}: 2003 edition {np.interp(e, *early.T):.3f}, 3-22 edition's 5 min "
              f"{np.interp(e, *late.T):.3f}")
    # Kodak's normal exposure for its motion picture films, 10/EI: the sheet's
    # own tungsten exposure table, f/5.6 at 200 footcandles (2153 lux) at 24 fps
    # with a 170-degree shutter (1/50.8 s), gives an 18% grey card
    # 0.65 x 0.18 x 2153 / pi x (1/50.8) / 5.6^2 = 0.049-0.050 lux-seconds,
    # 10/200, through a lens of ISO 2720's q = 0.65.
    normal = float(np.log10(10.0 / DOUBLEX.iso))
    metered = float(np.log10(0.65 * 12.5 / DOUBLEX.iso))
    write(build_film(DOUBLEX, cine.double_x_spectral(), curves[DOUBLE_X_MINUTES],
                     "Spectral sensitivity: the D = 1.0 over gross fog curve (equal-energy, erg/cm^2, effective "
                     "exposure 1.4 s, D-96 to the recommended control gamma, diffuse visual), drawn from 420 to "
                     "660 nm and continued to 400 and 670 nm by the D = 0.3 curve's own shape, joined where it "
                     "stops. The base is grey (its antihalation), 0.20 by the sheet.",
                     normal_log_h=normal,
                     normal_note=("Kodak's normal exposure, 10/EI at EI 250 daylight: the sheet's tungsten exposure "
                                  "table (f/5.6 at 200 footcandles, 24 fps, 170-degree shutter) gives an 18% grey "
                                  "card 0.049-0.050 lux-seconds at EI 200 through a lens of ISO 2720's q = 0.65, "
                                  "which is 10/EI."),
                     use="cine", target_print=PRINT_2302_STOCK, densitometer="status_M", created="2026-11-08",
                     cross_check=lambda shift: f"a meter's ISO 2720 mid-tone at EI {DOUBLEX.iso} would sit "
                                               f"{metered - normal:+.3f} decades from it"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
