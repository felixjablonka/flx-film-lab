"""Camera filters: for black-and-white film, read off Kodak's Wratten 2 sheets;
for colour film, Kodak's Wratten 2 81C, 82A, 82C, 85 and 85B, read off Kodak's
sheets for their series, and Tiffen's own 812, read off Tiffen's catalogue.

Kodak publishes each Wratten 2 filter's diffuse density from 300 to 900 nm as a
vector plot (kodak.com, "KODAK WRATTEN 2 Filters"). The sheets are stored
rotated a quarter turn — wavelength runs up the page, density right to left —
so each axis is calibrated on its own printed labels. Where a curve leaves the
plot above its top (density 3.0 and more), it is held there: a transmittance
under 0.1% is nothing a film can see next to the passband.

Kodak's sheets for a series (81, 85) chart each filter's diffuse density from
400 to 700 nm upright, as vector polylines in the curve's own colour and dash,
on a density scale (the 85 series' at the right, its transmittance scale at
the left); each axis is calibrated on its own labels, and past the plot's ends
the end values hold. The 82 series' sheet is drawn otherwise: stored rotated,
as the single filters' are, with its four curves all black, told apart by
their line, the 82 thin and the 82B thick, the 82A dotted and the 82C dashed.
A dotted or dashed line is drawn as small filled pieces, one per dot or dash,
as wide as the line, so a curve is read as the centres of its pieces, and the
legend's sample beside the filter's name says which line is its own.

Tiffen's *Filter Specifications* catalogue charts each filter's measured
transmittance from 360 to 700 nm, also as vectors: the 81 series on page 13
(the PDF's 7th page), the 812 on page 16 (its 9th), the 85 series on page 11
(its 6th, which charts the 80 series too, so a chart there is picked by its
title). Each curve is drawn as a chain of small filled pieces in the curve's
own colour, which its label on the chart shares, so a curve is read as the
centres of its pieces, and the axes are calibrated on the chart's own labels.
Past 700 nm, where the charts stop, the last value holds.

A filter is applied as what it is, a spectral transmittance: the film's own
spectral sensitivities, wavelength by wavelength, times the filter's
(`bake_packs.bake_lens_filters`), so each layer sees the scene's light
through it as the film would on the camera.
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
    chart: str | None = None    # the chart's title, where its page has more than one
    # The light it is made for, under which its filter factor and the print
    # time are set: daylight for a conversion filter, which goes on a tungsten
    # film outdoors; else the film's own light.
    light: str | None = None
    wratten = None

    def transmittance(self) -> np.ndarray:
        return warming_transmittance(self)


@dataclass(frozen=True)
class SeriesFilter:
    """A colour film's filter off one of Kodak's Wratten 2 sheets for a series."""
    id: str          # what the recipe stores
    name: str        # what the inspector shows, "Wratten" and Kodak's number
    series: str      # the sheet's series, "81", "82" or "85"
    # The curve's stroke colour on that sheet; None on the 82 series' sheet,
    # where every curve is black and the legend says which line is which.
    colour: tuple | None
    light: str | None = None    # as `WarmingFilter.light`
    wratten = None   # the name says it; the field marks a black-and-white filter

    @property
    def sheet(self) -> Path:
        return SHEETS / f"Kodak Wratten 2 Filters {self.series} Series.pdf"

    def transmittance(self) -> np.ndarray:
        if self.colour is None:
            return lined_series_transmittance(self)
        return series_transmittance(self)


# For colour film (Felix's pick: the filters that are typically used). Kodak's
# Wratten 81C warming filter, for slide film's cool shade, overcast and flash;
# its 82A and 82C cooling filters, for warm light, which Fujifilm's slide
# sheets name for morning and evening (with its LBB-2 and LBB-4), and which
# balance warmer tungsten for a 3200 K film (Tiffen: the 82A 3000 to 3200 K);
# and its conversion filters 85 and 85B, which balance daylight for tungsten
# film, 5500 to 3400 K and to 3200 K: the 85 is what Kodak's motion picture
# sheets name for its tungsten stocks in daylight (EI 125 for EXR 200T), the
# 85B the classic filter for a 3200 K stock outdoors. On a daylight film either
# warms strongly. And Tiffen's own 812 ("FS 1/3"), which Kodak does not make.
# The 81C's curve is the green one of the six on its sheet; the 82A's the
# dotted and the 82C's the dashed of the four on theirs; the 85's the black
# and the 85B's the dotted blue of the six on theirs. The 85 and 85B take their
# factor in daylight: in a tungsten film's own light an 85B cuts more, and a
# factor and print time set there left a daylight frame a third of a stop light.
WARMING = (
    SeriesFilter("81c", "Wratten 81C", "81", (0.0, 0.69, 0.314)),
    WarmingFilter("812", "Tiffen 812", 8, (0.19, 0.2, 0.54)),
    SeriesFilter("82a", "Wratten 82A", "82", None),
    SeriesFilter("82c", "Wratten 82C", "82", None),
    SeriesFilter("85", "Wratten 85", "85", (0.0, 0.0, 0.0), light="D55"),
    SeriesFilter("85b", "Wratten 85B", "85", (0.0, 0.439, 0.753), light="D55"),
)
TIFFEN = SHEETS / "Tiffen Filter Specifications.pdf"
_CHART_FRAME = (0.58, 0.59, 0.6)

# An orthochromatic film sees nothing past 565 nm, where an orange or red
# filter starts to pass: it takes the filters its sheet gives factors for
# (Ilford Ortho Plus: yellow, deep yellow, tricolour blue and green).
ONLY = {"ilford_ortho_plus": ("blue", "green", "yellow")}


def filters_for(stock: str, channel_model: str):
    """The camera filters a film takes: Kodak's Wratten set on black and
    white, warming, cooling and conversion filters on colour."""
    if channel_model != "bw":
        return WARMING
    return tuple(f for f in FILTERS if f.id in ONLY.get(stock, (f.id,)))


def _close(a, b, tolerance: float = 0.02) -> bool:
    return a is not None and max(abs(x - y) for x, y in zip(a, b)) < tolerance


def warming_transmittance(warming: WarmingFilter) -> np.ndarray:
    """A Tiffen filter's transmittance on the 380-780 nm, 5 nm grid."""
    page = pymupdf.open(str(TIFFEN))[warming.page]
    drawings = page.get_drawings()
    frames = [d["rect"] for d in drawings if _close(d.get("color"), _CHART_FRAME)]
    if warming.chart is None:
        frame = max(frames, key=lambda r: r.width * r.height)
    else:
        # the frame whose top the chart's title sits on
        words = page.get_text("words")
        first, *rest = warming.chart.split()
        titles = [pymupdf.Rect(w[:4]) for i, w in enumerate(words)
                  if w[4] == first and [x[4] for x in words[i + 1:i + 1 + len(rest)]] == rest]
        frame = min(((f, t) for f in frames for t in titles if f.x0 < (t.x0 + t.x1) / 2 < f.x1),
                    key=lambda ft: abs(ft[0].y0 - ft[1].y1))[0]
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


def series_transmittance(series_filter: SeriesFilter) -> np.ndarray:
    """A filter's transmittance on the 380-780 nm, 5 nm grid, off its series' sheet."""
    page = pymupdf.open(str(series_filter.sheet))[0]
    wavelength_labels, density_labels = [], []
    for w in page.get_text("words"):
        try:
            value = float(w[4])
        except ValueError:
            continue
        cx, cy = (w[0] + w[2]) / 2, (w[1] + w[3]) / 2
        if 300 <= value <= 800:
            wavelength_labels.append((cx, value))
        elif value <= 10:           # the series' own numbers, 81 and 85, are neither
            density_labels.append((cy, value))
    k_nm, b_nm = np.polyfit(*zip(*wavelength_labels), 1)
    k_d, b_d = np.polyfit(*zip(*density_labels), 1)
    # The curve, not its sample in the legend: the longest polyline in its colour.
    curves = [d for d in page.get_drawings()
              if d.get("color") is not None and _close(d["color"], series_filter.colour, 0.01)
              and (d.get("width") or 0) > 1.5]
    if not curves:
        raise ValueError(f"no {series_filter.name} curve on {series_filter.sheet.name}")
    segments = [i for i in max(curves, key=lambda d: len(d["items"]))["items"] if i[0] in ("l", "c")]
    poly = ds._flatten_path(segments)
    nm, density = k_nm * poly[:, 0] + b_nm, k_d * poly[:, 1] + b_d
    order = np.argsort(nm)
    return np.clip(10 ** (-np.interp(WAVELENGTHS, nm[order], density[order])), 0.0, 1.0)


def _rotated_axes(page):
    """A sheet stored rotated a quarter turn: its frame, its wavelength and
    density axes as (slope, offset) on the drawing's y and x, each calibrated
    on its own printed labels, and the density at the top of the plot."""
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
    nm_axis = np.polyfit(*zip(*wavelength_labels), 1)
    density_axis = np.polyfit(*zip(*density_labels), 1)
    return frame, nm_axis, density_axis, max(v for _, v in density_labels)


def transmittance(lens_filter: LensFilter) -> np.ndarray:
    """The filter's transmittance on the 380-780 nm, 5 nm grid."""
    page = pymupdf.open(str(lens_filter.sheet))[0]
    frame, (k_nm, b_nm), (k_d, b_d), top = _rotated_axes(page)

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


def lined_series_transmittance(series_filter: SeriesFilter) -> np.ndarray:
    """A filter's transmittance on the 380-780 nm, 5 nm grid, off the 82
    series' sheet: the curve drawn in the line its legend sample shows beside
    its name ("W2 82A"), a dotted or dashed line, read at its pieces' centres."""
    page = pymupdf.open(str(series_filter.sheet))[0]
    frame, (k_nm, b_nm), (k_d, b_d), _ = _rotated_axes(page)
    pieces = []     # each dot or dash: its centre and the line's width
    for d in page.get_drawings():
        items = d["items"]
        if d.get("fill") != (0.0, 0.0, 0.0) or len(items) != 3 or any(i[0] != "l" for i in items):
            continue
        corners = np.array([(p.x, p.y) for p in (items[0][1], *(i[2] for i in items))])
        across, along = sorted(np.linalg.norm(np.diff(corners[:3], axis=0), axis=1))
        # A sliver, a dash cut short where the line bends, lies across the
        # line, and its long side is the line's width.
        pieces.append((*corners.mean(axis=0), along if across < 0.5 else across))
    pieces = np.array(pieces)

    number = series_filter.name.removeprefix("Wratten ")
    words = page.get_text("words")
    label = next((pymupdf.Rect(a[:4]) | pymupdf.Rect(b[:4]) for a, b in zip(words, words[1:])
                  if a[4] == "W2" and b[4] == number), None)
    # The legend's text runs along the drawing's y, and each name's sample
    # beside it, level with it in x.
    level = pieces[(label.x0 < pieces[:, 0]) & (pieces[:, 0] < label.x1)] if label else pieces[:0]
    if not len(level):
        raise ValueError(f"no dotted or dashed {series_filter.name} in {series_filter.sheet.name}'s legend")
    sample = level[np.argmin(np.minimum(np.abs(level[:, 1] - label.y0), np.abs(level[:, 1] - label.y1)))]
    line = pieces[np.abs(pieces[:, 2] - sample[2]) < 0.25 * sample[2], :2]

    curve = _longest_chain(line)
    if len(curve) < 50:
        raise ValueError(f"no {series_filter.name} curve on {series_filter.sheet.name}")
    nm, density = k_nm * curve[:, 1] + b_nm, k_d * curve[:, 0] + b_d
    order = np.argsort(nm)
    return np.clip(10 ** (-np.interp(WAVELENGTHS, nm[order], density[order])), 0.0, 1.0)


def _longest_chain(points: np.ndarray) -> np.ndarray:
    """The most points that link up, each within three times the usual
    spacing of the next: a dotted or dashed curve, not its legend sample."""
    apart = np.linalg.norm(points[:, None] - points[None], axis=-1)
    np.fill_diagonal(apart, np.inf)
    reach = 3 * np.median(apart.min(axis=1))
    group = np.full(len(points), -1)
    for start in range(len(points)):
        if group[start] >= 0:
            continue
        group[start] = start
        todo = [start]
        while todo:
            near = np.flatnonzero((apart[todo.pop()] <= reach) & (group < 0))
            group[near] = start
            todo.extend(near)
    return points[group == np.bincount(group).argmax()]
