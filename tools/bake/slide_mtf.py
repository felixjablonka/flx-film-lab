"""Each slide film's sharpness, from its own datasheet's MTF.

Sharpness in the engine comes from two of agx's stages (`bw_mtf.py` works
through both): light scattering in the emulsion blurs, and the DIR couplers'
inhibitor, diffusing before it acts, sharpens back. How hard it sharpens is
set by the share b of a layer's own contrast that the spreading inhibition
takes away — the coupler matrix times the slopes of the curves — since detail
finer than the spread escapes it and keeps up to 1/(1 - b) times the contrast
of a large area.

Each sheet's MTF, read over its whole range, fixes that film's own blur (the
scatter's core, its tail and the tail's weight), the inhibitor's reach and how
much of the inhibition spreads that far; started anywhere, the fit lands on
the same values. The inhibition as a whole is the film's inter-image effect,
its colour, which no sheet gives as a number; it is set from the sheet's own
words (`stock_overrides.COLOUR`), and what is fitted here is the share of it
that spreads. The rest acts where it was released (`diffusionFraction`). The
fit is read one stop under mid-grey, about the gross density of 1.0 at which
the sheets read granularity; agx's edge effect keeps growing as a slide's
curve steepens into the shadows, so the engine holds it at the fitted level
above that (`edgeGainReference`, FilmMath.edgeGainScale).

The model below is the engine's small-signal response, and agrees with a
sinusoid rendered through it to a point up to 20 cycles/mm; finer than about
40 cycles/mm a 6000 px frame stops resolving the scatter's core, which is the
film's whatever the frame.

Run:  python tools/bake/slide_mtf.py   (fits every slide and prints it against its sheet)
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pymupdf
from scipy.optimize import least_squares

from bw_mtf import TAIL_SIGMA_PER_LAMBDA, _gauss
from datasheet import _flatten_path
from vector_plots import VectorPlot, _curve_paths, _to_data, by_position

SHEETS = Path(__file__).resolve().parents[2] / "film spec sheets"
REFERENCE_LOG_EXPOSURE = float(np.log10(0.5))     # one stop under mid-grey
FIT_RANGE = (1.0, 25.0)                            # cycles/mm; finer, a 6000 px frame stops resolving the scatter

_L = lambda v: float(np.log10(v))                  # noqa: E731


def _log_plot(pdf: str, page: int, frame, x_rules, y_rules) -> VectorPlot:
    """A plot with both axes logarithmic: rules against log10 of their values."""
    return VectorPlot(str(SHEETS / pdf), page, frame,
                      tuple((p, _L(v)) for p, v in x_rules), tuple((p, _L(v)) for p, v in y_rules))


def _as_curve(data: np.ndarray) -> np.ndarray:
    """(log f, log %) points into (cycles/mm, fraction), sorted and deduplicated."""
    c = 10 ** data[np.argsort(data[:, 0])]
    c[:, 1] /= 100
    return c[np.concatenate([[True], np.diff(c[:, 0]) > 1e-6])]


# Fujifilm, vector. The Velvia 100 sheet draws its labels as outlines, so its
# rules are named by the grid they make: 5-100 cycles/mm, 3-100%.
PROVIA_400X = _log_plot("Fujichrome-Provia-400X-Professional-RXP_AF3-0213E.pdf", 6, (83.3, 399.9, 258.9, 547.8),
                        ((83.3, 1), (106.9, 2), (137.9, 5), (160.94, 10), (183.71, 20), (214.16, 50),
                         (236.7, 100), (258.92, 200)),
                        ((399.86, 150), (414.51, 100), (426.56, 70), (437.85, 50), (455.42, 30),
                         (468.47, 20), (493.13, 10), (505.17, 7), (516.37, 5), (533.78, 3), (547.82, 2)))
ASTIA_100F = _log_plot("astia_100f_datasheet.pdf", 7, (89.3, 473.4, 270.3, 620.4),
                       ((89.3, 1), (144.8, 5), (168.09, 10), (192.35, 20), (222.61, 50), (246.15, 100),
                        (270.3, 200)),
                       ((473.4, 150), (487.87, 100), (499.66, 70), (511.41, 50), (528.16, 30), (542.38, 20),
                        (566.42, 10), (578.17, 7), (590.18, 5), (606.43, 3), (620.4, 2)))
VELVIA_100 = _log_plot("films_velvia-100_datasheet_01.pdf", 7, (85.8, 447.8, 281.3, 595.2),
                       ((85.82, 1), (145.76, 5), (170.91, 10), (197.1, 20), (229.78, 50), (255.2, 100),
                        (281.05, 200)),
                       ((447.8, 150), (462.39, 100), (474.2, 70), (485.98, 50), (502.77, 30), (517.02, 20),
                        (541.11, 10), (552.89, 7), (564.92, 5), (581.21, 3), (595.22, 2)))
# Kodak, vector: E100's R, G and B; E-55's one curve per Kodachrome, on rules
# read off each page's gridlines and labels (its "150" label sits off its
# rule, so the top is left out).
EKTACHROME_100 = _log_plot("e4000_ektachrome_100.pdf", 2, (357.6, 126.7, 503.4, 279.7),
                           ((357.6, 1), (379.6, 2), (392.4, 3), (401.5, 4), (408.6, 5), (430.5, 10),
                            (452.4, 20), (481.5, 50), (503.4, 100)),
                           ((127.5, 200), (146.9, 100), (157.9, 70), (167.3, 50), (181.7, 30), (193.0, 20),
                            (212.1, 10), (223.7, 7), (234.1, 5), (248.2, 3), (259.5, 2), (279.2, 1)))


def _kodachrome(page: int, left: float, top: float) -> VectorPlot:
    """E-55's MTF plot on a film's page: its frame's left edge and top."""
    x = ((0.0, 1), (21.7, 2), (34.4, 3), (43.7, 4), (50.3, 5), (71.7, 10), (94.3, 20), (123.3, 50),
         (145.0, 100), (166.4, 200))
    y = ((19.0, 100), (30.2, 70), (39.7, 50), (54.3, 30), (66.3, 20), (85.8, 10), (96.8, 7), (107.3, 5),
         (121.6, 3), (133.4, 2), (152.7, 1))
    return _log_plot("Kodachrome Spec Sheet.pdf", page, (left, top, left + 195.3, top + 153.0),
                     tuple((left + dx, v) for dx, v in x), tuple((top + dy, v) for dy, v in y))


KODACHROME = {"kodak_kodachrome_25": _kodachrome(2, 368.2, 399.6),
              "kodak_kodachrome_64": _kodachrome(3, 350.6, 385.6),
              "kodak_kodachrome_200": _kodachrome(4, 362.6, 385.6)}


def _single(plot: VectorPlot) -> np.ndarray:
    return _as_curve(by_position(plot, ("mtf",))["mtf"])


def ektachrome_100_green() -> np.ndarray:
    """E100's green-layer curve: the middle one of three at fine detail."""
    curves = [_to_data(EKTACHROME_100, _flatten_path(items, steps=24)) for _, items in _curve_paths(EKTACHROME_100)]
    if len(curves) != 3:
        raise ValueError(f"E-4000 MTF: {len(curves)} curves, expected 3")
    at = lambda c: np.interp(_L(40), np.sort(c[:, 0]), c[np.argsort(c[:, 0]), 1])   # noqa: E731
    return _as_curve(sorted(curves, key=at)[1])


def _bitmap(pdf: str, page: int, image: int, x_values, y_values, above: float = 20.0) -> np.ndarray:
    """A curve traced off an embedded bitmap MTF plot, its rules named in order
    (`None` for one that does not sit on the plot's own log scale). Only the
    plot above `above` percent is read: the sheets letter their exposure and
    process notes into its lower left corner, below every curve."""
    import trace_raster_plot as raster
    document = pymupdf.open(str(SHEETS / pdf))
    pixmap = pymupdf.Pixmap(document, document[page].get_images(full=True)[image][0])
    if pixmap.n > 4:
        pixmap = pymupdf.Pixmap(pymupdf.csRGB, pixmap)
    tmp = Path("/tmp") / f"_flx_mtf_{Path(pdf).stem}_{page}_{image}.png"
    pixmap.save(str(tmp))
    mask = raster.load_mask(str(tmp))
    rows, cols = raster.grid_lines(mask)
    if len(rows) != len(y_values) or len(cols) != len(x_values):
        raise ValueError(f"{pdf}: {len(cols)} x {len(rows)} rules, expected {len(x_values)} x {len(y_values)}")
    fit = lambda pos, vals: np.polyfit([p for p, v in zip(pos, vals) if v], [_L(v) for v in vals if v], 1)  # noqa: E731
    kx, ky = fit(cols, x_values), fit(rows, y_values)
    frame = raster.find_frame(mask, x_min=0, x_max=1, y_at_top=0, y_at_bottom=1)
    floor = int((_L(above) - ky[1]) / ky[0])           # the row of `above` percent
    xs, traces = raster.trace(mask, frame, max_curves=1,
                              exclude=[(0, floor, mask.shape[1], mask.shape[0])])
    px = frame.left + xs * (frame.right - frame.left)
    py = frame.top + traces[0] * (frame.bottom - frame.top)
    return _as_curve(np.stack([np.polyval(kx, px), np.polyval(ky, py)], axis=-1))


def velvia_50() -> np.ndarray:
    return _bitmap("VELVIA 50 Spec Sheet.pdf", 7, 0, (1, 5, 10, 20, 50, 100, 200),
                   (150, 100, 70, 50, None, 20, 10, 7, 5, None, None))


def provia_100f() -> np.ndarray:
    return _bitmap("films_provia-100f_datasheet_01.pdf", 5, 4, (1, 5, 10, 20, 50, 100, 200),
                   (150, 100, 70, 50, 30, 20, 10, 7, 5, 3, 2))


SHEET_MTF = {
    "fujifilm_provia_100f": provia_100f,
    "fujifilm_provia_400x": lambda: _single(PROVIA_400X),
    "fujifilm_astia_100f": lambda: _single(ASTIA_100F),
    "fujifilm_velvia_50": velvia_50,
    "fujifilm_velvia_100": lambda: _single(VELVIA_100),
    "kodak_ektachrome_100": ektachrome_100_green,
    **{stock: (lambda plot=plot: _single(plot)) for stock, plot in KODACHROME.items()},
}


def _matrix(dc) -> np.ndarray:
    """agx `compute_dir_couplers_matrix`, donor row, receiver column."""
    m = np.diag(np.asarray(dc.gamma_samelayer_rgb, float)) * dc.inhibition_samelayer
    inter = np.zeros((3, 3))
    inter[0, 1], inter[0, 2] = dc.gamma_interlayer_r_to_gb
    inter[1, 0], inter[1, 2] = dc.gamma_interlayer_g_to_rb
    inter[2, 0], inter[2, 1] = dc.gamma_interlayer_b_to_rg
    return (m + inter * dc.inhibition_interlayer) * dc.amount


def engine_response(f, log_exposure, curves, matrix, halation, *, fraction, reach, core, tail, weights,
                    x_ref=REFERENCE_LOG_EXPOSURE, layer=1) -> np.ndarray:
    """The engine's small-signal response of `layer` at log exposure `x_ref`.

    Scatter (`encodeHalation`: a Gaussian core, the tail's dominant Gaussian at
    1.5236 lambda, each layer's own weight) and renormalised back-reflection
    blur each layer's exposure; the inhibitor, `matrix` times each donor's
    slope, acts where it was released but for `fraction` of it, which first
    spreads by a Gaussian of `reach` um.
    """
    f = np.asarray(f, float)
    x = np.asarray(log_exposure, float)
    d = np.asarray(curves, float)
    slope = np.array([abs(np.interp(x_ref, x, np.gradient(d[:, c], x))) for c in range(3)])
    decay = halation.halation_bounce_decay ** np.arange(halation.halation_n_bounces)
    decay = decay / decay.sum()
    sigma_h = float(np.mean(halation.halation_first_sigma_um))
    halo = sum(wk * _gauss(sigma_h * np.sqrt(k + 1), f) for k, wk in enumerate(decay))
    transfer = []
    for c in range(3):
        w = float(np.clip(weights[c], 0, 1))
        scatter = (1 - w) * _gauss(core, f) + w * _gauss(TAIL_SIGMA_PER_LAMBDA * tail, f)
        h = float(halation.halation_strength[c])
        transfer.append(scatter * (1 + h * halo) / (1 + h))
    spread = (1 - fraction) + fraction * _gauss(reach, f)
    b = sum(matrix[c, layer] * slope[c] for c in range(3))
    return (transfer[layer] - spread * sum(matrix[c, layer] * slope[c] * transfer[c] for c in range(3))) / (1 - b)


def fit(stock: str, log_exposure, curves, matrix, halation) -> dict:
    """The film's own blur, inhibitor reach and spreading share, from its
    sheet's whole MTF; `matrix` is its inhibition as a whole (its colour)."""
    data = SHEET_MTF[stock]()
    f, response = data[:, 0], data[:, 1]
    relative = np.asarray(halation.scatter_tail_weight, float) / float(np.mean(halation.scatter_tail_weight))

    def model(p):
        fraction, reach, core, tail, weight = p
        return engine_response(f, log_exposure, curves, matrix, halation, fraction=fraction, reach=reach,
                               core=core, tail=tail, weights=relative * weight)

    def residual(p):
        return np.log(np.maximum(model(p), 1e-6)) - np.log(response)

    best = None
    for start in ([0.5, 20, 2, 9, 0.6], [0.2, 60, 1, 5, 0.3], [0.9, 10, 4, 15, 0.8], [0.5, 150, 3, 3, 0.5]):
        res = least_squares(residual, start, bounds=([0, 3, 0.3, 1, 0], [1, 300, 8, 40, 0.99 / relative.max()]))
        if best is None or res.cost < best.cost:
            best = res
    fraction, reach, core, tail, weight = (float(v) for v in best.x)
    res = residual(best.x)
    return {"fraction": fraction, "reachUm": reach, "coreUm": core, "tailUm": tail,
            "tailWeights": [float(v) for v in relative * weight],
            "rmsError": float(np.sqrt(np.mean(res ** 2))), "worstError": float(np.max(np.abs(np.exp(res) - 1))),
            "points": data}


if __name__ == "__main__":
    import stock_overrides

    for stock in SHEET_MTF:
        r = stock_overrides.slide_spatial(stock)
        print(f"{stock:24s} spreads {r['fraction']:.2f} over {r['reachUm']:.0f} um; scatter core "
              f"{r['coreUm']:.1f} um, tail {r['tailUm']:.1f} um at {np.mean(r['tailWeights']):.2f}; "
              f"rms {100 * r['rmsError']:.1f}%, worst {100 * r['worstError']:.0f}%")
