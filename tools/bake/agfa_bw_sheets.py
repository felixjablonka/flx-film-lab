"""Agfa's AGFAPAN APX 100 and APX 400, off two editions of their data.

- `agfa_apx100.pdf`, `agfa_apx400.pdf`: Agfa-Gevaert's technical data sheet
  for each film, "Status 08/1995, 1st edition". Page 2 carries the spectral
  sensitivity, the sharpness (MTF), the density curve and the gamma-time
  curves; page 1 the speed, the RMS granularity, the resolving power, the
  reciprocity table and the layers.
- `agfa_films_data.pdf`: Agfa's Professional film range, Technical Data PF,
  09/1998, which APX 25's profile is read from. Page 10 has the same four
  plots for APX 25, 100 and 400 side by side, and pages 4 and 5 say how they
  were measured: spectral sensitivity to an equal-energy spectrum, at density
  1.0 above minimum density; MTF and RMS granularity in daylight through a
  visual (V-lambda) filter, the RMS at diffuse density 1.0 through a 48 um
  aperture.

Every plot in both is vector art: a frame, thin rules and one stroked curve
path, numbered in text. Each is calibrated on its own rules, each printed label
paired with the rule it names (`plots`); the editions are then laid over each
other (`compare`).

Run:  python tools/bake/agfa_bw_sheets.py OUTDIR     (writes the overlays)
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pymupdf

sys.path.insert(0, str(Path(__file__).resolve().parent))
from datasheet import _flatten_path  # noqa: E402
from vector_plots import _subpaths  # noqa: E402

SHEETS = Path(__file__).resolve().parents[2] / "film spec sheets"
APX_1995 = {100: SHEETS / "agfa_apx100.pdf", 400: SHEETS / "agfa_apx400.pdf"}
RANGE_1998 = SHEETS / "agfa_films_data.pdf"
RANGE_PAGE = 9                    # page 10: APX 25, 100 and 400, left to right
RANGE_COLUMNS = {25: (20.0, 200.0), 100: (200.0, 380.0), 400: (380.0, 560.0)}

# The developers on the gamma-time plots, and the time and speed Agfa gives
# each for a small tank at 20 C (its exposure index table). RODINAL SPECIAL
# and STUDIONAL LIQUID share one curve.
DEVELOPERS = ("RODINAL SPECIAL / STUDIONAL LIQUID", "REFINAL", "RODINAL 1+25", "RODINAL 1+50")
EXPOSURE_INDEX = {
    100: {"REFINAL": (6.0, 125), "RODINAL 1+25": (8.0, 100), "RODINAL 1+50": (17.0, 125),
          "RODINAL SPECIAL / STUDIONAL LIQUID": (4.0, 125)},
    400: {"REFINAL": (6.0, 400), "RODINAL 1+25": (7.0, 320), "RODINAL 1+50": (11.0, 320),
          "RODINAL SPECIAL / STUDIONAL LIQUID": (4.5, 400)},
}
# Page 1 of each 1995 sheet, and the range sheet's page 10, agree on these.
RMS_GRANULARITY = {100: 9.0, 400: 14.0}       # x 1000, REFINAL 6 min at 20 C
RESOLVING_POWER = {100: 150, 400: 110}        # lines/mm at contrast 1000:1


@dataclass
class Axis:
    log: bool
    slope: float
    intercept: float
    residual: float                 # largest miss of a label, in the axis's own units (decades if log)

    def __call__(self, pos: np.ndarray) -> np.ndarray:
        v = self.slope * np.asarray(pos, float) + self.intercept
        return 10 ** v if self.log else v


@dataclass
class Plot:
    frame: pymupdf.Rect
    x: Axis
    y: Axis
    x_labels: list[float]
    y_labels: list[float]
    curves: list[np.ndarray] = field(default_factory=list)   # one per stroked sub-path, data units

    @property
    def kind(self) -> str:
        lo, hi = min(self.x_labels), max(self.x_labels)
        if lo >= 350 and hi <= 750:
            return "spectral"
        if lo <= -3.0:
            return "characteristic"
        if self.x.log:
            return "mtf"
        return "gamma_time"


def _number(text: str) -> float | None:
    t = text.replace("–", "-").replace("−", "-").replace("+", "").strip()
    try:
        return float(t)
    except ValueError:
        return None


def _frames(page) -> list[pymupdf.Rect]:
    """Rectangles drawn as a plot's frame: a stroked rectangle or quad, filled
    white or not filled (Agfa's grey panels behind them are filled grey)."""
    out = []
    for d in page.get_drawings():
        if not any(i[0] in ("re", "qu") for i in d["items"]) or d.get("type") not in ("s", "fs"):
            continue
        fill = d.get("fill")
        if fill is not None and min(fill) < 0.99:
            continue
        r = d["rect"]
        if r.width > 80 and r.height > 50:
            out.append(r)
    return out


def _rules(page, frame: pymupdf.Rect) -> tuple[list[float], list[float]]:
    """x of the vertical rules and y of the horizontal ones inside a frame,
    its own edges included."""
    xs, ys = {round(frame.x0, 2), round(frame.x1, 2)}, {round(frame.y0, 2), round(frame.y1, 2)}
    for d in page.get_drawings():
        for item in d["items"]:
            if item[0] != "l":
                continue
            a, b = item[1], item[2]
            if not all(frame.x0 - 1 <= p.x <= frame.x1 + 1 and frame.y0 - 1 <= p.y <= frame.y1 + 1 for p in (a, b)):
                continue
            if abs(a.x - b.x) < 0.05 and abs(a.y - b.y) > 0.5 * frame.height:
                xs.add(round(float(a.x), 2))
            if abs(a.y - b.y) < 0.05 and abs(a.x - b.x) > 0.5 * frame.width:
                ys.add(round(float(a.y), 2))
    return sorted(xs), sorted(ys)


def _labels(page, frame: pymupdf.Rect) -> tuple[list[tuple[float, float]], list[tuple[float, float]]]:
    """(centre, value) of the numbers printed under the frame and left of it."""
    below, left = [], []
    for w in page.get_text("words"):
        value = _number(w[4])
        if value is None:
            continue
        cx, cy = (w[0] + w[2]) / 2, (w[1] + w[3]) / 2
        # A number ending left of the frame is the y axis's, the bottom one
        # included, which sits a little below the frame's corner.
        if frame.x0 - 34 < w[2] < frame.x0 - 1 and frame.y0 - 7 < cy < frame.y1 + 7:
            left.append((cy, value))
        elif frame.y1 < cy < frame.y1 + 14 and frame.x0 - 14 < cx < frame.x1 + 14:
            below.append((cx, value))
    return below, left


def _axis(labels: list[tuple[float, float]], rules: list[float]) -> Axis:
    """Each label on the rule nearest it, then the straight line through them,
    on a log scale if that fits better (the MTF plot's axes)."""
    pos = np.array([min(rules, key=lambda r: abs(r - p)) for p, _ in labels])
    val = np.array([v for _, v in labels])
    fits = []
    for log in (False, True):
        if log and (val <= 0).any():
            continue
        target = np.log10(val) if log else val
        slope, intercept = np.polyfit(pos, target, 1)
        resid = float(np.abs(slope * pos + intercept - target).max())
        span = float(np.ptp(target))
        fits.append((resid / span, Axis(log, float(slope), float(intercept), resid)))
    best = min(fits, key=lambda f: f[0])
    assert best[0] < 0.01, f"axis does not calibrate: {labels} on {pos.tolist()}"
    return best[1]


def plots(pdf: Path, page: int, x_range: tuple[float, float] = (0.0, 1e9)) -> list[Plot]:
    """Every framed plot with stroked curves on `page` whose frame lies within
    `x_range` (PDF points), calibrated, its curves in data units."""
    doc = pymupdf.open(str(pdf))
    pg = doc[page]
    frames = [f for f in _frames(pg) if x_range[0] <= f.x0 and f.x1 <= x_range[1]]
    found: dict[tuple, Plot] = {}
    for d in pg.get_drawings():
        if d.get("type") != "s" or not any(i[0] == "c" for i in d["items"]):
            continue
        r = d["rect"]
        holders = [f for f in frames if f.x0 - 1 <= r.x0 and r.x1 <= f.x1 + 1 and f.y0 - 1 <= r.y0 and r.y1 <= f.y1 + 1]
        if not holders:
            continue
        frame = min(holders, key=lambda f: f.width * f.height)
        key = (round(frame.x0, 1), round(frame.y0, 1))
        if key not in found:
            xr, yr = _rules(pg, frame)
            below, left = _labels(pg, frame)
            found[key] = Plot(frame, _axis(below, xr), _axis(left, yr), [v for _, v in below], [v for _, v in left])
        plot = found[key]
        for run in _subpaths([i for i in d["items"] if i[0] in ("l", "c")]):
            pts = _flatten_path(run, steps=48)
            plot.curves.append(np.stack([plot.x(pts[:, 0]), plot.y(pts[:, 1])], axis=-1))
    return sorted(found.values(), key=lambda p: (round(p.frame.y0), p.frame.x0))


def _by_x(points: np.ndarray) -> np.ndarray:
    order = np.argsort(points[:, 0])
    pts = points[order]
    keep = np.concatenate([[True], np.diff(pts[:, 0]) > 1e-9])
    return pts[keep]


def edition(film: int, which: str) -> dict[str, object]:
    """One edition's plots of APX `film` (100 or 400): `which` is "1995" (its
    own sheet) or "1998" (the range sheet). Returns the spectral sensitivity
    (nm, log), the density curve (log H in lux-seconds, density), the MTF
    (lines/mm, fraction) and the gamma-time curves {developer: (min, gamma)}."""
    if which == "1995":
        found = plots(APX_1995[film], 1)
    else:
        found = plots(RANGE_1998, RANGE_PAGE, RANGE_COLUMNS[film])
    by_kind = {p.kind: p for p in found}
    assert set(by_kind) == {"spectral", "characteristic", "mtf", "gamma_time"}, [p.kind for p in found]
    one = lambda p: _by_x(np.concatenate(p.curves))                    # noqa: E731
    spectral = one(by_kind["spectral"])
    curve = one(by_kind["characteristic"])
    mtf = one(by_kind["mtf"])
    mtf[:, 1] /= 100.0
    # The gamma-time curves, named by where each reaches gamma 0.65: in Agfa's
    # order of development speed, fastest first.
    times = by_kind["gamma_time"].curves
    def at_065(c):
        c = _by_x(c)
        return float(np.interp(0.65, c[:, 1], c[:, 0])) if c[:, 1].min() <= 0.65 <= c[:, 1].max() else np.inf
    times = sorted((_by_x(c) for c in times), key=at_065)
    assert len(times) == len(DEVELOPERS), len(times)
    return dict(spectral=spectral, curve=curve, mtf=mtf, gamma_time=dict(zip(DEVELOPERS, times)),
                plots=by_kind)


def gamma(curve: np.ndarray) -> float:
    """Agfa's "negative contrast": the slope of the curve's straight part,
    taken here between density 0.30 and 1.50 over base + fog (its fit to a
    line over that stretch)."""
    base = curve[:, 1].min()
    net = curve[:, 1] - base
    sel = (net >= 0.30) & (net <= 1.50)
    return float(np.polyfit(curve[sel, 0], curve[sel, 1], 1)[0])


def compare(out: Path) -> None:
    """The two editions of each film, plot by plot, overlaid, and their
    differences printed."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    out.mkdir(parents=True, exist_ok=True)
    for film in (100, 400):
        a, b = edition(film, "1995"), edition(film, "1998")
        fig, axes = plt.subplots(2, 2, figsize=(12, 9))
        for ax, key, xlab, ylab in ((axes[0, 0], "spectral", "nm", "lg sensitivity"),
                                   (axes[0, 1], "curve", "lg H (lux-s)", "density"),
                                   (axes[1, 0], "mtf", "lines/mm", "transfer factor")):
            ax.plot(*a[key].T, lw=2.5, alpha=0.5, label="1995 sheet")
            ax.plot(*b[key].T, lw=1, color="k", label="1998 range sheet")
            ax.set_xlabel(xlab); ax.set_ylabel(ylab); ax.grid(alpha=0.3)
            lo, hi = max(a[key][0, 0], b[key][0, 0]), min(a[key][-1, 0], b[key][-1, 0])
            grid = np.linspace(lo, hi, 400)
            diff = np.interp(grid, *a[key].T) - np.interp(grid, *b[key].T)
            print(f"APX {film} {key}: 1995 - 1998 over {lo:.2f}..{hi:.2f}: max |d| {np.abs(diff).max():.3f}, "
                  f"mean {diff.mean():+.3f}")
            ax.legend()
        axes[1, 0].set_xscale("log")
        ax = axes[1, 1]
        for dev in DEVELOPERS:
            ax.plot(*a["gamma_time"][dev].T, lw=2.5, alpha=0.5)
            ax.plot(*b["gamma_time"][dev].T, lw=1, color="k")
            t, s = EXPOSURE_INDEX[film][dev]
            g = float(np.interp(t, *a["gamma_time"][dev].T))
            ax.plot([t], [g], "o")
            ax.annotate(f"{dev}\n{t:g} min, ISO {s}: gamma {g:.3f}", (t, g), fontsize=7)
        ax.axhline(gamma(a["curve"]), color="r", lw=0.8)
        ax.set_xlabel("min"); ax.set_ylabel("gamma"); ax.grid(alpha=0.3)
        fig.suptitle(f"AGFAPAN APX {film}: 1995 sheet (thick) and 1998 range sheet (thin); red: the density "
                     f"curve's gamma {gamma(a['curve']):.3f} (1995), {gamma(b['curve']):.3f} (1998)")
        fig.tight_layout()
        fig.savefig(out / f"apx{film}-editions.png", dpi=110)
        plt.close(fig)


if __name__ == "__main__":
    compare(Path(sys.argv[1]))
