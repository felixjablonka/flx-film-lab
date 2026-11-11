#!/usr/bin/env python
"""Polaroid's instant peel-apart print films, from Polaroid Corporation's data
sheets of 1999, read by `polaroid_sheets.py`:

- Polaroid 553 (T-553; T-53 sheet, T-803 8x10), black-and-white, ISO 800.
- Polaroid Polapan Pro 100 (T-554; T-54, T-664 pack, T-804), black-and-white.
- Polaroid Polacolor 559 (T-559), the Polacolor ER of T-669 pack film, which
  its sheet covers with T-88, T-108, T-59 and T-809: colour, ISO 80.

An instant print is the positive and the print at once: the camera's exposure
develops straight into the picture, which is seen by reflection. The profiles
are positives on a paper support (`support: "paper"`), which the app views as
it views a slide, white-pointed on the base, here the paper's white, D-min;
their curves are the print's own reflection densities ("Sample Density"), so
their black is a print's, about 1.7 to 1.9. They are 4x5 prints: the image is
3 1/2 x 4 5/8 in (89 x 117 mm), the frame grain and sharpness are laid on.

What each takes from its sheet, and the choices the sheets leave open:

- The characteristic curves, at 70 F, the sheets' normal temperature, and at
  55 F and 95 F (13 and 35 C), the cold and hot processing they plot. The
  pack carries the print at each of the three and no other (`processing`,
  the app's Processing choice): nothing between them is drawn.
- Mid-grey at the density the sheets name as their reference, "a neutral
  density of .75", which is also what an 18% grey card reads on a print
  (0.745): on the 70 F curve, the black-and-white films' own, Polacolor's the
  mean of its three. The sheets plot relative log exposure, and their ISO
  ratings, measured by Polaroid's method, place nothing on it.
- The spectral sensitivity, a single panchromatic curve on the
  black-and-white sheets and three on Polacolor's, each continued past its
  plotted ends at its own final gradient. The black-and-white sheets'
  filter factors check it (`check_filters`).
- Sharpness: the black-and-white sheets' MTF of the print, to 4 cycles/mm
  (`bw_mtf`). Polacolor's sheet gives only its resolving power, 9 line pairs/mm
  at 1000:1, where Type 553's is 12 to 15: Type 553's MTF moved along
  frequency as far as the two black-and-white sheets show a print's MTF moves
  with its resolving power (Polapan's is Type 553's at 1.33 times the
  frequency for 1.67 times the resolution): to 0.80 times. An estimate.
- No sheet gives a granularity. Prints seen at their own size show next to
  none: estimates (`bake_packs.BW_RMS_GRANULARITY`, `grain.SHEET_RMS`).
- Polacolor's sheet plots no dye spectra, its base or a neutral: the dyes are
  those of the agx stock, of the slides and colour papers here, that view its
  curves most neutral, and its base is its D-min in them. Its dye developers
  have no relative among them; this is the weakest part of the three. As
  drawn, on any of them, its shadows run magenta: Polacolor's known character
  (Felix, 2026-11-11).
- No DIR couplers (`stock_overrides.NO_COUPLERS`): the prints' MTFs fall
  from the start, with no edge effect.

Run:  PYTHONPATH=<spektrafilm>/src python tools/bake/build_instant_profiles.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_bw_profiles as bw  # noqa: E402
import build_profiles as bp  # noqa: E402
import polaroid_sheets as ps  # noqa: E402

from spektrafilm.profiles.io import (  # noqa: E402
    DensityCurvesModel, Profile, ProfileData, ProfileInfo, _json_safe, profile_to_dict,
)

OUT_DIR = bp.OUT_DIR
# Each instant print's curves at every processing temperature, and its frame:
# beside the profiles, which spektrafilm's format has no field for, and out of
# the profiles' own folder, which the bake reads as stocks.
PROCESSING_DIR = OUT_DIR / "processing"
WAVELENGTHS = bw.WAVELENGTHS
LOG_EXPOSURE = bw.LOG_EXPOSURE

# The sheets' reference density, "a neutral density of .75": mid-grey.
MID_GREY_DENSITY = 0.75
# The temperatures the sheets plot the print at: 55, 70 and 95 F.
COLD_C, NORMAL_C, HOT_C = ps.f_to_c(ps.COLD_F), ps.f_to_c(ps.NORMAL_F), ps.f_to_c(ps.HOT_F)
TEMPERATURES_C = [round(COLD_C, 2), round(NORMAL_C, 2), round(HOT_C, 2)]
# The 4x5 print's image, 3 1/2 x 4 5/8 in.
FRAME_MM = (117.5, 88.9)
CREATED = "2026-11-10"


def _monotone(grid: np.ndarray, density: np.ndarray) -> np.ndarray:
    """A positive's density falls with exposure: the trace held to that."""
    return bp._isotonic(density, increasing=False)


def _on(grid: np.ndarray, curve: np.ndarray) -> np.ndarray:
    return _monotone(grid, np.interp(grid, curve[:, 0], curve[:, 1]))


def at_temperature(curves_c: dict[float, np.ndarray], t: float, grid: np.ndarray) -> np.ndarray:
    """The sheet's print density on `grid` at `t` C, one of the temperatures it plots."""
    nearest = min(curves_c, key=lambda k: abs(k - t))
    if abs(nearest - t) > 0.01:
        raise ValueError(f"{t} C is not one of the sheet's temperatures {sorted(curves_c)}")
    return _on(grid, curves_c[nearest])


def _sheet_grid(curves) -> np.ndarray:
    lo = max(c[0, 0] for c in curves)
    hi = min(c[-1, 0] for c in curves)
    return np.linspace(lo, hi, 1024)


def mid_grey_shift(grid: np.ndarray, density: np.ndarray) -> float:
    """The sheet log E at which the (mean) density is the reference 0.75."""
    order = np.argsort(density)
    return float(np.interp(MID_GREY_DENSITY, density[order], grid[order]))


def _celsius(curves_f: dict[float, np.ndarray]) -> dict[float, np.ndarray]:
    return {ps.f_to_c(f): c for f, c in curves_f.items()}


# --- black and white -------------------------------------------------------------

BW_FILMS = {
    "polaroid_553": dict(
        name="Polaroid 553", iso=800, curves=ps.type_553_curves, sensitivity=ps.type_553_sensitivity,
        notes="Polaroid Corporation, T-53, T-553 and T-803 B&W High Speed Instant Peel-Apart Coaterless Film, "
              "Film Data Sheet (1999)"),
    "polaroid_polapan_pro_100": dict(
        name="Polaroid Polapan Pro 100", iso=100, curves=ps.polapan_100_curves,
        sensitivity=ps.polapan_100_sensitivity,
        notes="Polaroid Corporation, Polapan Pro 100 B&W T-54, T-554, T-664 and T-804, Film Data Sheet (1999)"),
}


def bw_profile(stock: str) -> tuple[Profile, dict]:
    film = BW_FILMS[stock]
    curves_c = _celsius(film["curves"]())
    grid = _sheet_grid(curves_c.values())
    normal = at_temperature(curves_c, NORMAL_C, grid)
    shift = -mid_grey_shift(grid, normal)
    e = grid + shift

    def laid(density):
        """On agx's axis, the ends held: net of D-min, three channels, fitted."""
        base = float(density.min())
        net = np.interp(LOG_EXPOSURE, e, density - base)
        three = np.repeat(net[:, None], 3, axis=1)
        centers, amplitudes, sigmas = bp.fit_cdf_model(LOG_EXPOSURE, three, positive=True)
        fitted = bp.layer_curves(LOG_EXPOSURE, centers, amplitudes, sigmas, positive=True).sum(axis=1)
        return fitted, base, (centers, amplitudes, sigmas), float(np.sqrt(np.mean((fitted - three) ** 2)))

    net, base, model, rms = laid(normal)
    traced = film["sensitivity"]()
    log_s = bp.continued_log_sensitivity(WAVELENGTHS, {"R": traced, "G": traced, "B": traced})[:, 0]
    log_s -= np.log10(bw.neutral_mid_grey_raw(log_s, "D55"))
    centers, amplitudes, sigmas = model
    info = ProfileInfo(stock=stock, name=film["name"], type="positive", support="paper", stage="filming",
                       use="still", antihalation="strong", target_print=None, channel_model="bw",
                       densitometer="diffuse_visual", log_sensitivity_density_over_min=0.75,
                       reference_illuminant="D55", viewing_illuminant="D50")
    data = ProfileData(
        wavelengths=WAVELENGTHS,
        log_sensitivity=np.repeat(log_s[:, None], 3, axis=1),
        channel_density=np.full((WAVELENGTHS.size, 3), 1.0 / 3.0),
        base_density=np.full(WAVELENGTHS.size, base),
        log_exposure=LOG_EXPOSURE,
        density_curves=net,
        density_curves_layers=bp.layer_curves(LOG_EXPOSURE, centers, amplitudes, sigmas, positive=True),
        density_curves_model=DensityCurvesModel(model_type="cdfs", centers=centers, amplitudes=amplitudes,
                                                sigmas=sigmas),
    )
    filters = check_filters(log_s) if stock == "polaroid_553" or stock == "polaroid_polapan_pro_100" else ""
    datasource = (
        f"Measured off {film['notes']}: the print's characteristic curve at 70 F (21 C), its spectral "
        f"sensitivity and its MTF, vector plots. A positive and the print at once, seen by reflection: the "
        f"curve is the print's density, D-min {base:.3f} (the paper's white) to D-max {base + net[:, 0].max():.2f}. "
        f"Mid-grey where it reads the sheet's reference density, 0.75 (18% grey on a print), on the sheet's "
        f"relative axis; the pack carries the sheet's prints at 55 F and 95 F (13 and 35 C) too, its cold and "
        f"hot processing. The spectral sensitivity continues past its plotted ends at its "
        f"own final gradient and is balanced so an 18% grey exposes it to 1.{filters} One silver layer, "
        f"carried as three identical channels."
    )
    profile = Profile(metadata=bw.metadata(datasource, CREATED), info=info, data=data)

    variants = []
    for t in TEMPERATURES_C:
        v_net, v_base, _, _ = laid(at_temperature(curves_c, t, grid))
        # each temperature's density on the normal print's base: D-min moves with it
        variants.append(v_net + (v_base - base))
    print(f"  {stock:26s} D-min {base:.3f}  D-max {base + net[:, 0].max():.2f}  mid-grey at sheet log E "
          f"{-shift:+.3f}  fit RMS {rms:.4f}")
    return profile, {"normalC": round(NORMAL_C, 2), "temperaturesC": TEMPERATURES_C, "densityCurves": variants}


def check_filters(log_s: np.ndarray) -> str:
    """The black-and-white sheets' filter factors (Wratten 8, 25, 47, 58; they
    list 6 and 15 too, which Kodak's Wratten 2 sheets here do not cover)
    against what the sensitivity gives in daylight (5500 K) and tungsten
    (3200 K)."""
    import lens_filters
    from spektrafilm.model.illuminants import standard_illuminant
    sheet = {"8": (2.0, 1.4), "25": (5.6, 2.8), "47": (6.3, 11.2), "58": (10.0, 11.2)}
    s = np.nan_to_num(10 ** log_s)
    out = []
    for f in lens_filters.FILTERS:
        if f.wratten not in sheet:
            continue
        t = f.transmittance()
        got = []
        for light in ("BB5500", "BB3200"):
            e = np.asarray(standard_illuminant(light), float)
            got.append(float(np.sum(s * e) / np.sum(s * e * t)))
        out.append(f"Wratten {f.wratten} {got[0]:.1f} and {got[1]:.1f} (sheet {sheet[f.wratten][0]} and "
                   f"{sheet[f.wratten][1]})")
    return " Filter factors in daylight and tungsten from it: " + "; ".join(out) + "."


# --- colour -------------------------------------------------------------------------

POLACOLOR = "polaroid_polacolor_559"
POLACOLOR_NOTES = ("Polaroid Corporation, Polacolor 559 Film Data Sheet and its Technical Data for T-88, T-108, "
                   "T-669, T-669S, T-559, T-559S, T-59 and T-809 Instant Color Peel-Apart Films (1999)")
# The positive donor the colour build stands on (its type, densitometer, the
# light it is balanced in and viewed under); the dyes are chosen below.
POLACOLOR_FRAME = "kodak_ektachrome_100"
# The dye sets tried for Polacolor: agx's slides and colour papers.
DYE_CANDIDATES = ("kodak_ektachrome_100", "kodak_elite_chrome_extra_color_100", "fujifilm_provia_100f",
                  "fujifilm_velvia_100", "kodak_kodachrome_64", "kodak_portra_endura",
                  "fujifilm_crystal_archive_typeii", "kodak_ektacolor_edge", "kodak_supra_endura",
                  "kodak_ultra_endura", "kodak_endura_premier")


def _polacolor_on(grid, curves_c, t):
    return np.stack([at_temperature({k: v[c] for k, v in curves_c.items()}, t, grid) for c in "RGB"], axis=-1)


def polacolor_inputs():
    curves_c = _celsius(ps.polacolor_curves())
    grid = _sheet_grid([c for chans in curves_c.values() for c in chans.values()])
    normal = _polacolor_on(grid, curves_c, NORMAL_C)
    shift = -mid_grey_shift(grid, normal.mean(axis=1))
    return curves_c, grid, normal, shift


def _dye_setup(dyes_stock: str):
    """The frame donor's grid and light, with `dyes_stock`'s dye spectra scaled
    to the donor's dye-amount scale (`build_profiles.scaled_like`)."""
    import bake_packs
    donor = bake_packs.read_profile(POLACOLOR_FRAME)
    d = donor.data
    wl = np.asarray(d.wavelengths, float)
    donor_cd = np.asarray(d.channel_density, float)
    candidate = np.asarray(bake_packs.read_profile(dyes_stock).data.channel_density, float)
    defined = np.isfinite(donor_cd).all(axis=1) & np.isfinite(candidate).all(axis=1)
    resp = bp.responsivities("status_A", wl) * defined[:, None]
    cd = bp.scaled_like(np.where(defined[:, None], np.nan_to_num(candidate), 0.0),
                        np.where(defined[:, None], np.nan_to_num(donor_cd), 0.0), resp)
    return donor, wl, cd, defined, resp


def _amounts(sheet_grid, density, shift, cd, defined, resp):
    """The sheet's Status A densities in dye amounts on agx's axis, the base
    their D-min in the same dyes."""
    fog, _ = bp.unmix(density.min(axis=0)[None, :], cd, np.zeros(cd.shape[0]), resp)
    bd = fog[0] @ cd.T
    target = np.stack([np.interp(LOG_EXPOSURE, sheet_grid + shift, density[:, c] - density[:, c].min())
                       for c in range(3)], axis=-1)
    amounts, err = bp.unmix(target, cd, bd, resp)
    return amounts, bd, err


def dye_neutrality() -> list[tuple[str, float, float, float]]:
    """How neutral each candidate dye set views Polacolor's 70 F greys, three
    stops under mid-grey to two over: (stock, RMS a*b*, worst a*, worst b*)."""
    _, sheet_grid, normal, shift = polacolor_inputs()
    rows = []
    for stock in DYE_CANDIDATES:
        donor, wl, cd, defined, resp = _dye_setup(stock)
        amounts, bd, _ = _amounts(sheet_grid, normal, shift, cd, defined, resp)
        lab = bp.viewed_wedge(LOG_EXPOSURE, amounts, cd, bd, defined, donor.info.viewing_illuminant,
                              (0.0, 0.0, 0.0), np.arange(-3, 3))
        rms = float(np.sqrt(np.mean(lab[:, 1] ** 2 + lab[:, 2] ** 2)))
        rows.append((stock, rms, float(np.abs(lab[:, 1]).max()), float(np.abs(lab[:, 2]).max())))
    return sorted(rows, key=lambda r: r[1])


def polacolor_profile(dyes_stock: str) -> tuple[Profile, dict]:
    curves_c, sheet_grid, normal, shift = polacolor_inputs()
    donor, wl, cd, defined, resp = _dye_setup(dyes_stock)
    amounts, bd, err = _amounts(sheet_grid, normal, shift, cd, defined, resp)
    centers, amplitudes, sigmas = bp.fit_cdf_model(LOG_EXPOSURE, amounts, positive=True)
    fitted = bp.layer_curves(LOG_EXPOSURE, centers, amplitudes, sigmas, positive=True).sum(axis=1)

    profile = donor.clone()
    profile.update_info(stock=POLACOLOR, name="Polaroid Polacolor 559", support="paper", target_print=None,
                        antihalation="strong")
    profile.update_data(log_exposure=LOG_EXPOSURE, density_curves=fitted,
                        density_curves_layers=bp.layer_curves(LOG_EXPOSURE, centers, amplitudes, sigmas, True),
                        channel_density=np.where(defined[:, None], cd, np.nan),
                        base_density=np.where(defined, bd, np.nan),
                        hanatos2025_adaptation_window_params=np.zeros(0))
    traced = ps.polacolor_sensitivity()
    profile.update_data(log_sensitivity=bp.continued_log_sensitivity(wl, traced))
    bp.balance(profile)
    profile.data.density_curves_model.centers = centers
    profile.data.density_curves_model.amplitudes = amplitudes
    profile.data.density_curves_model.sigmas = sigmas
    fog = bp.status_density(bd[None, :], resp)[0]
    profile.metadata.datasource = (
        f"Characteristic curves and spectral sensitivities from {POLACOLOR_NOTES}, vector plots: the print's "
        f"blue, green and red densities at 70 F (21 C), taken as Status A, converted into dye amounts. A "
        f"positive and the print at once, seen by reflection. Mid-grey where the three read the sheet's "
        f"reference density, 0.75 (18% grey on a print), on average, on the sheet's relative axis; the pack "
        f"carries the sheet's prints at 55 F and 95 F (13 and 35 C) too, its cold and hot processing. The "
        f"spectral sensitivities continue past their plotted ends at their own final "
        f"gradients and are balanced so an 18% grey exposes every layer equally. The sheet plots no dye spectra "
        f"and no base: the dyes are those of '{dyes_stock}', of agx's slides and colour papers the set that "
        f"views the sheet's 70 F greys most neutral, and the base is the print's D-min in them (R {fog[0]:.2f}, "
        f"G {fog[1]:.2f}, B {fog[2]:.2f}). No spectral adaptation window and no DIR couplers. Exposure frame "
        f"(type, densitometer, lights) from '{POLACOLOR_FRAME}'."
    )
    profile.metadata.created = CREATED

    variants = []
    for t in TEMPERATURES_C:
        density = _polacolor_on(sheet_grid, curves_c, t)
        v_amounts, _, _ = _amounts(sheet_grid, density, shift, cd, defined, resp)
        # fitted as the 70 F curves are, and above the 70 F print's base: D-min moves with temperature
        v_c, v_a, v_s = bp.fit_cdf_model(LOG_EXPOSURE, v_amounts, positive=True)
        v_fitted = bp.layer_curves(LOG_EXPOSURE, v_c, v_a, v_s, positive=True).sum(axis=1)
        lift, _ = bp.unmix((density.min(axis=0) - normal.min(axis=0))[None, :], cd, np.zeros(cd.shape[0]), resp)
        variants.append(fitted if abs(t - round(NORMAL_C, 2)) < 0.01 else v_fitted + lift[0][None, :])
    readback = bp.status_density(fitted @ cd.T + bd[None, :], resp) - fog
    target = np.stack([np.interp(LOG_EXPOSURE, sheet_grid + shift, normal[:, c] - normal[:, c].min())
                       for c in range(3)], axis=-1)
    print(f"  {POLACOLOR:26s} dyes {dyes_stock}  unmix {err:.1e}  readback vs sheet worst "
          f"{np.abs(readback - target).max():.4f}  mid-grey at sheet log E {-shift:+.3f}")
    return profile, {"normalC": round(NORMAL_C, 2), "temperaturesC": TEMPERATURES_C, "densityCurves": variants}


def write(profile: Profile, processing: dict) -> None:
    stock = profile.info.stock
    (OUT_DIR / f"{stock}.json").write_text(json.dumps(_json_safe(profile_to_dict(profile)), indent=4,
                                                      allow_nan=False))
    sidecar = {"normalC": processing["normalC"], "temperaturesC": processing["temperaturesC"],
               "frameMM": list(FRAME_MM),
               "densityCurves": [np.round(np.asarray(c, float), 6).ravel().tolist() for c in processing["densityCurves"]]}
    PROCESSING_DIR.mkdir(parents=True, exist_ok=True)
    (PROCESSING_DIR / f"{stock}.json").write_text(json.dumps(sidecar))


def main() -> int:
    for stock in BW_FILMS:
        write(*bw_profile(stock))
    ranking = dye_neutrality()
    print("  Polacolor's 70 F greys, three stops under to two over, by dye set (RMS a*b*, worst a*, worst b*):")
    for stock, rms, a, b in ranking:
        print(f"    {stock:36s} {rms:5.2f}  {a:5.2f}  {b:5.2f}")
    write(*polacolor_profile(ranking[0][0]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
