#!/usr/bin/env python3
"""Bake the V-Log LUT pack that goes out with a release.

Every film stock in the app but those in LEFT_OUT, on its reference paper (a
slide on its light table), as a table for a Lumix camera's Real Time LUT;
Tri-X through each of its five camera filters; and, for daylight, EXR 200T
through a Wratten 85 and an 85B, on F-CP 3519, and 5251 through an 85, on
5385. Tri-X, pushed and through its filters too, HP5
Plus and T-Max 100 are printed at Contrast +20, ISO R102: at the neutral
R115 they print a little flat. A film that opens at a contrast of its own
(Phoenix 200) is printed at it, as the app opens it.

    python3 tools/lut/vlog_pack.py [VERSION]

writes build/FLX-Film-Lab-V-Log-LUTs-VERSION.zip; VERSION defaults to the one
build.sh gives local builds. It needs only the system's Python: flxlut, which
it builds first, does the baking.
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import textwrap
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
INDEX = REPO / "FLXFilmLab" / "Resources" / "packs" / "index.json"
FLXLUT = REPO / "build" / "flxlut"
LICENCE = REPO / "profiles" / "LICENSE-CC-BY-SA-4.0.txt"

# Each stock's file name: at most eight letters and digits, for the camera's
# FAT32 card. A stock the app gains without one here or in LEFT_OUT stops the
# bake.
NAMES = {
    "agfa_optima_ii_100": "OPTI100",
    "agfa_portrait_xps_160": "XPS160",
    "fujifilm_c200": "FUJIC200",
    "fujifilm_pro_400h": "PRO400H",
    "fujifilm_superia_1600": "SUPR1600",
    "fujifilm_superia_reala_100": "REALA100",
    "fujifilm_xtra_400": "XTRA400",
    "harman_phoenix_200": "PHNX200",
    "kodak_ektar_100": "EKTAR100",
    "kodak_gold_200": "GOLD200",
    "kodak_portra_160": "PORTR160",
    "kodak_portra_400": "PORTR400",
    "kodak_portra_800": "PORTR800",
    "kodak_royal_gold_100": "RGOLD100",
    "kodak_ultramax_400": "UMAX400",
    "kodak_vericolor_iii_160": "VCIII160",
    "konica_centuria_super_100": "CSUP100",
    "konica_centuria_super_1600": "CSUP1600",
    "konica_impresa_50": "IMPRS50",
    "fujifilm_eterna_500t": "ETER500T",
    "fujifilm_eterna_vivid_160t": "EVIV160T",
    "fujifilm_reala_500d": "REAL500D",
    "kodak_exr_200t": "EXR200T",
    "kodak_exr_50d": "EXR50D",
    "kodak_5251_50t": "ECN5251",
    "kodak_verita_200d": "VERI200D",
    "kodak_vision3_200t": "VIS200T",
    "kodak_vision3_250d": "VIS250D",
    "kodak_vision3_500t": "VIS500T",
    "kodak_vision3_50d": "VIS50D",
    "fujifilm_astia_100f": "ASTI100F",
    "fujifilm_provia_100f": "PROV100F",
    "fujifilm_provia_400x": "PROV400X",
    "fujifilm_velvia_100": "VELV100",
    "fujifilm_velvia_50": "VELV50",
    "kodak_ektachrome_100": "E100",
    "kodak_kodachrome_25": "KCHR25",
    "kodak_kodachrome_64": "KCHR64",
    "kodak_kodachrome_200": "KCHR200",
    "agfa_apx_25": "APX25",
    "fujifilm_neopan_1600": "NEOP1600",
    "ilford_hp5_plus": "HP5PLUS",
    "ilford_delta_100": "DELTA100",
    "ilford_delta_400": "DELTA400",
    "ilford_fp4_plus": "FP4PLUS",
    "ilford_pan_f_plus": "PANFPLUS",
    "ilford_ortho_plus": "ORTHPLUS",
    "kodak_tri_x_400": "TRIX400",
    "kodak_tri_x_400_push2": "TRIXPSH2",
    "kodak_t_max_100": "TMAX100",
}
# Stocks the app has that the pack leaves out. Felix's pick: Portra 800 pushed
# one and two stops.
LEFT_OUT = {"kodak_portra_800_push1", "kodak_portra_800_push2"}
# Tri-X through each camera filter flxlut knows: its name, file name and
# Kodak Wratten number.
FILTERED = "kodak_tri_x_400"
FILTERS = [("blue", "TRIXBLU", 47), ("green", "TRIXGRN", 58), ("yellow", "TRIXYEL", 8),
           ("orange", "TRIXORG", 22), ("red", "TRIXRED", 25)]
# Colour films through one of their camera filters, each on a paper named here:
# film, filter, file name, paper. Felix's picks: the tungsten EXR 200T and
# 5251 converted for daylight on their own print films, EXR 200T with the 85
# its sheet names and with an 85B, 5251 with the 85 Kodak named for it.
CONVERTED = [("kodak_exr_200t", "85", "EXR2T85", "fujifilm_fcp_3519"),
             ("kodak_exr_200t", "85b", "EXR2T85B", "fujifilm_fcp_3519"),
             ("kodak_5251_50t", "85", "5251W85", "kodak_5385")]
# Printed harder than the neutral R115, which leaves them flat. Contrast +20
# is R115 - 20 x 0.65 (ControlVocabulary.contrast).
HARDER = {"kodak_tri_x_400": 102, "kodak_tri_x_400_push2": 102, "ilford_hp5_plus": 102, "kodak_t_max_100": 102}
HARDER_NOTE = "Contrast +20"
WORDS = ["no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"]


def number(n: int) -> str:
    return WORDS[n] if n < len(WORDS) else str(n)


def groups(films: list[dict]) -> list[tuple[str, list[dict]]]:
    """The film menu's groups, in its order (PackIndexEntry.groups)."""
    bw = [e for e in films if e["channelModel"] == "bw"]
    colour = [e for e in films if e["channelModel"] != "bw"]
    return [("Colour negative", [e for e in colour if e["use"] != "cine" and e["type"] != "positive"]),
            ("Motion picture", [e for e in colour if e["use"] == "cine"]),
            ("Colour slide", [e for e in colour if e["use"] != "cine" and e["type"] == "positive"]),
            ("Black and white", bw)]


def main() -> None:
    version = sys.argv[1] if len(sys.argv) > 1 else \
        re.search(r'^VERSION="\$\{VERSION:-([^}]*)\}"$', (REPO / "build.sh").read_text(), re.M).group(1)
    subprocess.run([str(REPO / "build.sh"), "lut"], check=True, stdout=subprocess.DEVNULL)

    index = json.loads(INDEX.read_text())
    films = [e for e in index["filming"] if e["stock"] not in LEFT_OUT]
    papers = {e["stock"]: e["name"] for e in index["printing"]}
    missing = [e["stock"] for e in films if e["stock"] not in NAMES]
    stale = sorted((set(NAMES) | LEFT_OUT) - {e["stock"] for e in index["filming"]})
    if missing or stale:
        sys.exit(f"error: file names to add for {missing}, to remove for {stale}")
    for name in list(NAMES.values()) + [f[1] for f in FILTERS] + [c[2] for c in CONVERTED]:
        assert re.fullmatch(r"[A-Z0-9]{1,8}", name), name
    filters = {e["stock"]: {f["id"]: f["name"] for f in json.loads((INDEX.parent / f"{e['stock']}.json")
                                                                    .read_text()).get("lensFilters") or []}
               for e in films}

    title = f"FLX Film Lab V-Log LUTs {version}"
    stage = REPO / "build" / "vlog-pack" / title
    shutil.rmtree(stage.parent, ignore_errors=True)
    stage.mkdir(parents=True)

    def bake(name: str, film: dict, filter_: tuple[str, str] | None = None, paper_stock: str | None = None,
             shared_paper: bool = False) -> str:
        """Bakes one table, through a filter (its id and how the read-me says
        it) and on a paper of its own if given; returns its line in the read-me."""
        paper_stock = paper_stock or film["targetPrint"]
        paper = papers.get(paper_stock) if paper_stock else None
        args = [str(FLXLUT), str(stage / f"{name}.cube"), "--film", film["stock"],
                "--input", "vlog", "--output", "srgb", "--size", "33"]
        if paper_stock != film["targetPrint"]:
            args += ["--paper", paper_stock]
            shared_paper = False
        what = film["name"]
        if filter_:
            args += ["--filter", filter_[0]]
            what += f" through {filter_[1]}"
        grade = HARDER.get(film["stock"])
        if grade:
            args += ["--grade", str(grade)]
            # flxlut's own title, which the camera shows, with the harder print.
            shown = film["name"] + (f" ({filter_[0].capitalize()} filter)" if filter_ else "")
            args += ["--title", f"{shown} on {paper}, {HARDER_NOTE}"]
        subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
        where = "" if shared_paper else f", on {paper}"
        return f"  {name:<9} {what}{where}" + (f", {HARDER_NOTE}" if grade else "")

    # A group whose films all print on one paper names it once, in its heading.
    listing = []
    for heading, entries in groups(films):
        prints = {e["targetPrint"] for e in entries}
        shared = len(prints) == 1
        if shared:
            only = prints.pop()
            heading += f", printed on {papers[only]}" if only else ", on a light table"
        listing += ["", heading]
        for film in entries:
            listing.append(bake(NAMES[film["stock"]], film, shared_paper=shared))
            if film["stock"] == FILTERED:
                listing += [bake(name, film, (colour, f"{'an' if colour[0] in 'aeiou' else 'a'} {colour} filter "
                                                     f"(Wratten {wratten})"), shared_paper=shared)
                            for colour, name, wratten in FILTERS]
            listing += [bake(name, film, (filter_, f"a {filters[stock][filter_]}"), paper, shared_paper=shared)
                        for stock, filter_, name, paper in CONVERTED if stock == film["stock"]]

    cubes = sorted(stage.glob("*.cube"))
    assert len(cubes) == len(NAMES) + len(FILTERS) + len(CONVERTED), len(cubes)
    for cube in cubes:
        head = cube.read_text().splitlines()[:12]
        assert "#LUMIXPHOTOSTYLE VLOG" in head and "LUT_3D_SIZE 33" in head, cube.name

    tungsten = [e["name"] for e in films if re.search(r"\d+T$", e["name"])]
    pushes = [e for e in films if "push" in e["stock"]]
    assert [e["stock"] for e in pushes] == ["kodak_tri_x_400_push2"], "the read-me names the pushes"
    text = README.format(
        title=title, underline="=" * len(title), version=version, count=len(cubes),
        stocks=len(films) - len(pushes), filters=number(len(FILTERS)),
        tungsten=", ".join(tungsten[:-1]) + " and " + tungsten[-1], listing="{listing}")
    # Paragraphs are written unwrapped and wrapped here, names and all.
    paragraphs = [p if p.startswith(("=", title)) else textwrap.fill(p, 78) for p in text.split("\n\n")]
    listing_text = "\n".join(listing).lstrip("\n")
    (stage / "Read me.txt").write_text("\n\n".join(paragraphs).replace("{listing}", listing_text) + "\n")
    shutil.copy(LICENCE, stage / "LICENSE.txt")

    archive = REPO / "build" / f"FLX-Film-Lab-V-Log-LUTs-{version}.zip"
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(stage.iterdir()):
            zf.write(path, f"{title}/{path.name}")
    print(f"wrote {archive} ({len(cubes)} tables, {archive.stat().st_size / 1e6:.1f} MB)")


README = """\
{title}
{underline}

All {stocks} film stocks in FLX Film Lab {version}, with Tri-X pushed two stops, Tri-X through each of its {filters} camera filters, and, for daylight, EXR 200T through a Wratten 85 and an 85B and Kodak 5251 through an 85: {count} LUTs for a Lumix camera's Real Time LUT, which applies them to photos (JPEG and HEIF) as well as to video.

Colour negatives are printed on their usual paper, black-and-white films on Ilford Multigrade, and slide films are shown as transparencies, all with the app's default settings, except that Tri-X, pushed and through its filters too, HP5 Plus and T-Max 100 are printed at Contrast +20 (ISO R102), as they come out a little flat otherwise. Phoenix 200 is printed at the slightly softer contrast the app opens it at, Contrast -30 on its own scale (ISO R200.5).

Each is a 33-point .cube file for V-Log: full-range V-Log in, the sRGB curve out, with V-Log named as the base Photo Style. The file names are at most eight letters and digits, as the camera's card needs.

In the camera

Copy the files you want onto the SD card and register them in the camera's LUT library, as its manual describes, then pick one under Real Time LUT. The base Photo Style is V-Log.

In DaVinci Resolve

Rec.709 Gamma 2.4 timeline without colour management; apply the LUT on a node or as the clip's LUT, and set Clip Attributes > Data Levels to Full. The LUT then reads the clip exactly as the camera does. The output keeps the camera's sRGB curve, so shadows sit slightly lighter than in a LUT made for a Gamma 2.4 timeline.

Shooting notes

White balance: set the camera to 5500 K, the way daylight film is balanced. Tungsten light then goes orange, as it would on film. For the tungsten stocks, {tungsten}, set 3200 K. A LUT takes whatever the camera records as white to be the light the film is balanced for: at 3200 K daylight records blue and a tungsten stock keeps it blue, while tungsten light prints neutral, like the real thing.

Exposure: shoot the negative stocks as metered. These are simulations of prints, so the limiting factor is usually the paper. Slide films hold only about 1.5 to 2 stops above mid-grey, as the real films do, so expose them for the highlights, around -2/3 to -1 EV. Astia, the softest of them, holds about half a stop more. Tri-X pushed two stops carries the push's contrast; shoot it as metered too.

Tri-X with filters: Kodak Wratten 47, 58, 8, 22 and 25, blue to red, as you would screw them onto the lens for black-and-white film. The filter factor is built in, so shoot as metered.

EXR 200T with an 85 or an 85B (EXR2T85, EXR2T85B) and 5251 with an 85 (5251W85): tungsten film shot in daylight through the orange filter that converts it, as on a motion picture camera outdoors, each printed on its own print film, Fujifilm F-CP 3519 and Kodak 5385. Set 3200 K, as for the other tungsten stocks: daylight records blue, and the filter, built in, converts it. The 85 is the filter Kodak names for both films in daylight. Rated to take daylight to 3400 K, a little short of their 3200 K, it prints 5251 neutral and EXR 200T a little cool, which on film the lab balanced out in printing. The 85B is rated to take daylight to 3200 K; as Kodak measures it, it goes a little further, and the film's own layers see it so: EXR 200T prints a little warm, as it would on the film. The filter factors are built in too, so shoot as metered.

Grain, halation and the film's sharpness depend on neighbouring pixels, so no LUT can carry them; FLX Film Lab itself renders them for RAW photos.

The LUTs

{listing}

Licence

Made with FLX Film Lab (github.com/felixjablonka/flx-film-lab) from its film and paper data, which builds on Andrea Volpato's spektrafilm. Like that data, these LUTs are licensed CC BY-SA 4.0 (LICENSE.txt): use them for anything, paid work included, and if you pass them on, changed or not, credit FLX Film Lab and keep the licence.

Film, paper and camera names are trademarks of their owners, used here only to say what a LUT models. FLX Film Lab is not affiliated with or endorsed by any of them.
"""

if __name__ == "__main__":
    main()
