#!/usr/bin/env python
"""Build agx-format film profiles from manufacturer datasheets.

The characteristic curves come from the datasheet itself, read out of the PDF's
vector paths (see datasheet.py). Everything the datasheet does not publish in a
usable form — the Hanatos-2025 sensitivity adaptation parameters, the spectral
base density, the sub-layer decomposition — is inherited from the nearest
already-measured agx profile, which for these stocks means the same dye set and
the same process.

That split is deliberate and is what the profile's `datasource` field records:
the part that distinguishes one speed of Kodachrome from another *is* the
characteristic curve, and that part is measured here rather than guessed.

Run:  conda activate flx-bake && python tools/bake/build_profiles.py
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from scipy.optimize import least_squares
from scipy.stats import norm

sys.path.insert(0, str(Path(__file__).resolve().parent))
import datasheet as ds  # noqa: E402
import trace_raster_plot as raster  # noqa: E402
import vector_plots  # noqa: E402
from vector_plots import VectorPlot  # noqa: E402

from spektrafilm.profiles.io import _json_safe, load_profile, profile_to_dict  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SHEETS = REPO / "film spec sheets"
OUT_DIR = REPO / "profiles"
DENSITOMETERS = Path(__file__).resolve().parent / "data" / "densitometer"


@dataclass(frozen=True)
class RasterPlot:
    """A characteristic-curve plot that ships as a bitmap rather than vectors.

    `image_index` is the position in the page's image list; the axis ranges and
    the blocks to ignore have to be read off the plot by eye, because a bitmap
    carries no text to calibrate from.
    """
    image_index: int
    x_min: float
    x_max: float
    d_top: float
    d_bottom: float
    exclude: tuple[tuple[int, int, int, int], ...] = ()


@dataclass(frozen=True)
class RasterSpectra:
    """Spectral curves (sensitivities or dye densities) that ship as a bitmap.

    The rules are named by value, left to right and top to bottom, `None` for
    a border that carries none; each curve is found from a point only it
    passes through (`trace_raster_plot.trace_curves`). `exclude` blanks the
    plot's own lettering, in image pixels.
    """
    image_index: int
    x_rules: tuple[float | None, ...]
    y_rules: tuple[float | None, ...]
    seeds: tuple[tuple[str, float, float], ...]
    exclude: tuple[tuple[int, int, int, int], ...] = ()


@dataclass(frozen=True)
class Build:
    stock: str
    name: str
    donor: str
    pdf: str
    page: int          # 0-based
    iso: int
    notes: str
    raster: RasterPlot | None = None
    # Take the datasheet's densities for what they are, integral densities,
    # and turn them into dye amounts with the donor's (or the sheet's own)
    # dye spectra (`build_unmixed`). Every build does now; `build_one`, which
    # stored them as dye amounts directly, is kept for reference.
    unmix: bool = False
    # R, G and B told apart by their dash pattern, for a sheet that draws all
    # three in black.
    dashes: tuple[tuple[float, ...], tuple[float, ...], tuple[float, ...]] | None = None
    # Mid-grey where a meter set to this exposure index puts it on the sheet's
    # absolute lux-second axis, instead of at the donor's speed point.
    metered_ei: int | None = None
    # The sheet plots its curves against camera stops from the normal exposure
    # (Fujifilm's motion picture sheets), read back in decades from mid-grey,
    # so mid-grey sits at the sheet's 0.
    stop_axis: bool = False
    # Mid-grey this many decades above the green layer's D-min + 0.1 speed
    # point, for a sheet whose absolute axis does not place its rated speed.
    normal_above_speed: float | None = None
    # The paper it prints on by default, where the donor's would not do.
    paper: str | None = None
    # Per-layer exposure shifts (decades, R G B), for a stock whose layer
    # speeds are set so a grey prints neutral rather than taken from the trace.
    layer_shifts: tuple[float, float, float] | None = None
    # Per-layer contrast factors about mid-grey (R G B), likewise.
    layer_gammas: tuple[float, float, float] | None = None
    # The sheet's own characteristic curves, spectral sensitivities and dye
    # spectra as vector plots, in place of the trace above and of the donor's.
    # `colours` (R, G, B stroke colours) picks the curves where they are
    # coloured; otherwise they are told apart by where they peak.
    vector_curves: VectorPlot | None = None
    sensitivity: VectorPlot | None = None
    dyes: VectorPlot | None = None
    # The dye plot also draws a visual neutral, which is left out (Kodak's).
    dyes_with_neutral: bool = False
    # Spectral sensitivities and dye spectra traced off bitmaps instead.
    sensitivity_raster: RasterSpectra | None = None
    dyes_raster: RasterSpectra | None = None
    # The sheet read by `sheet_readers.SOURCES[stock]`, for whatever of curves,
    # sensitivities and dyes it supplies.
    source: bool = False
    # A slide's layer speeds set so a grey views neutral (`neutral_layer_shifts`)
    # instead of taken from the donor.
    neutral_greys: bool = False
    colours: tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]] | None = None
    # A sheet that plots the spectral density of a midscale neutral and of
    # D-min (`sheet_readers.NEUTRAL_SPECTRA`): its D-min, base and fog, as the
    # base density in place of the donor's base and mask, and its neutral
    # stored with the profile.
    sheet_base: bool = False
    # And its dyes: the donor's measured shapes moved and stretched until they
    # add up to the sheet's neutral (`neutral_dyes`).
    fit_dyes: bool = False
    # The antihalation agx seeds the stock's halation from ("strong", "weak",
    # "no"), where the donor's would not do.
    antihalation: str | None = None
    # No Hanatos-2025 adaptation window: agx fits one to each of its own
    # stocks' sensitivities, and cannot refit it for another film's.
    no_window: bool = False
    # A sheet that plots relative log exposure, placed by the ISO 5800 speed
    # point its rating was measured at (`iso_speed_point`).
    relative_axis: bool = False
    # A film without an orange mask: its base is its fog and stain, the dyes in
    # the amounts that read the curves' own D-min.
    unmasked_base: bool = False
    # Spectral sensitivity plotted on a linear scale, each curve falling to
    # zero at both ends: taken as plotted, with none beyond, and no donor tails.
    sensitivity_linear: bool = False
    created: str = "2026-10-18"


def _kodachrome_sensitivity(page: int, left: float, top: float) -> VectorPlot:
    """E-55's spectral sensitivity plot, lower left of each film's page: the
    gridlines every 50 nm from 300 to 700 nm, log sensitivity +2 to -2."""
    return VectorPlot(str(SHEETS / "Kodachrome Spec Sheet.pdf"), page, (left, top, left + 200.5, top + 151.5),
                      tuple((left + 20.0 + 20.1 * i, 300 + 50 * i) for i in range(9)),
                      tuple((top + 37.9 * i, 2.0 - i) for i in range(5)))


def _kodachrome_dyes(page: int, left: float, top: float) -> VectorPlot:
    """E-55's spectral dye density plot, upper right: 400-700 nm, density 0-1
    (each dye at unit peak), with a visual neutral drawn among them."""
    return VectorPlot(str(SHEETS / "Kodachrome Spec Sheet.pdf"), page, (left, top, left + 184.3, top + 184.4),
                      ((left + 0.1, 400), (left + 61.5, 500), (left + 123.0, 600), (left + 184.4, 700)),
                      ((top + 185.5, 0.0), (top + 121.4, 0.5), (top + 57.1, 1.0)))


BUILDS = [
    # Every slide is built the same way, from its own datasheet: the sheet's
    # characteristic curves (Status A integral densities), unmixed into dye
    # amounts with its own dye spectra, its own spectral sensitivities on the
    # donor's tails, and layer speeds set so a grey views neutral, as each
    # sheet says its greys are. The donor (agx's profile of the same or a
    # sister stock) supplies only what a sheet does not: the base density,
    # the Hanatos-2025 adaptation and the couplers' matrix. The bake then sets
    # the matrix to the colour the sheet describes (`stock_overrides.COLOUR`)
    # and fits the scatter and the inhibitor's spread to its MTF (`slide_mtf.py`).
    #
    # Kodachrome 25 and 200 and Velvia 50 had stored their sheets' Status A
    # curves as if they were dye amounts, counting each dye's unwanted
    # absorptions twice: Kodachrome 25 read back at D-max 4.2/3.9/4.7 (R/G/B)
    # against Kodak's 3.8/3.5/3.4, Velvia 50 at 4.1/4.0/4.0 against 3.7/3.6/3.3,
    # and Kodachrome's excess yellow warmed its shadows (b* +3, Kodachrome 200
    # +6) where Kodak's curves run slightly cool. agx's own Kodachrome 64
    # matches its page of the same sheet to 0.03 in log sensitivity.
    Build("kodak_kodachrome_25", "Kodak Kodachrome 25", "kodak_kodachrome_64",
          "Kodachrome Spec Sheet.pdf", 2, 25,
          "Kodak E-55 (rev. 6-09), KODACHROME 25 Professional Film, K-14 process, Status A.",
          unmix=True, neutral_greys=True, sensitivity=_kodachrome_sensitivity(2, 86.7, 402.8),
          dyes=_kodachrome_dyes(2, 368.4, 133.7), dyes_with_neutral=True),
    Build("kodak_kodachrome_64", "Kodak Kodachrome 64", "kodak_kodachrome_64",
          "Kodachrome Spec Sheet.pdf", 3, 64,
          "Kodak E-55 (rev. 6-09), KODACHROME 64 Professional Film, K-14 process, Status A.",
          unmix=True, neutral_greys=True, source=True),
    Build("kodak_kodachrome_200", "Kodak Kodachrome 200", "kodak_kodachrome_64",
          "Kodachrome Spec Sheet.pdf", 4, 200,
          "Kodak E-55 (rev. 6-09), KODACHROME 200 Professional Film, K-14 process, Status A.",
          unmix=True, neutral_greys=True, sensitivity=_kodachrome_sensitivity(4, 86.7, 386.8),
          dyes=_kodachrome_dyes(4, 362.8, 119.6), dyes_with_neutral=True),
    Build("kodak_ektachrome_100", "Kodak Ektachrome 100", "kodak_ektachrome_100",
          "e4000_ektachrome_100.pdf", 2, 100,
          "Kodak Alaris E-4000 (rev. 8-18), KODAK PROFESSIONAL EKTACHROME Film E100, Process E-6, Status A.",
          unmix=True, neutral_greys=True, source=True),
    Build("fujifilm_velvia_50", "Fujifilm Velvia 50", "fujifilm_velvia_100",
          "VELVIA 50 Spec Sheet.pdf", 7, 50,
          "Fujifilm AF3-0221E2, FUJICHROME Velvia 50 Professional [RVP50], E-6/CR-56, Status A.",
          raster=RasterPlot(image_index=2, x_min=-3.0, x_max=1.0, d_top=4.0, d_bottom=0.0,
                            exclude=((950, 40, 1890, 380),      # exposure/process annotation
                                     (226, 1140, 760, 1420))),  # R/G/B legend
          unmix=True, neutral_greys=True,
          sensitivity_raster=RasterSpectra(
              3, (None, 400, 500, 600, 700, None), (2.0, 1.0, 0.0, -1.0),
              (("B", 430, 0.52), ("G", 530, 0.25), ("R", 640, 0.45)),
              exclude=((975, 95, 1780, 265),                      # process / densitometry note
                       (340, 1050, 580, 1220), (880, 1045, 1115, 1220), (1335, 1055, 1570, 1230))),  # layer names
          dyes_raster=RasterSpectra(
              1, (None, 400, 500, 600, 700, None), (None, 1.0, 0.5, 0.0),
              (("Y", 425, 0.75), ("M", 520, 0.75), ("C", 630, 0.80)),
              exclude=((995, 65, 1545, 185),                      # exposure / process note
                       (415, 555, 590, 615), (860, 550, 1095, 610), (1470, 545, 1610, 600)))),  # dye names
    Build("fujifilm_velvia_100", "Fujifilm Velvia 100", "fujifilm_velvia_100",
          "films_velvia-100_datasheet_01.pdf", 7, 100,
          "Fujifilm AF3-202E, FUJICHROME Velvia 100 Professional [RVP100], E-6/CR-56, Status A.",
          unmix=True, neutral_greys=True, source=True),
    Build("fujifilm_provia_100f", "Fujifilm Provia 100F", "fujifilm_provia_100f",
          "films_provia-100f_datasheet_01.pdf", 5, 100,
          "Fujifilm AF3-036E, FUJICHROME PROVIA 100F Professional [RDP III], E-6/CR-56, Status A.",
          unmix=True, neutral_greys=True, source=True),
    # Astia 100F shares Provia 100F's process and the PSHC couplers' dye family;
    # its sheet gives its own curves, sensitivities and dyes.
    Build("fujifilm_astia_100f", "Fujifilm Astia 100F", "fujifilm_provia_100f",
          "astia_100f_datasheet.pdf", 7, 100,
          "Fujifilm AF3-149E, FUJICHROME ASTIA 100F Professional [RAP100F], E-6/CR-56, Status A.",
          unmix=True, neutral_greys=True, source=True),
    # Provia 400X's sheet dyes are Velvia 100's: Fujifilm gives it "the
    # high-color-saturation yellow, magenta and cyan couplers used in Velvia
    # 100/100F and ASTIA 100F" and holds the inter-image effect back with its
    # Multi-Color-Correction Layer for "ISO 100-level color reproduction". So
    # its own dyes, with the inter-image effect held to Provia 100F's colour.
    Build("fujifilm_provia_400x", "Fujifilm Provia 400X", "fujifilm_provia_100f",
          "Fujichrome-Provia-400X-Professional-RXP_AF3-0213E.pdf", 6, 400,
          "Fujifilm AF3-0213E, FUJICHROME PROVIA 400X Professional [RXP], E-6/CR-56, Status A.",
          unmix=True, neutral_greys=True, source=True,
          dashes=((), (10.0, 1.2, 2.0, 1.2), (2.0, 1.2)),   # R solid, G dash-dot, B dashed
          # Gridlines read off page 7: 400-700 nm, log sensitivity 0-3.
          sensitivity=VectorPlot(str(SHEETS / "Fujichrome-Provia-400X-Professional-RXP_AF3-0213E.pdf"), 6,
                                 (332.39, 109.43, 536.33, 288.05),
                                 ((342.76, 400), (404.28, 500), (465.72, 600), (527.09, 700)),
                                 ((288.05, 0), (228.9, 1), (169.39, 2), (109.43, 3)))),
    # Superia Reala, on Superia X-TRA 400 like Superia 1600: metered at its ISO
    # 100 on the sheet's absolute axis, and with the per-layer contrast agx's
    # X-TRA 400 takes against Fujifilm's Status M sheet (see Superia 1600). The
    # sheet gives no single-dye spectra, so X-TRA's dyes stay.
    Build("fujifilm_superia_reala_100", "Fujifilm Superia Reala 100", "fujifilm_xtra_400",
          "fujicolor_superia_reala.pdf", 3, 100,
          "Fujifilm AF3-967E, FUJICOLOR SUPERIA REALA [CS], CN-16, Status M.",
          unmix=True, metered_ei=100, source=True, layer_gammas=(0.922, 0.965, 1.093)),
    # Fujifilm's motion picture negatives, on agx's Vision3 stock of the same
    # light balance and speed class (ECN-2 like them). Their sheets give the
    # curves and sensitivities but no single-dye spectra, so Kodak's dyes stay.
    Build("fujifilm_eterna_vivid_160t", "Fujifilm Eterna Vivid 160T", "kodak_vision3_200t",
          "fujifilm_eterna_vivid160.pdf", 0, 160,
          "Fujifilm KB-0701E, FUJICOLOR ETERNA Vivid 160 (35 mm Type 8543), ECN-2, Status M.",
          unmix=True, source=True, stop_axis=True),
    Build("fujifilm_eterna_500t", "Fujifilm Eterna 500T", "kodak_vision3_500t",
          "fujifilm_ETERNA_500t.pdf", 2, 500,
          "Fujifilm KB-501E, FUJICOLOR ETERNA 500 (35 mm Type 8573), ECN-2, Status M.",
          unmix=True, source=True, stop_axis=True),
    # Reala 500D's sheet plots absolute log H, but metered at its EI 500 on
    # that axis mid-grey sits 0.24 decades over the toe (a speed of about 86
    # by its own curve) and the shadows print flat. It goes where Fujifilm's
    # Eterna sheets put normal exposure: 1.03 and 1.07 decades above green's
    # speed point.
    Build("fujifilm_reala_500d", "Fujifilm Reala 500D", "kodak_vision3_250d",
          "Reala500D_curve.pdf", 0, 500,
          "Fujifilm, FUJICOLOR REALA 500D (35 mm Type 8592), ECN-2, Status M.",
          unmix=True, source=True, normal_above_speed=1.05),
    # Agfa's colour negatives, from its Professional range sheet. It plots no
    # single-dye spectra but the spectral density of a neutral at medium
    # density and at D-min; of agx's negatives, Superia X-TRA 400's dyes and
    # base fit both best (rms 0.05 and 0.04 for Optima II 100, 0.05 and 0.12
    # for Portrait XPS 160), so it is the donor. Agfa's axis puts each film's
    # rated speed exactly at its curves' D-min + 0.1 point (101 and 160). agx
    # has no Agfa paper; of its colour papers, Portra Endura prints both grey
    # wedges most neutral (within a* 3.1 and b* 1.4, and 1.4 and 0.8, from
    # three stops under to two over), X-TRA's Crystal Archive the least (6.4
    # and 6.6, 4.1 and 4.7).
    Build("agfa_optima_ii_100", "Agfa Optima II 100", "fujifilm_xtra_400",
          "agfa_films_data.pdf", 6, 100,
          "Agfa-Gevaert, AGFACOLOR OPTIMA II 100 PROFESSIONAL (Agfa Professional range, Technical Data), "
          "AP 70/C-41, Status M.",
          unmix=True, source=True, metered_ei=100, paper="kodak_portra_endura"),
    Build("agfa_portrait_xps_160", "Agfa Portrait XPS 160", "fujifilm_xtra_400",
          "agfa_films_data.pdf", 7, 160,
          "Agfa-Gevaert, AGFACOLOR PORTRAIT XPS 160 PROFESSIONAL (Agfa Professional range, Technical Data), "
          "AP 70/C-41, Status M.",
          unmix=True, source=True, metered_ei=160, paper="kodak_portra_endura"),
    # Kodak's Royal Gold 100 (1998) and Vericolor III (1997), each built on its
    # own sheet as far as it goes: curves, spectral sensitivities, placement on
    # the sheet's absolute axis, and the spectral density of D-min, base and
    # fog, as the base in place of a donor's base and mask. Neither plots
    # single dyes, only a midscale neutral: the dyes are those of agx's colour
    # negative that fits it best (`build_profiles.py --donors`), moved and
    # stretched until they add up to it. agx's adaptation window, fitted to its
    # own stocks' sensitivities, is left out. "Extremely high sharpness" and a
    # professional portrait film: the halation of a modern colour negative.
    # Royal Gold: Kodak's own dyes fit its neutral (Ektar 100 0.016 RMS
    # density, Gold 200 0.027); metered at ISO 100 a grey card reads red 0.84,
    # in Kodak's 0.80-1.00, and its curves' own ISO speed is 103. Kodak prints
    # it on Ektacolor papers: Ektacolor Edge.
    Build("kodak_royal_gold_100", "Kodak Royal Gold 100", "kodak_ektar_100",
          "e41-1998_02_Kodak Royal Gold 100.pdf", 2, 100,
          "Kodak E-41 (minor revision 2-98), KODAK ROYAL GOLD 100 Film, C-41, Status M; its spectral "
          "sensitivities read at density 0.2 above D-min.",
          unmix=True, source=True, metered_ei=100, paper="kodak_ektacolor_edge",
          sheet_base=True, fit_dyes=True, no_window=True, antihalation="strong", created="2026-10-25"),
    # Vericolor III's dyes are broader than Kodak's later ones: Fujifilm C200's
    # fit its neutral best (0.047; X-TRA 400 0.049, Portra 160 0.104), and
    # moved and stretched 0.012. Metered at its rated ISO 160 a grey card reads
    # red 0.69 against Kodak's 0.73-0.93; its curves' own ISO speed is 126,
    # and Kodak notes that many photographers expose it at EI 125. Kodak
    # prints it on Ektacolor Portra III: Portra Endura.
    Build("kodak_vericolor_iii_160", "Kodak Vericolor III 160", "fujifilm_c200",
          "e26-Vericolor_III.pdf", 3, 160,
          "Kodak E-26 (minor revision 4-97), KODAK VERICOLOR III Professional Film (5026, 6006), C-41, "
          "Status M.",
          unmix=True, source=True, metered_ei=160, paper="kodak_portra_endura",
          sheet_base=True, fit_dyes=True, no_window=True, antihalation="strong", created="2026-10-25"),
    # HARMAN Phoenix 200 (2024), an experimental C-41 film. Its sheet's bitmaps
    # give the curves, against relative log exposure, and the spectral
    # sensitivities, on a linear "Relative Sensitivity" scale. Its ISO 200 was
    # measured by the ISO method, which places the relative axis; it is metered
    # at EI 125 (Felix's pick), in HARMAN's own EI 100-200 and the rating HARMAN
    # gives its re-release. It has no
    # masking dyes, and its D-min is high (G 0.91, the "magenta / purple tint"):
    # the base is that fog and stain, in the dyes. HARMAN publishes no dye
    # spectra: the dyes and the coupler model are those of agx's other ISO 200
    # colour negative for general use, Kodak Gold 200. "Limited antihalation" and
    # "striking halation effects around bright light sources": agx's "no"
    # antihalation, a rem-jet-free film's, at Felix's pick.
    # Its wedge spectrogram is "to tungsten light (2856K)", read without taking
    # the lamp out, as HARMAN's HP5 Plus spectrogram is (`build_bw_profiles`).
    Build("harman_phoenix_200", "Harman Phoenix 200", "kodak_gold_200",
          "HARMAN Phoenix Technical Data sheet 0924.pdf", 2, 200,
          "HARMAN technology, HARMAN Phoenix 200 technical information (Sep 2024), C-41; the sheet names no "
          "densitometry, taken as Status M.",
          unmix=True, source=True, relative_axis=True, metered_ei=125, unmasked_base=True,
          sensitivity_linear=True, no_window=True, antihalation="no", paper="kodak_portra_endura",
          created="2026-10-25"),
    Build("fujifilm_superia_1600", "Fujifilm Superia 1600", "fujifilm_xtra_400",
          "superia_1600_datasheet.pdf", 5, 1600,
          "Fujifilm AF3-145E, FUJICOLOR SUPERIA 1600 [CU], CN-16, Status M.",
          unmix=True, metered_ei=1600,
          colours=((0.92, 0.18, 0.18), (0.0, 0.67, 0.31), (0.2, 0.23, 0.59)),
          # Status M curves drawn straight into the model print with yellow
          # highlights (b* +6), as agx's own Fuji negatives would against their
          # sheets; agx corrects those for print in processing that is not
          # public. Measured on its X-TRA 400, Superia's sister stock and donor,
          # agx's straight-line contrast is Fujifilm's sheet times 0.922 (red),
          # 0.965 (green) and 1.093 (blue), and Superia takes exactly that.
          # The layer speeds, which a trace is not trusted for, are then the
          # ones that print a grey wedge most neutral (within a* 1.4 and b* 2.9
          # from 4 stops under to 3 over, about as agx's X-TRA 400 does).
          layer_gammas=(0.922, 0.965, 1.093),
          layer_shifts=(0.01, 0.0, -0.13),
          # Gridlines read off page 6: every half decade and half density on the
          # curves; on the sensitivity plot 400-700 nm and the two rules its
          # "1.0" bar spans, one decade apart.
          vector_curves=VectorPlot(str(SHEETS / "superia_1600_datasheet.pdf"), 5, (84.6, 118.4, 280.4, 291.4),
                                   tuple(zip((84.6, 106.1, 128.3, 150.2, 171.8, 193.4, 215.2, 236.9, 258.5, 280.3),
                                             (-4.0, -3.5, -3.0, -2.5, -2.0, -1.5, -1.0, -0.5, 0.0, 0.5))),
                                   tuple(zip((291.4, 270.2, 248.8, 226.9, 205.0, 183.5, 161.6, 139.5, 118.4),
                                             (0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0)))),
          sensitivity=VectorPlot(str(SHEETS / "superia_1600_datasheet.pdf"), 5, (340.07, 117.48, 534.64, 288.19),
                                 ((351.43, 400), (408.94, 500), (465.91, 600), (522.85, 700)),
                                 ((231.7, 0.0), (174.51, 1.0)))),
]

def raster_curves(pdf: Path, page: int, spec: RasterPlot):
    """Characteristic curves traced off an embedded bitmap plot."""
    import pymupdf
    document = pymupdf.open(str(pdf))
    images = document[page].get_images(full=True)
    xref = images[spec.image_index][0]
    pixmap = pymupdf.Pixmap(document, xref)
    if pixmap.n > 4:
        pixmap = pymupdf.Pixmap(pymupdf.csRGB, pixmap)
    tmp = Path("/tmp") / f"_flx_{pdf.stem}_{spec.image_index}.png"
    pixmap.save(str(tmp))

    mask = raster.load_mask(str(tmp))
    frame = raster.find_frame(mask, x_min=spec.x_min, x_max=spec.x_max,
                              y_at_top=spec.d_top, y_at_bottom=spec.d_bottom)
    x, traces = raster.trace(mask, frame, exclude=list(spec.exclude), max_curves=3)
    # `trace` returns curves top to bottom on the plot; Fuji draws G above B
    # above R at the dense end, matching the descending order used elsewhere.
    curves = [np.stack([x, t], axis=-1) for t in traces]
    axes = ds.Axes(0, 0, 1, 1, spec.x_min, spec.x_max, spec.d_top, spec.d_bottom)
    return axes, curves


def raster_spectra(pdf: Path, page: int, spec: RasterSpectra) -> dict[str, np.ndarray]:
    """Named curves, as (x, y) in data units, traced off a bitmap plot."""
    import pymupdf
    document = pymupdf.open(str(pdf))
    xref = document[page].get_images(full=True)[spec.image_index][0]
    pixmap = pymupdf.Pixmap(document, xref)
    if pixmap.n > 4:
        pixmap = pymupdf.Pixmap(pymupdf.csRGB, pixmap)
    tmp = Path("/tmp") / f"_flx_{pdf.stem}_{page}_{spec.image_index}.png"
    pixmap.save(str(tmp))
    mask = raster.load_mask(str(tmp))
    rows, cols = raster.grid_lines(mask)
    if len(rows) != len(spec.y_rules) or len(cols) != len(spec.x_rules):
        raise ValueError(f"{pdf.name} page {page + 1}: {len(cols)} x {len(rows)} rules, "
                         f"expected {len(spec.x_rules)} x {len(spec.y_rules)}")

    def axis(positions, values):
        known = [(p, v) for p, v in zip(positions, values) if v is not None]
        k, c = np.polyfit([p for p, _ in known], [v for _, v in known], 1)
        return lambda p: k * p + c

    fx, fy = axis(cols, spec.x_rules), axis(rows, spec.y_rules)
    frame = raster.PlotFrame(left=cols[0], right=cols[-1], top=rows[0], bottom=rows[-1],
                             x_min=fx(cols[0]), x_max=fx(cols[-1]),
                             y_at_top=fy(rows[0]), y_at_bottom=fy(rows[-1]))
    return raster.trace_curves(mask, frame, {n: (x, y) for n, x, y in spec.seeds},
                               exclude=list(spec.exclude))


def characteristic_curves(pdf: Path, page: int):
    """The R, G, B characteristic curves in (log exposure, density)."""
    # These sheets draw a decorative box around each plot as well as the plot's
    # own axis frame, and only the inner one carries the tick labels. Rather
    # than guess, try each candidate innermost-first and keep the one that both
    # calibrates and contains three curves.
    candidates = [r for r in ds.frames(str(pdf), page, min_size=100)
                  if 130 < r.width < 300 and 120 < r.height < 300]
    # Upper-left plot on the page, then smallest first.
    candidates.sort(key=lambda r: (round(r.y0 / 60), round(r.x0 / 60), r.width * r.height))
    errors = []
    for frame in candidates:
        try:
            axes = ds.calibrate(str(pdf), page, frame)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        curves = ds.curves_in(str(pdf), page, axes)
        if len(curves) >= 3:
            break
        errors.append(f"frame {frame} held {len(curves)} curves")
    else:
        raise ValueError(f"no usable plot frame on page {page + 1} of {pdf.name}: {errors}")
    # Ordered by density at the dense (low-exposure) end: R above G above B,
    # which is how every one of these sheets is drawn.
    probe = axes.x_min + 0.25 * (axes.x_max - axes.x_min)
    curves = ds.order_curves(curves, at_x=probe, descending=True)[:3]
    return axes, curves


def speed_point(log_exposure: np.ndarray, density: np.ndarray, above_min: float = 1.0) -> float:
    """Log exposure at which the mean density sits `above_min` over D-min.

    A single well-defined point on the curve, used to line the datasheet's
    absolute exposure axis up with agx's normalised one. Density falls with
    exposure on a positive stock, so the axis is reversed for the search.
    """
    mean = density.mean(axis=1)
    target = mean.min() + above_min
    order = np.argsort(mean)
    return float(np.interp(target, mean[order], log_exposure[order]))


def speed_offset(build: Build, donor_profile, grid: np.ndarray) -> float:
    """How far to slide the datasheet curves onto agx's exposure axis.

    Every agx slide profile puts its D-min + 1.0 speed point within a fifth of a
    stop of the same place, because the spectral sensitivities are normalised so
    a mid-grey scene lands at log-exposure zero whatever the stock. The film's
    ISO rating is what the photographer dials into the meter — it is not
    supposed to make the picture darker when they switch stocks.

    So each stock is aligned on its *own* speed point rather than by a shared
    offset along the datasheet's absolute axis. What then distinguishes ISO 25
    from ISO 200 is the shape of the curve — contrast, D-max, toe and shoulder —
    which is exactly what the datasheet measures and what should carry over.
    """
    donor_le = np.asarray(donor_profile.data.log_exposure)
    donor_dc = np.asarray(donor_profile.data.density_curves)
    return speed_point(donor_le, donor_dc) - _sheet_speed_point(build)


def _sheet_speed_point(build: Build) -> float:
    """The same landmark, measured on this stock's own datasheet plot."""
    if build.raster is not None:
        axes, curves = raster_curves(SHEETS / build.pdf, build.page, build.raster)
    else:
        axes, curves = characteristic_curves(SHEETS / build.pdf, build.page)
    sheet_grid = np.linspace(axes.x_min, axes.x_max, 512)
    sheet_dc = np.stack([ds.resample(c, sheet_grid) for c in curves], axis=-1)
    sheet_dc = sheet_dc - sheet_dc.min(axis=0, keepdims=True)
    return speed_point(sheet_grid, sheet_dc)


def _isotonic(y: np.ndarray, increasing: bool) -> np.ndarray:
    """Pool-adjacent-violators: the closest monotonic curve in least squares.

    Preferable to clamping with a running min or max, which pins the whole
    curve to whichever end it starts from — and the ends are exactly where a
    traced bitmap is least trustworthy, because the line runs into the border.
    """
    values = (y if increasing else -y).astype(float).copy()
    weights = np.ones_like(values)
    blocks: list[tuple[float, float]] = []
    for v, w in zip(values, weights):
        blocks.append((v, w))
        while len(blocks) > 1 and blocks[-2][0] > blocks[-1][0]:
            v2, w2 = blocks.pop()
            v1, w1 = blocks.pop()
            merged = (v1 * w1 + v2 * w2) / (w1 + w2)
            blocks.append((merged, w1 + w2))
    out = np.concatenate([np.full(int(round(w)), v) for v, w in blocks])[: y.size]
    return out if increasing else -out


def condition(sheet_grid: np.ndarray, density: np.ndarray, positive: bool,
              smooth: int = 9, trim: float = 0.02) -> np.ndarray:
    """Smooth, trim and monotonise extracted curves.

    A characteristic curve is monotonic by physics: more light never gives a
    positive stock more density. A traced bitmap is not — line thickness,
    dashes and anti-aliasing put kinks in it — and a kink in the highlights
    becomes a posterised colour blotch once the three channels disagree about
    where it is. The outermost columns are dropped first because that is where
    the plotted line meets the border and the trace picks up the frame.
    """
    out = density.copy()
    n = out.shape[0]
    edge = max(int(n * trim), 1)
    if trim > 0:
        out[:edge] = out[edge]
        out[-edge:] = out[-edge - 1]
    if smooth > 1:
        kernel = np.ones(smooth) / smooth
        for c in range(3):
            padded = np.pad(out[:, c], smooth, mode="edge")
            out[:, c] = np.convolve(padded, kernel, mode="same")[smooth:-smooth]
    order = np.argsort(sheet_grid)
    for c in range(3):
        out[order, c] = _isotonic(out[order, c], increasing=not positive)
    return out


def channel_speed_point(log_exposure: np.ndarray, density: np.ndarray, channel: int,
                        above_min: float = 1.0) -> float:
    """The D-min + 1.0 landmark for one channel on its own."""
    column = density[:, channel]
    target = column.min() + above_min
    order = np.argsort(column)
    return float(np.interp(target, column[order], log_exposure[order]))


def iso_speed_point(log_exposure: np.ndarray, net: np.ndarray) -> float:
    """ISO 5800's speed point of a colour negative, H_m: the mean log exposure
    at which the green layer and the slowest one reach 0.15 over D-min. The
    rated speed S puts it at sqrt(2) / S lux-seconds."""
    points = [channel_speed_point(log_exposure, net, c, above_min=0.15) for c in range(3)]
    return 0.5 * (points[1] + max(points))


def linear_log_sensitivity(wl: np.ndarray, traced: dict[str, np.ndarray]) -> np.ndarray:
    """Sensitivities plotted on a linear scale, as log sensitivity: each layer as
    plotted, none (NaN) where it is zero and beyond its ends. Each layer's level
    is arbitrary here: `balance` sets it."""
    out = np.full((wl.size, 3), np.nan)
    for c, name in enumerate("RGB"):
        curve = traced[name][np.argsort(traced[name][:, 0])]
        y = np.interp(wl, curve[:, 0], curve[:, 1], left=0.0, right=0.0)
        out[y > 0, c] = np.log10(y[y > 0])
    return out


def align_channel_balance(sheet_grid: np.ndarray, density: np.ndarray,
                          donor_le: np.ndarray, donor_dc: np.ndarray) -> np.ndarray:
    """Match each layer's density at mid-grey to the donor's.

    The per-layer *speed* balance is the part of a datasheet extraction that
    cannot be trusted. It depends on exactly where each curve's D-min is read,
    and the three curves overlap for most of the scale; a few hundredths of
    density there is the difference between a neutral render and a cast. Left
    alone it showed: Kodachrome 25 and 200 came out yellow-green while agx's
    own Kodachrome 64 sits blue-magenta, from a dye set the datasheet says is
    identical.

    Each layer is therefore slid along the exposure axis until it develops the
    same density at mid-grey as the donor does. Curve *shape* — contrast,
    D-max, how each end rolls off, which is what the datasheet actually
    measures well and what separates ISO 25 from ISO 200 — is untouched.
    """
    out = np.empty_like(density)
    for c in range(3):
        target = float(np.interp(0.0, donor_le, donor_dc[:, c]))
        column = density[:, c]
        order = np.argsort(column)
        # Where this layer currently reaches the donor's mid-grey density
        x_at_target = float(np.interp(target, column[order], sheet_grid[order]))
        out[:, c] = np.interp(sheet_grid, sheet_grid - x_at_target, column)
    return out


def fit_cdf_model(log_exposure: np.ndarray, density: np.ndarray, positive: bool,
                  n_layers: int = 3):
    """Fit agx's `density_curves_model`: a sum of scaled normal CDFs per channel.

    This is the form the print-curve morph and the sub-layer grain model both
    expect, and fitting it here means the new stocks support every control the
    measured ones do.
    """
    centers = np.zeros((3, n_layers))
    amplitudes = np.zeros((3, n_layers))
    sigmas = np.zeros((3, n_layers))
    sign = -1.0 if positive else 1.0

    for c in range(3):
        y = density[:, c]
        span = float(y.max() - y.min())
        x_mid = float(np.interp(y.min() + span / 2, np.sort(y), log_exposure[np.argsort(y)]))

        def model(params):
            mu = params[0:n_layers]
            amp = params[n_layers:2 * n_layers]
            sig = params[2 * n_layers:3 * n_layers]
            total = np.zeros_like(log_exposure)
            for i in range(n_layers):
                total += amp[i] * norm.cdf(sign * (log_exposure - mu[i]) / max(sig[i], 1e-3))
            return total

        init = np.concatenate([
            x_mid + np.linspace(-0.6, 0.6, n_layers),
            np.full(n_layers, span / n_layers),
            np.full(n_layers, 0.5),
        ])
        lower = np.concatenate([np.full(n_layers, log_exposure.min() - 2),
                                np.full(n_layers, 0.0),
                                np.full(n_layers, 0.05)])
        upper = np.concatenate([np.full(n_layers, log_exposure.max() + 2),
                                np.full(n_layers, span * 1.5 + 1e-3),
                                np.full(n_layers, 4.0)])
        result = least_squares(lambda p: model(p) - y, init, bounds=(lower, upper),
                               max_nfev=20000)
        order = np.argsort(result.x[0:n_layers])
        centers[c] = result.x[0:n_layers][order]
        amplitudes[c] = result.x[n_layers:2 * n_layers][order]
        sigmas[c] = result.x[2 * n_layers:3 * n_layers][order]

    return centers, amplitudes, sigmas


def layer_curves(log_exposure, centers, amplitudes, sigmas, positive: bool):
    """Per-sub-layer density, shaped (n, 3 layers, 3 channels) like agx."""
    sign = -1.0 if positive else 1.0
    n_layers = centers.shape[1]
    out = np.zeros((log_exposure.size, n_layers, 3))
    for c in range(3):
        for l in range(n_layers):
            out[:, l, c] = amplitudes[c, l] * norm.cdf(
                sign * (log_exposure - centers[c, l]) / max(sigmas[c, l], 1e-3))
    return out


# ---------------------------------------------------------------------------
# Datasheet densities into dye amounts
# ---------------------------------------------------------------------------

def responsivities(densitometer: str, wavelengths: np.ndarray) -> np.ndarray:
    """Status A or M responsivities (agx's `info.densitometer`) on `wavelengths`."""
    columns = []
    for c in "rgb":
        a = np.loadtxt(DENSITOMETERS / densitometer / f"responsivity_{c}.csv", delimiter=",")
        a = a[np.argsort(a[:, 0])]
        columns.append(np.interp(wavelengths, a[:, 0], a[:, 1], left=0, right=0))
    s = np.stack(columns, axis=-1)
    return s / s.max(axis=0)


def status_density(spectral: np.ndarray, resp: np.ndarray) -> np.ndarray:
    """What a densitometer with `resp` reads off spectral densities (..., 81)."""
    return -np.log10((10.0 ** -spectral) @ resp / resp.sum(axis=0))


def unmix(target_net: np.ndarray, cd: np.ndarray, bd: np.ndarray, resp: np.ndarray,
          iters: int = 50) -> tuple[np.ndarray, float]:
    """Dye amounts whose densitometer reading over the base equals `target_net`.

    A datasheet curve is an integral density: each layer's dye also absorbs in
    the other channels, so the amount of dye a channel needs is less than its
    density. agx's own profiles are stored as amounts for that reason; Newton
    on the densitometer reading finds them.
    """
    base = status_density(bd[None, :], resp)[0]
    target = target_net + base
    a = np.clip(target_net, 0, None)
    eps = 1e-5
    err = np.inf
    for _ in range(iters):
        f0 = status_density(a @ cd.T + bd[None, :], resp)
        residual = f0 - target
        err = float(np.abs(residual).max())
        if err < 1e-8:
            break
        jacobian = np.empty((a.shape[0], 3, 3))
        for j in range(3):
            da = np.zeros(3)
            da[j] = eps
            jacobian[:, :, j] = (status_density((a + da) @ cd.T + bd[None, :], resp) - f0) / eps
        a = a - np.linalg.solve(jacobian, residual[..., None])[..., 0]
    return a, err


def _dash_pattern(path) -> tuple[float, ...]:
    text = path.get("dashes") or "[] 0"
    inside = text[text.index("[") + 1:text.index("]")]
    return tuple(round(float(v), 1) for v in inside.split())


def dashed_curves(pdf: Path, page: int, dashes):
    """The R, G and B curves of a sheet that draws all three in black.

    They are the plot's Bezier paths, told apart by dash pattern — ordering by
    density would not do, since which curve is densest changes along the axis.
    Rules, ticks and the legend's sample lines are straight, so they never
    match.
    """
    import pymupdf
    candidates = [r for r in ds.frames(str(pdf), page, min_size=100)
                  if 130 < r.width < 300 and 120 < r.height < 300]
    candidates.sort(key=lambda r: (round(r.y0 / 60), round(r.x0 / 60), r.width * r.height))
    drawings = pymupdf.open(str(pdf))[page].get_drawings()
    for frame in candidates:
        try:
            axes = ds.calibrate(str(pdf), page, frame)
        except ValueError:
            continue
        found = [None, None, None]
        for path in drawings:
            items = [i for i in path["items"] if i[0] in ("l", "c")]
            if path.get("type") != "s" or not any(i[0] == "c" for i in items):
                continue
            rect = path["rect"]
            if not (axes.x0 - 2 <= rect.x0 and rect.x1 <= axes.x1 + 2
                    and axes.y0 - 2 <= rect.y0 and rect.y1 <= axes.y1 + 2
                    and rect.width > 0.5 * (axes.x1 - axes.x0)):
                continue
            pattern = _dash_pattern(path)
            for k, want in enumerate(dashes):
                if pattern == tuple(round(v, 1) for v in want):
                    found[k] = axes.to_data(ds._flatten_path(items, steps=32))
        if all(f is not None for f in found):
            return axes, found
    raise ValueError(f"no plot on page {page + 1} of {pdf.name} holds all three dash patterns")


def sheet_curves(build: Build):
    """The sheet's R, G and B curves as (log H, density) and the range they span."""
    if build.source:
        import sheet_readers
        found = sheet_readers.SOURCES[build.stock]().get("curves")
        if found is not None:
            curves = [found[k][np.argsort(found[k][:, 0])] for k in "RGB"]
            return (max(c[0, 0] for c in curves), min(c[-1, 0] for c in curves)), curves
    if build.vector_curves is not None:
        found = vector_plots.by_colour(build.vector_curves, dict(zip("RGB", build.colours)))
        curves = [found[k][np.argsort(found[k][:, 0])] for k in "RGB"]
        return (max(c[0, 0] for c in curves), min(c[-1, 0] for c in curves)), curves
    if build.dashes is not None:
        axes, curves = dashed_curves(SHEETS / build.pdf, build.page, build.dashes)
        return (axes.x_min, axes.x_max), curves
    if build.raster is not None:
        axes, curves = raster_curves(SHEETS / build.pdf, build.page, build.raster)
        return (axes.x_min, axes.x_max), curves
    axes, curves = characteristic_curves(SHEETS / build.pdf, build.page)
    return (axes.x_min, axes.x_max), curves


def own_log_sensitivity(donor_ls: np.ndarray, wl: np.ndarray, traced: dict[str, np.ndarray]) -> np.ndarray:
    """The sheet's spectral sensitivity shapes, on the donor's tails.

    Inside the plotted range each layer is the sheet's curve; beyond it, where
    the sheet stops drawing, the donor's own extrapolation continues from
    where the sheet's ends. Each layer's level is arbitrary here: `balance`
    sets it.
    """
    out = donor_ls.copy()
    for c, name in enumerate("RGB"):
        curve = traced[name][np.argsort(traced[name][:, 0])]
        lo, hi = curve[0, 0], curve[-1, 0]
        inside = (wl >= lo) & (wl <= hi)
        out[inside, c] = np.interp(wl[inside], curve[:, 0], curve[:, 1])
        for edge, side in ((lo, wl < lo), (hi, wl > hi)):
            out[side, c] = donor_ls[side, c] + (np.interp(edge, curve[:, 0], curve[:, 1])
                                                - np.interp(edge, wl, donor_ls[:, c]))
    return out


def balance(profile) -> np.ndarray:
    """Scale each layer's sensitivity so an 18.4% grey exposes it to exactly 1.

    agx normalises its stocks that way, through the same spectral LUT the app
    bakes (the stock's own adaptation window included), so mid-grey lands at
    log exposure 0 in every layer.
    """
    from spektrafilm.utils.spectral_upsampling import compute_hanatos2025_tc_lut, rgb_to_raw_hanatos2025
    adaptation = profile.hanatos2025_adaptation()
    adaptation.apply_window = np.asarray(adaptation.window_params).size > 0
    adaptation.apply_surface = False
    adaptation.spectral_gaussian_blur = 0.0
    ls = np.asarray(profile.data.log_sensitivity, float)
    sensitivity = np.nan_to_num(10 ** ls)
    lut = compute_hanatos2025_tc_lut(sensitivity, adaptation, gamut_compress=None)
    raw = np.asarray(rgb_to_raw_hanatos2025(np.full((1, 1, 3), 0.184), sensitivity, "ITU-R BT.2020", False,
                                            profile.info.reference_illuminant, tc_lut=lut))[0, 0]
    profile.update_data(log_sensitivity=ls - np.log10(raw)[None, :])
    return raw


def own_dyes(traced: dict[str, np.ndarray], wl: np.ndarray, valid: np.ndarray,
             like: np.ndarray, resp: np.ndarray) -> np.ndarray:
    """The sheet's dye spectra (C, M, Y columns, as agx stores them), each scaled
    so a unit amount of it reads the same density in its own channel as the
    donor's does, which keeps the dye-amount scale the couplers work in."""
    out = np.zeros((wl.size, 3))
    for j, name in enumerate("CMY"):
        curve = traced[name][np.argsort(traced[name][:, 0])]
        out[:, j] = np.clip(np.interp(wl, curve[:, 0], curve[:, 1]), 0, None)
    out[~valid] = 0.0
    return scaled_like(out, like, resp)


def scaled_like(dyes: np.ndarray, like: np.ndarray, resp: np.ndarray) -> np.ndarray:
    """Each dye scaled so a unit amount reads the same density in its own
    channel as `like`'s does: the dye-amount scale the couplers work in."""
    out = dyes.copy()
    for j in range(3):
        target = status_density(like[:, j][None, :], resp)[0, j]
        lo, hi = 0.2, 5.0
        for _ in range(60):
            mid = 0.5 * (lo + hi)
            density = status_density((out[:, j] * mid)[None, :], resp)[0, j]
            lo, hi = (mid, hi) if density < target else (lo, mid)
        out[:, j] *= 0.5 * (lo + hi)
    return out


def neutral_dyes(stock: str, wl: np.ndarray, donor_cd: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """The donor's dye spectra made the sheet's own, as far as it measures them.

    A sheet that plots the spectral density of a midscale neutral and of D-min
    fixes what its three dyes add up to, if not each one. Each of the donor's
    measured dyes is moved along the spectrum and stretched about its peak, by
    at most 20 nm and 25% and held to small moves, until in the amounts that
    fit best the three add up to the neutral less D-min. Returns the dyes (C,
    M, Y columns, as agx stores them; NaN where a moved dye leaves the donor's
    measured range) and the fit (amounts, shifts in nm, stretches).
    """
    import sheet_readers
    from scipy.optimize import nnls
    sheet = sheet_readers.neutral_spectra(stock)
    inside = (wl >= max(c[0, 0] for c in sheet.values())) & (wl <= min(c[-1, 0] for c in sheet.values()))
    target = np.interp(wl, sheet["mid"][:, 0], sheet["mid"][:, 1]) - np.interp(wl, sheet["min"][:, 0], sheet["min"][:, 1])

    def moved(dye, shift, stretch):
        ok = np.isfinite(dye)
        peak = wl[ok][np.argmax(dye[ok])]
        return np.interp(peak + (wl - peak - shift) / stretch, wl[ok], dye[ok], left=np.nan, right=np.nan)

    def dyes(q):
        return np.stack([moved(donor_cd[:, j], q[3 + j], q[6 + j]) for j in range(3)], axis=-1)

    def residual(q):
        d = dyes(q)
        # A dye moved off where the sheet measures costs as much as a bad fit.
        r = np.where(np.isfinite(d).all(axis=1), np.nan_to_num(d) @ q[:3] - target, 0.3)[inside]
        return np.concatenate([r, 0.002 * np.asarray(q[3:6]), 0.2 * np.log(q[6:9])])

    ok = inside & np.isfinite(donor_cd).all(axis=1)
    amounts, _ = nnls(donor_cd[ok], target[ok])
    q = least_squares(residual, np.concatenate([amounts, [0.0] * 3, [1.0] * 3]),
                      bounds=([0] * 3 + [-20] * 3 + [0.8] * 3, [3] * 3 + [20] * 3 + [1.25] * 3)).x
    return dyes(q), q


def _lab(xyz: np.ndarray, white: np.ndarray) -> np.ndarray:
    f = lambda t: np.where(t > 0.008856, np.cbrt(t), 7.787 * t + 16 / 116)   # noqa: E731
    fx, fy, fz = (f(xyz[..., i] / white[i]) for i in range(3))
    return np.stack([116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)], axis=-1)


def viewed_wedge(grid, amounts, cd, bd, defined, viewing, shifts, stops) -> np.ndarray:
    """L*a*b* of a neutral wedge as the engine views a slide: each layer's dye
    amount at the grey's exposure (less its speed shift), transmitted under the
    light table over the wavelengths the dyes are defined on, and white-pointed
    on the clear base."""
    from spektrafilm.config import SPECTRAL_SHAPE, STANDARD_OBSERVER_CMFS
    from spektrafilm.model.illuminants import standard_illuminant
    cmf = np.asarray(STANDARD_OBSERVER_CMFS.align(SPECTRAL_SHAPE).values, float)
    light = np.asarray(standard_illuminant(viewing), float) * defined
    le = np.asarray(stops, float)[:, None] * np.log10(2) - np.asarray(shifts, float)[None, :]
    a = np.stack([np.interp(le[:, c], grid, amounts[:, c]) for c in range(3)], axis=-1)
    xyz = ((10.0 ** -(a @ cd.T + bd[None, :])) * light) @ cmf
    base = ((10.0 ** -bd) * light) @ cmf
    white = light @ cmf
    return _lab(xyz * (white / base), white)


def neutral_layer_shifts(grid, amounts, cd, bd, defined, viewing) -> np.ndarray:
    """Red and blue layer speeds (decades, green fixed) that view a grey wedge
    neutral from three stops under mid-grey to two over, mid-grey weighted most."""
    from scipy.optimize import least_squares as solve
    stops = np.array([-3, -2, -1, 0, 1, 2], float)
    weight = np.array([0.5, 1, 1, 1.5, 1, 0.5])

    def residual(p):
        lab = viewed_wedge(grid, amounts, cd, bd, defined, viewing, (p[0], 0.0, p[1]), stops)
        return np.concatenate([weight * lab[:, 1], weight * lab[:, 2]])

    p = solve(residual, [0.0, 0.0], bounds=([-0.3, -0.3], [0.3, 0.3])).x
    return np.array([p[0], 0.0, p[1]])


# agx's colour negatives: the dye sets and bases a negative built from its sheet
# can take (`donor_fit`).
NEGATIVE_DONORS = ("kodak_gold_200", "kodak_ultramax_400", "kodak_portra_160", "kodak_portra_400", "kodak_portra_800",
                   "kodak_ektar_100", "fujifilm_c200", "fujifilm_xtra_400", "fujifilm_pro_400h", "kodak_vision3_50d",
                   "kodak_vision3_200t", "kodak_vision3_250d", "kodak_vision3_500t", "kodak_verita_200d")


def donor_fit(stock: str, candidates=NEGATIVE_DONORS) -> list[tuple[str, float, float, float]]:
    """How well each candidate's dyes and base reproduce a sheet that plots the
    spectral density of a midscale neutral and of D-min instead of single dyes
    (`sheet_readers.NEUTRAL_SPECTRA`): (stock, dyes RMS, neutral RMS, D-min
    RMS), best dyes first.

    The dyes are fitted to what the neutral adds over D-min, in the
    non-negative amounts that fit it best: what a build that takes its base
    from the sheet's D-min (`Build.sheet_base`) borrows. A build that keeps the
    donor's base (Agfa's) was chosen by the other two: the neutral and D-min
    each as the candidate's base plus its dyes, at D-min the fog. All over the
    wavelengths the sheet and every candidate define, within 400-700 nm.
    """
    import sheet_readers
    from scipy.optimize import nnls
    sheet = sheet_readers.neutral_spectra(stock)
    profiles = {name: load_profile(name) for name in candidates}
    wl = np.asarray(next(iter(profiles.values())).data.wavelengths, float)
    ok = (wl >= 400) & (wl <= 700)
    for curve in sheet.values():
        ok &= (wl >= curve[0, 0]) & (wl <= curve[-1, 0])
    for profile in profiles.values():
        ok &= (np.isfinite(np.asarray(profile.data.channel_density, float)).all(axis=1)
               & np.isfinite(np.asarray(profile.data.base_density, float)))
    mid = np.interp(wl[ok], sheet["mid"][:, 0], sheet["mid"][:, 1])
    dmin = np.interp(wl[ok], sheet["min"][:, 0], sheet["min"][:, 1])
    out = []
    for name, profile in profiles.items():
        cd = np.asarray(profile.data.channel_density, float)[ok]
        bd = np.asarray(profile.data.base_density, float)[ok]
        rms = []
        for target, base in ((mid - dmin, 0.0), (mid, bd), (dmin, bd)):
            amounts, _ = nnls(cd, target - base)
            rms.append(float(np.sqrt(np.mean((cd @ amounts + base - target) ** 2))))
        out.append((name, *rms))
    return sorted(out, key=lambda t: t[1])


def build_unmixed(build: Build) -> Path:
    """A profile from the datasheet's curves, taken as the integral densities they are.

    The curves are placed on agx's exposure axis, given (for a slide) the
    donor's channel balance at mid-grey, and turned into dye amounts with the
    donor's dye spectra and base — the form agx's own profiles are stored in.
    Read back through the densitometer the sheet names, the profile's neutral
    is the sheet's curves.
    """
    donor = load_profile(build.donor)
    d = donor.data
    grid = np.asarray(d.log_exposure, float)
    wl = np.asarray(d.wavelengths, float)
    cd = np.nan_to_num(np.asarray(d.channel_density, float))
    bd = np.nan_to_num(np.asarray(d.base_density, float))
    positive = donor.info.type == "positive"
    # Read only where the dye spectra are defined, as the engine views a slide:
    # Kodachrome's stop at 695 nm, and a densitometer counting the red beyond
    # as clear could never reach the sheet's D-max.
    # (`bake_packs.spectral_valid_mask`: finite dye and base densities).
    defined = (np.isfinite(np.asarray(d.channel_density, float)).all(axis=1)
               & np.isfinite(np.asarray(d.base_density, float)))
    resp = responsivities(donor.info.densitometer, wl) * defined[:, None]
    base = status_density(bd[None, :], resp)[0]
    donor_net = status_density(np.asarray(d.density_curves, float) @ cd.T + bd[None, :], resp) - base
    donor_cd = np.asarray(d.channel_density, float)
    measured = {}
    if build.sheet_base or build.fit_dyes:
        # What the sheet measures in place of the donor's: its D-min as the
        # base, the donor's dyes moved until they add up to its neutral. Both
        # are read where each is defined.
        import sheet_readers
        sheet = sheet_readers.neutral_spectra(build.stock)
        stored_cd, stored_bd = donor_cd, np.asarray(d.base_density, float)
        if build.fit_dyes:
            stored_cd, measured["dye fit"] = neutral_dyes(build.stock, wl, donor_cd)
        if build.sheet_base:
            stored_bd = np.interp(wl, sheet["min"][:, 0], sheet["min"][:, 1], left=np.nan, right=np.nan)
            measured["neutral"] = np.interp(wl, sheet["mid"][:, 0], sheet["mid"][:, 1], left=np.nan, right=np.nan)
        defined = np.isfinite(stored_cd).all(axis=1) & np.isfinite(stored_bd)
        resp = responsivities(donor.info.densitometer, wl) * defined[:, None]
        cd = np.nan_to_num(stored_cd)
        if build.fit_dyes:
            cd = scaled_like(cd, np.nan_to_num(donor_cd), resp)
        bd = np.where(defined, np.nan_to_num(stored_bd), 0.0)
        base = status_density(bd[None, :], resp)[0]
        measured["dyes"] = np.where(defined[:, None], cd, np.nan)
        measured["base"] = np.where(defined, bd, np.nan)
    sourced = {}
    if build.source:
        import sheet_readers
        sourced = sheet_readers.SOURCES[build.stock]()
    if build.dyes is not None or build.dyes_raster is not None or "dyes" in sourced:
        valid = ~np.isnan(donor_cd).any(axis=1)
        traced_dyes = (sourced["dyes"] if "dyes" in sourced else
                       raster_spectra(SHEETS / build.pdf, build.page, build.dyes_raster)
                       if build.dyes_raster is not None else
                       vector_plots.by_position(build.dyes, ("Y", "M", "C"), drop_neutral=build.dyes_with_neutral))
        cd = own_dyes(traced_dyes, wl, valid, cd, resp)

    (x_lo, x_hi), curves = sheet_curves(build)
    sheet_grid = np.linspace(x_lo, x_hi, 1024)
    sheet_dc = np.stack([ds.resample(np.asarray(c), sheet_grid) for c in curves], axis=-1)
    sheet_dc = condition(sheet_grid, sheet_dc, positive,
                         smooth=9 if build.raster is not None else 1,
                         trim=0.02 if build.raster is not None else 0.0)
    sheet_net = sheet_dc - sheet_dc.min(axis=0, keepdims=True)
    if build.unmasked_base:
        # No orange mask: the base is the film's fog and stain, its dyes in the
        # amounts that read the curves' own D-min.
        defined = np.isfinite(donor_cd).all(axis=1)
        resp = responsivities(donor.info.densitometer, wl) * defined[:, None]
        fog, _ = unmix(sheet_dc.min(axis=0)[None, :], cd, np.zeros_like(bd), resp)
        bd = fog[0] @ cd.T
        base = status_density(bd[None, :], resp)[0]
        measured["dyes"] = np.where(defined[:, None], cd, np.nan)
        measured["base"] = np.where(defined, bd, np.nan)
        measured["fog"] = base

    if build.stop_axis:
        offset = 0.0
        placement = (f"Mid-grey at the sheet's camera stop 0, the normal exposure at EI {build.iso}.")
    elif build.normal_above_speed is not None:
        point = speed_point(sheet_grid, sheet_net[:, 1:2], above_min=0.1)
        offset = -(point + build.normal_above_speed)
        placement = (f"Mid-grey {build.normal_above_speed:.2f} decades above the green layer's D-min + 0.1 speed "
                     f"point (log H {point:.3f} on the sheet's axis).")
    elif build.relative_axis:
        point = iso_speed_point(sheet_grid, sheet_net)
        ei = build.metered_ei or build.iso
        above = float(np.log10(0.65 * 12.5 * build.iso / (np.sqrt(2) * ei)))
        offset = -(point + above)
        placement = (f"The sheet plots relative log exposure, and its ISO {build.iso} was measured by the ISO "
                     f"method: the curves' ISO 5800 speed point (0.15 over D-min, green and slowest layer) is "
                     f"taken as sqrt(2) / {build.iso} lux-seconds, and mid-grey goes where a meter set to EI "
                     f"{ei} puts it, {above:.3f} decades above.")
    elif build.metered_ei is not None:
        metered = float(np.log10(0.65 * 12.5 / build.metered_ei))
        offset = -metered
        placement = (f"Mid-grey where a meter set to EI {build.metered_ei} puts it on the sheet's absolute "
                     f"axis (log H {metered:.3f} lux-seconds).")
    else:
        offset = speed_point(grid, donor_net) - speed_point(sheet_grid, sheet_net)
        placement = (f"Exposure axis aligned to '{build.donor}' at the D-min + 1.0 speed point "
                     f"(offset {offset:+.3f} decades).")
    shifted = sheet_grid + offset
    if build.layer_gammas is not None:
        # Each layer's contrast about mid-grey (log exposure 0 on this axis).
        sheet_net = np.stack([np.interp(shifted, shifted / build.layer_gammas[c], sheet_net[:, c])
                              for c in range(3)], axis=-1)
        placement += (" Layer contrast about mid-grey scaled by " + ", ".join(
            f"{n} {v:.3f}" for n, v in zip("RGB", build.layer_gammas)) + ".")
    if build.layer_shifts is not None:
        # Each layer's speed, the one thing a trace is not trusted for, moved
        # so a grey prints grey; the curves themselves stay as drawn.
        sheet_net = np.stack([np.interp(sheet_grid, sheet_grid + build.layer_shifts[c], sheet_net[:, c])
                              for c in range(3)], axis=-1)
        placement += (" Layer speeds moved by " + ", ".join(f"{n} {v:+.3f}" for n, v in zip("RGB", build.layer_shifts))
                      + " decades so a grey prints grey; the curves are otherwise as drawn.")
    if positive and build.layer_shifts is None and not build.neutral_greys:
        # The per-layer speed balance is the part of a trace that cannot be
        # trusted (see align_channel_balance); a slide takes its donor's, read
        # the same way, so its greys come out as neutral as the donor's.
        sheet_net = align_channel_balance(shifted, sheet_net, grid, donor_net)
        placement += f" Each layer's density at mid-grey matched to '{build.donor}'."

    target = np.stack([np.interp(grid, shifted, sheet_net[:, c]) for c in range(3)], axis=-1)
    amounts, err = unmix(target, cd, bd, resp)
    if positive and build.neutral_greys:
        # Each sheet says its greys are neutral; at the speeds a trace gives them
        # they view a few a* or b* off. The red and blue layers' speeds are set
        # so a grey views neutral, the curves otherwise as drawn — solved on the
        # dye amounts, applied to the sheet's curves, and solved again.
        viewing = donor.info.viewing_illuminant
        total = np.zeros(3)
        drawn = sheet_net
        for _ in range(4):
            step = neutral_layer_shifts(grid, amounts, cd, bd, defined, viewing)
            total += step
            sheet_net = np.stack([np.interp(sheet_grid, sheet_grid + total[c], drawn[:, c]) for c in range(3)],
                                 axis=-1)
            target = np.stack([np.interp(grid, shifted, sheet_net[:, c]) for c in range(3)], axis=-1)
            amounts, err = unmix(target, cd, bd, resp)
            if np.abs(step).max() < 0.002:
                break
        placement += (f" Layer speeds set so a grey views neutral from three stops under mid-grey to two over: "
                      f"red {total[0]:+.3f}, blue {total[2]:+.3f} decades against green; the curves are "
                      f"otherwise as drawn.")
    centers, amplitudes, sigmas = fit_cdf_model(grid, amounts, positive)
    fitted = np.zeros_like(amounts)
    sign = -1.0 if positive else 1.0
    for c in range(3):
        for l in range(centers.shape[1]):
            fitted[:, c] += amplitudes[c, l] * norm.cdf(sign * (grid - centers[c, l]) / max(sigmas[c, l], 1e-3))
    rms = float(np.sqrt(np.mean((fitted - amounts) ** 2)))
    readback = status_density(fitted @ cd.T + bd[None, :], resp) - base
    worst = float(np.abs(readback - target).max())

    profile = donor.clone()
    profile.update_info(stock=build.stock, name=build.name, target_print=build.paper or donor.info.target_print)
    profile.update_data(log_exposure=grid, density_curves=fitted,
                        density_curves_layers=layer_curves(grid, centers, amplitudes, sigmas, positive))
    own = []
    if build.dyes is not None or build.dyes_raster is not None or "dyes" in sourced:
        profile.update_data(channel_density=np.where(np.isnan(donor_cd), np.nan, cd))
        own.append("dye spectra")
    if measured:
        profile.update_data(channel_density=measured["dyes"], base_density=measured["base"])
        if "neutral" in measured:
            profile.update_data(midscale_neutral_density=measured["neutral"])
    if build.antihalation is not None:
        profile.update_info(antihalation=build.antihalation)
    if build.no_window:
        profile.update_data(hanatos2025_adaptation_window_params=np.zeros(0))
    if build.sensitivity is not None or build.sensitivity_raster is not None or "sensitivity" in sourced:
        traced = (sourced["sensitivity"] if "sensitivity" in sourced else
                  raster_spectra(SHEETS / build.pdf, build.page, build.sensitivity_raster)
                  if build.sensitivity_raster is not None else
                  vector_plots.by_colour(build.sensitivity, dict(zip("RGB", build.colours)))
                  if build.colours is not None else
                  vector_plots.by_position(build.sensitivity, ("B", "G", "R")))
        profile.update_data(log_sensitivity=linear_log_sensitivity(wl, traced) if build.sensitivity_linear
                            else own_log_sensitivity(np.asarray(d.log_sensitivity, float), wl, traced))
        balance(profile)
        own.insert(0, "spectral sensitivities")
    profile.data.density_curves_model.centers = centers
    profile.data.density_curves_model.amplitudes = amplitudes
    profile.data.density_curves_model.sigmas = sigmas
    import sheet_readers
    how = (sheet_readers.CURVES_READ[build.stock] if build.source and build.stock in sheet_readers.CURVES_READ
           else "read from the vector plot, each layer by its colour" if build.vector_curves is not None
           else "read from the vector plot, each layer by its dash pattern" if build.dashes is not None
           else "traced off the sheet's bitmap" if build.raster is not None
           else "read from the vector plot, each layer by where it lies")
    inherited = [n for n in ("spectral sensitivities", "dye spectra") if n not in own]
    inherited += ["base density", "Hanatos-2025 adaptation parameters", "DIR couplers"]
    densitometer = donor.info.densitometer.replace('_', ' ').title()
    if measured or build.no_window:
        sensitivities = ("" if "spectral sensitivities" not in own else
                         " The sheet plots relative sensitivity on a linear scale, each layer falling to zero at both "
                         "ends: taken as plotted, with none beyond, and balanced so an 18% grey exposes every layer "
                         "equally." if build.sensitivity_linear else
                         " The sheet's sensitivity shapes continue on the donor's tails beyond the plotted range and "
                         "are balanced so an 18% grey exposes every layer equally.")
        base_note = (" The base density is the sheet's own D-min spectral density, base and fog, and the profile "
                     "keeps its midscale neutral." if build.sheet_base else
                     " The film has no orange mask: its base is its fog and stain, the dyes in the amounts that read "
                     "the curves' own D-min (" + ", ".join(f"{n} {v:.2f}" for n, v in zip("RGB", measured["fog"]))
                     + ")." if build.unmasked_base else "")
        dyes_note = ""
        if build.fit_dyes:
            q = measured["dye fit"]
            dyes_note = (f" The sheet plots no single-dye spectra: the dyes are those of the agx reference profile "
                         f"'{build.donor}', of agx's colour negatives the best fit to the sheet's neutral over D-min, "
                         f"each moved and stretched about its peak until the three add up to it: cyan {q[3]:+.1f} nm "
                         f"x{q[6]:.3f}, magenta {q[4]:+.1f} nm x{q[7]:.3f}, yellow {q[5]:+.1f} nm x{q[8]:.3f}.")
        window_note = (" No spectral adaptation window: agx fits one to each of its own stocks' sensitivities."
                       if build.no_window else "")
        borrowed = ([] if build.fit_dyes or "dye spectra" in own else ["dye spectra"]) \
            + ([] if build.sheet_base or build.unmasked_base else ["base density"]) \
            + ([] if build.no_window else ["Hanatos-2025 adaptation parameters"]) + ["DIR coupler model"]
        profile.metadata.datasource = (
            f"Characteristic curves{''.join(', ' + n for n in own)} from {build.notes} The curves were {how}, "
            f"and converted from {densitometer} densities into dye amounts. {placement}{sensitivities}{base_note}"
            f"{dyes_note}{window_note} {', '.join(borrowed)[0].upper() + ', '.join(borrowed)[1:]} from "
            f"'{build.donor}'."
        )
    else:
        profile.metadata.datasource = (
            f"Characteristic curves{''.join(', ' + n for n in own)} from {build.notes} The curves were {how}, "
            f"and converted from {densitometer} densities into dye amounts "
            f"with the {'sheet' if 'dye spectra' in own else 'donor'}'s dye spectra and the base density of the "
            f"agx reference profile '{build.donor}'. {placement}"
            + (" The sheet's sensitivity shapes continue on the donor's tails beyond the plotted range and are "
               "balanced so an 18% grey exposes every layer equally." if "spectral sensitivities" in own else "")
            + f" {', '.join(inherited)[0].upper() + ', '.join(inherited)[1:]} inherited from '{build.donor}', which shares this stock's "
            f"dye set and process."
        )
    profile.metadata.created = build.created

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUT_DIR / f"{build.stock}.json"
    path.write_text(json.dumps(_json_safe(profile_to_dict(profile)), indent=4, allow_nan=False))
    print(f"  {build.stock:26s} ISO {build.iso:<4d} D-max (amounts) {np.round(fitted.max(axis=0), 2).tolist()}"
          f"  unmix {err:.1e}  fit RMS {rms:.4f}  readback vs sheet worst {worst:.4f}")
    return path


def build_one(build: Build) -> Path:
    donor = load_profile(build.donor)
    grid = np.asarray(donor.data.log_exposure)

    if build.raster is not None:
        axes, curves = raster_curves(SHEETS / build.pdf, build.page, build.raster)
    else:
        axes, curves = characteristic_curves(SHEETS / build.pdf, build.page)
    sheet_grid = np.linspace(axes.x_min, axes.x_max, 1024)
    sheet_dc = np.stack([ds.resample(c, sheet_grid) for c in curves], axis=-1)

    positive_stock = donor.info.type == "positive"
    sheet_dc = condition(sheet_grid, sheet_dc, positive_stock,
                         smooth=9 if build.raster is not None else 1,
                         trim=0.02 if build.raster is not None else 0.0)

    # agx stores curves with the base subtracted; the spectral base density is
    # carried separately (and inherited from the donor here).
    sheet_dc = sheet_dc - sheet_dc.min(axis=0, keepdims=True)

    offset = speed_offset(build, donor, grid)
    shifted = sheet_grid + offset

    donor_le = np.asarray(donor.data.log_exposure)
    donor_dc = np.asarray(donor.data.density_curves)
    sheet_dc = align_channel_balance(shifted, sheet_dc, donor_le, donor_dc)

    # Resample onto agx's grid, holding the endpoint values outside the
    # datasheet's plotted range — the same clamping agx's own interpolation uses.
    density = np.stack([np.interp(grid, shifted, sheet_dc[:, c]) for c in range(3)], axis=-1)

    positive = donor.info.type == "positive"
    centers, amplitudes, sigmas = fit_cdf_model(grid, density, positive)
    fitted = np.zeros_like(density)
    sign = -1.0 if positive else 1.0
    for c in range(3):
        for l in range(centers.shape[1]):
            fitted[:, c] += amplitudes[c, l] * norm.cdf(
                sign * (grid - centers[c, l]) / max(sigmas[c, l], 1e-3))
    rms = float(np.sqrt(np.mean((fitted - density) ** 2)))

    # Ship the *fitted* curves, not the conditioned trace.
    #
    # The isotonic step that makes a traced bitmap monotonic does it by pooling
    # violators into flat runs, and a flat run in a characteristic curve is a
    # tone that cannot change — which is exactly what produced the posterised
    # patches on Velvia 50. The three-CDF fit is smooth by construction and
    # tracks the trace to well under a hundredth of a density.
    density = fitted

    profile = donor.clone()
    profile.update_info(
        stock=build.stock,
        name=build.name,
        target_print=donor.info.target_print,
    )
    profile.update_data(
        log_exposure=grid,
        density_curves=density,
        density_curves_layers=layer_curves(grid, centers, amplitudes, sigmas, positive),
    )
    profile.data.density_curves_model.centers = centers
    profile.data.density_curves_model.amplitudes = amplitudes
    profile.data.density_curves_model.sigmas = sigmas
    profile.metadata.datasource = (
        f"Characteristic curves extracted from the vector plots of {build.notes} "
        f"Exposure axis aligned to the agx reference profile '{build.donor}' at the "
        f"D-min + 1.0 speed point (offset {offset:+.3f} decades). "
        f"Spectral sensitivities, dye densities, base density and Hanatos-2025 "
        f"adaptation parameters inherited from '{build.donor}', which shares this "
        f"stock's dye set and process."
    )
    profile.metadata.created = "2026-08-10"

    # `save_profile` only ever writes inside the installed package; these
    # profiles belong to this repo, so serialise with the same helpers by hand.
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUT_DIR / f"{build.stock}.json"
    path.write_text(json.dumps(_json_safe(profile_to_dict(profile)), indent=4, allow_nan=False))
    dmax = density.max(axis=0)
    print(f"  {build.stock:26s} ISO {build.iso:<4d} offset {offset:+.3f}  "
          f"D-max {np.round(dmax, 2).tolist()}  fit RMS {rms:.4f}")
    return path


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--only", nargs="*", help="build only these stocks")
    ap.add_argument("--donors", metavar="STOCK",
                    help="rank agx's negatives as donors for a sheet that plots neutral spectra, and stop")
    args = ap.parse_args()
    if args.donors:
        print(f"{args.donors}: RMS density of the dyes against the neutral over D-min; and of base + dyes "
              f"against the neutral and D-min")
        for name, dyes, neutral, dmin in donor_fit(args.donors):
            print(f"  {name:22s} dyes {dyes:.3f}   base + dyes: neutral {neutral:.3f}  D-min {dmin:.3f}")
        return 0
    builds = [b for b in BUILDS if not args.only or b.stock in args.only]
    print(f"building {len(builds)} profiles from datasheets -> {OUT_DIR}")
    for build in builds:
        (build_unmixed if build.unmix else build_one)(build)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
