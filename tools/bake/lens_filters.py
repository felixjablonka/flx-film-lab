"""Camera filters: for black-and-white film, read off Kodak's Wratten 2 sheets;
for slide film, Tiffen's warming filters, read off Tiffen's catalogue.

Kodak publishes each Wratten 2 filter's diffuse density from 300 to 900 nm as a
vector plot (kodak.com, "KODAK WRATTEN 2 Filters"). The sheets are stored
rotated a quarter turn — wavelength runs up the page, density right to left —
so each axis is calibrated on its own printed labels. Where a curve leaves the
plot above its top (density 3.0 and more), it is held there: a transmittance
under 0.1% is nothing a film can see next to the passband.

Tiffen's *Filter Specifications* catalogue charts each filter's transmittance
from 360 to 700 nm, also as vectors: the 81 series on page 13 (the PDF's 7th
page), the 812 on page 16 (its 9th). Each curve is drawn as a chain of small
filled pieces in the curve's own colour, so a curve is read as the centres of
its pieces, and the axes are calibrated on the chart's own labels. Past
700 nm, where the charts stop, the last value holds.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pymupdf

import datasheet as ds

SHEETS = Path(__file__).resolve().parents[2] / "film spec sheets"
WAVELENGTHS = np.arange(380.0, 781.0, 5.0)


@dataclass(frozen=True)
class LensFilter:
    id: str          # what the recipe stores
    name: str        # what the inspector shows
    wratten: str

    @property
    def sheet(self) -> Path:
        return SHEETS / f"Kodak Wratten 2 Filter {self.wratten}.pdf"

    def transmittance(self) -> np.ndarray:
        return transmittance(self)


# The black-and-white photographer's set. Kodak's current Wratten 2 range has
# no 21, the classic orange, so orange is the 22, a shade deeper.
FILTERS = (
    LensFilter("blue", "Blue", "47"),
    LensFilter("green", "Green", "58"),
    LensFilter("yellow", "Yellow", "8"),
    LensFilter("orange", "Orange", "22"),
    LensFilter("red", "Red", "25"),
)


@dataclass(frozen=True)
class WarmingFilter:
    id: str          # what the recipe stores
    name: str        # what the inspector shows
    page: int        # the chart's page in Tiffen's catalogue, counted from 0
    colour: tuple    # the curve's colour on that chart
    wratten = None

    def transmittance(self) -> np.ndarray:
        return warming_transmittance(self)


# Tiffen's 81C (FS 1/3) and its own 812 warming filter (FS 1/3), for slide
# film. The 81C's curve is the teal one of the four on its chart.
WARMING = (
    WarmingFilter("81c", "Tiffen 81C", 6, (0.05, 0.67, 0.53)),
    WarmingFilter("812", "Tiffen 812", 8, (0.19, 0.2, 0.54)),
)
TIFFEN = SHEETS / "Tiffen Filter Specifications.pdf"
_CHART_FRAME = (0.58, 0.59, 0.6)


def _close(a, b, tolerance: float = 0.02) -> bool:
    return a is not None and max(abs(x - y) for x, y in zip(a, b)) < tolerance


def warming_transmittance(warming: WarmingFilter) -> np.ndarray:
    """A Tiffen filter's transmittance on the 380-780 nm, 5 nm grid."""
    page = pymupdf.open(str(TIFFEN))[warming.page]
    drawings = page.get_drawings()
    frame = max((d["rect"] for d in drawings if _close(d.get("color"), _CHART_FRAME)),
                key=lambda r: r.width * r.height)
    wavelength_labels, percent_labels = [], []
    for w in page.get_text("words"):
        try:
            value = float(w[4])
        except ValueError:
            continue
        cx, cy = (w[0] + w[2]) / 2, (w[1] + w[3]) / 2
        if frame.y1 < cy < frame.y1 + 12 and frame.x0 - 8 < cx < frame.x1 + 8 and 300 <= value <= 800:
            wavelength_labels.append((cx, value))
        if frame.x0 - 25 < cx < frame.x0 and frame.y0 - 6 < cy < frame.y1 + 6 and 0 <= value <= 100:
            percent_labels.append((cy, value))
    k_nm, b_nm = np.polyfit(*zip(*wavelength_labels), 1)
    k_t, b_t = np.polyfit(*zip(*percent_labels), 1)

    centres = []
    for d in drawings:
        r = d["rect"]
        if d.get("color") is None and _close(d.get("fill"), warming.colour) and r.width < 8 and r.height < 8 \
                and frame.x0 - 2 <= r.x0 and r.x1 <= frame.x1 + 2 and frame.y0 - 2 <= r.y0 and r.y1 <= frame.y1 + 2:
            points = [p for item in d["items"] for p in item[1:] if isinstance(p, pymupdf.Point)]
            centres.append((np.mean([p.x for p in points]), np.mean([p.y for p in points])))
    if len(centres) < 50:
        raise ValueError(f"no {warming.name} curve on page {warming.page + 1} of {TIFFEN.name}")
    centres = np.array(sorted(centres))
    nm = k_nm * centres[:, 0] + b_nm
    percent = k_t * centres[:, 1] + b_t
    return np.clip(np.interp(WAVELENGTHS, nm, percent) / 100, 0.0, 1.0)


def transmittance(lens_filter: LensFilter) -> np.ndarray:
    """The filter's transmittance on the 380-780 nm, 5 nm grid."""
    page = pymupdf.open(str(lens_filter.sheet))[0]
    frame = max((p["rect"] for p in page.get_drawings() if any(i[0] == "re" for i in p["items"])),
                key=lambda r: r.width * r.height)
    wavelength_labels, density_labels = [], []
    for w in page.get_text("words"):
        try:
            value = float(w[4])
        except ValueError:
            continue
        cx, cy = (w[0] + w[2]) / 2, (w[1] + w[3]) / 2
        # The corner label "0.0" sits by both axes; wavelength labels stay
        # level with the frame, density labels sit below it.
        if cx > frame.x1 + 3 and frame.y0 - 10 < cy < frame.y1 + 2:
            wavelength_labels.append((cy, value))
        if cy > frame.y1 + 3 and frame.x0 - 15 < cx < frame.x1 + 15:
            density_labels.append((cx, value))
    k_nm, b_nm = np.polyfit(*zip(*wavelength_labels), 1)
    k_d, b_d = np.polyfit(*zip(*density_labels), 1)
    top = max(v for _, v in density_labels)

    points = []
    for p in page.get_drawings():
        segments = [i for i in p["items"] if i[0] in ("l", "c")]
        r = p["rect"]
        if len(segments) < 8 or not (frame.x0 - 1 <= r.x0 and r.x1 <= frame.x1 + 1
                                     and frame.y0 - 1 <= r.y0 and r.y1 <= frame.y1 + 1):
            continue
        poly = ds._flatten_path(segments)
        points.append(np.stack([k_nm * poly[:, 1] + b_nm, k_d * poly[:, 0] + b_d], axis=-1))
    if not points:
        raise ValueError(f"no curve on {lens_filter.sheet.name}")
    points = np.concatenate(points)
    points = points[np.argsort(points[:, 0])]

    density = np.interp(WAVELENGTHS, points[:, 0], points[:, 1])
    plotted = np.array([np.abs(points[:, 0] - w).min() < 4 for w in WAVELENGTHS])
    density[~plotted] = np.maximum(density[~plotted], top)
    return np.clip(10 ** (-density), 0.0, 1.0)
