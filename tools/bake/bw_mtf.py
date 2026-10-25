"""The spatial response of the black-and-white stocks, fitted to Kodak's MTF.

A colour negative's sharpness in the engine comes from two of agx's stages:
light scattering in the emulsion blurs, and the DIR couplers' inhibitor,
diffusing before it acts, sharpens edges back — an unsharp mask that lifts
the response above 100% at coarse detail. A black-and-white film has no
couplers, but development does the same: its by-products (bromide) diffuse
and hold back development next to dense areas, the adjacency effect behind
the "Response > 100%" hump of every silver film's published MTF.

The black-and-white packs first took a colour film's scatter and no edge
effect, which left Tri-X at 36% response at 24 cycles/mm where Kodak measures
101%: visibly softer than any colour stock on the same raw file. So both are
fitted here, to the Modulation Transfer Function on page 8 of Kodak's F-4017
(Tri-X 400 in D-76), with the engine's own kernels:

- scatter (`encodeHalation`): a Gaussian core and, for agx's exponential tail
  of decay constant lambda, its dominant Gaussian, sigma = 1.5236 lambda.
  Fitted freely, core and tail come out the same width, so the fit uses the
  core alone: Kodak's curve is one Gaussian of about 4 um under the hump;
- halation: the stock's back-reflection, unchanged;
- adjacency, as one layer's own DIR inhibition (`couplers_amount`,
  `film_develop_couplers`): the first development's density, times g and
  blurred by a Gaussian of the diffusion size, is taken off the log exposure,
  which then develops again on the curve that makes large areas come out as
  the datasheet's. For a small signal on a curve of slope gamma that is a
  response of (1 - b H(f)) / (1 - b), b = g gamma, H the diffusion's transfer.

Ilford publishes no MTF for HP5 Plus; it takes Tri-X's fit, the same class of
emulsion in an equivalent developer (ID-11, D-76's formula).

Run:  python tools/bake/bw_mtf.py   (prints the fit against Kodak's points)
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pymupdf
from scipy.optimize import least_squares

import datasheet as ds

SHEETS = Path(__file__).resolve().parents[2] / "film spec sheets"
TRI_X_SHEET = SHEETS / "Kodak Tri-X 320 and 400 F-4017.pdf"

# The engine's tail Gaussian for an exponential of decay constant lambda
# (`encodeHalation`), and the halation every black-and-white pack keeps.
TAIL_SIGMA_PER_LAMBDA = 1.5236
HALATION_STRENGTH = 0.015
HALATION_SIGMA_UM = 65.0
HALATION_BOUNCES = 3
HALATION_DECAY = 0.5


def kodak_tri_x_mtf() -> np.ndarray:
    """Kodak's Tri-X 400 MTF as (cycles/mm, response as a fraction), read off
    the vector plot. Both axes are logarithmic and calibrated on their labels."""
    page = pymupdf.open(str(TRI_X_SHEET))[7]
    # The plot's own stroked frame, top left of the page (the sheet's other
    # frames are the figure boxes and the spectral sensitivity plot).
    frame = next(d["rect"] for d in page.get_drawings()
                 if d.get("color") is not None and any(i[0] == "re" for i in d["items"])
                 and d["rect"].width > 150 and d["rect"].x1 < 300 and d["rect"].y1 < 250)
    xs, ys = [], []
    for w in page.get_text("words"):
        try:
            value = float(w[4])
        except ValueError:
            continue
        cx, cy = (w[0] + w[2]) / 2, (w[1] + w[3]) / 2
        if frame.y1 < cy < frame.y1 + 14 and frame.x0 - 6 < cx < frame.x1 + 8:
            xs.append((cx, np.log10(value)))         # spatial frequency, below the plot
        elif frame.x0 - 18 < cx < frame.x0 and frame.y0 - 6 < cy < frame.y1 + 6:
            ys.append((cy, np.log10(value)))         # response %, left of it
    kx, bx = np.polyfit(*zip(*xs), 1)
    ky, by = np.polyfit(*zip(*ys), 1)

    points = []
    for d in page.get_drawings():
        r = d["rect"]
        segments = [i for i in d["items"] if i[0] in ("l", "c")]
        if not (segments and r.width > 20 and r.height > 10 and frame.contains(r)):
            continue
        poly = ds._flatten_path(segments)
        points.append(np.stack([10 ** (kx * poly[:, 0] + bx), 10 ** (ky * poly[:, 1] + by) / 100], axis=-1))
    if len(points) != 1:
        raise ValueError(f"expected one MTF curve on {TRI_X_SHEET.name}, found {len(points)}")
    curve = points[0][np.argsort(points[0][:, 0])]
    return curve[np.concatenate([[True], np.diff(curve[:, 0]) > 1e-6])]


def agfa_apx25_mtf() -> np.ndarray:
    """Agfa's APX 25 MTF ("visual filter") as (cycles/mm, fraction), off page
    10 of its range sheet: a vector plot, both axes logarithmic and labelled
    in text on their rules."""
    from vector_plots import VectorPlot, by_position
    plot = VectorPlot(str(SHEETS / "agfa_films_data.pdf"), 9, (53.4, 339.8, 179.8, 425.4),
                      tuple((x, np.log10(v)) for x, v in ((53.4, 2), (67.5, 3), (82.3, 5), (105.9, 10), (127.3, 20),
                                                          (141.1, 30), (157.3, 50), (179.8, 100))),
                      tuple((y, np.log10(v)) for y, v in ((339.8, 150), (352.8, 100), (374.2, 50), (391.0, 30),
                                                          (404.2, 20), (425.4, 10))))
    data = by_position(plot, ("mtf",))["mtf"]
    curve = 10 ** data[np.argsort(data[:, 0])]
    curve[:, 1] /= 100
    return curve[np.concatenate([[True], np.diff(curve[:, 0]) > 1e-6])]


def kodak_t_max_100_mtf() -> np.ndarray:
    """Kodak's T-MAX 100 MTF (F-4016 page 8, D-76, small tank) as (cycles/mm,
    fraction). A vector plot, both axes logarithmic; its 100% and 200% labels
    are set larger than the rest, so it is calibrated on its gridlines, which
    sit up to a point off a true logarithmic scale, fitted in least squares."""
    from vector_plots import VectorPlot, by_position
    plot = VectorPlot(str(SHEETS / "f4016_tmax_100-2018.pdf"), 7, (71.2, 260.1, 273.7, 413.1),
                      tuple((x, np.log10(v)) for x, v in ((92.8, 2), (105.5, 3), (114.8, 4), (121.5, 5), (142.9, 10),
                                                          (165.5, 20), (194.4, 50), (216.1, 100), (237.5, 200))),
                      tuple((y, np.log10(v)) for y, v in ((260.1, 200), (279.4, 100), (289.9, 70), (299.6, 50),
                                                          (314.3, 30), (326.1, 20), (346.2, 10), (356.5, 7),
                                                          (366.7, 5), (380.9, 3), (393.5, 2), (413.1, 1))))
    data = by_position(plot, ("mtf",))["mtf"]
    curve = 10 ** data[np.argsort(data[:, 0])]
    curve[:, 1] /= 100
    return curve[np.concatenate([[True], np.diff(curve[:, 0]) > 1e-6])]


# A stock whose own sheet publishes its MTF is fitted to that, up to 50
# cycles/mm (finer, a 6000 px frame stops resolving the scatter's core: fitted
# to Agfa's 91 the worst error was 17%, to 50 it is 3%); the others take
# Kodak's Tri-X curve.
SHEET_MTF = {"agfa_apx_25": lambda: (lambda c: c[c[:, 0] <= 50])(agfa_apx25_mtf()),
             "kodak_t_max_100": lambda: (lambda c: c[c[:, 0] <= 50])(kodak_t_max_100_mtf())}


def _gauss(sigma_um, f_cmm):
    return np.exp(-2 * np.pi ** 2 * (np.asarray(sigma_um) * 1e-3) ** 2 * np.asarray(f_cmm) ** 2)


def engine_mtf(f, core_um, tail_um, tail_weight, b, diffusion_um):
    """The engine's small-signal response at f cycles/mm."""
    scatter = (1 - tail_weight) * _gauss(core_um, f) + tail_weight * _gauss(TAIL_SIGMA_PER_LAMBDA * tail_um, f)
    decay = HALATION_DECAY ** np.arange(HALATION_BOUNCES)
    decay /= decay.sum()
    halo = sum(w * _gauss(HALATION_SIGMA_UM * np.sqrt(k + 1), f) for k, w in enumerate(decay))
    halation = (1 + HALATION_STRENGTH * halo) / (1 + HALATION_STRENGTH)
    adjacency = (1 - b * _gauss(diffusion_um, f)) / (1 - b)
    return scatter * halation * adjacency


def fit(data: np.ndarray | None = None) -> dict:
    """Scatter and adjacency that reproduce a measured MTF in the engine:
    `data` as (cycles/mm, fraction), Kodak's Tri-X curve by default."""
    data = kodak_tri_x_mtf() if data is None else data
    f, response = data[:, 0], data[:, 1]

    def residual(p):
        core, b, diffusion = p
        return np.log(engine_mtf(f, core, 0.0, 0.0, b, diffusion)) - np.log(response)

    p = least_squares(residual, [2.0, 0.15, 60.0], bounds=([0.3, 0.0, 10.0], [8.0, 0.5, 250.0])).x
    core, b, diffusion = (float(v) for v in p)
    return {"scatterCoreUm": core, "scatterTailUm": 0.0, "scatterTailWeight": 0.0,
            "inhibition": b, "diffusionSizeUm": diffusion,
            "worstLogError": float(np.max(np.abs(residual(p)))), "points": data}


def inhibition_gamma(inhibition: float, curve_slope: float) -> float:
    """The same-layer coupler gamma g that takes off b = g gamma of a small
    signal at the fitted frequency scale, on a curve of slope gamma (density
    per decade of exposure, at mid-grey)."""
    return inhibition / curve_slope


if __name__ == "__main__":
    result = fit()
    print("fitted to Kodak's Tri-X 400 MTF (D-76):")
    for key in ("scatterCoreUm", "scatterTailUm", "scatterTailWeight", "inhibition", "diffusionSizeUm"):
        print(f"  {key:18} {result[key]:.4f}")
    print(f"  worst error        {100 * (np.exp(result['worstLogError']) - 1):.1f}% of the response")
    params = [result[k] for k in ("scatterCoreUm", "scatterTailUm", "scatterTailWeight",
                                  "inhibition", "diffusionSizeUm")]
    print("\n  c/mm   Kodak  fitted")
    for f, r in result["points"][::2]:
        print(f"  {f:5.1f}  {100 * r:5.1f}  {100 * engine_mtf(f, *params):5.1f}")
