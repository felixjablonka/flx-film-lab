#!/usr/bin/env python
"""Build colour print film profiles from their makers' datasheets.

Fujifilm's motion picture print films, the stocks a lab prints a camera
negative onto, as agx ships Kodak's 2383 and 2393; each from its own sheet
(`fuji_print_sheets`): F-CP 3519 (1999), F-CP 3510 (2003) and ETERNA-CP 3513DI
(2005). And Kodak's Eastman Color Print 5385 (1962), from the paper that
introduced it (`kodak_1962_sheets`); where it differs, below.

From each sheet:

- The dyes: its three single-dye spectral densities, 400-700 nm, less the
  floor all three share, put on agx's dye-amount scale (a unit of each reads
  in its own channel what a unit of 2383's does). As plotted the three add up
  to more than the sheet's grey, by twice a smooth floor of 0.03-0.08 that
  the grey holds once, and the yellow and magenta curves lie flat at about
  0.05 across 600-700 nm, where neither dye absorbs: each single-dye density
  was measured with the film's base under it. Taken off, the dyes add up to
  the grey exactly, the floor as its base; read as plotted they would carry
  the base three times over, and print 0.65-0.75 of 2383's chroma where they
  print 0.8-0.9 of it so. The grey is kept as the profile's midscale neutral.
- The base: the curves' own D-min, Status A, as fog and stain, the sheet's
  dyes in the amounts that read it.
- The characteristic curves, Status A, with the green and blue curves moved
  back by the decade and two the sheet shifted them by "to avoid
  overlapping" (cumulatively, as their positions show: put back, the three
  reach the aim density within 0.15 decades of each other under the sheet's
  exposure), turned into dye amounts with those dyes and that base. Past its
  last plotted point a curve continues as agx's curve model fitted to what is
  plotted: 3519's stop at about 3.6, a step tablet's range, still rising, and
  3513DI's green leaves the plot at density 4.0.
- The spectral sensitivities' shapes, as plotted (log, at D-min + 1.0), none
  beyond them.
- Each layer's sensitivity level, from where its curve lies under the exposure
  the sheet states: 2854 K tungsten through CC-90Y, CC-60M and a Fuji SC-41,
  which the sheet says simulates practical printing. The CC filters are taken
  as dyed gelatin of the sheet's own yellow and magenta dye shapes at those
  peak densities, the SC-41 as a sharp cut at 410 nm. (The sensitivity plots'
  own levels put the green layer 0.2-0.3 decades slower than the curves show;
  the curves, measured as densities, are taken.) Printed with agx's lamp
  instead, Vision3 250D then prints neutral at M 57-67, Y 78-79, where it
  takes M 51, Y 54 on 2383.

Not on the sheet, and taken as agx takes them for every print stock it ships:

- Printed with agx's printer lamp, TH-KG3, and projected with its xenon lamp,
  K75P (the sheet's aim densities assume xenon projection), as 2383 is.
- Where on the exposure axis mid-grey prints: log H 0 in each layer is where
  it forms its share of a neutral grey of 18.4% luminance under the projector,
  as on 2383 (to 0.02 decades). The sheet's aim densities, about 1.05, are a
  lab's density for projection; agx renders a print's grey at 18%, on its own
  stocks and on these.
- How much of the lamp it collects: as much as 2383, each through its own
  filtration for Kodak Vision3 250D, which keeps Highlight protection (a
  pre-flash, added after the mid-grey normalisation) the same on either.

Eastman Color Print 5385, from W. I. Kisner's paper (SMPTE 1962), scanned:

- The dyes: its three dye spectral densities as drawn, which add up to its
  neutral's within 0.012 from 400 to 680 nm (they carry no floor of base).
- The curves: integral density, taken as Status A, each moved along the
  exposure axis by the paper "to illustrate the curve shape", so where each
  lies says nothing; each placed by agx's rule for mid-grey, as Fujifilm's
  are. Their exposure axis has no numbered ticks and is read at the scale of
  their density, as Kodak draws them (`kodak_1962_sheets`). They leave the
  plot at density 3.56, still rising, and go on as agx's curve model fitted
  to them.
- The layer speeds: the spectral sensitivities' own levels, on one scale in
  the paper (the one it labels for 5385), each layer's taken where its curve
  reaches D-min + 1.0, the density Kodak plots a print film's sensitivity at;
  the three moved together to collect as much of agx's lamp as 2383 does.

Run:  PYTHONPATH=<spektrafilm>/src python tools/bake/build_print_profiles.py
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass
from pathlib import Path

import colour
import numpy as np
from scipy.optimize import least_squares

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fuji_print_sheets  # noqa: E402
from build_bw_profiles import metadata  # noqa: E402
from build_profiles import (  # noqa: E402
    _isotonic, fit_cdf_model, layer_curves, own_dyes, responsivities, status_density, unmix,
)

from spektrafilm.config import STANDARD_OBSERVER_CMFS  # noqa: E402
from spektrafilm.model.color_filters import custom_dichroic_filters  # noqa: E402
from spektrafilm.model.illuminants import standard_illuminant  # noqa: E402
from spektrafilm.profiles.io import (  # noqa: E402
    DensityCurvesModel, Profile, ProfileData, ProfileInfo, _json_safe, load_profile, profile_to_dict,
)

REPO = Path(__file__).resolve().parents[2]
OUT_DIR = REPO / "profiles"

WAVELENGTHS = np.arange(380.0, 781.0, 5.0)      # spektrafilm's SPECTRAL_SHAPE
LOG_EXPOSURE = np.linspace(-3.0, 4.0, 256)      # every bundled profile's axis
TEMPLATE = "kodak_2383"                         # agx's print film: dye-amount scale, lamp, projector
SHIFTS = (0.0, 1.0, 2.0)                        # R, G, B: how far left of where they lie the sheet draws them
MID_GREY = 0.184
LEVEL_NEGATIVE = "kodak_vision3_250d"           # the negative the lamp levels are matched through


@dataclass(frozen=True)
class Print:
    stock: str
    name: str
    notes: str
    created: str = "2026-11-01"
    # Read off Fujifilm's sheet (`fuji_print_sheets`) or Kodak's 1962 paper (`kodak_1962_sheets`).
    sheet: str = "fuji"


MANUAL = ("its curves, aim densities and printer settings also in Fujifilm's FUJIFILM MOTION PICTURE FILM "
          "MANUAL (KB-0707E, 2008), pages 25-28, whose plot agrees with the sheet's to 0.003 in density")
PRINTS = (
    Print("fujifilm_fcp_3519", "Fujifilm F-CP 3519",
          "Fujifilm data sheet KB-918E (1999), FUJICOLOR POSITIVE FILM F-CP, 35mm Type 3519 / 16mm Type 3619"),
    Print("fujifilm_fcp_3510", "Fujifilm F-CP 3510",
          "Fujifilm data sheet KB-301E (2003), FUJICOLOR POSITIVE FILM F-CP, 35mm Type 3510 / 16mm Type 3610; "
          + MANUAL),
    Print("fujifilm_eterna_cp_3513di", "Fujifilm Eterna-CP 3513DI",
          "Fujifilm data sheet KB-505E (2005), FUJICOLOR POSITIVE FILM ETERNA-CP, 35mm Type 3513DI / 16mm Type "
          "3613DI; " + MANUAL),
    Print("kodak_5385", "Kodak 5385",
          "W. I. Kisner, 'A Higher Speed Color Print Film', Journal of the SMPTE 71(10), 1962, pp. 779-781, "
          "Figs. 1-3: EASTMAN Color Print Film, Type 5385 (35mm) / 7385 (16mm)", sheet="kodak_1962"),
)


def net_of_floor(dyes: dict[str, np.ndarray], grey: np.ndarray) -> tuple[dict[str, np.ndarray], np.ndarray]:
    """The single-dye curves less the floor they share: half of what the three
    add up to beyond the grey, which holds the base once where they hold it
    three times. Returns the dyes, nm against density, and the floor."""
    grid = np.arange(400.0, 701.0, 1.0)
    plotted = {n: np.interp(grid, dyes[n][:, 0], dyes[n][:, 1]) for n in "CMY"}
    floor = (plotted["C"] + plotted["M"] + plotted["Y"] - np.interp(grid, grey[:, 0], grey[:, 1])) / 2
    return ({n: np.stack([grid, np.clip(v - floor, 0.0, None)], axis=-1) for n, v in plotted.items()},
            np.stack([grid, floor], axis=-1))


def log_sensitivity(strokes: dict[str, list[np.ndarray]]) -> np.ndarray:
    """The plotted sensitivities on the grid, none (NaN) where nothing is drawn."""
    out = np.full((WAVELENGTHS.size, 3), np.nan)
    for c, name in enumerate("RGB"):
        for stroke in strokes[name]:
            inside = (WAVELENGTHS >= stroke[0, 0]) & (WAVELENGTHS <= stroke[-1, 0])
            out[inside, c] = np.interp(WAVELENGTHS[inside], stroke[:, 0], stroke[:, 1])
    return out


def sheet_exposure(dyes: np.ndarray) -> np.ndarray:
    """The light the sheet exposes its curves with: 2854 K tungsten through a
    Fuji SC-41 (a sharp cut at 410 nm) and CC-90Y and CC-60M, dyed gelatin of
    the sheet's own yellow and magenta shapes at those peak densities."""
    lamp = colour.sd_blackbody(2854, colour.SpectralShape(380, 780, 5)).values
    sc41 = 1.0 / (1.0 + np.exp(-(WAVELENGTHS - 410.0) / 3.0))
    yellow, magenta = dyes[:, 2] / dyes[:, 2].max(), dyes[:, 1] / dyes[:, 1].max()
    return lamp / lamp.max() * sc41 * 10 ** (-0.90 * yellow - 0.60 * magenta)


def viewed(spectral: np.ndarray, valid: np.ndarray, viewing: str) -> np.ndarray:
    """L*a*b* and luminance of a print's spectral density under the projector,
    nothing passing where its dyes are undefined (as agx and the engine see it)."""
    light = standard_illuminant(viewing)
    cmfs = np.asarray(STANDARD_OBSERVER_CMFS[:], float)
    white = light @ cmfs
    xyz = (light * np.where(valid, 10 ** -np.nan_to_num(spectral), 0.0)) @ cmfs
    lab = colour.XYZ_to_Lab(xyz / white[1], colour.XYZ_to_xy(white / white[1]))
    return np.append(lab, xyz[1] / white[1])


def neutral_amounts(cd: np.ndarray, bd: np.ndarray, valid: np.ndarray, viewing: str) -> np.ndarray:
    """Dye amounts that view as a neutral grey of 18.4% luminance."""
    def residual(a):
        lab = viewed(a @ cd.T + bd, valid, viewing)
        return [lab[1], lab[2], np.log10(lab[3] / MID_GREY) * 100]
    return least_squares(residual, np.full(3, 0.5), bounds=(0, 5)).x


def continued(x: np.ndarray, d: np.ndarray, u: np.ndarray) -> np.ndarray:
    """A curve at `u`: as plotted inside its range, D-min held before it, and
    past its end agx's curve model fitted to the plotted curve, joined on."""
    grid = np.linspace(x[0], x[-1], 400)
    plotted = np.interp(grid, x, d)
    centers, amplitudes, sigmas = fit_cdf_model(grid, np.repeat(plotted[:, None], 3, axis=1), positive=False)
    model = layer_curves(u, centers, amplitudes, sigmas, positive=False).sum(axis=1)[:, 0]
    at_end = layer_curves(np.array([x[-1]]), centers, amplitudes, sigmas, positive=False).sum(axis=1)[0, 0]
    out = np.interp(u, x, d)
    past = u > x[-1]
    out[past] = np.maximum(model[past] - at_end + d[-1], d[-1])
    return out


def build(spec: Print, level: float = 0.0) -> tuple[Profile, dict]:
    kodak = spec.sheet == "kodak_1962"
    if kodak:
        import kodak_1962_sheets
        sheet = kodak_1962_sheets.print_film()
        sheet["ctf"] = None
    else:
        sheet = fuji_print_sheets.read(spec.stock)
    template = load_profile(TEMPLATE)
    viewing = template.info.viewing_illuminant
    like = np.nan_to_num(np.asarray(template.data.channel_density, float))

    # Dyes and base
    valid = (WAVELENGTHS >= 400) & (WAVELENGTHS <= 700)
    resp = responsivities("status_A", WAVELENGTHS) * valid[:, None]
    if kodak:
        dyes, floor = sheet["dyes"], np.stack([np.array([400.0, 700.0]), np.zeros(2)], axis=-1)
    else:
        dyes, floor = net_of_floor(sheet["dyes"], sheet["neutral"])
    cd = own_dyes(dyes, WAVELENGTHS, valid, like, resp)
    floor_on_grid = np.where(valid, np.interp(WAVELENGTHS, floor[:, 0], floor[:, 1]), 0.0)
    curves = {}
    for c, name in enumerate("RGB"):
        drawn = sheet["curves"][name]
        curves[name] = (drawn[:, 0] + (0.0 if kodak else SHIFTS[c]), _isotonic(drawn[:, 1], increasing=True))
    dmin = np.array([curves[n][1].min() for n in "RGB"])
    fog, _ = unmix(dmin[None, :], cd, np.zeros(WAVELENGTHS.size), resp)
    bd = fog[0] @ cd.T

    # Mid-grey: each layer's share of a neutral 18.4% grey at log H 0
    neutral = neutral_amounts(cd, bd, valid, viewing)
    share = status_density((neutral @ cd.T + bd)[None, :], resp)[0]
    at = np.array([np.interp(share[c], curves[n][1], curves[n][0]) for c, n in enumerate("RGB")])
    gross = np.stack([continued(*curves[n], LOG_EXPOSURE + at[c]) for c, n in enumerate("RGB")], axis=-1)
    amounts, err = unmix(gross - dmin, cd, bd, resp)
    centers, amplitudes, sigmas = fit_cdf_model(LOG_EXPOSURE, amounts, positive=False)
    fitted = layer_curves(LOG_EXPOSURE, centers, amplitudes, sigmas, positive=False).sum(axis=1)
    rms = float(np.sqrt(np.mean((fitted - amounts) ** 2)))
    readback = status_density(fitted @ cd.T + bd[None, :], resp)
    worst = float(np.abs(readback - gross).max())

    # Sensitivities: the sheet's shapes, each layer's level from where its
    # curve lies under the sheet's exposure
    log_s = log_sensitivity(sheet["sensitivity"])
    if kodak:
        # the plot's own levels, each layer's where its curve reaches D-min + 1.0
        reach = np.array([np.interp(dmin[c] + 1.0, curves[n][1], curves[n][0]) for c, n in enumerate("RGB")])
        log_s += (reach - at)[None, :] + level
    else:
        collected = np.log10(sheet_exposure(cd) @ np.nan_to_num(10 ** log_s))
        log_s += (-at - collected)[None, :] + level

    stored_cd = np.where(valid[:, None], cd, np.nan)
    stored_bd = np.where(valid, bd, np.nan)
    grey = np.interp(WAVELENGTHS, sheet["neutral"][:, 0], sheet["neutral"][:, 1], left=np.nan, right=np.nan)
    info = ProfileInfo(stock=spec.stock, name=spec.name, type="negative", support="film", stage="printing",
                       use="cine", antihalation=template.info.antihalation, target_print=None,
                       channel_model="color", densitometer="status_A", log_sensitivity_density_over_min=1.0,
                       reference_illuminant=template.info.reference_illuminant, viewing_illuminant=viewing)
    data = ProfileData(
        wavelengths=WAVELENGTHS,
        log_sensitivity=log_s,
        channel_density=stored_cd,
        base_density=stored_bd,
        midscale_neutral_density=grey,
        log_exposure=LOG_EXPOSURE,
        density_curves=fitted,
        density_curves_layers=layer_curves(LOG_EXPOSURE, centers, amplitudes, sigmas, positive=False),
        density_curves_model=DensityCurvesModel(model_type="cdfs", centers=centers, amplitudes=amplitudes,
                                                sigmas=sigmas),
    )
    found = {"dmin": dmin, "neutral": neutral, "share": share, "at": at, "unmix": err, "rms": rms,
             "reach": (reach - at) if kodak else None,
             "worst": worst, "dmax": fitted.max(axis=0), "readback_max": readback.max(axis=0),
             "plotted_max": np.array([curves[n][1].max() for n in "RGB"]), "cd": cd, "valid": valid,
             "grey": grey, "ctf": sheet["ctf"],
             "floor": status_density(floor_on_grid[None, :], resp)[0], "floor_range": (floor[:, 1].min(), floor[:, 1].max())}
    return Profile(metadata=metadata("", spec.created), info=info, data=data), found


def lamp_collected(log_s: np.ndarray, filtration) -> float:
    """What a print stock collects of agx's printer lamp through a filtration,
    the geometric mean of its three layers."""
    lamp = custom_dichroic_filters.apply_cc(standard_illuminant("TH-KG3"), filter_cc_values=filtration)
    return float(np.mean(np.log10(lamp @ np.nan_to_num(10 ** log_s))))


def datasource_kodak_1962(spec: Print, found: dict, filtration, reference, level: float) -> str:
    top, dmax = found["plotted_max"], found["readback_max"]
    reach = found["reach"]
    return (
        f"From {spec.notes}, scanned; each figure levelled, calibrated on its own tick marks and its solid "
        "curves traced with 5382's dashed ones taken out (kodak_1962_sheets.py). Dyes: the paper's three dye "
        "spectral densities, 400-700 nm, as drawn: they add up to its neutral's within 0.012 from 400 to 680 nm, "
        "and the neutral is kept as the midscale neutral; put on agx's dye-amount scale, a unit of each reading "
        "in its own channel what a unit of Kodak 2383's does. Base: the curves' own D-min, R "
        f"{found['dmin'][0]:.3f} G {found['dmin'][1]:.3f} B {found['dmin'][2]:.3f}, as fog and stain, the dyes "
        "in the amounts that read it. Characteristic curves: integral density, taken as Status A; their exposure "
        "axis carries no numbered ticks and is read at the scale of their density, one decade drawn as long as "
        "one density unit, as Kodak draws them (its 1968 paper on 5254 marks its axis so, to 3%); the paper "
        "moved each curve along the axis 'to illustrate the curve shape', so where each lies is not used. "
        f"Past the plot's top (R {top[0]:.2f} G {top[1]:.2f} B {top[2]:.2f}, still rising) each continues as "
        f"agx's curve model fitted to what is plotted, to D-max R {dmax[0]:.2f} G {dmax[1]:.2f} B {dmax[2]:.2f}. "
        "Mid-grey, as on every agx print stock: log H 0 in each layer is where it forms its share of a neutral "
        "grey of 18.4% luminance under agx's xenon projector (K75P), Status A R "
        f"{found['share'][0]:.2f} G {found['share'][1]:.2f} B {found['share'][2]:.2f}. Spectral sensitivities: "
        "the paper's shapes, on the scale it labels for 5385, none beyond them (the red layer's as its two "
        "strokes, blue and red); each layer's level from that one scale, taken where its curve reaches D-min + "
        f"1.0, the density Kodak plots a print film's sensitivity at (R {reach[0]:+.2f}, G {reach[1]:+.2f}, "
        f"B {reach[2]:+.2f} decades from its mid-grey). Printed with agx's lamp, Kodak Vision3 250D prints "
        f"neutral at M {filtration[1]:.0f}, Y {filtration[2]:.0f} (on 2383 at agx's M {reference[1]:.0f}, Y "
        f"{reference[2]:.0f}); all three levels moved together by {level:+.3f} decades to collect as much of "
        "agx's printer lamp (TH-KG3) as 2383 does, each through its own filtration for Vision3 250D. Printing "
        "lamp TH-KG3 and projector K75P as agx's 2383."
    )


def datasource(spec: Print, found: dict, filtration, reference, level: float) -> str:
    ctf = found["ctf"]
    ctf_at = lambda f: float(np.interp(np.log10(f), np.log10(ctf[:, 0]), ctf[:, 1]))   # noqa: E731
    top, dmax = found["plotted_max"], found["readback_max"]
    clipped = [n for n, t in zip("RGB", top) if t >= 3.99]
    floor = found["floor"]
    return (
        f"From {spec.notes}. Every plot read off the sheet's vector paths, each curve by its colour. "
        "Dyes: the sheet's three single-dye spectral densities, 400-700 nm, each less the floor the three share, "
        f"half of what they add up to beyond the sheet's grey ({found['floor_range'][0]:.2f}-"
        f"{found['floor_range'][1]:.2f}, Status A R {floor[0]:.3f} G {floor[1]:.3f} B {floor[2]:.3f}): the "
        "film's base, which each single-dye density holds and the grey once, as the yellow and magenta curves "
        "show, lying flat at about 0.05 across 600-700 nm where neither dye absorbs. So taken they add up to the "
        "grey, kept as the midscale neutral, exactly; they are put on agx's dye-amount scale, a unit of each "
        "reading in its own channel what a unit of Kodak 2383's does. Base: the curves' own D-min, Status A R "
        f"{found['dmin'][0]:.3f} G {found['dmin'][1]:.3f} B {found['dmin'][2]:.3f}, as fog and stain, the dyes "
        "in the amounts that read it. Characteristic curves: Status A, exposed with 2854 K tungsten through "
        "CC-90Y, CC-60M and SC-41; the green and blue curves moved back by the 1.0 and 2.0 decades the sheet "
        "shifted them by, and converted into dye amounts with those dyes and that base; past its last plotted "
        f"point (R {top[0]:.2f} G {top[1]:.2f} B {top[2]:.2f}"
        + (f", the {'/'.join(clipped)} at the top of the plot" if clipped else "")
        + f") each continues as agx's curve model fitted to what is plotted, to D-max R {dmax[0]:.2f} "
        f"G {dmax[1]:.2f} B {dmax[2]:.2f}. Mid-grey, as on every agx print stock: log H 0 in each layer is where it forms its share of a "
        "neutral grey of 18.4% luminance under agx's xenon projector (K75P), Status A R "
        f"{found['share'][0]:.2f} G {found['share'][1]:.2f} B {found['share'][2]:.2f}; the sheet's aim print "
        "densities (R 1.10, G 1.05, B 1.05) are a lab's for projection. Spectral sensitivities: the sheet's "
        "shapes, none beyond them; each layer's level from where its curve lies under the sheet's exposure, the "
        "CC filters taken as dyed gelatin of the sheet's own yellow and magenta dye shapes, the SC-41 as a sharp "
        f"cut at 410 nm. Printed with agx's lamp, Kodak Vision3 250D prints neutral at M {filtration[1]:.0f}, "
        f"Y {filtration[2]:.0f} (on 2383 at agx's M {reference[1]:.0f}, Y {reference[2]:.0f}); all three levels "
        f"moved together by {level:+.3f} decades to collect as much of agx's printer lamp (TH-KG3) as 2383 does, "
        f"each through its own filtration for Vision3 250D. The sheet's CTF (response {ctf_at(10):.2f} at 10, "
        f"{ctf_at(20):.2f} at 20 and {ctf_at(40):.2f} at 40 cycles/mm) is not modelled: no print stage in agx or "
        "this app blurs. Printing lamp TH-KG3 and projector K75P as agx's 2383."
    )


def main() -> int:
    import print_neutral
    print(f"building print film profiles from datasheets -> {OUT_DIR}")
    template = load_profile(TEMPLATE)
    import spektrafilm
    table = json.loads((Path(spektrafilm.__file__).parent / "data" / "filters" / "neutral_print_filters.json")
                       .read_text())
    reference = table[TEMPLATE]["TH-KG3"][LEVEL_NEGATIVE]
    reference_raw = lamp_collected(np.asarray(template.data.log_sensitivity, float), reference)
    negative = load_profile(LEVEL_NEGATIVE)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for spec in PRINTS:
        profile, found = build(spec)
        filtration = (0.0, *print_neutral.neutral_filters(negative, profile))
        level = reference_raw - lamp_collected(np.asarray(profile.data.log_sensitivity, float), filtration)
        profile, found = build(spec, level)
        profile.metadata.datasource = (datasource_kodak_1962 if spec.sheet == "kodak_1962" else datasource)(
            spec, found, filtration, reference, level)
        path = OUT_DIR / f"{spec.stock}.json"
        path.write_text(json.dumps(_json_safe(profile_to_dict(profile)), indent=4, allow_nan=False))
        print(f"  {spec.stock:28s} D-min {np.round(found['dmin'], 3).tolist()}  grey share "
              f"{np.round(found['share'], 3).tolist()}  D-max {np.round(found['readback_max'], 2).tolist()} "
              f"(plotted {np.round(found['plotted_max'], 2).tolist()})  unmix {found['unmix']:.1e}  "
              f"fit RMS {found['rms']:.4f}  readback worst {found['worst']:.4f}  250D filtration "
              f"M {filtration[1]:.1f} Y {filtration[2]:.1f}  level {level:+.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
