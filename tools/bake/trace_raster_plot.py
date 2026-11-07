"""Trace curves off a raster plot, for datasheets that ship pictures.

Kodak publishes its sensitometry as vector art; Fujifilm embeds bitmaps. The
bitmaps here are clean, high-resolution, white-on-black line plots, which trace
reliably once the frame and the grid are identified and removed.

Where two curves lie on top of each other — which on Velvia is most of the
scale, because its three layers really are matched there — a raster carries no
information to separate them, and this returns the shared trace for all three.
The layers only diverge in the toe, and that is exactly where the trace can
still tell them apart.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from PIL import Image


@dataclass
class PlotFrame:
    left: float
    right: float
    top: float
    bottom: float
    x_min: float
    x_max: float
    y_at_top: float
    y_at_bottom: float

    def x_to_data(self, px: np.ndarray) -> np.ndarray:
        t = (px - self.left) / (self.right - self.left)
        return self.x_min + t * (self.x_max - self.x_min)

    def y_to_data(self, py: np.ndarray) -> np.ndarray:
        t = (py - self.top) / (self.bottom - self.top)
        return self.y_at_top + t * (self.y_at_bottom - self.y_at_top)


def load_mask(path: str, threshold: int = 128, invert: bool = False) -> np.ndarray:
    image = np.asarray(Image.open(path).convert("L"))
    mask = image > threshold
    return ~mask if invert else mask


def _bands(indices: np.ndarray, gap: int = 6) -> list[float]:
    """Group neighbouring indices into rules and return their centres."""
    if indices.size == 0:
        return []
    groups: list[list[int]] = [[int(indices[0])]]
    for i in indices[1:]:
        if int(i) - groups[-1][-1] <= gap:
            groups[-1].append(int(i))
        else:
            groups.append([int(i)])
    return [float(np.mean(g)) for g in groups]


def grid_lines(mask: np.ndarray, fill: float = 0.40) -> tuple[list[float], list[float]]:
    """Centres of the horizontal and vertical rules, borders included.

    A single threshold finds both: rules run the full width or height of the
    plot, while curves, legends and annotations never do.
    """
    h, w = mask.shape
    rows = _bands(np.where(mask.sum(axis=1) > fill * w)[0])
    cols = _bands(np.where(mask.sum(axis=0) > fill * h)[0])
    return rows, cols


def find_frame(mask: np.ndarray, *, x_min: float, x_max: float,
               y_at_top: float, y_at_bottom: float, fill: float = 0.40) -> PlotFrame:
    """Calibrate from the outermost rules — the plot border.

    Note that the interior grid is not necessarily aligned to the tick labels:
    Fujifilm draws a square grid whose spacing has nothing to do with the
    decade marks, so only the border can be trusted to sit on a known value.
    """
    rows, cols = grid_lines(mask, fill)
    if len(rows) < 2 or len(cols) < 2:
        raise ValueError(f"not enough grid rules: {len(rows)} rows, {len(cols)} cols")
    return PlotFrame(left=cols[0], right=cols[-1], top=rows[0], bottom=rows[-1],
                     x_min=x_min, x_max=x_max,
                     y_at_top=y_at_top, y_at_bottom=y_at_bottom)


def _grid_rows_cols(mask: np.ndarray, frame: PlotFrame, halfwidth: int = 8):
    """Pixel rows and columns occupied by rules, widened to cover their edges.

    Anti-aliased rule edges are the thing that has to go: a leftover edge pixel
    reads as a curve crossing and snaps the trace to a grid value.
    """
    rows, cols = grid_lines(mask)
    grid_rows = {int(round(r)) + d for r in rows for d in range(-halfwidth, halfwidth + 1)}
    grid_cols = {int(round(c)) + d for c in cols for d in range(-halfwidth, halfwidth + 1)}
    return grid_rows, grid_cols


def trace(mask: np.ndarray, frame: PlotFrame, *,
          exclude: list[tuple[int, int, int, int]] | None = None,
          max_curves: int = 3) -> tuple[np.ndarray, list[np.ndarray]]:
    """Return (x in data units, [curve values]) sampled once per usable column.

    Curves are ordered top to bottom on the plot. Columns where fewer runs are
    visible than `max_curves` mean the curves have merged; the available traces
    are then spread across all outputs, which is the honest reading of a plot
    that has drawn them on top of one another.
    """
    # Find the grid before masking anything out. Blanking a text block first
    # would thin the rules crossing it below the detection threshold, and an
    # undetected grid line reads as a curve.
    grid_rows, grid_cols = _grid_rows_cols(mask, frame)

    working = mask.copy()
    if exclude:
        for x0, y0, x1, y1 in exclude:
            working[y0:y1, x0:x1] = False
    top, bottom = int(frame.top) + 3, int(frame.bottom) - 3
    left, right = int(frame.left) + 3, int(frame.right) - 3

    xs: list[float] = []
    traces: list[list[float]] = [[] for _ in range(max_curves)]

    for col in range(left, right):
        if col in grid_cols:
            continue
        column = working[top:bottom, col]
        rows = [top + i for i, v in enumerate(column) if v and (top + i) not in grid_rows]
        if not rows:
            continue
        # Group contiguous pixels into runs; each run is one curve crossing.
        runs: list[list[int]] = [[rows[0]]]
        for r in rows[1:]:
            if r - runs[-1][-1] <= 3:
                runs[-1].append(r)
            else:
                runs.append([r])
        centers = [float(np.mean(run)) for run in runs]
        if len(centers) > max_curves:
            # Keep the longest runs: stray text or tick marks make short ones.
            keep = sorted(range(len(runs)), key=lambda i: -len(runs[i]))[:max_curves]
            centers = sorted(centers[i] for i in keep)
        while len(centers) < max_curves:
            centers.append(centers[-1])          # merged: same value for each layer
        xs.append(float(col))
        for i, c in enumerate(sorted(centers)):
            traces[i].append(c)

    x_data = frame.x_to_data(np.asarray(xs))
    curves = [frame.y_to_data(np.asarray(t)) for t in traces]
    return x_data, curves


def trace_curves(mask: np.ndarray, frame: PlotFrame, seeds: dict[str, tuple[float, float]], *,
                 exclude: list[tuple[int, int, int, int]] | None = None,
                 rule_halfwidth: int = 4, gap: int = 120) -> dict[str, np.ndarray]:
    """Follow each named curve from a seed point (data units) to both ends.

    For plots whose curves are not functions of x across the frame — spectral
    sensitivities and dye densities, each spanning its own band, crossing its
    neighbours and falling near-vertically at its edges. The strokes are
    thinned to one-pixel lines with the rules removed, and each curve is
    walked from its seed: at a crossing it takes the branch that carries on
    in its own direction, and across a gap — where a rule was taken out, or
    a peak ran along one — it jumps to the nearest line ahead of it.
    """
    from skimage.morphology import skeletonize

    rows, cols = grid_lines(mask)
    m = mask.copy()
    removed = np.zeros_like(mask, dtype=bool)
    for i, r in enumerate(rows):
        hw = rule_halfwidth * (2 if i in (0, len(rows) - 1) else 1)     # the border, anti-aliased wider
        removed[max(int(r) - hw, 0):int(r) + hw + 1, :] = True
    for i, c in enumerate(cols):
        hw = rule_halfwidth * (2 if i in (0, len(cols) - 1) else 1)
        removed[:, max(int(c) - hw, 0):int(c) + hw + 1] = True
    m &= ~removed
    if exclude:
        for x0, y0, x1, y1 in exclude:
            m[y0:y1, x0:x1] = False
    skeleton = skeletonize(m)
    on = set(zip(*np.nonzero(skeleton)))
    ys, xs = np.nonzero(skeleton)
    coords = np.stack([ys, xs], axis=-1)

    def neighbours(p):
        y, x = p
        return [(y + dy, x + dx) for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                if (dy or dx) and (y + dy, x + dx) in on]

    def direction(path, span=25):
        a, b = np.array(path[-min(len(path), span)], float), np.array(path[-1], float)
        d = b - a
        n = np.hypot(*d)
        return d / n if n > 0 else None

    def straightest(cur, prev, heading, visited, avoid):
        """The neighbour that best carries on along `heading`."""
        nxt = [q for q in neighbours(cur) if q != prev and q not in visited and q not in avoid]
        if not nxt:
            return None
        if heading is None:
            return nxt[0]
        here = np.array(cur, float)
        return max(nxt, key=lambda q: float((np.array(q, float) - here) @ heading))

    def look_ahead(start, came_from, heading, visited, steps=25):
        path, prev, cur = [came_from, start], came_from, start
        for _ in range(steps):
            q = straightest(cur, prev, direction(path, 10) if len(path) > 6 else heading, visited, set(path))
            if q is None:
                break
            prev, cur = cur, q
            path.append(cur)
        return path

    def walk(start, first, visited, heading0=None):
        path = [start, first]
        visited.update(path)
        while True:
            here = path[-1]
            options = [q for q in neighbours(here) if q not in visited]
            heading = direction(path) if len(path) > 8 else heading0 if heading0 is not None else direction(path)
            if len(options) == 1:
                path.append(options[0])
            elif len(options) > 1 and heading is not None:
                def score(q):
                    ahead = look_ahead(q, here, heading, visited)
                    d = np.array(ahead[-1], float) - np.array(here, float)
                    n = np.hypot(*d)
                    return float(d @ heading / n) if n > 0 else -1.0
                path.append(max(options, key=score))
            else:
                # An end. If a rule was taken out here — the curve crossed
                # one, or a peak ran along one — carry on from the nearest
                # line ahead across it; a curve's real end has no rule to
                # explain the gap, and stays an end.
                if heading is None:
                    break
                d = coords - np.array(here)
                dist = np.hypot(d[:, 0], d[:, 1])
                ahead = (d @ heading) / np.maximum(dist, 1e-9)
                ok = (dist > 1.5) & (dist <= gap) & (ahead > 0.3)
                if ok.any():
                    ok &= np.array([tuple(c) not in visited for c in coords])
                target = None
                for j in np.argsort(np.where(ok, dist, np.inf))[:20]:
                    if not ok[j]:
                        break
                    seg = np.linspace(np.array(here, float), coords[j].astype(float), 24)[1:-1]
                    inside = removed[np.round(seg[:, 0]).astype(int), np.round(seg[:, 1]).astype(int)]
                    if inside.mean() >= 0.5:
                        target = tuple(coords[j])
                        break
                if target is None:
                    break
                path.append(target)
            visited.add(path[-1])
        return path

    def to_px(x_data, y_data):
        x = frame.left + (x_data - frame.x_min) / (frame.x_max - frame.x_min) * (frame.right - frame.left)
        y = frame.top + (y_data - frame.y_at_top) / (frame.y_at_bottom - frame.y_at_top) * (frame.bottom - frame.top)
        return y, x

    out: dict[str, np.ndarray] = {}
    for name, (sx, sy) in seeds.items():
        y0, x0 = to_px(sx, sy)
        k = int(np.argmin(np.hypot(coords[:, 0] - y0, coords[:, 1] - x0)))
        start = tuple(coords[k])
        visited = {start}
        branches = neighbours(start)
        if not branches:
            raise ValueError(f"{name}: the seed is not on a line")
        def one_way(path):
            # A spectral curve is a function of wavelength: a walk that turns
            # back on itself has taken another curve at a crossing, and ends
            # there. Points off the plot are not the curve either.
            xs_ = [q[1] for q in path]
            sign = np.sign(xs_[min(len(xs_) - 1, 20)] - xs_[0]) or 1.0
            extreme, keep = xs_[0], len(path)
            for i, (y, x) in enumerate(path):
                inside = frame.top <= y <= frame.bottom and frame.left <= x <= frame.right
                if not inside or sign * (x - extreme) < -6:
                    keep = i
                    break
                extreme = max(extreme, x) if sign > 0 else min(extreme, x)
            return path[:max(keep, 2)]

        first = walk(start, branches[0], visited)
        rest = [q for q in neighbours(start) if q not in visited]
        back = None
        if len(first) > 8:
            d = np.array(first[1], float) - np.array(first[min(len(first) - 1, 25)], float)
            back = d / max(np.hypot(*d), 1e-9)
        second = walk(start, rest[0], visited, back) if rest else [start]
        first, second = one_way(first), one_way(second)
        path = np.array(second[::-1] + first[1:], float)
        data = np.stack([frame.x_to_data(path[:, 1]), frame.y_to_data(path[:, 0])], axis=-1)
        order = np.argsort(data[:, 0], kind="stable")
        out[name] = data[order]
    return out


def follow(ink: np.ndarray, seed: tuple[float, float], heading: tuple[float, float] = (1.0, 0.0), *,
           erased: np.ndarray | None = None, step: float = 1.0, reach: float = 2.5, max_gap: float = 14.0,
           max_erased: float = 40.0, apart: float = 6.0, back: int = 10) -> np.ndarray:
    """(x, y) pixel points along one stroke through `seed`, walked both ways.

    For plots that draw their curves dashed or dash-dotted in one ink and let
    them cross, where neither a column scan (`trace`) nor a skeleton walk
    (`trace_curves`) can keep a curve apart from its neighbours. Each step
    looks across the stroke's own direction for ink where it should be next,
    and takes only ink within `reach` of that: a dashed line's next dash, not a
    neighbour. Ink that touches the walk but lies off that line is the stroke
    turning (a sharp peak), and is followed; a new dash sets the direction
    afresh, since it may turn at once. Across a gap the walk carries on in the
    same direction, for at most `max_gap` pixels of a dash's break (or
    `max_erased` through `erased`, where a rule was blanked), so a curve's real
    end stays an end. Where another stroke crosses, the ink across is wider
    than the stroke's own: the walk goes straight on until it narrows, and
    `apart` pixels further, where the two have parted."""
    h, w = ink.shape
    ys, xs = np.nonzero(ink)
    j = int(np.argmin(np.hypot(xs - seed[0], ys - seed[1])))
    start = np.array([xs[j], ys[j]], float)

    def local_direction(p, u, radius=7.0):
        sel = np.hypot(xs - p[0], ys - p[1]) <= radius
        pts = np.stack([xs[sel], ys[sel]], axis=-1).astype(float)
        if len(pts) < 6:
            return None
        v = np.linalg.svd(pts - pts.mean(axis=0))[2][0]
        return v if v @ u >= 0 else -v

    u0 = local_direction(start, np.asarray(heading, float), 8.0)
    u0 = np.asarray(heading, float) if u0 is None else u0
    u0 = u0 / np.hypot(*u0)
    ring = [(dx, dy) for dx in range(-3, 4) for dy in range(-3, 4) if 1.5 <= np.hypot(dx, dy) <= 3.2]

    def inked(x, y):
        x, y = int(round(x)), int(round(y))
        return 0 <= x < w and 0 <= y < h and bool(ink[y, x])

    def across(q, n, limit):
        """(centre, width) of the run of ink across the walk at q nearest it."""
        t = np.arange(-limit, limit + 0.01, 0.5)
        on = np.array([inked(*(q + n * s)) for s in t])
        if not on.any():
            return None
        idx = np.flatnonzero(on)
        cut = np.flatnonzero(np.diff(idx) > 1)
        a, b = min(zip(np.r_[idx[0], idx[cut + 1]], np.r_[idx[cut], idx[-1]]), key=lambda r: abs(t[r[0]] + t[r[1]]))
        lo, hi = t[a], t[b]
        while lo > -30 and inked(*(q + n * (lo - 0.5))):
            lo -= 0.5
        while hi < 30 and inked(*(q + n * (hi + 0.5))):
            hi += 0.5
        return (lo + hi) / 2, hi - lo

    def walk(u):
        found, widths = [start.copy()], []
        p, gap, rubbed, crossing, coast = start.copy(), 0.0, 0.0, False, 0.0
        while True:
            q = p + u * step
            if not (0 <= q[0] < w and 0 <= q[1] < h):
                break
            n = np.array([-u[1], u[0]])
            got = across(q, n, reach + 0.15 * gap + 0.25 * rubbed)
            own = float(np.median(widths[-20:])) if len(widths) >= 5 else 7.0
            if got is not None and got[1] > own + 3.0 and gap == 0 and rubbed == 0:
                crossing, coast = True, apart
            if got is not None and not crossing and coast <= 0:
                p = q + n * got[0]
                if gap > 0 or rubbed > 0:
                    v = local_direction(p, u)
                    if v is not None:
                        u = v
                else:
                    widths.append(got[1])
                gap = rubbed = 0.0
            else:
                if got is not None and crossing and got[1] <= own + 3.0:
                    crossing = False
                if got is None and not crossing and coast <= 0 and gap == 0 and rubbed == 0:
                    turn = [(dx, dy) for dx, dy in ring if inked(p[0] + dx, p[1] + dy)
                            and (dx * u[0] + dy * u[1]) > 0.2 * np.hypot(dx, dy)]
                    if turn:
                        dx, dy = max(turn, key=lambda d: (d[0] * u[0] + d[1] * u[1]) / np.hypot(*d))
                        p = p + np.array([dx, dy], float)
                        u = 0.5 * u + 0.5 * np.array([dx, dy], float) / np.hypot(dx, dy)
                        u /= np.hypot(*u)
                        found.append(p.copy())
                        continue
                p = q
                if crossing or coast > 0:
                    coast -= 0.0 if crossing else step
                elif erased is not None and erased[int(q[1]), int(q[0])]:
                    rubbed += step
                    if rubbed > max_erased:
                        break
                else:
                    gap += step
                    if gap > max_gap:
                        break
                continue
            found.append(p.copy())
            if len(found) > back:
                d = found[-1] - found[-1 - back]
                if np.hypot(*d) > 0:
                    u = 0.5 * u + 0.5 * d / np.hypot(*d)
                    u /= np.hypot(*u)
        return found

    return np.array(walk(-u0)[::-1][:-1] + walk(u0))
