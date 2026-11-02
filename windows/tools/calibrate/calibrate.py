"""Measure Apple's colour for one camera from one of its raw files, for the
port's table of Apple's camera profiles (engine/data/apple-colour.json).

Apple's decoder (flxmeasure) renders the file at the app's own white balances
and a grid of temperatures and tints; the port's decoder (flxport-decode)
gives the camera's own RGB for the same pixels. At each white balance Apple's
colour is then a 3x3 transform of the camera's RGB:

    Rec.2020 = C diag(1 / w) camera

with w the camera's neutral for the light (its largest channel 1) and C the
colour. Stored per camera:
- Apple's crop of the mosaic.
- The transform measured at each of the app's own white balances.
- The transform over a grid of temperatures and tints, which Custom
  interpolates in mired and tint. On most cameras Apple's profile is a
  two-illuminant DNG one, C blended by 1/T between 2856 K and 6504 K and the
  same at any tint, but nothing here assumes that. Nor is the transform split
  into C and w to interpolate: at some lights a camera has no neutral (on the
  X-T30 at warm, magenta ones the grey takes negative green), while the
  transform itself stays smooth.
- As shot looks the camera's own neutral up in the grid, for its temperature
  and tint, and takes the transform there with the camera's neutral in place
  of the grid's.

    calibrate.py <raw> <workdir> [--size PX]

Prints a summary and writes <workdir>/calibration.json; the pictures it
measures from (most of a gigabyte) go into <workdir> too, or into a scratch
folder that the next camera's overwrite. Run from anywhere; the tools are
found in the repo's build folders.
"""
import json
import os
import subprocess
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "compare"))
from compare import ciede2000, lab, read_dump  # noqa: E402

FLXMEASURE = os.path.join(ROOT, "build", "mac-reference", "flxmeasure")
FLXPORT_DECODE = os.environ.get("FLX_PORT_DECODE", os.path.join(ROOT, "build", "mac", "flxport-decode"))
REC2020_TO_XYZ = np.array([[0.6369580, 0.1446169, 0.1688810], [0.2627002, 0.6779981, 0.0593017],
                           [0.0000000, 0.0280727, 1.0609851]])
D65 = np.array([0.95047, 1.0, 1.08883])
GRID_T = [2000, 2500, 2856, 3400, 4500, 5500, 6504, 8000, 10000]
GRID_TINT = [-50, -25, -10, 0, 10, 25, 50]
# off the grid, where Custom and As shot land, to check the grid interpolated
PROBES = [(2856, -5), (2856, 15), (3200, -15), (3200, 5), (4000, -20), (4000, 15), (7000, -15), (7000, 20)]
# BB3200 since 1.3.0: a table measured before has none, and the port takes a
# 3200 K lamp from the grid there (RawDecoder's lightOf)
FIXED = ["D50", "D55", "D65", "T", "BB3200", "TH-KG3", "K75P", "Tungsten"]

def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{os.path.basename(args[0])} failed: {r.stderr.strip() or r.stdout.strip()}")
    return r.stdout


def shift(a, b):
    """The (dx, dy) by which b is a shifted: a(x, y) ~ b(x + dx, y + dy), by
    phase correlation, to a fraction of a pixel."""
    h, w = a.shape
    window = np.outer(np.hanning(h), np.hanning(w))
    fa = np.fft.rfft2((a - a.mean()) * window)
    fb = np.fft.rfft2((b - b.mean()) * window)
    r = fb * np.conj(fa)
    corr = np.fft.irfft2(r / np.maximum(np.abs(r), 1e-12), s=a.shape)
    py, px = np.unravel_index(np.argmax(corr), corr.shape)

    def refine(m, p, n):
        # a parabola through the peak and its neighbours
        c = corr[(py + m * np.array([-1, 0, 1])) % h, (px + (1 - m) * np.array([-1, 0, 1])) % w]
        den = c[0] - 2 * c[1] + c[2]
        frac = 0.5 * (c[0] - c[2]) / den if den != 0 else 0.0
        return ((p + n // 2) % n - n // 2) + frac
    return refine(0, px, w), refine(1, py, h), corr.max()


def luma(rgb):
    return np.log(np.maximum(rgb[..., :3] @ REC2020_TO_XYZ[1], 1e-5))


def usable(sensor, apple, smooth):
    """Well exposed, and away from the camera's clipping, below which Apple
    rebuilds highlights from about 0.85 of white; and away from edges and
    fine detail, where the two decoders' demosaics differ. Apple's output is
    linear far above 1 and below 0, where a strong white balance takes some
    of its channels."""
    y = apple @ REC2020_TO_XYZ[1]
    return (smooth & (y > 0.01) & (y < 0.6) & np.all(sensor > 0.002, axis=1) & np.all(sensor < 0.85, axis=1))


def smoothness(rgb):
    """The smoother 70% of the picture: the relative gradient of its luminance"""
    y = np.log(np.maximum(rgb @ np.array([0.3, 0.6, 0.1]), 1e-5))
    g = np.zeros_like(y)
    g[1:-1, 1:-1] = (np.abs(y[1:-1, 2:] - y[1:-1, :-2]) + np.abs(y[2:, 1:-1] - y[:-2, 1:-1]))
    g[0, :] = g[-1, :] = g[:, 0] = g[:, -1] = np.inf
    return (g <= np.quantile(g, 0.7)).ravel()


def fit(sensor, apple, k):
    """apple = A @ sensor, by least squares, then again without the 2% of the
    pixels it fits worst"""
    a = np.linalg.lstsq(sensor[k], apple[k], rcond=None)[0].T
    miss = np.linalg.norm(sensor[k] @ a.T - apple[k], axis=1) / np.maximum(apple[k] @ REC2020_TO_XYZ[1], 1e-6)
    keep = np.flatnonzero(k)[miss <= np.quantile(miss, 0.98)]
    return np.linalg.lstsq(sensor[keep], apple[keep], rcond=None)[0].T


def fit_neutral(sensor, apple, k, c):
    """With the colour C known, only the neutral: apple = C diag(1 / w) sensor"""
    balanced = apple[k] @ np.linalg.inv(c).T                   # diag(1 / w) sensor
    inverse = np.array([np.dot(balanced[:, i], sensor[k][:, i]) / np.dot(sensor[k][:, i], sensor[k][:, i])
                        for i in range(3)])
    w = 1 / inverse
    return w / w.max()


def de(apple, predicted):
    return ciede2000(lab(apple @ REC2020_TO_XYZ.T, D65), lab(predicted @ REC2020_TO_XYZ.T, D65))


def de_stats(apple, predicted, k):
    """ΔE2000 median and 99th percentile over the pixels of k that are
    colours (no channel below zero on either side), at most 100,000 of them"""
    k = np.flatnonzero(k)
    k = k[np.all(apple[k] > 0, axis=1) & np.all(predicted[k] > 0, axis=1)]
    if len(k) < 100:
        return float("nan"), float("nan")
    k = k[::max(1, len(k) // 100000)]
    d = de(apple[k], predicted[k])
    return float(np.median(d)), float(np.quantile(d, 0.99))


def parts(a):
    """A = C diag(1 / w): the neutral w, largest channel 1, and the colour C"""
    w = np.linalg.solve(a, np.ones(3))
    w = w / w.max()
    return w, a @ np.diag(w)


def calibrate(raw, workdir, size=1200, scratch=None):
    os.makedirs(workdir, exist_ok=True)
    result_dir, workdir = workdir, scratch or workdir
    os.makedirs(workdir, exist_ok=True)
    apple_dir = os.path.join(workdir, "apple")
    run([FLXMEASURE, raw, apple_dir, "--size", str(size), *[a for t, n in PROBES for a in ("--probe", f"{t}:{n}")]])
    apple_info = json.load(open(os.path.join(apple_dir, "apple.json")))
    dumps = apple_info["dumps"]
    native_w, native_h = apple_info["nativeWidth"], apple_info["nativeHeight"]

    # the port's decode, in the sensor's frame like Apple's, at half size
    # over the whole mosaic (Apple may show more than the decoder's crop),
    # against Apple's half-size middle: where Apple's crop lies in the mosaic
    probe = os.path.join(workdir, "port-probe.f32")
    run([FLXPORT_DECODE, raw, probe, "--camera-rgb", "--sensor-frame", "--size", "64"])
    port_info = json.load(open(probe[:-4] + ".json"))
    lp, tp, wp, hp = 0, 0, port_info["mosaicWidth"], port_info["mosaicHeight"]
    half = max(wp, hp) // 2
    port_half_path = os.path.join(workdir, "port-half.f32")
    run([FLXPORT_DECODE, raw, port_half_path, "--camera-rgb", "--sensor-frame", "--size", str(half),
         "--crop", "0", "0", str(wp), str(hp)])
    scale_p = half / max(wp, hp)
    port_half = read_dump(port_half_path)
    align = dumps["align"]
    apple_mid = read_dump(os.path.join(apple_dir, "align.f32"))
    mx, my, w, h = align["x"], align["y"], align["width"], align["height"]
    # CIImage counts y from the bottom; the dumps are written top row first
    my_top = align["fullHeight"] - (my + h)
    window = port_half[my_top:my_top + h, mx:mx + w]
    if window.shape[:2] != (h, w):
        raise RuntimeError("Apple's picture is larger than the mosaic")
    dx, dy, peak = shift(luma(apple_mid), luma(window))
    # pixel centres: Apple's half-size pixel i is at 2i + 0.5 of its crop, the
    # port's i' at (i' + 0.5) / scale - 0.5 of its own, at the window's
    # middle; rows also move by the fraction of a row that scaling to whole
    # pixels drops at the top (Core Image counts from the bottom)
    cx, cy = mx + w / 2, my_top + h / 2
    frac_a = native_h * 0.5 - align["fullHeight"]
    frac_p = hp * scale_p - port_half.shape[0]
    left = lp + (cx + dx + 0.5) / scale_p - 0.5 - (2 * cx + 0.5)
    top = tp + (cy + dy + 0.5 + frac_p) / scale_p - 0.5 - (2 * (cy + frac_a) + 0.5)
    crop = [int(round(left)), int(round(top)), native_w, native_h]
    crop_residual = max(abs(left - crop[0]), abs(top - crop[1]))
    if crop[0] < 0 or crop[1] < 0 or crop[0] + native_w > port_info["mosaicWidth"] or crop[1] + native_h > port_info["mosaicHeight"]:
        raise RuntimeError(f"Apple's crop {crop} does not fit the mosaic")

    # the port's camera RGB over Apple's crop, at Apple's size: the same pixels
    port_path = os.path.join(workdir, "port.f32")
    run([FLXPORT_DECODE, raw, port_path, "--camera-rgb", "--sensor-frame", "--size", str(size),
         "--crop", *map(str, crop)])
    port_info = json.load(open(port_path[:-4] + ".json"))
    port = read_dump(port_path)[..., :3].astype(np.float64)
    d55 = read_dump(os.path.join(apple_dir, "D55.f32"))[..., :3].astype(np.float64)
    if port.shape != d55.shape:
        raise RuntimeError(f"the port's picture is {port.shape[1]} x {port.shape[0]}, Apple's {d55.shape[1]} x {d55.shape[0]}")
    rx, ry, _ = shift(luma(d55), luma(port))
    sensor = port.reshape(-1, 3) / np.array(port_info["whiteBalance"])
    smooth = smoothness(port)

    # colour variety: the spread of the usable pixels' chromaticity, the
    # narrower of its two directions; a grey or one-coloured picture pins
    # down only part of the 3x3
    k_d55 = usable(sensor, d55.reshape(-1, 3), smooth)
    chroma = np.log(sensor[k_d55][:, [0, 2]] / sensor[k_d55][:, [1]])
    variety = float(np.sqrt(np.linalg.eigvalsh(np.cov(chroma.T)).min())) if k_d55.sum() > 100 else 0.0

    apple = {name: read_dump(os.path.join(apple_dir, name + ".f32"))[..., :3].astype(np.float64).reshape(-1, 3)
             for name in dumps if name != "align"}
    measured = {}
    for name, picture in apple.items():
        k = usable(sensor, picture, smooth)
        measured[name] = {"T": dumps[name]["temperature"], "tint": dumps[name]["tint"], "k": k}
        if k.sum() >= 20000:
            a = fit(sensor, picture, k)
            median, p99 = de_stats(picture, sensor @ a.T, k)
            measured[name].update(A=a, fitMedian=median, fit99=p99)
            measured[name]["w"], measured[name]["C"] = parts(a)

    for name in FIXED:
        if "A" not in measured[name]:
            raise RuntimeError(f"too few usable pixels at {name} ({measured[name]['k'].sum()})")

    # Apple's transform over the grid: each point's own 3x3, where it had
    # enough of the picture. Where a strong white balance leaves less (the far
    # corners), its neutral alone, with the colour part of the nearest point
    # that has its own: Apple's colour changes slowly, its neutral fast. Where
    # it leaves next to nothing, from the row's other tints.
    grid = {(t, tint): measured[f"grid-{t}-{tint}"] for t in GRID_T for tint in GRID_TINT}
    own = [p for p, m in grid.items() if "A" in m]
    sparse = []
    for (t, tint), m in grid.items():
        if "A" in m:
            continue
        if m["k"].sum() >= 1000:
            near = min(own, key=lambda p: (abs(1e6 / p[0] - 1e6 / t), abs(p[1] - tint)))
            c = parts(grid[near]["A"])[1]
            m["A"] = c @ np.diag(1 / fit_neutral(sensor, apple[f"grid-{t}-{tint}"], m["k"], c))
        else:
            sparse.append(f"grid-{t}-{tint}")
    for t in GRID_T:
        have = [n for n in GRID_TINT if "A" in grid[(t, n)]]
        if not have:
            raise RuntimeError(f"no usable pixels at {t} K")
        for n in GRID_TINT:
            if "A" in grid[(t, n)]:
                continue
            near = sorted(have, key=lambda h: abs(h - n))[:2]
            a0 = grid[(t, near[0])]["A"]
            a1 = grid[(t, near[-1])]["A"]
            grid[(t, n)]["A"] = a0 if len(near) == 1 else a0 + (a1 - a0) * (n - near[0]) / (near[1] - near[0])
    profile = {"grid": {"temperatures": GRID_T, "tints": GRID_TINT,
                        "transforms": [[grid[(t, n)]["A"].tolist() for n in GRID_TINT] for t in GRID_T]}}

    # how well that does, as the port does it:
    # - the app's own white balances: their measured transforms
    # - Custom: the grid interpolated, at the app's white balances that lie
    #   between its points (TH-KG3 and T lie on it), and at the probes
    # - As shot: the camera's own neutral, its temperature and tint where the
    #   grid has that neutral, and the grid's transform there
    fixed = {name: measured[name]["fitMedian"] for name in FIXED}
    custom = {}
    for name in ["D50", "D55", "D65", "BB3200", "K75P", "Tungsten"] + [f"probe-{t}-{n}" for t, n in PROBES]:
        m = measured[name]
        custom[name] = de_stats(apple[name], sensor @ transform(profile, m["T"], m["tint"]).T, m["k"])[0]
    worst_custom = max(custom, key=custom.get)
    as_shot = np.array(port_info["asShotNeutral"])       # the camera's, in its own RGB
    as_shot_t, as_shot_tint = grid_temperature(profile, as_shot)
    predicted = transform_for_neutral(profile, as_shot)
    as_shot_de = de_stats(apple["as-shot"], sensor @ predicted.T, measured["as-shot"]["k"])
    as_shot_gap = float(np.abs(as_shot / as_shot.max() - measured["as-shot"]["w"]).max())

    result = {
        "make": port_info["make"], "model": port_info["model"], "decoder": port_info["decoder"],
        "input": os.path.basename(raw), "macOS": apple_info["os"],
        "black": port_info["black"], "white": port_info["white"],
        "mosaic": [port_info["mosaicWidth"], port_info["mosaicHeight"]],
        "crop": crop, "baselineExposure": apple_info["baselineExposure"], "appleDefaults": apple_info["defaults"],
        **profile,
        "fixed": {name: {"temperature": measured[name]["T"], "tint": measured[name]["tint"],
                         "neutral": measured[name]["w"].tolist(), "transform": measured[name]["A"].tolist()}
                  for name in FIXED},
        "quality": {
            "cropResidual": crop_residual, "alignment": [rx, ry], "correlationPeak": float(peak),
            "colourVariety": variety, "usablePixels": int(k_d55.sum()),
            "fitMedian": float(np.nanmedian([m["fitMedian"] for m in measured.values() if "fitMedian" in m])),
            "fit99": measured["D55"]["fit99"],
            "fixed": fixed, "fixedWorst": max(fixed.values()),
            "custom": custom, "customWorst": custom[worst_custom], "customWorstAt": worst_custom,
            "asShot": as_shot_de[0], "asShot99": as_shot_de[1],
            "asShotTemperature": [float(as_shot_t), float(as_shot_tint)],
            "appleAsShot": [measured["as-shot"]["T"], measured["as-shot"]["tint"]],
            "asShotNeutralGap": as_shot_gap, "gridFromNeighbours": sparse,
        },
    }
    json.dump(result, open(os.path.join(result_dir, "calibration.json"), "w"), indent=1)
    return result


def summary(r):
    q = r["quality"]
    return (f"{r['make']} {r['model']} ({r['decoder']}): crop {r['crop']} (off by {q['cropResidual']:.2f} px, "
            f"aligned to {max(map(abs, q['alignment'])):.2f} px), variety {q['colourVariety']:.3f}; ΔE at the app's "
            f"white balances {q['fixedWorst']:.2f} at worst, Custom off the grid {q['customWorst']:.2f} at worst "
            f"({q['customWorstAt']}), As shot "
            f"{q['asShot']:.2f} ({q['asShotTemperature'][0]:.0f} K {q['asShotTemperature'][1]:+.1f}, Apple "
            f"{q['appleAsShot'][0]:.0f} K {q['appleAsShot'][1]:+.1f})")


def transform(profile, temperature, tint):
    """A camera's Apple transform at a temperature and tint (Custom): the
    grid's, interpolated in mired and tint, as the port computes it
    (engine/AppleColour)"""
    g = profile["grid"]
    order = np.argsort([1e6 / t for t in g["temperatures"]])
    mireds = np.array([1e6 / g["temperatures"][i] for i in order])
    tints = np.array(g["tints"], float)
    a = np.array(g["transforms"])[order]                              # [mired][tint][3][3]
    m = float(np.clip(1e6 / temperature, mireds[0], mireds[-1]))
    n = float(np.clip(tint, tints[0], tints[-1]))
    i = int(np.clip(np.searchsorted(mireds, m) - 1, 0, len(mireds) - 2))
    j = int(np.clip(np.searchsorted(tints, n) - 1, 0, len(tints) - 2))
    fi = (m - mireds[i]) / (mireds[i + 1] - mireds[i])
    fj = (n - tints[j]) / (tints[j + 1] - tints[j])
    return ((1 - fi) * (1 - fj) * a[i, j] + fi * (1 - fj) * a[i + 1, j]
            + (1 - fi) * fj * a[i, j + 1] + fi * fj * a[i + 1, j + 1])


def neutral_of(a):
    """The camera RGB a transform takes to grey, largest channel 1"""
    w = np.linalg.solve(a, np.ones(3))
    return w / w.max()


def grid_temperature(profile, neutral):
    """The temperature and tint whose transform in the grid has this neutral:
    a search over mired and tint, then finer around the best, three times;
    only where the grid's neutral is a colour"""
    target = np.log(np.array(neutral)[[0, 2]] / np.array(neutral)[1])
    g = profile["grid"]
    mireds = np.sort([1e6 / t for t in g["temperatures"]])
    tints = np.array(g["tints"], float)
    best = None
    span = [(mireds[0], mireds[-1]), (tints[0], tints[-1])]
    for _ in range(4):
        ms = np.linspace(*span[0], 21) if best else np.linspace(mireds[0], mireds[-1], 81)
        ns = np.linspace(*span[1], 21) if best else np.linspace(tints[0], tints[-1], 41)
        for m in ms:
            for n in ns:
                w = neutral_of(transform(profile, 1e6 / m, n))
                if w.min() <= 0:
                    continue
                d = float(((np.log(w[[0, 2]] / w[1]) - target) ** 2).sum())
                if best is None or d < best[0]:
                    best = (d, m, n)
        _, m, n = best
        sm, sn = (span[0][1] - span[0][0]) / 10, (span[1][1] - span[1][0]) / 10
        span = [(max(mireds[0], m - sm), min(mireds[-1], m + sm)), (max(tints[0], n - sn), min(tints[-1], n + sn))]
    return 1e6 / best[1], best[2]


def transform_for_neutral(profile, neutral):
    """As shot: the grid's transform where the grid has this neutral, with the
    camera's own neutral in place of the grid's"""
    t, tint = grid_temperature(profile, neutral)
    a = transform(profile, t, tint)
    camera = np.array(neutral, float) / max(neutral)
    return a @ np.diag(neutral_of(a) / camera)


if __name__ == "__main__":
    size = int(sys.argv[sys.argv.index("--size") + 1]) if "--size" in sys.argv else 1200
    print(summary(calibrate(sys.argv[1], sys.argv[2], size)))
