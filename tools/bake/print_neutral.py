"""How a colour negative's grey wedge prints, through agx's own pipeline.

agx sets its own negatives up so a grey prints grey, out of public view; a
negative built here from a datasheet's Status M curves is checked the same
way, and its paper chosen by it where agx has none of its maker's (Agfa's
on Portra Endura). This renders a neutral wedge through
spektrafilm's pipeline in its per-pixel mode (which the Metal engine matches,
`tests/parity`) onto the stock's paper, with the enlarger filtration that
prints mid-grey neutral (the engine's `solveNeutralFiltration`), and returns
each step's L*a*b* against the paper's viewing light.
"""

from __future__ import annotations

import dataclasses

import colour
import numpy as np

from spektrafilm import init_params
from spektrafilm.runtime.params_builder import digest_params
from spektrafilm.runtime.pipeline import SimulationPipeline

WORKING = "ITU-R BT.2020"
MID_GREY = 0.184


def _pipeline(profile, paper: str, filters):
    # init_params loads a film by name from agx's own set; the profile itself replaces it
    params = init_params(film_profile="kodak_portra_400", print_profile=paper)
    params.film = profile
    params.io.input_color_space = WORKING
    params.io.input_cctf_decoding = False
    params.io.output_color_space = WORKING
    params.io.output_cctf_encoding = False
    params.io.output_gamut_compress = dataclasses.replace(params.io.output_gamut_compress, algorithm="off")
    params.debug.lut_mode = True
    params.settings.neutral_print_filters_from_database = False
    params.enlarger.c_filter_neutral = 0.0
    params.enlarger.m_filter_neutral = float(filters[0])
    params.enlarger.y_filter_neutral = float(filters[1])
    return SimulationPipeline(digest_params(params))


def _lab(rgb: np.ndarray) -> np.ndarray:
    xyz = colour.RGB_to_XYZ(rgb, colourspace=WORKING, apply_cctf_decoding=False)
    return colour.XYZ_to_Lab(xyz, colour.CCS_ILLUMINANTS["CIE 1931 2 Degree Standard Observer"]["D65"])


def printed(profile, paper: str, filters, stops) -> np.ndarray:
    """L*a*b* of greys at `stops` relative to 18%, printed with (magenta, yellow)."""
    v = MID_GREY * 2.0 ** np.asarray(stops, float)
    image = np.repeat(v.reshape(1, -1, 1), 3, axis=2)
    out = np.asarray(_pipeline(profile, paper, filters).process(image), float)[0]
    return _lab(out)


def neutral_filters(profile, paper: str, start=(65.0, 55.0)) -> np.ndarray:
    """Magenta and yellow that print mid-grey neutral: Newton from agx's defaults."""
    my = np.array(start, float)
    for _ in range(12):
        f = printed(profile, paper, my, [0.0])[0, 1:]
        if np.hypot(*f) < 0.005:
            break
        step = 0.5
        dm = (printed(profile, paper, my + [step, 0], [0.0])[0, 1:] - f) / step
        dy = (printed(profile, paper, my + [0, step], [0.0])[0, 1:] - f) / step
        jac = np.stack([dm, dy], axis=1)
        if abs(np.linalg.det(jac)) < 1e-12:
            break
        my = np.clip(my - np.linalg.solve(jac, f), 0, 400)
    return my


def wedge(profile, paper: str, stops=(-3, -2, -1, 0, 1, 2)) -> np.ndarray:
    """The wedge's L*a*b*, mid-grey printed neutral."""
    return printed(profile, paper, neutral_filters(profile, paper), stops)


if __name__ == "__main__":
    # python print_neutral.py STOCK [PAPER ...]: the stock's grey wedge from
    # four stops under to three over, on its own paper or on each one named.
    import sys
    import bake_packs
    stock, papers = sys.argv[1], sys.argv[2:]
    profile = bake_packs.read_profile(stock)
    for paper in papers or [profile.info.target_print]:
        lab = wedge(profile, paper, (-4, -3, -2, -1, 0, 1, 2, 3))
        print(f"{stock} on {paper}: L* " + "/".join(f"{v:.0f}" for v in lab[:, 0])
              + "  a*/b* " + "  ".join(f"{a:+.1f}/{b:+.1f}" for a, b in lab[:, 1:]))
