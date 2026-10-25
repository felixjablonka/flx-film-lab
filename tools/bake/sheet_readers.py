"""Each film's own datasheet, read: characteristic curves, spectral
sensitivities and dye spectra, for `build_profiles.build_unmixed` (the slides,
and the colour negatives built since).

Every plot is described by what can be read off its page — its frame and rules
whose values are known (from their labels, or, where a sheet draws its labels
as outlines, from the grid they make) — and its curves are picked by stroke
colour, dash pattern, position or order. Fujifilm's Provia 100F sheet ships
its curves and dyes as bitmap stencils, one per curve; those are traced
(`trace_raster_plot`). Each function returns {name: (x, y)} in data units:
log H in lux-seconds against density, or wavelength against log sensitivity
or diffuse spectral density. Bitmaps of Velvia 50 and vector plots of
Kodachrome 25 and 200 and Provia 400X's curves are read by `build_profiles`
itself.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pymupdf

import trace_raster_plot as raster
import vector_plots
from vector_plots import VectorPlot

SHEETS = Path(__file__).resolve().parents[2] / "film spec sheets"


def _plot(pdf: str, page: int, frame, x_rules, y_rules) -> VectorPlot:
    return VectorPlot(str(SHEETS / pdf), page, frame, tuple(x_rules), tuple(y_rules))


# --- Fujichrome Velvia 100 (AF3-202E), page 8 --------------------------------
# Its labels are outlines, so the rules are named by the grid: the curves at
# -3.5 to +1.0 in half decades and density 0 to 4 in halves; sensitivities at
# 400-700 nm and log 1, 0, -1; dyes at 400-700 nm and 0, 0.5, 1.0.
V100 = "films_velvia-100_datasheet_01.pdf"
V100_CURVES = _plot(V100, 7, (86.0, 174.2, 281.9, 347.5),
                    zip((86.0, 107.57, 129.46, 151.63, 172.94, 194.83, 216.43, 238.32, 259.92, 281.8),
                        np.arange(-3.5, 1.01, 0.5)),
                    zip((347.5, 326.29, 304.59, 282.75, 261.13, 239.57, 217.74, 195.66, 174.2),
                        np.arange(0.0, 4.01, 0.5)))
V100_SENSITIVITY = _plot(V100, 7, (335.5, 174.9, 533.8, 345.1),
                         ((347.05, 400), (405.68, 500), (463.76, 600), (521.8, 700)),
                         ((205.13, 1.0), (262.76, 0.0), (320.39, -1.0)))
V100_DYES = _plot(V100, 7, (333.2, 449.4, 534.4, 596.8),
                  ((344.97, 400), (404.43, 500), (463.32, 600), (522.19, 700)),
                  ((596.8, 0.0), (537.28, 0.5), (477.73, 1.0)))


def velvia_100():
    return {"curves": vector_plots.by_dashes(V100_CURVES, {"R": (), "G": (9.0, 1.5, 2.5, 1.5), "B": (3.0, 1.5)}),
            "sensitivity": vector_plots.by_position(V100_SENSITIVITY, ("B", "G", "R")),
            "dyes": vector_plots.by_position(V100_DYES, ("Y", "M", "C"))}


# --- Kodak Ektachrome E100 (E-4000) --------------------------------------------
# Curves on page 3, drawn as polylines and labelled B, G, R from the top at
# the dense end; sensitivities and dyes on page 4, the dyes "normalized to form
# a visual neutral density of 1.0" with that neutral drawn among them.
E100 = "e4000_ektachrome_100.pdf"
E100_CURVES = _plot(E100, 2, (368.7, 358.6, 553.9, 543.0),
                    ((368.7, -3.0), (415.0, -2.0), (461.3, -1.0), (507.6, 0.0), (553.9, 1.0)),
                    ((543.0, 0.0), (496.9, 1.0), (450.8, 2.0), (404.7, 3.0), (358.6, 4.0)))
E100_SENSITIVITY = _plot(E100, 3, (70.0, 104.0, 280.0, 262.0),
                         tuple((75.1 + 20.05 * i, 250 + 50 * i) for i in range(11)),
                         ((144.88, 1.0), (182.73, 0.0), (220.64, -1.0), (258.5, -2.0)))
E100_DYES = _plot(E100, 3, (70.6, 349.6, 271.1, 501.1),
                  ((70.64, 400), (271.1, 700)), ((501.08, 0.0), (349.66, 1.4)))


def ektachrome_100():
    return {"curves": vector_plots.by_order(E100_CURVES, ("B", "G", "R"), at_x=-2.5, polylines=True),
            "sensitivity": vector_plots.by_position(E100_SENSITIVITY, ("B", "G", "R")),
            "dyes": vector_plots.by_position(E100_DYES, ("Y", "M", "C"), drop_neutral=True)}


# --- Kodak Kodachrome 64 (E-55), page 4 -----------------------------------------
def kodachrome_64():
    import build_profiles as bp
    return {"sensitivity": vector_plots.by_position(bp._kodachrome_sensitivity(3, 74.7, 386.8), ("B", "G", "R")),
            "dyes": vector_plots.by_position(bp._kodachrome_dyes(3, 350.8, 119.6), ("Y", "M", "C"),
                                             drop_neutral=True)}


# --- Fujichrome Provia 400X (AF3-0213E), page 7: its own dye spectra -------------
P400X_DYES = _plot("Fujichrome-Provia-400X-Professional-RXP_AF3-0213E.pdf", 6, (333.34, 400.06, 534.52, 547.5),
                   ((345.08, 400), (404.26, 500), (463.45, 600), (522.33, 700)),
                   ((547.5, 0.0), (487.95, 0.5), (428.41, 1.0)))


def provia_400x():
    return {"dyes": vector_plots.by_position(P400X_DYES, ("Y", "M", "C"))}


# --- Fujichrome Provia 100F (AF3-036E), page 6 ------------------------------------
# The curves and the dyes are bitmap stencils, one per curve, laid over a stencil
# of the grid. Green and blue are only drawn where they part from red, at the
# dense end; elsewhere they lie under it. The sensitivities are vector.
P100F = "films_provia-100f_datasheet_01.pdf"
P100F_SENSITIVITY = _plot(P100F, 5, (338.2, 172.0, 538.3, 348.9),
                          ((349.63, 400), (409.04, 500), (468.45, 600), (527.86, 700)),
                          ((203.74, 1.0), (263.13, 0.0), (322.52, -1.0)))
P100F_COLOURS = {"R": (0.92, 0.179, 0.179), "G": (0.0, 0.666, 0.311), "B": (0.196, 0.23, 0.595)}


def _stencil(pdf: str, page: int, image: int) -> np.ndarray:
    document = pymupdf.open(str(SHEETS / pdf))
    pixmap = pymupdf.Pixmap(document, document[page].get_images(full=True)[image][0])
    samples = np.frombuffer(pixmap.samples, dtype=np.uint8).reshape(pixmap.height, pixmap.width, pixmap.n)
    return samples[..., -1] > 128


def _stencil_curve(pdf, page, grid_image, image, x_values, y_values, exclude=()) -> np.ndarray:
    """One curve traced off its own stencil, calibrated on the grid stencil's
    rules (named left to right and top to bottom, `None` for a bare border)."""
    grid = _stencil(pdf, page, grid_image)
    rows, cols = raster.grid_lines(grid)
    if len(rows) != len(y_values) or len(cols) != len(x_values):
        raise ValueError(f"{pdf} page {page + 1}: {len(cols)} x {len(rows)} rules, "
                         f"expected {len(x_values)} x {len(y_values)}")
    fit = lambda pos, vals: np.polyfit([p for p, v in zip(pos, vals) if v is not None],  # noqa: E731
                                       [v for v in vals if v is not None], 1)
    kx, ky = fit(cols, x_values), fit(rows, y_values)
    mask = _stencil(pdf, page, image)
    for x0, y0, x1, y1 in exclude:
        mask[y0:y1, x0:x1] = False
    xs, ys = [], []
    for col in range(int(cols[0]) + 2, int(cols[-1]) - 1):
        hit = np.nonzero(mask[:, col])[0]
        if hit.size == 0:
            continue
        runs = np.split(hit, np.nonzero(np.diff(hit) > 3)[0] + 1)
        run = max(runs, key=len)
        xs.append(col)
        ys.append(float(np.mean(run)))
    return np.stack([np.polyval(kx, np.array(xs, float)), np.polyval(ky, np.array(ys))], axis=-1)


def provia_100f():
    x = tuple(np.arange(-3.5, 1.01, 0.5))
    y = tuple(np.arange(4.0, -0.01, -0.5))
    legend = ((60, 540, 360, 740),)                   # the R/G/B legend's sample strokes
    red = _stencil_curve(P100F, 5, 0, 2, x, y, legend)
    curves = {"R": red}
    for name, image in (("G", 1), ("B", 3)):
        own = _stencil_curve(P100F, 5, 0, image, x, y, legend)
        lo, hi = own[:, 0].min(), own[:, 0].max()
        rest = red[(red[:, 0] < lo) | (red[:, 0] > hi)]
        curves[name] = np.concatenate([own, rest])[np.argsort(np.concatenate([own[:, 0], rest[:, 0]]))]
    dx = (None, 400, 500, 600, 700, None)
    dy = (None, 1.0, 0.5, 0.0)
    dyes = {name: _stencil_curve(P100F, 5, 5, image, dx, dy) for name, image in (("Y", 6), ("M", 7), ("C", 8))}
    return {"curves": curves,
            "sensitivity": vector_plots.by_colour(P100F_SENSITIVITY, P100F_COLOURS),
            "dyes": dyes}


# --- Fujichrome Astia 100F (AF3-149E), page 8 ------------------------------------
# All vector: the curves told apart by dash pattern (red solid, green dash-dot,
# blue dashed, as on Velvia 100's sheet), the spectra by colour.
ASTIA = "astia_100f_datasheet.pdf"
ASTIA_CURVES = _plot(ASTIA, 7, (84.2, 197.9, 280.1, 370.9),
                     zip((105.78, 127.68, 149.84, 171.16, 193.04, 214.65, 236.53, 258.13, 280.01),
                         np.arange(-3.0, 1.01, 0.5)),
                     ((197.9, 4.0), (219.07, 3.5), (241.15, 3.0), (262.99, 2.5), (284.55, 2.0), (306.17, 1.5),
                      (328.3, 1.0), (349.73, 0.5), (370.9, 0.0)))
ASTIA_SENSITIVITY = _plot(ASTIA, 7, (336.3, 197.1, 537.4, 372.7),
                          ((347.99, 400), (407.44, 500), (466.33, 600), (525.18, 700)),
                          ((228.24, 1.0), (286.34, 0.0), (344.15, -1.0)))
ASTIA_DYES = _plot(ASTIA, 7, (336.8, 472.0, 537.9, 619.4),
                   ((348.57, 400), (408.02, 500), (466.9, 600), (525.76, 700)),
                   ((619.4, 0.0), (559.88, 0.5), (500.35, 1.0)))


def astia_100f():
    return {"curves": vector_plots.by_dashes(ASTIA_CURVES, {"R": (), "G": (9.0, 1.5, 2.5, 1.5), "B": (3.0, 1.5)}),
            "sensitivity": vector_plots.by_colour(ASTIA_SENSITIVITY, {"R": (0.924, 0.173, 0.18),
                                                                       "G": (0.0, 0.666, 0.309),
                                                                       "B": (0.199, 0.232, 0.593)}),
            "dyes": vector_plots.by_colour(ASTIA_DYES, {"Y": (1.0, 0.949, 0.0), "M": (0.926, 0.0, 0.548),
                                                        "C": (0.0, 0.681, 0.938)})}


# --- Fujicolor Superia Reala (AF3-967E), page 4 -------------------------------------
# Vector, every curve black and the labels outlines, so the rules are named by
# the grid: the curves at -4.0 to +1.0 in half decades and density 0 to 3.5 in
# halves, the sensitivities at 400-700 nm and the decade the sheet's "1.0" bar
# spans. The fourth, cyan-sensitive layer's sensitivity is drawn dashed and
# left out: the engine has three layers.
REALA = "fujicolor_superia_reala.pdf"
REALA_CURVES = _plot(REALA, 3, (68.5, 125.0, 274.1, 270.7),
                     zip((68.5, 88.5, 109.2, 130.6, 150.8, 171.6, 191.8, 212.1, 233.2, 253.8, 274.1),
                         np.arange(-4.0, 1.01, 0.5)),
                     zip((270.7, 248.9, 228.6, 207.9, 187.6, 167.3, 145.9, 125.0), np.arange(0.0, 3.51, 0.5)))
REALA_SENSITIVITY = _plot(REALA, 3, (335.0, 124.1, 512.0, 275.6),
                          ((347.7, 400), (399.1, 500), (449.8, 600), (499.8, 700)),
                          ((225.6, 0.0), (174.8, 1.0)))


def superia_reala():
    return {"curves": vector_plots.by_order(REALA_CURVES, ("B", "G", "R"), at_x=-1.0),
            "sensitivity": vector_plots.by_position(REALA_SENSITIVITY, ("B", "G", "R"), solid_only=True)}


# --- Fujifilm motion picture negatives -----------------------------------------------
# Vector, their labels outlines and their curves told apart by colour. Eterna's
# characteristic curves run against camera stops from the normal exposure and
# come back in decades from mid-grey; Reala 500D's against absolute log H.
STOP = float(np.log10(2))


def _cine(pdf, page, curves, sensitivity, colours, stops: bool):
    found = vector_plots.by_colour(_plot(pdf, page, *curves), colours)
    if stops:
        found = {k: np.stack([v[:, 0] * STOP, v[:, 1]], axis=-1) for k, v in found.items()}
    return {"curves": found, "sensitivity": vector_plots.by_colour(_plot(pdf, page, *sensitivity), colours)}


def eterna_vivid_160():
    return _cine("fujifilm_eterna_vivid160.pdf", 0,
                 ((330.7, 103.4, 540.9, 254.4),
                  zip((345.7, 360.7, 375.7, 390.7, 405.8, 420.8, 435.8, 450.8, 465.8, 480.8, 495.8, 510.8, 525.8),
                      range(-6, 7)),
                  zip((254.4, 229.2, 204.1, 178.9, 153.8, 128.6, 103.4), np.arange(0.0, 3.01, 0.5))),
                 ((331.3, 411.3, 540.3, 528.5),
                  zip((343.4, 374.3, 404.6, 435.2, 466.1, 497.0, 527.9), range(400, 701, 50)),
                  ((528.5, 0.0), (489.1, 1.0), (450.5, 2.0), (411.3, 3.0))),
                 {"R": (0.902, 0.0, 0.071), "G": (0.0, 0.6, 0.267), "B": (0.0, 0.357, 0.675)}, stops=True)


def eterna_500():
    return _cine("fujifilm_ETERNA_500t.pdf", 2,
                 ((331.8, 103.1, 541.7, 254.0),
                  zip((346.8, 361.8, 376.8, 391.8, 406.8, 421.8, 436.8, 451.8, 466.8, 481.7, 496.7, 511.7, 526.7),
                      range(-6, 7)),
                  zip((254.0, 228.9, 203.7, 178.6, 153.4, 128.3, 103.1), np.arange(0.0, 3.01, 0.5))),
                 ((332.0, 410.8, 541.5, 528.3),
                  zip((343.4, 374.5, 405.6, 436.8, 467.9, 499.0, 530.1), range(400, 701, 50)),
                  ((528.3, 0.0), (489.1, 1.0), (450.0, 2.0), (410.8, 3.0))),
                 {"R": (0.875, 0.0, 0.141), "G": (0.0, 0.659, 0.22), "B": (0.0, 0.451, 0.733)}, stops=True)


def reala_500d():
    # Its log H labels are in bar notation (4.5 bar is -3.5); the fourth,
    # cyan-sensitive layer's sensitivity is drawn dashed and left out.
    return _cine("Reala500D_curve.pdf", 0,
                 ((305.3, 67.3, 559.8, 237.8),
                  zip((305.3, 334.0, 361.8, 389.7, 417.6, 446.7, 475.0, 503.3, 530.7, 559.8), np.arange(-3.5, 1.01, 0.5)),
                  zip((237.8, 209.5, 181.3, 153.0, 124.7, 96.4, 67.3), np.arange(0.0, 3.01, 0.5))),
                 ((328.3, 382.7, 560.5, 512.8),
                  zip((342.3, 375.9, 411.1, 444.4, 478.8, 512.9, 546.9), range(400, 701, 50)),
                  ((512.8, 0.0), (469.7, 1.0), (426.2, 2.0), (382.7, 3.0))),
                 {"R": (0.89, 0.294, 0.145), "G": (0.0, 0.655, 0.231), "B": (0.082, 0.4, 0.686)}, stops=False)


# --- Agfa Professional range (agfa_films_data.pdf), pages 7 and 8 ----------------
# One vector page per three films, each plot labelled in text; every curve
# black, told apart by dash pattern (blue dashed, green solid, red dash-dot).
AGFA = "agfa_films_data.pdf"
AGFA_CURVES_Y = tuple(zip((576.1, 562.7, 550.3, 537.9, 525.6, 513.2, 500.8, 488.4, 476.1), np.arange(0.0, 4.01, 0.5)))


def _agfa(page, sensitivity, curves, sensitivity_dashes, curve_dashes=((3.157, 0.789), (3.157, 0.789, 0.789, 0.789))):
    b, r = curve_dashes
    sb, sr = sensitivity_dashes
    return {"curves": vector_plots.by_dashes(_plot(AGFA, page, *curves), {"B": b, "G": (), "R": r}),
            "sensitivity": vector_plots.by_dashes(_plot(AGFA, page, *sensitivity), {"B": sb, "G": (), "R": sr})}


def optima_ii_100():
    return _agfa(6, ((70.4, 80.2, 196.8, 175.0), ((86.2, 400), (117.8, 500), (149.4, 600), (180.9, 700)),
                     ((96.0, 2.0), (127.6, 1.0), (159.2, 0.0))),
                 ((71.6, 476.1, 196.5, 576.1),
                  zip((71.6, 84.2, 96.7, 109.1, 121.6, 134.1, 146.6, 159.1, 171.6, 184.0, 196.5),
                      np.arange(-4.0, 1.01, 0.5)), AGFA_CURVES_Y),
                 ((3.159, 0.79), (3.159, 0.79, 0.79, 0.79)))


def portrait_xps_160():
    return _agfa(7, ((53.4, 80.2, 179.8, 175.0), ((69.2, 400), (100.8, 500), (132.4, 600), (164.0, 700)),
                     ((80.2, 2.0), (111.8, 1.0), (143.4, 0.0), (175.0, -1.0))),
                 ((54.6, 476.1, 179.6, 576.1),
                  zip((54.6, 67.2, 79.7, 92.2, 104.7, 117.1, 129.6, 142.1, 154.6, 167.0, 179.6),
                      np.arange(-3.5, 1.51, 0.5)), AGFA_CURVES_Y),
                 ((2.011, 0.503), (2.011, 0.503, 0.503, 0.503)))


# --- Kodak Royal Gold 100 (E-41), page 3, and Vericolor III (E-26), page 4 --------
# Vector, every curve black, the labels text with a separate bar for each minus
# sign, so the rules are named here. A curve is often drawn in several strokes,
# spread over paths that carry another curve's too; they are joined end to end
# (`vector_plots.joined`). The curves are named top to bottom at the dense
# end (B, G, R), the sensitivities by where they peak.
ROYAL_GOLD = "e41-1998_02_Kodak Royal Gold 100.pdf"
RG_CURVES = _plot(ROYAL_GOLD, 2, (93.7, 71.8, 278.2, 256.3),
                  zip((93.7, 139.8, 186.0, 232.1, 278.2), range(-3, 2)),
                  zip((256.3, 210.1, 164.0, 117.9, 71.8), range(0, 5)))
RG_SENSITIVITY = _plot(ROYAL_GOLD, 2, (361.4, 71.8, 562.0, 223.3),
                       ((381.5, 300), (421.7, 400), (461.9, 500), (502.1, 600), (542.3, 700)),
                       ((223.3, 0.0), (185.4, 1.0), (147.6, 2.0), (109.7, 3.0), (71.8, 4.0)))
VERICOLOR = "e26-Vericolor_III.pdf"
VC_CURVES = _plot(VERICOLOR, 3, (75.3, 72.0, 259.8, 257.3),
                  zip((75.3, 121.4, 167.5, 213.7, 259.8), range(-3, 2)),
                  zip((257.3, 211.0, 164.7, 118.4, 72.0), range(0, 5)))
VC_SENSITIVITY = _plot(VERICOLOR, 3, (356.6, 75.0, 549.9, 211.7),
                       zip((375.9, 395.3, 414.7, 434.1, 453.5, 472.8, 492.2, 511.6, 531.0), range(300, 701, 50)),
                       ((75.0, 2.0), (109.2, 1.0), (143.4, 0.0), (177.6, -1.0), (211.7, -2.0)))


def royal_gold_100():
    return {"curves": vector_plots.joined_by_value(RG_CURVES, ("B", "G", "R"), at_x=0.0),
            "sensitivity": vector_plots.joined_by_peak(RG_SENSITIVITY, ("B", "G", "R"))}


def vericolor_iii():
    return {"curves": vector_plots.joined_by_value(VC_CURVES, ("B", "G", "R"), at_x=0.0),
            "sensitivity": vector_plots.joined_by_peak(VC_SENSITIVITY, ("B", "G", "R"))}


# --- HARMAN Phoenix 200 (Sep 2024), pages 2 and 3 --------------------------------
# Bitmaps: black curves on white over a grey grid, calibrated on the grid —
# relative log exposure 0-3 and density 0-4 every half; 300-800 nm every 50
# and "Relative Sensitivity" 0-5 every half. The characteristic curves are
# read column by column and named top to bottom at the dense end, green, blue,
# red, as the sheet labels them (blue and red lie on one another below log E
# 1); each spectral sensitivity is walked from a point on its peak, the layer
# names on the plot blanked first. The values are returned as plotted.
PHOENIX = "HARMAN Phoenix Technical Data sheet 0924.pdf"


def _phoenix_plot(page: int, x_values, y_values) -> tuple[np.ndarray, "raster.PlotFrame"]:
    document = pymupdf.open(str(SHEETS / PHOENIX))
    pixmap = pymupdf.Pixmap(document, document[page].get_images(full=True)[0][0])
    grey = np.frombuffer(pixmap.samples, dtype=np.uint8).reshape(pixmap.height, pixmap.width, pixmap.n)[..., :3]
    grey = grey.mean(axis=2)
    rows, cols = raster.grid_lines(grey < 235)
    if len(rows) != len(y_values) or len(cols) != len(x_values):
        raise ValueError(f"{PHOENIX} page {page + 1}: {len(cols)} x {len(rows)} rules, "
                         f"expected {len(x_values)} x {len(y_values)}")
    frame = raster.PlotFrame(left=cols[0], right=cols[-1], top=rows[0], bottom=rows[-1],
                             x_min=float(x_values[0]), x_max=float(x_values[-1]),
                             y_at_top=float(y_values[0]), y_at_bottom=float(y_values[-1]))
    return grey < 90, frame


def phoenix_200():
    ink, frame = _phoenix_plot(2, np.arange(0.0, 3.01, 0.5), np.arange(4.0, -0.01, -0.5))
    x, (green, blue, red) = raster.trace(ink, frame, max_curves=3)
    curves = {"G": np.stack([x, green], axis=-1), "B": np.stack([x, blue], axis=-1), "R": np.stack([x, red], axis=-1)}
    ink, frame = _phoenix_plot(1, np.arange(300.0, 801.0, 50.0), np.arange(5.0, -0.01, -0.5))
    names = ((178, 90, 228, 114), (370, 90, 428, 114), (516, 90, 556, 114))     # "Blue", "Green", "Red"
    sensitivity = raster.trace_curves(ink, frame, {"B": (410.0, 2.37), "G": (520.0, 2.6), "g": (565.0, 1.3),
                                                   "R": (650.0, 2.5)}, exclude=list(names))
    # Green's peak is too sharp to walk over: its falling side has a seed of its own.
    fall, rise = sensitivity.pop("g"), sensitivity["G"]
    sensitivity["G"] = np.concatenate([rise[rise[:, 0] < fall[0, 0]], fall])
    return {"curves": curves, "sensitivity": sensitivity}


# The spectral density of a neutral at medium density and at D-min, which
# these negative sheets plot in place of single-dye spectra: what the donor's
# dyes are chosen against (`build_profiles.donor_fit`). Agfa's plots are
# labelled in text on their rules and calibrated from it; Kodak's are named.
NEUTRAL_SPECTRA = {
    "agfa_optima_ii_100": (AGFA, 6, (70.7, 227.5, 196.8, 290.4)),
    "agfa_portrait_xps_160": (AGFA, 7, (53.7, 227.5, 179.8, 290.4)),
    "kodak_royal_gold_100": _plot(ROYAL_GOLD, 2, (234.1, 325.0, 418.4, 509.3),
                                  ((234.1, 400), (295.5, 500), (357.0, 600), (418.4, 700)),
                                  zip((509.3, 472.4, 435.6, 398.7, 361.8, 325.0), np.arange(0.0, 2.51, 0.5))),
    "kodak_vericolor_iii_160": _plot(VERICOLOR, 3, (358.0, 374.6, 542.3, 558.9),
                                     ((358.0, 400), (419.4, 500), (480.9, 600), (542.3, 700)),
                                     zip((558.9, 522.0, 485.2, 448.3, 411.4, 374.6), np.arange(0.0, 2.51, 0.5))),
}


def neutral_spectra(stock: str) -> dict[str, np.ndarray]:
    """The sheet's midscale neutral ("mid") and D-min ("min") spectral
    densities, as (wavelength, density)."""
    entry = NEUTRAL_SPECTRA[stock]
    if not isinstance(entry, VectorPlot):
        import datasheet as ds
        pdf, page, frame = entry
        axes = ds.calibrate(str(SHEETS / pdf), page, pymupdf.Rect(*frame))
        entry = VectorPlot(str(SHEETS / pdf), page, frame, ((axes.x0, axes.x_min), (axes.x1, axes.x_max)),
                           ((axes.y1, axes.y_at_bottom), (axes.y0, axes.y_at_top)))
    return vector_plots.joined_by_value(entry, ("mid", "min"), at_x=550.0)


SOURCES = {
    "agfa_optima_ii_100": optima_ii_100,
    "agfa_portrait_xps_160": portrait_xps_160,
    "fujifilm_astia_100f": astia_100f,
    "fujifilm_eterna_vivid_160t": eterna_vivid_160,
    "fujifilm_eterna_500t": eterna_500,
    "fujifilm_reala_500d": reala_500d,
    "fujifilm_superia_reala_100": superia_reala,
    "fujifilm_velvia_100": velvia_100,
    "kodak_ektachrome_100": ektachrome_100,
    "kodak_kodachrome_64": kodachrome_64,
    "fujifilm_provia_400x": provia_400x,
    "fujifilm_provia_100f": provia_100f,
    "kodak_royal_gold_100": royal_gold_100,
    "kodak_vericolor_iii_160": vericolor_iii,
    "harman_phoenix_200": phoenix_200,
}

# How the curves of a sheet read here were told apart, for the profile's
# `datasource`.
CURVES_READ = {
    "agfa_optima_ii_100": "read from the vector plot, each layer by its dash pattern",
    "agfa_portrait_xps_160": "read from the vector plot, each layer by its dash pattern",
    "fujifilm_astia_100f": "read from the vector plot, each layer by its dash pattern",
    "fujifilm_eterna_vivid_160t": "read from the vector plot against camera stops, each layer by its colour",
    "fujifilm_eterna_500t": "read from the vector plot against camera stops, each layer by its colour",
    "fujifilm_reala_500d": "read from the vector plot, each layer by its colour",
    "fujifilm_superia_reala_100": "read from the vector plot, each layer by its order",
    "fujifilm_velvia_100": "read from the vector plot, each layer by its dash pattern",
    "kodak_ektachrome_100": "read from the vector plot, each layer by its order at the dense end",
    "fujifilm_provia_100f": "traced off the sheet's bitmap stencils, one per layer",
    "kodak_royal_gold_100": "read from the vector plot, each layer by its order at the dense end",
    "kodak_vericolor_iii_160": "read from the vector plot, each layer by its order at the dense end",
    "harman_phoenix_200": "traced off the sheet's bitmap, each layer by its order at the dense end",
}
