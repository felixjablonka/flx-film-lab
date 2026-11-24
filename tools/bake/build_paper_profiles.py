#!/usr/bin/env python
"""Build Fujifilm colour paper profiles from Fujifilm's own bulletins.

Fujicolor Crystal Archive Type CA, Crystal Archive Digital Paper Type DPII and
Crystal Archive Professional Paper Maxima, read off the bulletins Fujifilm
Tokyo printed with vector plots (`fuji_paper_sheets`): DP II's AF3-212E
(2005), which plots its characteristic curves, spectral sensitivity and dye
densities, and Crystal Archive Paper's AF3-198E (2004), Type CA's line, which
plots the last two. Fujifilm's current sheets (Type CA 2018, DP II and
Maxima 2019) give each paper its D-max aims, and the same redrawn dye and
sensitivity plots for all three.

From the sheets:

- Dyes: the bulletin's spectral reflection densities, each normalised to 1.0
  and so plotted, put on agx's dye-amount scale (a unit of each reads in its
  own channel what a unit of agx's Crystal Archive Type II's does). Type CA
  takes AF3-198E's, DP II and Maxima DP II's. The two bulletins' dyes agree
  within 0.05 everywhere.
- Spectral sensitivity: the bulletin's three curves, relative log
  sensitivity at D-min + 1.0 (Status A) on one scale, as plotted; each
  layer's level is the plot's own, set where its curve reaches D-min + 1.0
  (as Kodak's 5385 is in `build_print_profiles`), and the three moved
  together to collect as much of agx's printer lamp as agx's Type II does,
  each through its own filtration for Kodak Portra 400.
- Characteristic curves: DP II's, Status A, the only ones Fujifilm plots for
  any of these papers. Type CA and Maxima take DP II's shape, each layer's
  density over D-min scaled so its top moves by the difference between the
  paper's D-max aim and DP II's in its channel: Fujifilm aims all three
  papers' laser-printer calibration at 0.15-0.20 under their flashed black
  (CLP013 rev-E), and a measured comparison of Type CA and DP II (koraks,
  'On a Color Mission', 2025) finds them alike in exposure, from where they
  build density to where they reach D-max, and apart in that maximum.
- The base: the curves' own D-min, as fog and stain, the dyes in the amounts
  that read it.

Not on the sheets, and taken as agx takes them for its papers: printed with
agx's lamp, TH-KG3, and viewed in D50; log H 0 in each layer where it forms
its share of a neutral grey of 18.4% luminance (agx's Type II puts 18.6%
there, its Portra Endura 17.7%).

Not used: Fujifilm Europe's redrawn dye and sensitivity plots (DP II from
2014, all three current sheets). They give all three papers the same curves,
broader than the bulletins' in every tail (the green layer at 450 nm 0.9
decades under its peak, where both bulletins have 1.7), and put the red
layer 0.6 decades under the blue where both bulletins, and agx's Type II,
put it 1.5-1.8.

Run:  PYTHONPATH=<spektrafilm>/src python tools/bake/build_paper_profiles.py
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fuji_paper_sheets  # noqa: E402
from build_bw_profiles import metadata  # noqa: E402
from build_print_profiles import lamp_collected, log_sensitivity, neutral_amounts  # noqa: E402
from build_profiles import (  # noqa: E402
    _isotonic, fit_cdf_model, layer_curves, own_dyes, responsivities, status_density, unmix,
)

from spektrafilm.profiles.io import (  # noqa: E402
    DensityCurvesModel, Profile, ProfileData, ProfileInfo, _json_safe, load_profile, profile_to_dict,
)

REPO = Path(__file__).resolve().parents[2]
OUT_DIR = REPO / "profiles"
WAVELENGTHS = np.arange(380.0, 781.0, 5.0)      # spektrafilm's SPECTRAL_SHAPE
LOG_EXPOSURE = np.linspace(-3.0, 4.0, 256)      # every bundled profile's axis
TEMPLATE = "fujifilm_crystal_archive_typeii"    # agx's Fuji paper: dye-amount scale, lamp, viewing light
LEVEL_NEGATIVE = "kodak_portra_400"             # the negative the lamp levels are matched through
DPII_AIMS = (2.35, 2.35, 2.25)                  # R G B, Fujifilm's DP II sheet (2019) and CLP013


@dataclass(frozen=True)
class Paper:
    stock: str
    name: str
    spectra: str                         # "dpii" or "crystal_archive": whose dyes and sensitivities
    aims: tuple[float, float, float]     # Fujifilm's D-max aims, R G B, Status A
    notes: str
    created: str = "2026-11-22"


PAPERS = (
    Paper("fujifilm_crystal_archive_type_ca", "Fujifilm Crystal Archive Type CA", "crystal_archive",
          (2.00, 2.00, 1.95),
          "Fujifilm product information bulletin AF3-198E (2004), FUJICOLOR CRYSTAL ARCHIVE PAPER, its dyes and "
          "spectral sensitivity; D-max aims 2.00/2.00/1.95 from Fujifilm's AF3-0250U2 (Type CA, 2018)"),
    Paper("fujifilm_crystal_archive_dpii", "Fujifilm Crystal Archive DP II", "dpii", DPII_AIMS,
          "Fujifilm product information bulletin AF3-212E (2005), FUJICOLOR CRYSTAL ARCHIVE DIGITAL PAPER TYPE "
          "DPII, its dyes, spectral sensitivity and characteristic curves; D-max aims 2.35/2.35/2.25 from "
          "Fujifilm's AF3-0212E3 (DP II, 2019)"),
    Paper("fujifilm_crystal_archive_maxima", "Fujifilm Crystal Archive Maxima", "dpii", (2.55, 2.55, 2.45),
          "Fujifilm's AF3-0252E2 (2019), FUJICOLOR CRYSTAL ARCHIVE PROFESSIONAL PAPER MAXIMA, D-max aims "
          "2.55/2.55/2.45 (as in CLP013 rev-E); it plots the same dyes and sensitivities as DP II's sheet of "
          "that year, so it takes DP II's own, from AF3-212E (2005), with its characteristic curves"),
)


def scaled_curves(drawn: dict[str, np.ndarray], aims) -> dict[str, tuple[np.ndarray, np.ndarray]]:
    """DP II's curves, each layer's density over D-min scaled so its top moves
    by the paper's aim less DP II's, in its channel."""
    out = {}
    for c, name in enumerate("RGB"):
        x, d = drawn[name][:, 0], _isotonic(drawn[name][:, 1], increasing=True)
        dmin, top = d.min(), d.max()
        k = (top - dmin + aims[c] - DPII_AIMS[c]) / (top - dmin)
        out[name] = (x, dmin + (d - dmin) * k)
    return out


def held(x: np.ndarray, d: np.ndarray, u: np.ndarray) -> np.ndarray:
    """A curve at `u`: as plotted inside its range, D-min held before it and
    its last density after. DP II's curves end on their shoulders (green and
    blue gaining 0.05-0.07 density a decade, red 0.2), where agx's curve
    model, fitted and run on as the print films' are, would climb another
    0.4 in blue."""
    return np.interp(u, x, d)


def build(spec: Paper, level: float = 0.0) -> tuple[Profile, dict]:
    dpii = fuji_paper_sheets.dpii()
    spectra = dpii if spec.spectra == "dpii" else fuji_paper_sheets.crystal_archive()
    template = load_profile(TEMPLATE)
    viewing = template.info.viewing_illuminant
    like = np.nan_to_num(np.asarray(template.data.channel_density, float))

    # Dyes and base
    valid = (WAVELENGTHS >= 400) & (WAVELENGTHS <= 700)
    resp = responsivities("status_A", WAVELENGTHS) * valid[:, None]
    cd = own_dyes(spectra["dyes"], WAVELENGTHS, valid, like, resp)
    curves = scaled_curves(dpii["curves"], spec.aims)
    dmin = np.array([curves[n][1].min() for n in "RGB"])
    fog, _ = unmix(dmin[None, :], cd, np.zeros(WAVELENGTHS.size), resp)
    bd = fog[0] @ cd.T

    # Mid-grey: each layer's share of a neutral 18.4% grey at log H 0
    neutral = neutral_amounts(cd, bd, valid, viewing)
    grey = neutral @ cd.T + bd
    share = status_density(grey[None, :], resp)[0]
    at = np.array([np.interp(share[c], curves[n][1], curves[n][0]) for c, n in enumerate("RGB")])
    gross = np.stack([held(*curves[n], LOG_EXPOSURE + at[c]) for c, n in enumerate("RGB")], axis=-1)
    amounts, err = unmix(gross - dmin, cd, bd, resp)
    centers, amplitudes, sigmas = fit_cdf_model(LOG_EXPOSURE, amounts, positive=False)
    fitted = layer_curves(LOG_EXPOSURE, centers, amplitudes, sigmas, positive=False).sum(axis=1)
    rms = float(np.sqrt(np.mean((fitted - amounts) ** 2)))
    readback = status_density(fitted @ cd.T + bd[None, :], resp)
    worst = float(np.abs(readback - gross).max())

    # Sensitivities: the plot's shapes and levels, each layer's where its curve
    # reaches D-min + 1.0, the density the plot is drawn at
    log_s = log_sensitivity(spectra["sensitivity"])
    reach = np.array([np.interp(dmin[c] + 1.0, curves[n][1], curves[n][0]) for c, n in enumerate("RGB")])
    log_s += (reach - at)[None, :] + level

    info = ProfileInfo(stock=spec.stock, name=spec.name, type="negative", support="paper", stage="printing",
                       use="still", antihalation=template.info.antihalation, target_print=None,
                       channel_model="color", densitometer="status_A", log_sensitivity_density_over_min=1.0,
                       reference_illuminant=template.info.reference_illuminant, viewing_illuminant=viewing)
    data = ProfileData(
        wavelengths=WAVELENGTHS,
        log_sensitivity=log_s,
        channel_density=np.where(valid[:, None], cd, np.nan),
        base_density=np.where(valid, bd, np.nan),
        midscale_neutral_density=np.where(valid, grey, np.nan),
        log_exposure=LOG_EXPOSURE,
        density_curves=fitted,
        density_curves_layers=layer_curves(LOG_EXPOSURE, centers, amplitudes, sigmas, positive=False),
        density_curves_model=DensityCurvesModel(model_type="cdfs", centers=centers, amplitudes=amplitudes,
                                                sigmas=sigmas),
    )
    found = {"dmin": dmin, "share": share, "at": at, "reach": reach - at, "unmix": err, "rms": rms,
             "worst": worst, "readback_max": readback.max(axis=0),
             "plotted_max": np.array([curves[n][1].max() for n in "RGB"]),
             "black": status_density((fitted[-1] @ cd.T + bd)[None, :], resp)[0]}
    return Profile(metadata=metadata("", spec.created), info=info, data=data), found


def datasource(spec: Paper, found: dict, filtration, reference, level: float) -> str:
    d = found["dmin"]
    if spec.spectra == "dpii":
        dyes = ("its dyes and spectral sensitivity are DP II's own" if spec.aims == DPII_AIMS else
                "its dyes and spectral sensitivity are DP II's, as its own sheet's plots are")
    else:
        dyes = "its dyes and spectral sensitivity are Crystal Archive Paper's own (AF3-198E)"
    curves = ("its characteristic curves are DP II's own" if spec.aims == DPII_AIMS else
              "its characteristic curves are DP II's (Fujifilm plots none for it), each layer's density over "
              "D-min scaled so its top moves by its own D-max aim less DP II's "
              f"({spec.aims[0] - DPII_AIMS[0]:+.2f}/{spec.aims[1] - DPII_AIMS[1]:+.2f}/"
              f"{spec.aims[2] - DPII_AIMS[2]:+.2f} R/G/B): Fujifilm aims all its papers 0.15-0.20 under their "
              "flashed black (CLP013 rev-E), and koraks' measurements of Type CA beside DP II ('On a Color "
              "Mission', tinker.koraks.nl, 2025) find the two alike in exposure and apart in D-max")
    return (
        f"From {spec.notes}; vector plots, read on their own frames and rules (fuji_paper_sheets.py). "
        f"{dyes[0].upper() + dyes[1:]}, the dyes spectral reflection densities normalised to 1.0, put on agx's "
        "dye-amount scale (a unit of each reads in its own Status A channel what a unit of agx's Fujifilm "
        "Crystal Archive Type II's does); the sensitivities relative log sensitivity at D-min + 1.0, one scale "
        "for the three, each layer's level where its curve reaches D-min + 1.0 "
        f"({found['reach'][0]:+.3f}/{found['reach'][1]:+.3f}/{found['reach'][2]:+.3f} decades from mid-grey); "
        f"{curves} (DP II's: laser exposure, CP-48S, Status A). Base: the curves' D-min, R {d[0]:.3f} G "
        f"{d[1]:.3f} B {d[2]:.3f}, as fog and stain, the dyes in the amounts that read it. Not on the sheets, "
        "taken as agx takes them for its papers: printed with agx's TH-KG3 lamp and viewed in D50; log H 0 in "
        "each layer where it forms its share of a neutral grey of 18.4% luminance (Status A "
        f"{found['share'][0]:.3f}/{found['share'][1]:.3f}/{found['share'][2]:.3f}); collects as much of the "
        "lamp as agx's Type II, each through its own filtration for Kodak Portra 400 (here M "
        f"{filtration[1]:.1f} Y {filtration[2]:.1f}, Type II's M {reference[1]:.1f} Y {reference[2]:.1f}), a "
        f"shift of {level:+.3f} decades. Black, the curves' top as fitted: Status A {found['black'][0]:.2f}/"
        f"{found['black'][1]:.2f}/{found['black'][2]:.2f}. Fujifilm Europe's redrawn dye and sensitivity plots "
        "(2014 on, one set for all three papers) differ from these and are not used."
    )


def main() -> int:
    import print_neutral
    import spektrafilm
    print(f"building colour paper profiles from datasheets -> {OUT_DIR}")
    template = load_profile(TEMPLATE)
    table = json.loads((Path(spektrafilm.__file__).parent / "data" / "filters" / "neutral_print_filters.json")
                       .read_text())
    reference = table[TEMPLATE]["TH-KG3"][LEVEL_NEGATIVE]
    reference_raw = lamp_collected(np.asarray(template.data.log_sensitivity, float), reference)
    negative = load_profile(LEVEL_NEGATIVE)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for spec in PAPERS:
        profile, found = build(spec)
        filtration = (0.0, *print_neutral.neutral_filters(negative, profile))
        level = reference_raw - lamp_collected(np.asarray(profile.data.log_sensitivity, float), filtration)
        profile, found = build(spec, level)
        profile.metadata.datasource = datasource(spec, found, filtration, reference, level)
        path = OUT_DIR / f"{spec.stock}.json"
        path.write_text(json.dumps(_json_safe(profile_to_dict(profile)), indent=4, allow_nan=False))
        print(f"  {spec.stock:34s} D-min {np.round(found['dmin'], 3).tolist()}  grey share "
              f"{np.round(found['share'], 3).tolist()}  D-max {np.round(found['readback_max'], 2).tolist()} "
              f"(plotted {np.round(found['plotted_max'], 2).tolist()})  unmix {found['unmix']:.1e}  "
              f"fit RMS {found['rms']:.4f}  readback worst {found['worst']:.4f}  Portra 400 "
              f"M {filtration[1]:.1f} Y {filtration[2]:.1f} (Type II M {reference[1]:.1f} Y {reference[2]:.1f})  "
              f"level {level:+.3f}  reach {np.round(found['reach'], 3).tolist()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
