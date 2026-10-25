"""Where FLX Film Lab's stock parameters depart from agx's on purpose.

Applied to agx's digested parameters both when the packs are baked and when
the parity references are rendered, so the parity harness keeps checking the
port — the engine against agx given the same numbers — rather than these
choices. NOTICE.md lists each one.
"""

from __future__ import annotations

from functools import lru_cache

import numpy as np

import slide_mtf

# Each stock's inter-image colour: agx's coupler matrix for the stock, its
# inter-layer terms scaled (inhibition within a layer adds no colour, only
# edge gain). No sheet gives it as a number; each says how its colour stands
# next to a sister stock, and the rendered colour is matched to that stock's:
# CIELAB chroma on a ColorChecker in the engine, unclipped (BT.2020, no gamut
# compression), against the scene's.
COLOUR: dict[str, dict] = {
    # agx's own per-stock tuning, which the sheets' words do not contradict.
    # Measured like the matches below: Velvia 100 1.31x the scene's chroma,
    # Kodachrome 64 0.92x, Ektachrome E100 1.05x ("moderately enhanced color
    # saturation").
    "fujifilm_velvia_100": {},
    "kodak_kodachrome_64": {},
    "kodak_ektachrome_100": {},
    # Kodak E-55 says of Kodachrome 25 what it says of 64, "Reproduces subtle
    # color naturally", and of 200 "Natural colors": Kodachrome 64's colour,
    # 0.92x the scene's chroma. On 64's coupler strength, with their own sheets'
    # dyes and curves, they render 0.87x and 0.80x. Kodachrome 25's C*/L* then
    # matches 64's too (0.85x to 0.84x); Kodachrome 200's is 0.90x.
    "kodak_kodachrome_25": {"interlayer": 1.25},
    "kodak_kodachrome_200": {"interlayer": 1.6},
    # Felix's pick from side-by-sides (b5c4e09): agx's own Provia 100F had
    # rendered a ColorChecker at 0.94x the scene's chroma, where Fujifilm calls
    # it "vivid and faithful".
    "fujifilm_provia_100f": {"interlayer": 1.3},
    # "Subdued color reproduction and the softest tone reproduction among the
    # 100F films" (Fujifilm Professional Data Guide), below Provia 100F's
    # "medium", with "one of the world's highest levels of color fidelity"
    # (AF3-149E): the scene's own chroma, 1.00x. Its own purer dyes on Provia
    # 100F's coupler strength render 1.14x, above Provia 100F.
    "fujifilm_astia_100f": {"interlayer": 0.65},
    # "the world's highest color saturation equal to that of the current
    # Velvia" (AF3-0221E2), which is what Velvia 100's sheet says of it too
    # ("equal to that of Velvia (ISO50)", reached "through ... new cyan,
    # magenta and yellow couplers"): Velvia 100's saturation, which Velvia 50's
    # less pure dyes need stronger inter-image effects to reach. As chroma, the
    # a*b* distance colour reproduction is plotted by: 1.31x the scene's, as
    # Velvia 100's, where agx's strength gives 1.10x. Its darker colours put
    # C*/L* at 1.40x to Velvia 100's 1.29x; matching that instead would be 1.5.
    "fujifilm_velvia_50": {"interlayer": 1.8},
    # "Brilliant ISO 100-level color reproduction through ... Multi-Color-
    # Correcting Layer Technology", the layer used "to control the inter-image
    # effect": Provia 100F's colour, 1.04x the scene's chroma and C*/L* where
    # Provia 100F's are 1.03x and 1.05x (agx's strength gives 1.28x).
    "fujifilm_provia_400x": {"interlayer": 0.45},
    # Colour negatives, measured on the print. Where a sheet places its colour
    # only in words, "restrained" or "natural" is taken at Kodak Portra 160's
    # chroma (1.06x on Portra Endura) and "high saturation" at Ektar 100's,
    # whose sheet says just that (1.20x, 1.13 times Portra 160's): the one pair
    # in agx's set whose makers' words name the two ends.
    # Agfa ranks its range: Portrait XPS 160 "restrained saturation and flat
    # contrast", Optima II "high". On X-TRA 400's couplers, the dyes its sheet
    # fits best, they printed 1.31x and 1.37x, beyond any film agx's set has
    # on Portra Endura.
    "agfa_optima_ii_100": {"interlayer": 0.7},
    "agfa_portrait_xps_160": {"interlayer": 0.48},
    # "High color saturation ... the highest contrast in the ETERNA series":
    # 1.13 times Eterna 500T's 0.99x on 2383, 1.12x. On Vision3 200T's
    # couplers it printed 1.00x, no more than Eterna 500T.
    "fujifilm_eterna_vivid_160t": {"interlayer": 1.25},
    # Kodak's Vericolor III: "moderate contrast and color saturation" and
    # "excellent flesh-to-neutral color reproduction", and Kodak names Portra
    # 160NC its successor: Portra 160's chroma on Portra Endura (1.048x the
    # scene's, measured with the films below in one harness). On agx's coupler
    # strength, with its own curves, sensitivities, base and dyes, it printed
    # 1.29x.
    "kodak_vericolor_iii_160": {"interlayer": 0.46},
    # Kodak's Royal Gold 100: its sheet puts its colour in no more words than
    # "outstanding flesh-tone and color reproduction", and the film in the Gold
    # family, processed "along with other KODAK GOLD and ROYAL GOLD Films":
    # Gold 200's chroma on the same Ektacolor Edge, 1.19x. On agx's coupler
    # strength it printed 1.36x, beyond any film agx's set has.
    "kodak_royal_gold_100": {"interlayer": 0.59},
}

# Each stock's reference paper, where it is not agx's `target_print`: what
# the inspector offers as "this film's reference paper", what a colour film
# moves to from black and white, and flxlut's and flxrender's default paper.
# The film's own data is unchanged. Felix's pick: Kodak Gold 200 and Ultramax
# 400 on Kodak Ektacolor Edge rather than Portra Endura; agx's
# neutral-filtration table covers both pairs.
PAPER: dict[str, str] = {
    "kodak_gold_200": "kodak_ektacolor_edge",
    "kodak_ultramax_400": "kodak_ektacolor_edge",
}


# Stocks run without DIR couplers. HARMAN Phoenix 200's curves rise about 1.3
# in density a decade, twice a usual colour negative's: at agx's coupler
# strength the inhibition would take more than all of that contrast (b = 1.31,
# where agx's own negatives reach 0.45-0.67), which no development reaches, and
# a film held back as theirs are would not print that hard. It has no masking
# couplers either.
NO_COUPLERS = {"harman_phoenix_200"}

# Stocks whose enlarger filtration is solved at bake time for every colour
# paper and carried in the neutral-filter table (`bake_packs.bake_shared`,
# `print_neutral.neutral_filters`), as agx's own pairs are. Phoenix's unmasked
# base wants twice the usual yellow, and the engine's own solve, started at
# agx's default filtration, does not reach it on most papers.
BAKED_FILTRATION = ("harman_phoenix_200",)

# Films far harder than a colour negative, whose softest paper grade is scaled
# by how much (`bake_packs.softest_grade`), so that Contrast and Auto Contrast
# can print them as softly, for their contrast, as any other film: Phoenix 200
# prints 2.2 times as hard as Portra 400 about mid-grey, and at the usual
# softest grade, R180, still about 40% harder than Portra at R115.
WIDE_GRADES = ("harman_phoenix_200",)

# Films that open at a contrast of their own, as a position of the app's
# Contrast slider on the film's own grade scale (`bake_packs.default_grade`).
# Felix's pick: Phoenix a little softer than its curves print, Contrast -30
# (R200.5), short of the -49 that would print it as hard as Portra 400.
DEFAULT_CONTRAST = {"harman_phoenix_200": -30}


def colour_matrix(stock: str, dir_couplers) -> None:
    """Scale a digested `dir_couplers` in place to the stock's colour."""
    scale = COLOUR.get(stock, {})
    names = {"interlayer": ("gamma_interlayer_r_to_gb", "gamma_interlayer_g_to_rb", "gamma_interlayer_b_to_rg")}
    for which, k in scale.items():
        for name in names[which]:
            setattr(dir_couplers, name, tuple(float(v) * k for v in getattr(dir_couplers, name)))


@lru_cache(maxsize=None)
def slide_spatial(stock: str) -> dict:
    """The slide's own blur and spreading inhibition, fitted to its sheet's MTF
    on the parameters its pack is baked from (`bake_packs.digested_for`)."""
    import bake_packs
    profile = bake_packs.read_profile(stock)
    film_render = bake_packs.digested_for(profile).film_render
    colour_matrix(stock, film_render.dir_couplers)
    d = np.asarray(profile.data.density_curves, float)
    return slide_mtf.fit(stock, profile.data.log_exposure, d - np.nanmin(d, axis=0),
                         slide_mtf._matrix(film_render.dir_couplers), film_render.halation)


def apply(stock: str, profile, film_render) -> dict:
    """Set a digested `film_render` in place to the stock's departures; returns
    the pack fields agx has no parameter for.

    Any stock in `COLOUR`: its colour; in `NO_COUPLERS`: none. Stocks whose sheets plot an MTF
    (`slide_mtf.SHEET_MTF`: the slides and Vericolor III) also: the blur, the
    inhibitor's reach and the share of it that spreads, from the sheet's MTF. A scatter agx has switched off (its LUT mode, which
    the parity references run in) stays off; the colour, a single pixel's, is
    set there as well.
    """
    colour_matrix(stock, film_render.dir_couplers)
    if stock in NO_COUPLERS:
        film_render.dir_couplers.active = False
    if stock not in slide_mtf.SHEET_MTF:
        return {}
    fit = slide_spatial(stock)
    hal = film_render.halation
    if hal.active and max(hal.scatter_tail_um) > 0:
        hal.scatter_core_um = (fit["coreUm"],) * 3
        hal.scatter_tail_um = (fit["tailUm"],) * 3
        hal.scatter_tail_weight = tuple(fit["tailWeights"])
        film_render.dir_couplers.diffusion_size_um = fit["reachUm"]
    return {"edgeGainReference": slide_mtf.REFERENCE_LOG_EXPOSURE, "diffusionFraction": fit["fraction"]}
