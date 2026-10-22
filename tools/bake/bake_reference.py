#!/usr/bin/env python
"""Render parity references with the real agx-emulsion pipeline.

The Metal engine is a port, not a wrapper, so "it looks like film" is not
evidence that it is correct. This emits ground truth from the Python
implementation at every pipeline tap, so the Swift side can be checked stage by
stage instead of only end-to-end.

Everything runs with `debug.lut_mode = True`, which forces agx into a
deterministic per-pixel transform (no spatial blurs, no grain, no
auto-exposure). Spatial and stochastic stages are verified separately.

Outputs, under tests/parity/:
  manifest.json          cases, taps, shapes, and the fully resolved params
  <case>.<tap>.f32       raw float32, row-major (H, W, C)

Run:  conda activate flx-bake && python tools/bake/bake_reference.py
"""

from __future__ import annotations

import argparse
import dataclasses
import json
from pathlib import Path

import colour
import numpy as np

import spektrafilm.model.develop
from spektrafilm import init_params
from spektrafilm.runtime.params_builder import digest_params
from spektrafilm.runtime.pipeline import SimulationPipeline
from spektrafilm.runtime.topology import Tap

import stock_overrides  # noqa: E402  (tools/bake, next to this script)

# The engine inverts the couplers on a grid this many times finer than the
# density curves' (Constants.couplerOversampling, FilmMath.oversampled): on
# agx's own grid the curve strong couplers invert to is undersampled. The
# references take the same grid, so parity compares equal models.
COUPLER_OVERSAMPLING = 16
_apply_couplers = spektrafilm.model.develop.apply_density_correction_dir_couplers


def _apply_couplers_oversampled(density_cmy, log_raw, pixel_size_um, log_exposure, density_curves,
                                *args, **kwargs):
    n = len(log_exposure)
    fine = np.linspace(log_exposure[0], log_exposure[-1], (n - 1) * COUPLER_OVERSAMPLING + 1)
    curves = np.stack([np.interp(fine, log_exposure, density_curves[:, c]) for c in range(3)], axis=-1)
    return _apply_couplers(density_cmy, log_raw, pixel_size_um, fine, curves, *args, **kwargs)


spektrafilm.model.develop.apply_density_correction_dir_couplers = _apply_couplers_oversampled

REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / "tests" / "parity"

WORKING_COLOR_SPACE = "ITU-R BT.2020"

TAPS = [Tap.RGB_PRE, Tap.LOG_E_FILM, Tap.CMY_FILM, Tap.LOG_E_PRINT, Tap.CMY_PRINT, Tap.RGB_OUT]

# (film, print, scan_film). The scan_film entries cover agx's `io.scan_film`
# path, where the developed film is read directly instead of being printed —
# how the app treats slide stocks by default.
COMBOS = [
    ("kodak_portra_400", "kodak_portra_endura", False),
    ("kodak_ektar_100", "kodak_supra_endura", False),
    ("kodak_vision3_500t", "kodak_2383", False),
    ("fujifilm_velvia_100", "kodak_portra_endura", False),
    ("fujifilm_velvia_100", "kodak_portra_endura", True),
    ("fujifilm_provia_100f", "kodak_portra_endura", True),
]


def case_neutral_ramp() -> np.ndarray:
    """Grey wedge over the full useful exposure range — the density curves' spine."""
    v = np.logspace(-4.0, np.log10(8.0), 256, dtype=np.float64)
    return np.repeat(v.reshape(16, 16, 1), 3, axis=2)


def case_colorchecker() -> np.ndarray:
    """24 ColorChecker patches under D55, the film's own reference illuminant."""
    cc = colour.SDS_COLOURCHECKERS["ColorChecker N Ohta"]
    illuminant = colour.SDS_ILLUMINANTS["D55"]
    cmfs = colour.MSDS_CMFS["CIE 1931 2 Degree Standard Observer"]
    patches = []
    for sd in cc.values():
        xyz = colour.sd_to_XYZ(sd, cmfs, illuminant) / 100.0
        rgb = colour.XYZ_to_RGB(
            xyz,
            colourspace=WORKING_COLOR_SPACE,
            apply_cctf_encoding=False,
            illuminant=colour.CCS_ILLUMINANTS["CIE 1931 2 Degree Standard Observer"]["D55"],
            chromatic_adaptation_transform="CAT16",
        )
        patches.append(rgb)
    return np.asarray(patches, dtype=np.float64).reshape(4, 6, 3)


def case_primaries() -> np.ndarray:
    """Saturated primaries and secondaries across 8 stops.

    These are where a spectral-upsampling port goes wrong first: fully saturated
    Rec.2020 corners sit near the spectral locus, so they exercise the tc_lut's
    edges and the input gamut compression baked into it.
    """
    hues = np.array(
        [[1, 0, 0], [0, 1, 0], [0, 0, 1], [0, 1, 1], [1, 0, 1], [1, 1, 0], [1, 1, 1], [0.5, 0.2, 0.1]],
        dtype=np.float64,
    )
    scales = np.logspace(-2, 1, 8, dtype=np.float64)
    return (hues[None, :, :] * scales[:, None, None]).reshape(8, 8, 3)


def case_random_cube(seed: int = 20260810) -> np.ndarray:
    """Broad random coverage, log-distributed so shadows are sampled properly."""
    rng = np.random.default_rng(seed)
    v = 10 ** rng.uniform(-3.5, 0.9, size=(64, 64, 3))
    return v.astype(np.float64)


CASES = {
    "neutral_ramp": case_neutral_ramp,
    "colorchecker": case_colorchecker,
    "primaries": case_primaries,
    "random_cube": case_random_cube,
}


def build_params(film: str, print_stock: str, gamut_compress: bool, scan_film: bool = False):
    import bake_packs
    params = init_params(film_profile=film, print_profile=print_stock)
    # The profile the pack is baked from: this repo's own where it rebuilds one
    # (`profiles/`), as for the slide films, agx's otherwise.
    params.film = bake_packs.read_profile(film)
    params.io.scan_film = scan_film
    params.io.input_color_space = WORKING_COLOR_SPACE
    params.io.input_cctf_decoding = False
    params.io.output_color_space = "sRGB"
    params.io.output_cctf_encoding = False  # compare linear values, not an encoding
    if not gamut_compress:
        params.io.output_gamut_compress = dataclasses.replace(
            params.io.output_gamut_compress, algorithm="off"
        )
    params.debug.lut_mode = True
    digested = digest_params(params)
    # The references carry the same departures as the packs, so parity checks
    # the engine against agx at equal parameters.
    stock_overrides.apply(film, params.film, digested.film_render)
    return digested


def resolved_params(digested) -> dict:
    e = digested.enlarger
    dc = digested.film_render.dir_couplers
    return {
        "film": digested.film.info.stock,
        "print": digested.print.info.stock,
        "inputColorSpace": digested.io.input_color_space,
        "outputColorSpace": digested.io.output_color_space,
        "outputCctfEncoding": bool(digested.io.output_cctf_encoding),
        "outputGamutCompress": digested.io.output_gamut_compress.algorithm,
        "scanFilm": bool(digested.io.scan_film),
        "enlarger": {
            "illuminant": e.illuminant,
            "printExposure": float(e.print_exposure),
            "printExposureCompensation": bool(e.print_exposure_compensation),
            "normalizePrintExposure": bool(e.normalize_print_exposure),
            "cFilterNeutral": float(e.c_filter_neutral),
            "mFilterNeutral": float(e.m_filter_neutral),
            "yFilterNeutral": float(e.y_filter_neutral),
            "yFilterShift": float(e.y_filter_shift),
            "mFilterShift": float(e.m_filter_shift),
            "preflashExposure": float(e.preflash_exposure),
        },
        "exposureCompensationEV": float(digested.camera.exposure_compensation_ev),
        "dirCouplersActive": bool(dc.active),
        "dirCouplersDiffusionSizeUm": float(dc.diffusion_size_um),
        "grainActive": bool(digested.film_render.grain.active),
        "halationActive": bool(digested.film_render.halation.active),
        "glareActive": bool(digested.print_render.glare.active),
        "densityCurveGamma": float(digested.film_render.density_curve_gamma),
        "printCurvesMorphActive": bool(digested.print_render.density_curves_morph.active),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--gamut-compress", action="store_true",
                    help="leave output gamut compression on (default: off, to isolate the physics)")
    args = ap.parse_args()

    out_dir: Path = args.out
    out_dir.mkdir(parents=True, exist_ok=True)

    manifest = {
        "schema": 1,
        "workingColorSpace": WORKING_COLOR_SPACE,
        "lutMode": True,
        "taps": list(TAPS),
        "combos": [],
    }

    for film, print_stock, scan_film in COMBOS:
        digested = build_params(film, print_stock, args.gamut_compress, scan_film)
        pipeline = SimulationPipeline(digested)
        combo_key = f"{film}__{print_stock}" + ("__scanfilm" if scan_film else "")
        print(f"{combo_key}")

        combo = {"key": combo_key, "params": resolved_params(digested), "cases": {}}

        for case_name, make in CASES.items():
            image = make()
            in_path = out_dir / f"{combo_key}.{case_name}.rgb_in.f32"
            in_path.write_bytes(np.ascontiguousarray(image, dtype=np.float32).tobytes())

            entry = {
                "shape": list(image.shape),
                "input": in_path.name,
                "taps": {},
                "stats": {},
            }
            taps = [t for t in TAPS if not (scan_film and t in (Tap.LOG_E_PRINT, Tap.CMY_PRINT))]
            for tap in taps:
                result = pipeline.process(image, collect=tap)
                result = np.ascontiguousarray(np.asarray(result), dtype=np.float32)
                path = out_dir / f"{combo_key}.{case_name}.{tap}.f32"
                path.write_bytes(result.tobytes())
                entry["taps"][tap] = {"file": path.name, "shape": list(result.shape)}
                finite = result[np.isfinite(result)]
                entry["stats"][tap] = {
                    "min": float(finite.min()) if finite.size else None,
                    "max": float(finite.max()) if finite.size else None,
                    "mean": float(finite.mean()) if finite.size else None,
                    "nonFinite": int(result.size - finite.size),
                }
            combo["cases"][case_name] = entry
            s = entry["stats"][Tap.RGB_OUT]
            print(f"   {case_name:14s} {str(image.shape):14s} "
                  f"rgb_out [{s['min']:.4f}, {s['max']:.4f}] mean {s['mean']:.4f}"
                  + (f"  !! {s['nonFinite']} non-finite" if s["nonFinite"] else ""))

        manifest["combos"].append(combo)

    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=1))
    total = sum(f.stat().st_size for f in out_dir.iterdir() if f.is_file())
    print(f"\nwrote {len(manifest['combos'])} combos x {len(CASES)} cases x {len(TAPS)} taps "
          f"-> {out_dir} ({total / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
