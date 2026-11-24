"""Fujifilm's colour papers, off the product information bulletins Fujifilm
Tokyo printed with its plots as vectors.

- FUJICOLOR CRYSTAL ARCHIVE DIGITAL PAPER TYPE DPII, AF3-212E (October 2005):
  characteristic curves (laser exposure, CP-48S, Status A, against relative
  log H), spectral sensitivity (relative log sensitivity at D-min + 1.0,
  Status A, its scale a bar marking one decade) and spectral reflection dye
  densities, each normalised to 1.0.
- FUJICOLOR CRYSTAL ARCHIVE PAPER, AF3-198E (December 2004), the paper sold
  in the Americas as Type II and later Type CA: spectral sensitivity and dye
  densities drawn the same way. It plots no characteristic curves; no
  Crystal Archive bulletin does.

Fujifilm Europe's later editions (DP II from 2014, and today's sheets for
Type CA, DP II and Maxima, 2018-19) redraw the dye and sensitivity plots as
Excel charts, one set for all three papers, that differ from these: see
`build_paper_profiles.py`. Each plot here is calibrated on its own frame and
gridlines (`vector_plots.VectorPlot`); coordinates are PDF points.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pymupdf

from datasheet import _flatten_path
from vector_plots import VectorPlot, _subpaths, _to_data, by_position

SHEETS = Path(__file__).resolve().parents[2] / "film spec sheets"
DPII = "Fujicolor_Crystal_Archive_Digital_Paper_Type_DPII_AF3-212E_2005.pdf"
CRYSTAL_ARCHIVE = "Fujicolor_Crystal_Archive_Paper_AF3-198E_2004.pdf"


def _path(pdf: str, page: int, index: int) -> list:
    return pymupdf.open(str(SHEETS / pdf))[page].get_drawings()[index]


def _strokes(plot: VectorPlot, items) -> list[np.ndarray]:
    """A path's strokes as data, each split where it turns back along x: a
    pen that draws one dye's curve left to right and the next right to left
    without lifting draws two curves."""
    out = []
    for run in _subpaths(items):
        data = _to_data(plot, _flatten_path(run, steps=32))
        turns = np.flatnonzero(np.diff(np.sign(np.diff(data[:, 0]))) != 0) + 1
        # a turn is real where the stroke runs on the other way for a while
        cuts = [t for t in turns if abs(data[min(t + 40, len(data) - 1), 0] - data[t, 0]) > 5]
        for piece in np.split(data, cuts):
            if np.ptp(piece[:, 0]) > 20:
                out.append(piece[np.argsort(piece[:, 0], kind="stable")])
    return out


def _named_by_peak(curves: list[np.ndarray], names: tuple[str, ...]) -> dict[str, np.ndarray]:
    if len(curves) != len(names):
        raise ValueError(f"{len(curves)} curves, expected {len(names)}")
    return dict(zip(names, sorted(curves, key=lambda c: c[np.argmax(c[:, 1]), 0])))


def dpii() -> dict:
    """DP II, AF3-212E. Characteristic curves: R solid, G dash-dot, B dashed;
    G is drawn only where it leaves R, above about density 1.0, and follows R
    below. The relative log H axis is ruled every 0.5 (the plot's own 0.5
    bar spans one rule), taken as 0 at the frame's left edge."""
    pdf = str(SHEETS / DPII)
    rules = (81.3, 103.95, 126.63, 149.3, 171.97, 194.65, 217.32, 239.99, 262.67, 285.3)
    curves_plot = VectorPlot(pdf, 3, (81.3, 370.2, 285.3, 551.5), tuple(zip(rules, np.arange(0.0, 4.51, 0.5))),
                             ((551.5, 0.0), (528.83, 0.5), (506.16, 1.0), (483.5, 1.5), (460.83, 2.0),
                              (438.17, 2.5), (415.51, 3.0), (392.84, 3.5)))
    curves = {}
    for name, index in (("R", 21), ("G", 20), ("B", 19)):
        runs = [_to_data(curves_plot, _flatten_path(run, steps=32)) for run in _subpaths(_path(DPII, 3, index)["items"])]
        curve = max(runs, key=lambda r: np.ptp(r[:, 0]))          # the legend's sample left out
        curves[name] = curve[np.argsort(curve[:, 0], kind="stable")]
    r, g = curves["R"], curves["G"]
    curves["G"] = np.concatenate([r[r[:, 0] < g[0, 0]], g])

    sensitivity_plot = VectorPlot(pdf, 3, (333.4, 369.4, 533.4, 550.5),
                                  ((346.94, 400.0), (404.55, 500.0), (461.89, 600.0), (518.68, 700.0)),
                                  ((493.73, 0.0), (437.36, 1.0), (381.0, 2.0)))
    sensitivity = by_position(sensitivity_plot, ("B", "G", "R"))
    dye_plot = VectorPlot(pdf, 4, (79.7, 103.4, 287.5, 251.3),
                          ((93.19, 400.0), (153.43, 500.0), (212.55, 600.0)),
                          ((251.46, 0.0), (191.64, 0.5), (131.82, 1.0)))
    dyes = by_position(dye_plot, ("Y", "M", "C"))
    return {"curves": curves, "sensitivity": {k: [v] for k, v in sensitivity.items()},
            "dyes": dyes, "plots": (curves_plot, sensitivity_plot, dye_plot)}


def crystal_archive() -> dict:
    """Crystal Archive Paper, AF3-198E: spectral sensitivity (all three in one
    path) and dye densities (one path, magenta drawn into yellow without the
    pen lifting)."""
    pdf = str(SHEETS / CRYSTAL_ARCHIVE)
    sensitivity_plot = VectorPlot(pdf, 4, (75.4, 110.85, 285.32, 289.59),
                                  ((89.95, 400.0), (150.25, 500.0), (210.57, 600.0), (269.9, 700.0)),
                                  ((230.23, 1.0), (171.2, 2.0)))
    sensitivity = _named_by_peak(_strokes(sensitivity_plot, _path(CRYSTAL_ARCHIVE, 4, 11)["items"]), ("B", "G", "R"))
    dye_plot = VectorPlot(pdf, 4, (334.9, 112.52, 542.82, 260.24),
                          ((348.74, 400.0), (408.2, 500.0), (468.78, 600.0), (528.95, 700.0)),
                          ((201.12, 0.5), (141.52, 1.0)))
    dyes = _named_by_peak(_strokes(dye_plot, _path(CRYSTAL_ARCHIVE, 4, 15)["items"]), ("Y", "M", "C"))
    return {"sensitivity": {k: [v] for k, v in sensitivity.items()}, "dyes": dyes,
            "plots": (sensitivity_plot, dye_plot)}
