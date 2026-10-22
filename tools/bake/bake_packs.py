#!/usr/bin/env python
"""Bake spektrafilm (agx-emulsion) profiles into FLX Film Lab data packs.

The Metal engine ships no Python. Everything it needs is emitted here:

  packs/shared.json          CMFs, illuminants, dichroic filters, neutral filter DB
  packs/<stock>.json         one profile: curves, spectra, metadata
  packs/<stock>.tclut        filming stocks only: 128x128x3 float32 chromaticity LUT

The tclut is the load-bearing artefact. `compute_hanatos2025_tc_lut` collapses
the Hanatos-2025 spectral reconstruction *and* the film's 81-band sensitivity
projection into a single 2D texture indexed by tri-quad chromaticity, so the
runtime film-exposure step is a texture fetch instead of a spectral integral.

Run:  conda activate flx-bake && python tools/bake/bake_packs.py
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys
from pathlib import Path

import colour
import numpy as np

from spektrafilm.config import SPECTRAL_SHAPE, STANDARD_OBSERVER_CMFS
from spektrafilm.model.color_filters import custom_dichroic_filters
from spektrafilm.model.illuminants import standard_illuminant
from spektrafilm.profiles.io import load_profile, profile_from_dict
from spektrafilm.utils.gamut_compression import InputGamutCompressSpec
from spektrafilm.utils.spectral_upsampling import compute_hanatos2025_tc_lut

import stock_overrides  # noqa: E402  (tools/bake, next to this script)

REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / "FLXFilmLab" / "Resources" / "packs"

# The working space the app decodes RAW into (CGColorSpace.extendedLinearITUR_2020).
# Matching agx's own `_rgb_to_tc_b` default keeps the parity tests honest.
WORKING_COLOR_SPACE = "ITU-R BT.2020"

# Illuminants the engine may be asked for at runtime, beyond the ones named by
# the profiles themselves.
EXTRA_ILLUMINANTS = ("TH-KG3", "TH-KG3-L", "D50", "D55", "D65", "T", "K75P")

# Output spaces the app can deliver. sRGB and Display P3 cover the screen;
# the wide ones are for 16-bit TIFF masters headed elsewhere.
OUTPUT_COLOR_SPACES = ("sRGB", "Display P3", "ITU-R BT.2020", "ProPhoto RGB")


# Profiles this repo builds from datasheets (tools/bake/build_profiles.py).
# They live here rather than inside the installed package so they are versioned
# with the app and survive reinstalling spektrafilm.
LOCAL_PROFILES = REPO / "profiles"


def _spektrafilm_data_dir() -> Path:
    import spektrafilm

    return Path(spektrafilm.__file__).parent / "data"


def read_profile(stock: str):
    """Load a profile, preferring this repo's own over the bundled ones."""
    local = LOCAL_PROFILES / f"{stock}.json"
    if local.exists():
        return profile_from_dict(json.loads(local.read_text()))
    return load_profile(stock)


def available_stocks() -> list[str]:
    bundled = {os.path.splitext(os.path.basename(p))[0]
               for p in glob.glob(str(_spektrafilm_data_dir() / "profiles" / "*.json"))}
    local = {p.stem for p in LOCAL_PROFILES.glob("*.json")} if LOCAL_PROFILES.exists() else set()
    return sorted(bundled | local)


def _finite_list(a) -> list:
    """JSON has no NaN. Emit null and let Swift decide (it uses the mask below)."""
    a = np.asarray(a, dtype=float)
    return [None if not np.isfinite(v) else float(v) for v in a.ravel().tolist()]


def _f(a) -> list:
    """Flatten to a JSON list of float32-safe numbers.

    Values below float32's smallest normal are flushed to zero: Swift's
    JSONDecoder refuses to decode a Double literal that has no float32
    representation, and these are all numerical dust from the spectral fits.
    """
    a = np.asarray(a, dtype=float)
    if not np.isfinite(a).all():
        raise ValueError("non-finite value in an array that must be clean")
    a = np.where(np.abs(a) < 1e-37, 0.0, a)
    return [float(v) for v in np.float32(a).ravel().tolist()]


def spectral_valid_mask(channel_density, base_density) -> np.ndarray:
    """Wavelengths where the dye/base data is real.

    agx zeroes non-finite transmittance in `utils/conversions.density_to_light`
    (`transmitted[np.isnan(transmitted)] = 0`). Datasheet spectra stop short of
    the 380-780 nm grid, so the edges are NaN and end up contributing nothing.
    We hoist that into an explicit per-wavelength mask so the shader can just
    multiply, and NaN never enters the GPU at all.
    """
    n = np.asarray(channel_density).shape[0]
    valid = np.isfinite(np.asarray(channel_density)).all(axis=1)
    if np.asarray(base_density).size:
        valid &= np.isfinite(np.asarray(base_density))
    assert valid.shape == (n,)
    return valid.astype(float)


def rgb_to_xyz_matrix(reference_illuminant: str) -> np.ndarray:
    """Linear working RGB -> XYZ, CAT16-adapted to the stock's reference illuminant.

    agx does this per pixel inside `_rgb_to_tc_b` via `colour.RGB_to_XYZ(...,
    chromatic_adaptation_transform='CAT16')`. It is linear, so we collapse it to
    one 3x3 by pushing the identity basis through.
    """
    from spektrafilm.utils.spectral_upsampling import _illuminant_to_xy

    illu_xy = _illuminant_to_xy(reference_illuminant)
    basis = np.eye(3)
    xyz = colour.RGB_to_XYZ(
        basis,
        colourspace=WORKING_COLOR_SPACE,
        apply_cctf_decoding=False,
        illuminant=illu_xy,
        chromatic_adaptation_transform="CAT16",
    )
    # rows of `xyz` are the images of the basis vectors, so the matrix is its transpose
    return np.asarray(xyz).T


# Black-and-white films: diffuse RMS granularity (x1000) at net density 1.0
# through a 48 um aperture, the ISO 10505 reading datasheets quote.
BW_RMS_GRANULARITY = {
    "kodak_tri_x_400": 17.0,    # Kodak F-4017, HC-110 (B)
    "ilford_hp5_plus": 17.0,    # Ilford publishes none; same class of emulsion as Tri-X
    # Fujifilm publishes none, in the English or the Japanese AF3-608E. For one
    # emulsion technology granularity goes as the square root of speed, and in
    # D-76 the two sheet curves put the ISO speed points at 721 and 400:
    # Tri-X's 17 x sqrt(721 / 400).
    "fujifilm_neopan_1600": 23.0,
    # Kodak gives no figure for a push, only that graininess rises. The grains
    # are the same ones, but each that develops carries more silver: Selwyn's
    # granularity goes as the square root of the density per grain, which rises
    # with the slope, 0.718 at the push time against 0.581 at the normal one:
    # 17 x sqrt(0.718 / 0.581).
    "kodak_tri_x_400_push2": 19.0,
    "agfa_apx_25": 7.0,         # Agfa's range sheet, REFINAL 6 min at 20 C
}


# The pixel pitch the grain is calibrated at: a 6000 px export of a 35 mm frame.
GRAIN_REFERENCE_PITCH_UM = 6.0


def softened_aperture_variance(blur_px: float, pitch_um: float) -> float:
    """How much of a 48 um aperture's grain variance survives the softening.

    The engine softens grain by agx's `blur` in pixels, which moves part of
    each pixel's noise into its neighbours — and across the edge of an
    aperture only a few pixels wide. Per axis, each noise pixel counts with the
    share of its kernel that lands inside the aperture.
    """
    radius = int(np.ceil(3 * blur_px))
    w = np.exp(-np.arange(-radius, radius + 1) ** 2 / (2 * blur_px ** 2))
    w /= w.sum()
    k = max(int(round(np.sqrt(np.pi) * 24.0 / pitch_um)), 1)
    per_axis = float(np.sum(np.convolve(np.ones(k), w) ** 2) / k)
    return per_axis ** 2


def bw_particle_area(profile, rms: float, density_min: float, uniformity: float, blur: float) -> float:
    """Particle area that gives the engine's grain the datasheet's RMS granularity.

    The engine draws, per pixel, Poisson(n / s) seeds of which Binomial(p)
    develop, each worth D_max / n (`film_grain` in flx.metal; agx
    `layer_particle_model`). Its variance is D_max s D / n with s = 1 - p u and
    p = D / D_max, and n = pixel area / particle area. Averaged over a 48 um
    aperture that leaves D_max s D a / A_aperture whatever the pixel size —
    Selwyn's law — less what the softening carries out of the aperture, which
    does depend on the pitch: a tenth or so at the reference pitch. Solved for
    the particle area a.
    """
    curves = np.asarray(profile.data.density_curves)
    d_max = float(np.nanmax(curves) - np.nanmin(curves)) + density_min
    d = 1.0 + density_min
    s = 1.0 - (d / d_max) * uniformity
    aperture = np.pi * 24.0 ** 2
    survives = softened_aperture_variance(blur, GRAIN_REFERENCE_PITCH_UM) if blur > 0.4 else 1.0
    return (rms / 1000.0) ** 2 * aperture / (d_max * s * d * survives)


def bw_defaults(profile, out: dict) -> dict:
    """One silver layer: halation without colour, uniform grain, and the
    sharpness of Kodak's measured MTF (`bw_mtf.py`)."""
    import bw_mtf

    hal = out["halation"]
    # The emulsion sits on the base as a colour film's bottom (red) layer does,
    # so it takes that layer's halation, on every channel: a neutral glow.
    for key in ("halationStrength",):
        hal[key] = [hal[key][0]] * 3
    hal["halationFirstSigmaUm"] = [float(np.mean(hal["halationFirstSigmaUm"]))] * 3

    # Scatter, and development's adjacency effect run as the single layer's
    # own DIR inhibition, both fitted to the stock's own sheet MTF where it
    # publishes one (`bw_mtf.SHEET_MTF`), to Kodak's Tri-X MTF otherwise. A
    # colour film's scatter with no edge effect left Tri-X at a third of
    # Kodak's response at 24 cycles/mm. The inhibition is set from this
    # stock's own curve slope at mid-grey, so it gets the fitted response.
    own = bw_mtf.SHEET_MTF.get(profile.info.stock)
    fit = bw_mtf.fit(own() if own else None)
    hal.update(scatterCoreUm=[fit["scatterCoreUm"]] * 3,
               scatterTailUm=[fit["scatterTailUm"]] * 3,
               scatterTailWeight=[fit["scatterTailWeight"]] * 3)
    curve = np.asarray(profile.data.density_curves)[:, 0]
    log_exposure = np.asarray(profile.data.log_exposure)
    ok = np.isfinite(curve)
    slope = float(np.interp(0.0, log_exposure[ok], np.gradient(curve[ok], log_exposure[ok])))
    gamma = bw_mtf.inhibition_gamma(fit["inhibition"], slope)
    out["dirCouplers"] = {
        "active": True, "amount": 1.0,
        "inhibitionSameLayer": 1.0, "inhibitionInterLayer": 0.0,
        "gammaSameLayerRGB": [gamma] * 3,
        "gammaInterlayerRtoGB": [0.0, 0.0], "gammaInterlayerGtoRB": [0.0, 0.0],
        "gammaInterlayerBtoRG": [0.0, 0.0],
        "diffusionSizeUm": fit["diffusionSizeUm"], "diffusionTailUm": 0.0, "diffusionTailWeight": 0.0,
    }
    grain = out["grain"]
    density_min = float(np.mean(grain["densityMin"]))
    uniformity = float(np.mean(grain["uniformity"]))
    area = bw_particle_area(profile, BW_RMS_GRANULARITY[profile.info.stock], density_min, uniformity,
                            float(grain["blur"]))
    grain.update(particleAreaUm2=area, particleScale=[1.0, 1.0, 1.0],
                 densityMin=[density_min] * 3, uniformity=[uniformity] * 3)
    return out


def _bundled_stocks() -> set[str]:
    return {os.path.splitext(os.path.basename(p))[0]
            for p in glob.glob(str(_spektrafilm_data_dir() / "profiles" / "*.json"))}


def _built_from(stock: str) -> str | None:
    """The donor a stock was built on from its datasheet (`build_profiles`), if any."""
    import build_profiles
    for build in build_profiles.BUILDS:
        if build.stock == stock and build.unmix:
            return build.donor
    return None


def _donor_for(profile) -> str:
    """A bundled stock with the same character, for default lookup."""
    built_from = _built_from(profile.info.stock)
    if built_from is not None:
        return built_from
    if profile.info.type == "positive":
        return "kodak_kodachrome_64" if "kodachrome" in profile.info.stock else "fujifilm_velvia_100"
    return "kodak_portra_400"


def digested_for(profile):
    """agx's digested parameters for a stock, as its pack takes them.

    `digest_params` mutates DIR-coupler gammas by profile type (and by stock for
    the two Fuji slide films) and seeds the halation baseline from the
    `(use, antihalation)` tags. Running the real functions here keeps the packs
    honest if upstream retunes those tables.
    """
    from spektrafilm.runtime.params_builder import digest_params, init_params

    # `init_params` only resolves profiles bundled with spektrafilm, so a
    # locally built stock stands in with its donor for the purpose of reading
    # back the digest-time defaults, then has its own profile swapped in. The
    # defaults depend on `info.type`/`use`/`antihalation`, which the donor
    # shares.
    stock = profile.info.stock
    bundled = set(load_profile.__module__ and _bundled_stocks())
    proxy = stock if stock in bundled else _donor_for(profile)
    if profile.info.stage == "filming":
        params = init_params(film_profile=proxy, print_profile="kodak_portra_endura")
    else:
        params = init_params(film_profile="kodak_portra_400", print_profile=proxy)
    params.film = profile if profile.info.stage == "filming" else params.film
    params.settings.neutral_print_filters_from_database = False
    digested = digest_params(params)
    donor = _built_from(stock)
    if donor is not None and donor != stock:
        # A stock built on a donor's dyes takes its couplers too, including
        # any agx tunes for that stock by name (Provia 100F's, for instance).
        donor_params = init_params(film_profile=donor, print_profile="kodak_portra_endura")
        donor_params.settings.neutral_print_filters_from_database = False
        donor_couplers = digest_params(donor_params).film_render.dir_couplers
        for name in ("gamma_samelayer_rgb", "gamma_interlayer_r_to_gb",
                     "gamma_interlayer_g_to_rb", "gamma_interlayer_b_to_rg"):
            setattr(digested.film_render.dir_couplers, name, getattr(donor_couplers, name))
    return digested


def stock_defaults(profile) -> dict:
    """The per-stock defaults a pack carries: agx's digested parameters, with
    this app's departures (`stock_overrides`)."""
    digested = digested_for(profile)
    stock = profile.info.stock
    extra = stock_overrides.apply(stock, profile, digested.film_render)

    out: dict = {}
    if profile.info.stage == "filming":
        dc = digested.film_render.dir_couplers
        hal = digested.film_render.halation
        out["dirCouplers"] = {
            "active": bool(dc.active),
            "amount": float(dc.amount),
            "inhibitionSameLayer": float(dc.inhibition_samelayer),
            "inhibitionInterLayer": float(dc.inhibition_interlayer),
            "gammaSameLayerRGB": _f(dc.gamma_samelayer_rgb),
            "gammaInterlayerRtoGB": _f(dc.gamma_interlayer_r_to_gb),
            "gammaInterlayerGtoRB": _f(dc.gamma_interlayer_g_to_rb),
            "gammaInterlayerBtoRG": _f(dc.gamma_interlayer_b_to_rg),
            "diffusionSizeUm": float(dc.diffusion_size_um),
            "diffusionTailUm": float(dc.diffusion_tail_um),
            "diffusionTailWeight": float(dc.diffusion_tail_weight),
        }
        out["dirCouplers"].update({k: float(v) for k, v in extra.items()})
        out["halation"] = {
            "active": bool(hal.active),
            "scatterCoreUm": _f(hal.scatter_core_um),
            "scatterTailUm": _f(hal.scatter_tail_um),
            "scatterTailWeight": _f(hal.scatter_tail_weight),
            "halationStrength": _f(hal.halation_strength),
            "halationFirstSigmaUm": _f(hal.halation_first_sigma_um),
            "halationNBounces": int(hal.halation_n_bounces),
            "halationBounceDecay": float(hal.halation_bounce_decay),
            "halationRenormalize": bool(hal.halation_renormalize),
            "boostRange": float(hal.boost_range),
            "protectEV": float(hal.protect_ev),
        }
        grain = digested.film_render.grain
        out["grain"] = {
            "particleAreaUm2": float(grain.particle_area_um2),
            "particleScale": _f(grain.particle_scale),
            "densityMin": _f(grain.density_min),
            "uniformity": _f(grain.uniformity),
            "blur": float(grain.blur),
            "microStructure": _f(grain.micro_structure),
        }
        if profile.info.channel_model == "bw":
            out = bw_defaults(profile, out)
    return out


def bake_shared(out_dir: Path, illuminant_names: set[str]) -> None:
    wl = np.asarray(SPECTRAL_SHAPE.wavelengths, dtype=float)
    illuminants = {}
    for name in sorted(illuminant_names | set(EXTRA_ILLUMINANTS)):
        try:
            illuminants[name] = _f(standard_illuminant(name))
        except Exception as exc:  # a profile naming something exotic shouldn't kill the bake
            print(f"  ! illuminant {name!r} unavailable: {exc}", file=sys.stderr)

    cmfs = np.asarray(STANDARD_OBSERVER_CMFS[:], dtype=float)  # (81, 3)

    neutral_path = _spektrafilm_data_dir() / "filters" / "neutral_print_filters.json"
    with open(neutral_path) as fh:
        neutral_print_filters = json.load(fh)

    # Scan stage: XYZ under the print's viewing illuminant -> output RGB. agx does
    # this per pixel with `colour.XYZ_to_RGB(..., illuminant=illuminant_xy)`, which
    # folds a chromatic adaptation (CAT02, colour's default) in with the primaries
    # matrix. Linear, so one 3x3 per (viewing illuminant, output space).
    xyz_to_rgb: dict[str, dict[str, list]] = {}
    for illum_name in sorted(illuminant_names | set(EXTRA_ILLUMINANTS)):
        try:
            illum = standard_illuminant(illum_name)
        except Exception:
            continue
        normalization = float(np.sum(illum * cmfs[:, 1]))
        illuminant_xyz = np.einsum("k,kl->l", illum, cmfs) / normalization
        illuminant_xy = colour.XYZ_to_xy(illuminant_xyz)
        per_space = {}
        for space in OUTPUT_COLOR_SPACES:
            m = colour.XYZ_to_RGB(
                np.eye(3), colourspace=space, apply_cctf_encoding=False, illuminant=illuminant_xy
            )
            per_space[space] = _f(np.asarray(m).T)
        xyz_to_rgb[illum_name] = per_space
        xyz_to_rgb[illum_name]["_normalization"] = [normalization]
        xyz_to_rgb[illum_name]["_illuminantXYZ"] = _f(illuminant_xyz)

    shared = {
        "wavelengths": _f(wl),
        "nWavelengths": int(wl.size),
        "cmfs": _f(cmfs),  # row-major (81, 3), CIE 1931 2 degree
        "illuminants": illuminants,
        # CMY order, matching `EnlargerParams.{c,m,y}_filter_neutral`
        "dichroicFilters": _f(np.asarray(custom_dichroic_filters.filters, dtype=float)),
        "neutralPrintFilters": neutral_print_filters,
        "workingColorSpace": WORKING_COLOR_SPACE,
        "xyzToRGB": xyz_to_rgb,
        "outputColorSpaces": list(OUTPUT_COLOR_SPACES),
    }
    (out_dir / "shared.json").write_text(json.dumps(shared))
    print(f"  shared.json  ({(out_dir / 'shared.json').stat().st_size / 1024:.0f} KB)"
          f"  illuminants: {', '.join(illuminants)}")


def bake_profile(stock: str, out_dir: Path, tc_lut_size_note: dict) -> dict:
    p = read_profile(stock)
    d = p.data
    is_filming = p.info.stage == "filming"

    dmax = np.nanmax(np.asarray(d.density_curves), axis=0)
    dmin = np.nanmin(np.asarray(d.density_curves), axis=0)

    pack = {
        "schema": 1,
        "stock": p.info.stock,
        "name": p.info.name,
        "type": p.info.type,            # negative | positive
        "support": p.info.support,      # film | paper
        "stage": p.info.stage,          # filming | printing
        "use": p.info.use,
        "antihalation": p.info.antihalation,
        "channelModel": p.info.channel_model,
        "targetPrint": stock_overrides.PAPER.get(p.info.stock, p.info.target_print),
        "referenceIlluminant": p.info.reference_illuminant,
        "viewingIlluminant": p.info.viewing_illuminant,

        "logExposure": _f(d.log_exposure),                       # (256,)
        "densityCurves": _f(d.density_curves),                   # (256, 3) row-major
        "densityCurvesLayers": _f(d.density_curves_layers),      # (256, 3, 3)
        "densityMax": _f(dmax),
        "densityMin": _f(dmin),

        # Linear, not log: agx takes `nan_to_num(10 ** log_sensitivity)`, so an
        # unmeasured wavelength becomes zero sensitivity. Baking the log form
        # would turn those NaNs into 10**0 = 1 — a silent sensitivity spike at
        # exactly the wavelengths where there is no data.
        "sensitivity": _f(np.nan_to_num(10 ** np.asarray(d.log_sensitivity))),  # (81, 3)
        "channelDensity": _f(np.nan_to_num(np.asarray(d.channel_density))),  # (81, 3)
        "baseDensity": _f(np.nan_to_num(np.asarray(d.base_density))) if d.base_density.size else [],
        "spectralValid": _f(spectral_valid_mask(d.channel_density, d.base_density)),

        "curveModel": {
            "modelType": d.density_curves_model.model_type,
            "nChannels": int(d.density_curves_model.n_channels),
            "nLayers": int(d.density_curves_model.n_layers),
            "centers": _f(d.density_curves_model.centers),
            "amplitudes": _f(d.density_curves_model.amplitudes),
            "sigmas": _f(d.density_curves_model.sigmas),
        },

        # Per-stock overrides that agx applies in `digest_params`
        # (`_apply_film_specifics`, `_apply_halation_preset`). Resolved here so
        # the engine reads them as data instead of re-encoding the branch table.
        "defaults": stock_defaults(p),
    }

    if is_filming:
        pack["rgbToXYZ"] = _f(rgb_to_xyz_matrix(p.info.reference_illuminant))

        adaptation = p.hanatos2025_adaptation()
        # Mirrors SettingsParams defaults: window on, surface off, no spectral blur.
        # A profile measured off a datasheet alone (the black-and-white ones)
        # has no fitted window, and gets the plain sensitivity.
        adaptation.apply_window = np.asarray(adaptation.window_params).size > 0
        adaptation.apply_surface = False
        adaptation.spectral_gaussian_blur = 0.0

        sensitivity = np.nan_to_num(10 ** np.asarray(d.log_sensitivity))
        tc_lut = compute_hanatos2025_tc_lut(
            sensitivity,
            adaptation,
            gamut_compress=InputGamutCompressSpec(),  # active by default; baked in
        )
        tc_lut = np.ascontiguousarray(np.asarray(tc_lut, dtype=np.float32))
        assert tc_lut.ndim == 3 and tc_lut.shape[2] == 3, tc_lut.shape
        (out_dir / f"{stock}.tclut").write_bytes(tc_lut.tobytes())
        pack["tcLut"] = {
            "file": f"{stock}.tclut",
            "size": int(tc_lut.shape[0]),
            "channels": 3,
            "format": "float32",
            "applyWindow": bool(adaptation.apply_window),
            "applySurface": False,
            "inputGamutCompress": "xy",
        }
        tc_lut_size_note[stock] = tc_lut.shape
        if p.info.channel_model == "bw":
            pack["lensFilters"] = bake_lens_filters(stock, sensitivity, adaptation, out_dir,
                                                    p.info.reference_illuminant)

    (out_dir / f"{stock}.json").write_text(json.dumps(pack))
    return pack


def bake_lens_filters(stock: str, sensitivity: np.ndarray, adaptation, out_dir: Path,
                      reference_illuminant: str) -> list[dict]:
    """One exposure LUT per camera filter, for a black-and-white film.

    The film sees the scene through the filter: its sensitivity times the
    filter's transmittance (tools/bake/lens_filters.py, Kodak's Wratten 2
    data). The exposure is opened up by the filter factor, as a photographer
    does, so mid-grey still exposes 1 and only colours move: a red filter
    darkens a blue sky without darkening the picture.
    """
    import lens_filters
    from spektrafilm.utils.spectral_upsampling import rgb_to_raw_hanatos2025

    baked = []
    grey = np.full((1, 1, 3), 0.184)
    for lens_filter in lens_filters.FILTERS:
        filtered = sensitivity * lens_filters.transmittance(lens_filter)[:, None]
        lut = compute_hanatos2025_tc_lut(filtered, adaptation, gamut_compress=InputGamutCompressSpec())
        raw = rgb_to_raw_hanatos2025(grey, filtered, color_space=WORKING_COLOR_SPACE,
                                     apply_cctf_decoding=False,
                                     reference_illuminant=reference_illuminant, tc_lut=lut)
        factor = 1.0 / float(np.ravel(raw)[0])     # the unfiltered film's mid-grey exposes 1
        lut = np.ascontiguousarray(np.asarray(lut, dtype=np.float64) * factor, dtype=np.float32)
        name = f"{stock}.{lens_filter.id}.tclut"
        (out_dir / name).write_bytes(lut.tobytes())
        baked.append({"id": lens_filter.id, "name": lens_filter.name, "wratten": lens_filter.wratten,
                      "file": name, "factor": factor})
        print(f"    {lens_filter.name:7s} Wratten {lens_filter.wratten:3s} filter factor {factor:5.2f}")
    return baked


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--only", nargs="*", help="bake only these stocks")
    args = ap.parse_args()

    out_dir: Path = args.out
    out_dir.mkdir(parents=True, exist_ok=True)

    stocks = available_stocks()
    if args.only:
        stocks = [s for s in stocks if s in set(args.only)]

    print(f"baking {len(stocks)} profiles -> {out_dir}")
    index = {"schema": 1, "filming": [], "printing": []}
    index_path = out_dir / "index.json"
    if args.only and index_path.exists():
        # Rebaking a few stocks leaves the rest of the index as it was.
        index = json.loads(index_path.read_text())
        for stage in ("filming", "printing"):
            index[stage] = [e for e in index[stage] if e["stock"] not in set(stocks)]
    illuminant_names: set[str] = set()
    tc_lut_shapes: dict = {}

    for stock in stocks:
        pack = bake_profile(stock, out_dir, tc_lut_shapes)
        illuminant_names.add(pack["referenceIlluminant"])
        illuminant_names.add(pack["viewingIlluminant"])
        entry = {
            "stock": pack["stock"],
            "name": pack["name"],
            "type": pack["type"],
            "support": pack["support"],
            "targetPrint": pack["targetPrint"],
            "channelModel": pack["channelModel"],
            "use": pack["use"],
        }
        index["filming" if pack["stage"] == "filming" else "printing"].append(entry)
        shape = tc_lut_shapes.get(stock)
        print(f"  {stock:34s} {pack['stage']:8s} {pack['type']:8s}"
              f"{'  tclut ' + str(shape) if shape else ''}")

    for stage in ("filming", "printing"):
        index[stage].sort(key=lambda e: e["stock"])
    index_path.write_text(json.dumps(index))
    bake_shared(out_dir, illuminant_names)

    total = sum(f.stat().st_size for f in out_dir.iterdir() if f.is_file())
    print(f"\n{len(index['filming'])} filming + {len(index['printing'])} printing profiles, "
          f"{total / 1e6:.1f} MB total")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
