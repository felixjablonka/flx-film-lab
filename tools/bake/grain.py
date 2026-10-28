"""Grain: how coarse each stock's grain is at every density, how it is shared
between its emulsion layers, and how large their dye clouds or silver clumps
are — from the stock's own datasheet.

What a pack carries, and the engine draws (`film_grain_layer`, flx.metal):
for each colour record (or a black-and-white film's one silver record) and
each of its fast, medium and slow emulsion layers, a Selwyn coefficient
against the record's net density. Granularity follows Selwyn's law: a
layer's density fluctuation, averaged over an area A, has variance S / A. So
a pixel of area A draws zero-mean noise of that variance, which is then
spread over the layer's clouds — their size is micrometres on the film,
whatever the pixel pitch — and the three layers add. Through the sheets' 48 um
aperture the layers give the record's RMS granularity back, the clouds
keeping `survival` of it inside.

Where each stock's figures come from:

- Fujifilm, and Agfa's range sheet: diffuse RMS granularity at a *visual*
  diffuse density of 1.0 over the minimum, 48 um aperture — colour negatives
  included (the Eterna sheets say so).
- Kodak slides: the same at a *gross* visual density of 1.0.
- Black-and-white: at a net silver density of 1.0.
- Kodak's colour negatives for stills: a Print Grain Index only, a
  perceptual scale on which four units are a just-noticeable difference.
  Doubling the magnification adds 22 at the 4x6 inch print size (the
  sheets' own 4.4x to 8.8x steps), so a difference of d units is a factor
  2 ** (d / 22) in granularity. The scale is anchored at Portra 400 = 4.0,
  where Kodak's own VISION3 curves put its 250D and 500T (3.7 and 4.0),
  and which Fujifilm gives its Pro 400H and X-TRA 400.
- Kodak VISION3: each layer's granularity against density
  (`vision3_granularity.py`). Every colour negative follows that measured
  shape, 50D's and 250D's averaged, at its own sheet's level: blue
  (yellow-dye) grain about twice the red and green, and highest just above
  the fog, where the coarse fast emulsion alone has developed.
- Slides and black-and-white films publish no curve: theirs is agx's
  counting model's (`layer_particle_model` on spektrafilm's sub-layers),
  at the sheet's level.

A density's grain is shared between the layers as agx's counting model shares
it: in proportion to each sub-layer's density, its particle size
(spektrafilm's fast-to-slow 2 : 1 : 0.5) and how far it is from filling up.
In the toe the fast emulsion holds it all; in the densest parts the slow ones.

A visual density is the ISO one: illuminant A through the CIE luminous
efficiency, on the profile's own dyes and base. Its granularity is that of
the records weighted by how much each moves it, the records' grain being
independent.
"""

from __future__ import annotations

import numpy as np
import colour
from scipy.special import j1

from spektrafilm.config import SPECTRAL_SHAPE

import vision3_granularity

APERTURE_RADIUS_UM = 24.0
APERTURE_AREA = np.pi * APERTURE_RADIUS_UM ** 2
TABLE_SAMPLES = 64
DENSITY_MIN = 0.03                     # agx's fog density, shared out over the sub-layers
UNIFORMITY = {"color": (0.97, 0.99, 0.97), "bw": (0.9767, 0.9767, 0.9767)}   # agx's

# Sub-layer particle areas, fast, medium and slow: spektrafilm's
# (GrainParams.particle_scale_layers); a slide's colour records': agx's
# (particle_scale). They shape the slides' and black-and-white films' curves
# and share every stock's grain between its layers.
SUBLAYER_SCALE = (2.0, 1.0, 0.5)
SLIDE_CHANNEL_SCALE = (1.0, 1.0, 2.0)

# Dye clouds and silver clumps: for the same density in each, Selwyn's
# coefficient grows with a cloud's area, so a stock's clouds are as much larger
# than its kind's reference as its granularity is higher — the medium
# layer's; the fast one's are SUBLAYER_CLOUD times larger, the slow one's
# smaller. References: Portra 400 (dye clouds), Provia 100F (a slide's, which
# reads coarser for its contrast) and Tri-X (silver clumps).
CLOUD_REFERENCE = {"negative": (4.0, 4.0), "positive": (8.0, 3.0), "bw": (17.0, 2.5)}   # sheet RMS, um
SUBLAYER_CLOUD = (2 ** 0.5, 1.0, 2 ** -0.5)
CLOUD_LIMITS_UM = (0.5, 10.0)

PGI_PER_DOUBLING = 22.0
PGI_ANCHOR = ("kodak_portra_400", 37.0, 4.0)


def _pgi(pgi_4x6: float) -> float:
    _, ref_pgi, ref_rms = PGI_ANCHOR
    return round(ref_rms * 2 ** ((pgi_4x6 - ref_pgi) / PGI_PER_DOUBLING), 2)


# stock -> (RMS granularity x 1000, what density it is read at, where it is from)
SHEET_RMS: dict[str, tuple[float, str, str]] = {
    # colour negatives
    "fujifilm_c200": (4.0, "visual", "Fujifilm C200 sheet"),
    "fujifilm_pro_400h": (4.0, "visual", "Fujifilm Pro 400H sheet"),
    "fujifilm_superia_reala_100": (4.0, "visual", "Fujifilm Superia Reala sheet"),
    "fujifilm_xtra_400": (4.0, "visual", "Fujifilm Professional Data Guide, Superia X-TRA 400"),
    "fujifilm_superia_1600": (7.0, "visual", "Fujifilm Superia 1600 sheet"),
    "fujifilm_eterna_500t": (3.5, "visual", "Fujifilm Eterna 500 sheet"),
    "fujifilm_eterna_vivid_160t": (3.5, "visual", "Fujifilm Eterna Vivid 160 sheet"),
    "fujifilm_reala_500d": (3.5, "visual", "none published; Fujifilm's Eterna 500, of its generation and speed"),
    "agfa_optima_ii_100": (4.0, "visual", "Agfa range sheet (visual filter)"),
    "agfa_portrait_xps_160": (3.5, "visual", "Agfa range sheet (visual filter)"),
    "kodak_portra_160": (_pgi(28), "visual", "Kodak Print Grain Index 28 (4x6)"),
    "kodak_portra_400": (_pgi(37), "visual", "Kodak Print Grain Index 37 (4x6), the scale's anchor"),
    "kodak_portra_800": (_pgi(48), "visual", "Kodak Print Grain Index 48 (4x6)"),
    # Ektar is below the index's threshold (25) at 4x6; at 8x10 it is 38
    # against Portra 400's 59, where the index rises about 25 per doubling.
    "kodak_ektar_100": (round(4.0 * 2 ** ((38 - 59) / 25), 2), "visual", "Kodak Print Grain Index 38 (8x10)"),
    "kodak_gold_200": (_pgi(44), "visual", "Kodak Print Grain Index 44 (4x6)"),
    "kodak_royal_gold_100": (_pgi(28), "visual", "Kodak Print Grain Index 28 (4x6)"),
    "kodak_vericolor_iii_160": (_pgi(39), "visual", "Kodak Print Grain Index 39 (4x6)"),
    "kodak_ultramax_400": (5.0, "visual", "no sheet; Kodak's consumer line, as Gold 200"),
    "harman_phoenix_200": (7.0, "visual", "none published ('strong visible grain'); as Superia 1600"),
    # slides
    "fujifilm_provia_100f": (8.0, "visual", "Fujifilm Provia 100F sheet"),
    "fujifilm_provia_400x": (11.0, "visual", "Fujifilm Provia 400X sheet"),
    "fujifilm_velvia_50": (9.0, "visual", "Fujifilm Velvia 50 sheet"),
    "fujifilm_velvia_100": (8.0, "visual", "Fujifilm Velvia 100 sheet"),
    "fujifilm_astia_100f": (7.0, "visual", "Fujifilm Astia 100F sheet"),
    "kodak_ektachrome_100": (8.0, "visual-gross", "Kodak E100 sheet"),
    "kodak_kodachrome_25": (9.0, "visual-gross", "Kodak Kodachrome sheet"),
    "kodak_kodachrome_64": (10.0, "visual-gross", "Kodak Kodachrome sheet"),
    "kodak_kodachrome_200": (16.0, "visual-gross", "Kodak Kodachrome sheet"),
    # black-and-white: bake_packs.BW_RMS_GRANULARITY, which says how the
    # unpublished ones are derived
}

# VISION3: Kodak's own per-layer curves. 200T's brochure repeats 500T's plot,
# so 200T and Verita 200D are interpolated between 50D and 250D on a
# log-speed scale — 200T at the 100-speed image structure Kodak claims for
# it, Verita 200D at its own speed.
VISION3_MEASURED = ("kodak_vision3_50d", "kodak_vision3_250d", "kodak_vision3_500t")
VISION3_INTERPOLATED = {
    "kodak_vision3_200t": (np.log(100 / 50) / np.log(250 / 50), "VISION3 50D and 250D curves, at Kodak's '100-speed image structure'"),
    "kodak_verita_200d": (np.log(200 / 50) / np.log(250 / 50), "VISION3 50D and 250D curves, at its speed"),
}

# Pushes: the same grains, each developed to more density (Selwyn's
# granularity goes as the root of the density per grain), so the normal
# stock's figure times the root of the slope ratio at mid-grey.
PUSHED = {"kodak_portra_800_push1": "kodak_portra_800", "kodak_portra_800_push2": "kodak_portra_800"}


# --- the profile's layers ----------------------------------------------------

def _curves(profile):
    dc = np.asarray(profile.data.density_curves, dtype=float)
    return dc - np.nanmin(dc, axis=0)


def table_densities(profile) -> np.ndarray:
    """The densities the tables are sampled at, [channel][sample]: 0 to the
    curve's maximum."""
    top = np.nanmax(_curves(profile), axis=0)
    return np.linspace(0.0, 1.0, TABLE_SAMPLES)[None, :] * top[:, None]


def sublayer_shares(profile) -> np.ndarray:
    """The share of a channel's net density in each sub-layer at the table's
    densities, [channel][sample][sub-layer], mapped through the curves as
    spektrafilm's `interp_density_cmy_layers` does — through the negated
    curves for a slide."""
    dc = _curves(profile)
    layers = np.asarray(profile.data.density_curves_layers, dtype=float)   # [logE][sub-layer][channel]
    positive = profile.info.type == "positive"
    grid = table_densities(profile)
    out = np.zeros((3, TABLE_SAMPLES, 3))
    for c in range(3):
        ok = np.isfinite(dc[:, c]) & np.isfinite(layers[:, :, c]).all(axis=1)
        curve, parts = dc[ok, c], layers[ok, :, c]
        x, xs = (-curve, -grid[c]) if positive else (curve, grid[c])
        order = np.argsort(x, kind="stable")
        x, parts = np.maximum.accumulate(x[order]), parts[order]
        shares = np.stack([np.interp(xs, x, parts[:, k]) for k in range(3)], axis=-1)
        total = shares.sum(axis=-1, keepdims=True)
        first = int(np.argmax(total[:, 0] > 1e-6))       # at zero density, the shares just above it
        shares[:first], total[:first] = shares[first], total[first]
        out[c] = shares / np.maximum(total, 1e-12)
    return out


def counting_terms(profile) -> np.ndarray:
    """agx's counting model per sub-layer at the table's densities: the
    variance per unit area of unit-area particles, D'max * s * D', with D'
    and D'max carrying the sub-layer's share of the fog, s = 1 - p * u.
    [channel][sample][sub-layer]."""
    layers = np.asarray(profile.data.density_curves_layers, dtype=float)
    dmax = np.nanmax(layers, axis=0).T                                   # [channel][sub-layer]
    fog = 0.0 if profile.info.type == "positive" else DENSITY_MIN       # a slide's clear base has no grain
    dmin = fog * dmax / np.maximum(dmax.sum(axis=1, keepdims=True), 1e-12)
    dmaxp = dmax + dmin
    u = np.array(UNIFORMITY[profile.info.channel_model])
    dp = sublayer_shares(profile) * table_densities(profile)[:, :, None] + dmin[:, None, :]
    p = np.clip(dp / dmaxp[:, None, :], 1e-6, 1 - 1e-6)
    s = 1 - p * u[:, None, None] * (1 - 1e-6)
    return dmaxp[:, None, :] * s * dp


def layer_split(profile) -> np.ndarray:
    """How a density's grain variance is shared between the sub-layers,
    [channel][sample][sub-layer], summing to one."""
    w = counting_terms(profile) * np.array(SUBLAYER_SCALE)[None, None, :]
    return w / np.maximum(w.sum(axis=-1, keepdims=True), 1e-30)


def survival(cloud_um) -> np.ndarray:
    """The share of a 48 um aperture's grain variance that Gaussian clouds of
    `cloud_um` leave inside it: the aperture's power spectrum times the
    clouds', against the aperture's alone."""
    f = np.linspace(0, 1.0, 40001)[1:]             # cycles per um
    z = 2 * np.pi * APERTURE_RADIUS_UM * f
    disc = (2 * j1(z) / z) ** 2 * 2 * np.pi * f
    cloud = np.atleast_1d(np.asarray(cloud_um, dtype=float)).ravel()
    kernel = np.exp(-4 * np.pi ** 2 * cloud[:, None] ** 2 * f[None, :] ** 2)
    return (np.trapezoid(disc * kernel, f, axis=1) * APERTURE_AREA).reshape(np.shape(cloud_um))


def cloud_size(profile, rms: float) -> np.ndarray:
    """Each sub-layer's cloud size in um, [channel][sub-layer]."""
    kind = "bw" if profile.info.channel_model == "bw" else profile.info.type
    ref_rms, ref_um = CLOUD_REFERENCE[kind]
    lo, hi = CLOUD_LIMITS_UM
    mid = ref_um * rms / ref_rms
    return np.clip(np.outer(np.ones(3), np.array(SUBLAYER_CLOUD) * mid), lo, hi)


# --- visual density -----------------------------------------------------------

def _visual_weighting() -> np.ndarray:
    a = colour.SDS_ILLUMINANTS["A"].copy().align(SPECTRAL_SHAPE).values
    v = colour.colorimetry.MSDS_CMFS["CIE 1931 2 Degree Standard Observer"].copy().align(SPECTRAL_SHAPE).values[:, 1]
    return a * v


def visual_density(profile, cmy) -> np.ndarray:
    dyes = np.nan_to_num(np.asarray(profile.data.channel_density, dtype=float))
    base = np.nan_to_num(np.asarray(profile.data.base_density, dtype=float))
    if base.size == 0:
        base = np.zeros(dyes.shape[0])
    s = _visual_weighting()
    t = 10.0 ** (-(np.asarray(cmy, dtype=float) @ dyes.T + base))
    return -np.log10((t * s).sum(axis=-1) / s.sum())


def measuring_exposure(profile, basis: str) -> float:
    """The log exposure, from mid-grey, of the neutral that develops to the
    density the sheet reads its granularity at."""
    dc = _curves(profile)
    e = np.asarray(profile.data.log_exposure, dtype=float)
    ok = np.isfinite(dc).all(axis=1)
    if basis == "silver":
        reading, target = dc[ok, 0], 1.0
    else:
        reading = visual_density(profile, dc[ok])
        target = 1.0 + (visual_density(profile, np.zeros(3)) if basis == "visual" else 0.0)
    order = np.argsort(reading)
    return float(np.interp(target, reading[order], e[ok][order]))


def measuring_point(profile, basis: str) -> np.ndarray:
    """The neutral exposure's net channel densities where the sheet reads its
    granularity."""
    dc = _curves(profile)
    e = np.asarray(profile.data.log_exposure, dtype=float)
    at = measuring_exposure(profile, basis)
    return np.array([np.interp(at, e[np.isfinite(dc[:, c])], dc[np.isfinite(dc[:, c]), c]) for c in range(3)])


def visual_weights(profile, cmy) -> np.ndarray:
    step = 0.005
    return np.array([(visual_density(profile, cmy + step * np.eye(3)[c])
                      - visual_density(profile, cmy - step * np.eye(3)[c])) / (2 * step) for c in range(3)])


def as_the_sheet_reads(profile, sigma: np.ndarray, basis: str) -> float:
    """RMS granularity x 1000 from per-record curves `sigma` [channel][sample]
    at the table's densities, read as the stock's sheet reads it."""
    grid = table_densities(profile)
    cmy = measuring_point(profile, basis)
    at = np.array([np.interp(cmy[c], grid[c], sigma[c]) for c in range(3)])
    if basis == "silver":
        return 1000 * float(at[0])
    return 1000 * float(np.sqrt(np.sum((visual_weights(profile, cmy) * at) ** 2)))


# --- each stock's curves ------------------------------------------------------

def _kodak_curves(stock) -> dict[int, tuple[np.ndarray, np.ndarray]]:
    return {c: (np.asarray(layer["netDensity"]), np.asarray(layer["sigmaAtNet"]))
            for c, layer in vision3_granularity.read(stock).items() if "sigmaAtNet" in layer}


_SHAPE: dict | None = None


def colour_negative_shape() -> dict[int, tuple[np.ndarray, np.ndarray]]:
    """Kodak's measured granularity against net density, per record, as a
    multiple of the stock's visual granularity: 50D's and 250D's, averaged."""
    global _SHAPE
    if _SHAPE is None:
        from bake_packs import read_profile
        per_stock = []
        for stock in ("kodak_vision3_50d", "kodak_vision3_250d"):
            p = read_profile(stock)
            curves = _kodak_curves(stock)
            sigma = np.array([np.interp(table_densities(p)[c], *curves[c]) for c in range(3)])
            visual = as_the_sheet_reads(p, sigma, "visual") / 1000
            per_stock.append({c: (curves[c][0], curves[c][1] / visual) for c in range(3)})
        _SHAPE = {}
        for c in range(3):
            top = min(ps[c][0].max() for ps in per_stock)
            net = np.linspace(0.0, top, 120)
            ratio = np.exp(np.mean([np.log(np.interp(net, *ps[c])) for ps in per_stock], axis=0))
            _SHAPE[c] = (net, ratio)
    return _SHAPE


def _mid_grey_slope(profile) -> float:
    dc = _curves(profile)
    e = np.asarray(profile.data.log_exposure, dtype=float)
    ok = np.isfinite(dc[:, 1])
    return float(np.interp(0.0, e[ok], np.gradient(dc[ok, 1], e[ok])))


def record_curves(profile, bw_rms: dict[str, float]) -> tuple[np.ndarray, float, str, str, str]:
    """Each record's RMS granularity through the 48 um aperture at the table's
    densities, [channel][sample], and what it is read as: (curves, sheet
    RMS, basis, source, note)."""
    stock = profile.info.stock
    grid = table_densities(profile)
    note = ""

    def on_grid(curves):
        return np.array([np.interp(grid[c], *curves[c]) for c in range(3)])

    if profile.info.channel_model == "bw" or profile.info.type == "positive":
        if profile.info.channel_model == "bw":
            rms, basis, source = bw_rms[stock], "silver", "bake_packs.BW_RMS_GRANULARITY"
            records = np.ones(3)
        else:
            rms, basis, source = SHEET_RMS[stock]
            records = np.array(SLIDE_CHANNEL_SCALE)
        # agx's counting model on the stock's own sub-layers
        terms = counting_terms(profile) * np.array(SUBLAYER_SCALE)[None, None, :] * records[:, None, None]
        sigma = np.sqrt(terms.sum(axis=-1))
        return sigma * rms / as_the_sheet_reads(profile, sigma, basis), rms, basis, source, note

    basis = "visual"
    if stock in VISION3_MEASURED:
        source = "Kodak's per-layer curves (vision3_granularity.py)"
        curves = _kodak_curves(stock)
        if 0 not in curves:
            # 500T's plot draws no red curve: its red follows its green as
            # 50D's and 250D's red follow theirs
            shape = colour_negative_shape()
            net, green = curves[1]
            curves[0] = (net, green * np.interp(net, *shape[0]) / np.interp(net, *shape[1]))
            note = "red record from its green, as 50D's and 250D's red follow theirs"
        sigma = on_grid(curves)
        return sigma, as_the_sheet_reads(profile, sigma, basis), basis, source, note
    if stock in VISION3_INTERPOLATED:
        t, source = VISION3_INTERPOLATED[stock]
        low, high = _kodak_curves("kodak_vision3_50d"), _kodak_curves("kodak_vision3_250d")
        sigma = on_grid(low) ** (1 - t) * on_grid(high) ** t
        return sigma, as_the_sheet_reads(profile, sigma, basis), basis, source, note
    if stock in PUSHED:
        from bake_packs import read_profile
        rms0, basis, source0 = SHEET_RMS[PUSHED[stock]]
        ratio = _mid_grey_slope(profile) / _mid_grey_slope(read_profile(PUSHED[stock]))
        rms = round(rms0 * np.sqrt(ratio), 2)
        source = f"{source0}, times the root of the mid-grey slope ratio {ratio:.3f}"
    else:
        rms, basis, source = SHEET_RMS[stock]
    sigma = on_grid(colour_negative_shape())
    return sigma * rms / as_the_sheet_reads(profile, sigma, basis), rms, basis, source, note


def grain_defaults(profile, bw_rms: dict[str, float]) -> dict:
    """The pack's grain section for a stock."""
    sigma, rms, basis, source, note = record_curves(profile, bw_rms)
    clouds = cloud_size(profile, rms)
    # Selwyn coefficient per sub-layer: its share of the record's variance,
    # through the aperture, before the clouds took their part out of it
    selwyn = (sigma ** 2)[:, :, None] * APERTURE_AREA * layer_split(profile) / survival(clouds)[:, None, :]
    point = measuring_point(profile, basis)
    weights = np.array([1.0, 0.0, 0.0]) if basis == "silver" else visual_weights(profile, point)
    out = {
        "tableDensityMax": [float(v) for v in table_densities(profile)[:, -1]],
        # six figures, and nothing a Float cannot hold: a slide's clear end is all but zero
        "selwynUm2": [float(f"{v:.6g}") if v > 1e-12 else 0.0 for v in selwyn.ravel()],   # [channel][sample][sub-layer], OD^2 um^2
        "cloudUm": [float(v) for v in clouds.ravel()],                    # [channel][sub-layer]
        # what the sheet says, and where it reads it: the records' net
        # densities, how much each moves the reading, the neutral's exposure
        "sheet": {"rms": float(round(rms, 2)), "basis": basis, "source": source,
                  "point": [float(v) for v in point], "weights": [float(v) for v in weights],
                  "logExposure": measuring_exposure(profile, basis)},
    }
    if note:
        out["sheet"]["note"] = note
    return out


def engine_rms(profile, grain: dict) -> float:
    """A pack's grain read back as the stock's sheet reads it."""
    selwyn = np.array(grain["selwynUm2"]).reshape(3, TABLE_SAMPLES, 3)
    clouds = np.array(grain["cloudUm"]).reshape(3, 3)
    sigma = np.sqrt(np.sum(selwyn * survival(clouds)[:, None, :], axis=-1) / APERTURE_AREA)
    return as_the_sheet_reads(profile, sigma, grain["sheet"]["basis"])


if __name__ == "__main__":
    import sys
    from bake_packs import read_profile, available_stocks, BW_RMS_GRANULARITY
    stocks = sys.argv[1:] or [s for s in available_stocks() if read_profile(s).info.stage == "filming"]
    for stock in stocks:
        p = read_profile(stock)
        g = grain_defaults(p, BW_RMS_GRANULARITY)
        sigma = np.sqrt(np.sum(np.array(g["selwynUm2"]).reshape(3, TABLE_SAMPLES, 3)
                               * survival(np.array(g["cloudUm"]).reshape(3, 3))[:, None, :], axis=-1) / APERTURE_AREA)
        grid = table_densities(p)
        picks = [0.1, 0.3, 0.6, 1.0, 1.5]
        curve = " | ".join(f"{'RGB'[c]} " + " ".join(f"{1000 * np.interp(d, grid[c], sigma[c]):4.1f}" for d in picks)
                           for c in range(3))
        fast = layer_split(p)[1, [2, 20, 40], 0]
        print(f"{stock:28s} {g['sheet']['basis']:12s} sheet {g['sheet']['rms']:5.2f} read back {engine_rms(p, g):5.2f}  "
              f"clouds {np.round(np.array(g['cloudUm'])[:3], 1).tolist()}  RMS at net D {picks}: {curve}  "
              f"green fast share {np.round(fast, 2).tolist()}")
