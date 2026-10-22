# Notices and attribution

FLX Film Lab is licensed under the **GNU General Public License v3.0** (see `LICENSE`).
Copyright © 2026 Felix Jablonka.

## agx-emulsion / spektrafilm

Copyright © Andrea Volpato — <https://github.com/andreavolpato/spektrafilm>
(the project was formerly, and is still reachable as, `agx-emulsion`).

FLX Film Lab's render engine is a port of spektrafilm's physical model to Metal.
The following are direct ports, and the Metal source names its Python original
at each one:

- Hanatos-2025 spectral upsampling and the film sensitivity projection
  (`utils/spectral_upsampling.py`), consumed here as a baked chromaticity LUT
- characteristic-curve development and DIR couplers (`model/develop.py`,
  `model/couplers.py`)
- the dichroic colour enlarger and print exposure (`runtime/stages/printing.py`,
  `model/color_filters.py`)
- the print scan and viewing transform (`runtime/stages/scanning.py`)
- in-emulsion scatter and back-reflection halation (`model/diffusion.py`)
- the Poisson–Binomial grain particle model (`model/grain.py`)
- Mitchell–Netravali LUT interpolation (`utils/fast_interp_lut.py`)

spektrafilm is GPLv3, and its author states that derivative work includes
software directly inspired by its methods. FLX Film Lab is therefore GPLv3.

Film modelling powered by `spektrafilm`.

### Profile data — CC BY-SA 4.0

The contents of `FLXFilmLab/Resources/packs/` are derived from spektrafilm's
profile JSONs and spectral LUTs, which are licensed **CC BY-SA 4.0**
(`packs/LICENSE-CC-BY-SA-4.0.txt`). They are redistributed here in a
re-serialised binary/JSON form for the Metal engine; the underlying measurements
and fits are unchanged. Attribution: spektrafilm profiles by Andrea Volpato.

The profiles were built from published manufacturer datasheets (Kodak,
Fujifilm), scientific publications, and public reflectance datasets — see the
`metadata.datasource` field inside each pack.

## NegPy

Copyright © marcinz606 and contributors — <https://github.com/marcinz606/NegPy>

No NegPy code is included. Its **darkroom control design** is the model for
FLX Film Lab's tone stage, and several of its numeric choices are reused:

- contrast as an **ISO-R** grade value, and split grade as zone-local slope
  rotation in ISO-R points
- **zone controls**: separate shadow/highlight moves that spare the midtones,
  and regional colour weighted by the same zones
- regional (global / shadow / highlight) **colour timing**
- per-layer **crossover trims** on grade, toe, shoulder and midtone gamma
- **masked dodge & burn** as a print-exposure change rather than a brightness
  overlay, with polygon / oval / gradient shapes, invert, and a per-mask
  **local grade**
- **auto density and auto grade metered on textured areas only** (NegPy 0.58,
  after Boyack & Juenger, US 5,724,456), with a partial pull so a low-key or
  high-key frame keeps its mood

NegPy is GPLv3.

## Documented departures from the references

Kept here rather than buried in the source, because they are the places where
FLX Film Lab is *not* a faithful reproduction:

1. **Output gamut compression** is spektrafilm's `oklch` algorithm — chroma as
   a fraction of the most the output space can hold at that lightness and hue,
   through the Reinhard knee — with the boundary found per pixel by bisection
   instead of read from a cached table. spektrafilm's main-branch default is
   CAM16-UCS, which is far costlier per pixel. The knee is upstream's retuned
   (0.95, 1.0, 1.6) from its experimental branch, which leaves in-gamut colour
   alone; its optional lightness compression is not ported, since it would pull
   a slide's clear base below white.
2. **In-emulsion scatter** uses the mean of the three per-channel sigmas rather
   than blurring each channel separately. They differ by under 30% and land
   below one pixel at any real output size. Back-reflection halation — the
   visible part — keeps its exact per-channel strengths and sqrt(k) bounce
   widths.
3. **Grain** implements the single-layer Poisson–Binomial particle model
   (spektrafilm's `sublayers_active = False` path), softened by its `blur`
   (0.65 px) as agx softens it — but only the grain is softened, not the image
   under it, so turning grain on costs no sharpness. The three-sub-layer
   refinement and the dye-cloud micro-structure are not ported yet. Its effect
   on the *average* colour is taken back out: the grain-free print is rendered
   too, and each pixel is scaled so that its neighbourhood (a 3 px Gaussian)
   holds the grain-free light. Grain drawn per pixel is noisier the smaller the
   pixel, and through the nonlinear print and scan that noise shifted the mean
   colour with resolution — a full-size export came out cooler and lighter
   than the preview. The measured curves are large-area densities with grain
   already in them, so the grain-free mean is also the faithful one. The
   correction is a gain rather than an offset because an offset pushed the dark
   channel of saturated colours below zero, where every file clips it.
4. **Toe and shoulder** are exponential-reach lifts of the paper-black ceiling
   and paper-white floor, not NegPy's softplus knees. NegPy draws its own
   straight line and adds knees to it; here the paper curve already carries its
   own toe and shoulder, so a softplus bound would compress a second time even
   at slider zero. The reformulation is exactly identity at zero, which is what
   lets the parity tests hold.
5. **RAW decoding** goes through Core Image rather than LibRaw, so the absolute
   scale of the decoded linear values differs from spektrafilm's. The mid-grey
   metering normalises this away before the film stage.
6. **Scanner black/white correction** (`ScannerParams.black_correction` /
   `white_correction`) is not ported. It is off by default upstream.

7. **Exposure holds the print time fixed** rather than re-timing the enlarger
   for the negative's density (agx's `print_exposure_compensation`). Upstream's
   behaviour is the darkroom reflex but makes the control invisible. This
   matches what agx itself does under `lut_mode`.
8. **Transparency scans are white-balanced on the film base**, which agx does
   not do — its `scan_film` reports absolute transmittance. A projector, a light
   table and a scanner all take clear film as their white point. Here that is a
   von Kries adaptation (CAT02) from the base to the light table's own
   illuminant. Off (`Recipe.normalizeToBase = false`) reproduces agx exactly,
   which is how the parity harness runs it.
9. **Brightness, Shadows and Highlights act on the print's lightness**, not on
   density, and move all three dye layers together by their mean density.
   NegPy's zone density offsets and its density-domain zone weights reached
   far into the midtones on these papers and could reverse tones at full
   travel; here each control keeps to its band and the curve stays monotone.
   The split-contrast and regional-colour zones use the same lightness scale.
   Auto Contrast fits the frame's textured shadow-to-highlight range onto the
   paper rather than NegPy's textural-range formula.
10. **16 colour profiles are built here rather than upstream** — every
   slide film (Provia 100F, Provia 400X, Astia 100F, Velvia 50, Velvia 100,
   Ektachrome E100, Kodachrome 25, 64 and 200), Superia 1600 and Superia Reala
   100, Agfa Optima II 100 and Portrait XPS 160, and Fujifilm's Eterna Vivid
   160T, Eterna 500T and Reala 500D motion picture negatives, in `profiles/`;
   for the four agx also has (Provia 100F, Velvia 100, Ektachrome E100 and
   Kodachrome 64), they replace its profiles. Their characteristic curves,
   spectral sensitivities and, where a sheet plots them, dye spectra are
   extracted from the manufacturer datasheets. What a sheet does not give — the
   base density, the Hanatos-2025 adaptation, the coupler matrix, and the dye
   spectra the negatives' sheets lack — is inherited from the agx profile of
   the same or a sister stock (for Agfa's two, which agx has none of, the one
   whose dyes fit the neutral spectra Agfa plots best: X-TRA 400). Each records the split in its own `datasource`
   field. They are derivative of agx profile data and so remain CC BY-SA 4.0.
   The sheets' integral densities (Status A for a slide, Status M for the
   negative) are converted into dye amounts with the dye spectra. A slide's red
   and blue layer speeds are set so a grey views neutral from three stops under
   mid-grey to two over, as each of these sheets says its greys are, within
   0.07 decades of where the sheet draws them; a trace alone left visible
   casts. Superia 1600 is metered at its EI 1600, takes the per-layer contrast
   correction agx applied to its X-TRA 400 against Fujifilm's sheet (red
   x0.922, green x0.965, blue x1.093), and has its layer speeds set so a grey
   prints neutral; Superia Reala 100 takes the same correction, metered at its
   ISO 100. A negative is metered at its rating on its sheet's axis where the
   axis places that rating (Agfa's do exactly), at camera stop 0 on Fujifilm's
   Eterna sheets, and else (Reala 500D) where those put normal exposure, 1.05
   decades over the speed point. Agfa's two print on Portra Endura, agx having
   no Agfa paper, as the paper their greys print most neutral on. A colour negative missing from agx's neutral-filtration
   table, as Superia 1600 is, has the filtration solved for a neutral mid-grey,
   as item 11's black-and-white negatives on colour paper do. The Status A and
   M responsivities in `tools/bake/data/densitometer/` are agx's digitisation
   of ISO 5-3, after Giorgianni & Madden (2009).
11. **Black-and-white stocks are built here, from datasheets alone** — Kodak
   Tri-X 400 (Kodak F-4017), Ilford HP5 Plus and Ilford Multigrade RC Deluxe
   (HARMAN technology technical information), Fujifilm Neopan 1600
   (Fujifilm AF3-608E) and Agfa APX 25 (Agfa's Professional range sheet), in
   `profiles/` by
   `tools/bake/build_bw_profiles.py`. agx's own black-and-white work (Double-X,
   2302, a Tri-X reversal) sits on its unmerged `dev` branch with a one-channel
   pipeline; here a silver layer is carried as three identical channels a third
   each, so the three-layer engine renders it unchanged and neutral by
   construction, with grain drawn once for all three. These profiles copy no
   agx data, only its format. Choices the datasheets leave open: mid-grey sits
   1.007 decades over the ISO speed point; film curves continue at their final
   gradient for 0.8 decades past the plotted range; Ilford's wedge spectrograms
   are read as relative log sensitivity rather than as tungsten-weighted, which
   would give HP5 Plus a red-filter factor of 12-16x; HP5 Plus, for which Ilford
   publishes no RMS granularity, takes Tri-X's 17; Multigrade is modelled as one
   emulsion at grade 1, so its contrast comes from the Contrast control rather
   than from enlarger filtration; halation, which agx tunes per dye layer,
   takes the bottom layer's strength on all three; and the scatter, with
   development's adjacency effect run as the layer's own DIR-style inhibition,
   is fitted to Kodak's published MTF for Tri-X in D-76 (F-4017) rather than
   taken from agx's colour defaults — HP5 Plus and Neopan 1600, with no
   published MTF, take the same, and APX 25 is fitted to Agfa's own curve (to
   50 cycles/mm) with Agfa's RMS 7. Neopan 1600, whose EI 1600 rating sits a push
   above its curve's own ISO speed point (721), has its mid-grey where a meter
   set to EI 1600 puts it on Fujifilm's absolute axis; its daylight
   spectrogram is divided by CIE daylight at 5400 K; and, with no published
   granularity, its grain is Tri-X's 17 scaled by the square root of the two
   speed points, RMS 23. Tri-X 400 (Push 2) takes Kodak's D-76 time for EI 1600,
   10 3/4 minutes, which the sheet does not plot: its curve is interpolated
   between the sheet's 10 and 12 minute curves along exposure at each density,
   and its mid-grey sits where a meter set to EI 1600 puts it on Kodak's
   absolute axis. Kodak gives no granularity for a push, so its grain is Tri-X's
   17 scaled by the square root of the two curves' slopes (Selwyn), RMS 19. A
   black-and-white negative on colour paper, a pair agx's neutral-filtration
   table does not cover, has its filtration solved so a mid-grey prints
   neutral. Camera filters for these
   stocks use Kodak's published Wratten 2 transmittance (47, 58, 8, 22, 25),
   with the exposure opened up by each filter factor. And where agx clamps the
   spectral LUT's coordinate for a chromaticity outside the xy triangle — a
   negative X, Y or Z, which demosaicing noise produces in deep shadow — a
   black-and-white stock has the chromaticity pulled back towards white onto
   the triangle instead: a single layer otherwise prints such a pixel several
   times too light. Colour stocks keep agx's clamp.
12. **Inter-image colour is set from each datasheet's words** for the stocks
   built here, rather than taken from agx, which sets coupler strength by eye.
   No sheet gives it as a number; each says how its colour stands next to a
   sister stock. Only the inter-layer terms of agx's coupler matrix are scaled,
   since same-layer inhibition shapes edges rather than colour, until a
   ColorChecker renders at that stock's chroma, measured unclipped in BT.2020
   against the scene's: Velvia 50 at Velvia 100's 1.31x (its sheet: saturation
   "equal to that of the current Velvia"; x1.8), Provia 400X at Provia 100F's
   1.04x ("ISO 100-level color reproduction"; x0.45), Kodachrome 25 and 200 at
   Kodachrome 64's 0.92x (Kodak E-55 gives them the same words; x1.25 and x1.6),
   Astia 100F at the scene's own 1.00x ("subdued color reproduction ... among
   the 100F films" and "color fidelity"; x0.65). Provia 100F runs at x1.3,
   picked from side-by-sides: at agx's value it rendered 0.94x the scene's
   chroma, where Fujifilm calls it "vivid and faithful"; it now renders 1.03x.
   Velvia 100, Kodachrome 64 and Ektachrome E100 keep agx's values. Colour
   negatives are measured on the print, a sheet's "restrained" or "natural"
   taken at Kodak Portra 160's chroma (1.06x on Portra Endura) and "high
   saturation" at Ektar 100's (1.20x), whose sheet says just that: Agfa Portrait
   XPS 160 ("restrained") x0.48, Optima II 100 ("high") x0.7, and Eterna Vivid
   160T ("high color saturation") 1.13 times Eterna 500T's chroma on 2383,
   x1.25. The overrides are in `tools/bake/stock_overrides.py` and are applied
   to the parity references too, so parity still compares the engine with agx at
   equal parameters.
13. **Slide films' sharpness is fitted to their datasheets' MTF**
   (`tools/bake/slide_mtf.py`) rather than taken from agx's defaults, with two
   engine changes agx does not have. agx's DIR couplers sharpen fine detail by
   1/(1 - b), b the share of a layer's contrast its inhibition takes, which
   grows with the curve's slope, so on a slide's steep shadows the edge effect
   ran away: two stops under mid-grey Velvia 50 read 145% at 10 cycles/mm, and
   Provia 400X on 100F's couplers 180%, where their sheets show 122% and 92%.
   Each slide now lets only a share of its inhibitor diffuse
   (`diffusionFraction`; the rest acts where it is released), and the engine
   holds the diffused part's fine-detail gain at the gain one stop under
   mid-grey (`edgeGainReference`, `FilmMath.edgeGainScale`). The share, the
   inhibitor's reach and the scatter's core, tail and per-layer weights are
   fitted to the sheet's whole MTF curve: one stop under, every slide is within
   6 points of its sheet from 5 to 30 cycles/mm. Separately, the engine inverts
   the couplers (agx's `compute_density_curves_before_dir_couplers`) on a
   log-exposure grid 16 times finer than agx's 256 samples, which undersampled
   the inverted curve under strong couplers (up to 0.017 in density, and slope
   steps of 0.6, on Velvia 50 and 100). The parity references run with spatial
   effects off, so of these only the grid reaches them, and they use it too.
14. **Kodak Gold 200 and Ultramax 400 suggest Kodak Ektacolor Edge** as their
   paper, where agx's profiles name Portra Endura (`target_print`): the paper
   the inspector offers as the film's reference paper, and flxlut's and
   flxrender's default. Only the suggestion changes, not the films' data, and
   agx's neutral-filtration table covers both pairs
   (`tools/bake/stock_overrides.py`).

Everything else matches: `tests/parity` checks the film exposure and negative
density taps as bit-exact and the final RGB to within 6e-4 across four
film/paper combinations, including a positive slide film and a cine print film,
plus two combinations through the direct transparency path.

## Glow

`FLXFilmLab/Effects/` holds Glow's bloom, glow and halation, vendored unchanged
from Glow, the companion stills app, apart from a header, so they can be kept in
step with it. Glow is by the same author, who licenses that code here under the
GPLv3 with the rest. Glow's bloom is the Photoshop "Pro Mist" recipe; its glow and
halation follow NegPy. The Stills mode uses the bloom and the glow; the Motion
mode all three.

## Log curves: N-Log, V-Log, Apple Log, Apple Log 2, F-Log, Canon Log, S-Log3

The N-Log curve (`FLXFilmLab/Engine/NLog.swift`) is Nikon's, from the *N-Log
Specification Document* 1.0.0 (Nikon, 2018), with its constants as
colour-science (BSD-3-Clause) implements them in `log_encoding_NLog`. The
V-Log curve and V-Gamut primaries (`FLXFilmLab/Engine/VLog.swift`) are
Panasonic's, from the *V-Log/V-Gamut Reference Manual* (Panasonic, 2014), with
the constants as colour-science gives them in `log_encoding_VLog`. The
Apple Log curve (`FLXFilmLab/Engine/AppleLog.swift`) is Apple's, from the
*Apple Log Profile White Paper* (Apple, 2023), with its constants as
colour-science implements them in `log_encoding_AppleLogProfile`; Apple Log
2's Apple Wide Gamut primaries are from the *Apple Log 2 Profile White Paper*
(Apple, 2025) as OpenColorIO's built-in `APPLE_LOG2` transform gives them.
The F-Log and F-Log2 curves (`FLXFilmLab/Engine/FLog.swift`) are Fujifilm's,
from the *F-Log Data Sheet* Ver.1.1 and *F-Log2 Data Sheet* Ver.1.0
(Fujifilm, 2022), with the constants as colour-science gives them in
`log_encoding_FLog` and `log_encoding_FLog2`. The Canon Log 2 and Canon Log 3
curves (`FLXFilmLab/Engine/CanonLog.swift`) are Canon's, v1.2, from the *Input
Transform Version 202007 for EOS C300 Mark II* (Canon, 2020), with the
constants as colour-science gives them in `log_encoding_CanonLog2_v1_2` and
`log_encoding_CanonLog3_v1_2`; the Cinema Gamut primaries are colour-science's,
from Canon's *EOS C500 Firmware Update* (2014). The S-Log3 curve and the
S-Gamut3 and S-Gamut3.Cine primaries (`FLXFilmLab/Engine/SLog3.swift`) are
Sony's, from the *Technical Summary for S-Gamut3.Cine/S-Log3 and
S-Gamut3/S-Log3* (Sony), with the constants as colour-science gives them in
`log_encoding_SLog3`. No colour-science or OpenColorIO code is included.

## App icon

The app icon (`FLXFilmLab/Resources/AppIcon.icns`, and the artwork it is made
from) is © Felix Jablonka, all rights reserved. Neither the GPL nor CC BY-SA
covers it: a fork or redistribution needs its own icon.

## Manufacturer datasheets

The profiles are measured off the manufacturers' published datasheets (Kodak,
Kodak Alaris, Fujifilm, Agfa, HARMAN technology / Ilford). The sheets themselves
are not included; `docs/datasheets.md` lists them. Film, paper, filter, camera and
format names are trademarks of their owners, used only to say what a profile or
format models; FLX Film Lab is not affiliated with or endorsed by any of them.
