# How each profile is built

How each film and paper profile in `profiles/` was built from its
manufacturer's datasheet, and where a sheet disagrees with itself.
`docs/building-profiles.md` holds the rules every build follows, and
`docs/datasheets.md` lists the sheets and the file names the build scripts
expect them under in `film spec sheets/`.

`tools/bake/build_profiles.py` builds film profiles straight out of a
manufacturer's PDF. Kodak publishes its sensitometry as vector art, so
`datasheet.py` reads the curve geometry out of the drawing commands and
calibrates it against the plot's own tick labels — no clicking on pixels, and
accurate to the line width. Fujifilm embeds bitmaps, so `trace_raster_plot.py`
traces those instead, then smooths and monotonises the result (a characteristic
curve is monotonic by physics; a traced bitmap is not, and a kink in the
highlights becomes a posterised colour blotch).

Everything the datasheet does not publish usably — the Hanatos-2025 adaptation
parameters, the spectral base density — is inherited from the nearest measured
agx profile with the same dye set and process. What distinguishes one speed of
Kodachrome from another *is* the characteristic curve, and that part is
measured. Each profile records exactly this split in its `datasource` field.

Built this way and shipping in `profiles/`: the slide films **Provia 100F**
(Fujifilm AF3-036E), **Provia 400X** (AF3-0213E), **Astia 100F** (AF3-149E),
**Velvia 50** (AF3-0221E2), **Velvia 100** (AF3-202E), **Ektachrome E100**
(Kodak Alaris E-4000) and **Kodachrome 25, 64 and 200** (Kodak E-55), and
**Superia 1600** (AF3-145E); the Elite Chromes are below. Each takes the
sheet's curves for what they are, integral densities (Status A for a slide,
Status M for a negative), and turns them into
dye amounts with the sheet's own dye spectra, the form agx's own profiles are
stored in; read back through the densitometer — over the wavelengths the dye
spectra are defined on, as the engine views a slide — each matches its sheet
within 0.04 in density (Velvia 50, traced off bitmaps, within 0.10). Each
takes its own spectral sensitivities from the sheet too, on the donor's tails
beyond the plotted range, balanced so a mid-grey exposes every layer equally.
Every one of these sheets says its greys are neutral, so a slide's red and
blue layer speeds are then set so a grey views neutral from three stops under
to two over (`build_profiles.neutral_layer_shifts`, within 0.07 decades of where
the sheet draws them). The donor, agx's profile of the same or a sister
stock, supplies only what a sheet does not: the base density, the Hanatos-2025
adaptation and the coupler matrix.

`vector_plots.py` reads vector plots: Fujifilm draws a sheet's three spectral
curves as one path and its characteristic curves in two Bézier segments each,
so each plot is described by its frame and gridlines and its curves are picked
by colour, dash pattern, position or order. Where a sheet's labels are
outlines, as on Velvia 100's, its rules are named by the grid they make.
Provia 100F's curves and dyes are bitmap stencils, one per curve, traced on
the stencil of the grid (`sheet_readers.py`). Bitmap spectra are thinned to
one-pixel lines and walked from a point only that curve passes through, taking
at each crossing the branch that carries on in its own direction
(`trace_raster_plot.trace_curves`).

Three slides had first been built another way. **Kodachrome 25 and 200**
(on Kodachrome 64) and **Velvia 50** (on Velvia 100) stored their sheets'
Status A curves as if they were dye amounts, which counts each dye's unwanted
absorptions twice: Kodachrome 25 read back at D-max 4.2/3.9/4.7 (R/G/B)
against Kodak's 3.8/3.5/3.4, Velvia 50 at 4.1/4.0/4.0 against 3.7/3.6/3.3,
and Kodachrome's excess yellow warmed its shadows (b* +3, Kodachrome 200 +6).
Rebuilt, they render lighter than they did, by 2 to 5 L* in a photograph.
**Provia 400X** had taken Provia 100F's dyes; it now has its own.

Two things a sheet gives only in words or as a curve are then set from it, by
`tools/bake/stock_overrides.py`:

- **Colour.** No sheet gives the inter-image effect as a number. Each says how
  its colour stands next to a sister stock, and the coupler matrix's
  inter-layer terms are scaled until the rendered ColorChecker's chroma
  (unclipped in BT.2020, against the scene's) matches that stock's. Velvia 50's
  saturation is "equal to that of the current Velvia", as Velvia 100's sheet
  says of its own: 1.31x the scene's chroma, at 1.8x agx's inter-layer
  coupling. Provia 400X has "ISO 100-level color reproduction", its
  inter-image effect held back by a colour-correction layer: Provia 100F's
  1.04x, at 0.45x. Kodak says of Kodachrome 25 what it says of 64, "Reproduces
  subtle color naturally", and of 200 "Natural colors": Kodachrome 64's 0.92x,
  at 1.25x and 1.6x. Astia 100F gives "subdued color reproduction ... among
  the 100F films" with "one of the world's highest levels of color fidelity":
  the scene's own 1.00x, at 0.65x (its purer dyes render 1.14x on Provia
  100F's couplers). Velvia 100, Kodachrome 64 and Ektachrome E100 keep agx's
  own tuning (1.31x, 0.92x, 1.05x), Provia 100F the 1.3x picked from
  side-by-sides (1.03x). Kodak's Elite Chromes keep the coupling of E100,
  whose couplers they take: on it their own curves and dyes render 1.12x and
  1.08x, measured beside E100 and Velvia 100 in one harness, which reads those
  two at 1.035x and 1.275x. So Extra Color 100, "the highest color saturation
  available today in a 100-speed consumer film", is the most saturated of the
  consumer slides here, and Elite Chrome 200's "excellent color ... similar to
  100-speed daylight slide films" sits next to E100's. Matched to Velvia 100
  instead, at 1.55x the coupling, Extra Color looked extreme.
- **Sharpness.** agx's couplers sharpen detail finer than the inhibitor's
  reach by 1/(1 - b), b the share of a layer's contrast its inhibition takes,
  and b grows with the curve's slope: on a slide's steep shadows a
  datasheet-sized edge effect at mid-grey becomes a rim. Before this, from one
  stop under mid-grey to two, Velvia 50's response at 10 cycles/mm rose from
  112% to 145% and Provia 100F's from 121% to 142%, where the sheets show
  gentle edge effects of long reach. Each slide's scatter, the inhibitor's
  reach and the share of it that spreads are now fitted to its own sheet's MTF
  over the whole plotted range (`tools/bake/slide_mtf.py`), and the engine
  holds the fine-detail gain below one stop under to the gain it has there
  (`FilmMath.edgeGainScale`). One stop under, every slide is within 6 points
  of its sheet from 5 to 30 cycles/mm; two and three stops under it rises by
  at most 5 points, Velvia 50 by 15, where its strong inter-image coupling
  sharpens green as red scatters more.

Strong couplers exposed one more thing. Inverting them squeezes the
log-exposure axis by 1 - b, and resampled onto agx's 256 samples the curve
the inversion gives is undersampled: on Velvia 50 and 100 the developed curve
was off the sheet's by up to 0.017 in density and its slope stepped by up to
0.6 from one sample to the next, where the sheet's is smooth. The engine
builds the pre-coupler curves on a grid 16 times finer (`FilmMath.oversampled`),
and the parity references do the same: within 0.001 of the sheet.

**Kodak Elite Chrome Extra Color 100** (E-126E) and **Elite Chrome 200**
(E-148E), Kodak's E-6 consumer slides of 2005, are built from their own sheets
as far as they go, as the colour negatives since Royal Gold are. Each sheet
plots its curves, spectral sensitivities, dyes and a visual neutral, and its
MTF, all as vector art, and gives its RMS granularity, 11 and 12 at a gross
visual density of 1.0. The sensitivities continue past their plotted ends on
their own gradients. Neither sheet plots a D-min spectrum (Extra Color's dye
plot is captioned for one but draws none), so each slide's base is its D-min:
its own dyes in the amounts that read the curves' own D-min. agx's adaptation
window, fitted to E100's own sensitivities, is left out; only the coupler model
is E100's. They are placed as every slide is, at E100's D-min + 1.0 speed point;
ISO 2240's speed point, read on the mean of their three curves, rates them 111
and 193 against the sheets' 100 and 200. The layers stay as drawn: unlike the
slides above, neither has a layer moved to make a grey view neutral. Kodak
draws the blue layer reaching a density of 1.0 0.10 (Extra Color) and 0.07
(200) decades before the green, where E100's sheet draws it 0.04 before, and
the sheets' own spectral sensitivities put it there too in daylight (0.12 and
0.10). So their greys view blue on the light table, b* -11 (Extra Color) and
-8 (200) at mid-grey; through an 81C, b* 0.0 and +2.7.

**Superia 1600**, on Superia X-TRA 400, is metered at its EI 1600 on the
sheet's absolute axis (its curve's own speed is nearer ISO 800), so the shadows
sit on the toe as on film shot at 1600. The sheet gives no single-dye spectra,
so X-TRA's dyes stay. Status M is not what the paper sees, and drawn straight
in the curves printed yellow highlights (b* +6). agx corrects its own negatives
for that out of public view; measured on X-TRA 400, agx's contrast is
Fujifilm's sheet times 0.922 (red), 0.965 (green) and 1.093 (blue), and Superia
takes exactly that, with layer speeds set so a grey prints neutral (red +0.01,
blue -0.13 decades). It then prints greys within a* 1.4 and b* 2.9 from four
stops under to three over, about as X-TRA does.

**Superia Reala 100** (AF3-967E) is built as Superia 1600 is, on X-TRA 400 with
its per-layer contrast. Metered at its ISO 100 on the sheet's absolute axis,
its mid-grey reads red 1.04 in Status M, inside the 1.02 to 1.20 the sheet
gives an 18% grey card. It prints on Crystal Archive, the Fujicolor paper its
sheet names, and there its shadows print greener than agx's own X-TRA's (a*
-5.9 two stops under, against -3.9; on Portra Endura it stays within 2.4). No
layer speed or contrast within reason moves that: it is its green curve's long
toe on agx's paper.

**Agfa Optima II 100** and **Portrait XPS 160** come from Agfa's Professional
range sheet. Agfa plots the spectral density of a neutral at medium density and
at D-min instead of single dyes; of agx's negatives, X-TRA 400's dyes and base
fit both best, so it is the donor. Agfa's axis puts each film's rated speed
exactly at its curves' D-min + 0.1 point, so each is metered at its rating. agx
has no Agfa paper. They print on Portra Endura, the paper on which their greys
print most neutral (within a* 3.1 and b* 1.4, and 1.4 and 0.8, from three stops
under to two over), where Crystal Archive leaves 6.4 and 6.6. Agfa ranks their
colour: Optima II "high", Portrait XPS "restrained saturation and flat
contrast". On X-TRA's couplers they printed 1.37x and 1.31x the scene's chroma,
beyond any of agx's films on that paper. "High saturation" is taken at Ektar
100's 1.20x, whose sheet says the same, and "restrained" at Portra 160's 1.06x.

Fujifilm's motion picture negatives **Eterna Vivid 160T** (KB-0701E), **Eterna
500T** (KB-501E) and **Reala 500D** are built on agx's Vision3 stocks of the
same light balance (200T, 500T, 250D), and print on Fujifilm's own F-CP 3510
(below). The Eterna sheets
plot against camera stops from the normal exposure, so mid-grey sits at their
0, 1.03 and 1.07 decades over the green speed point. Reala 500D's sheet plots
absolute log H, but metered at EI 500 on that axis its mid-grey sat 0.24
decades over the toe and every shadow printed at L* 27. It goes where the
Eterna sheets put normal exposure instead, 1.05 decades over its speed point.
Their greys print within a* 3.6 and b* 4.8 on 2383, as close as Vision3's own,
and within a* 6.9 and b* 6.6 on F-CP 3510, where Vision3 500T's reach 7.6 and
7.1.
Eterna Vivid, "High color saturation ... the highest contrast in the ETERNA
series", printed no more saturated than Eterna 500T (1.00x and 0.99x); it takes
the step Ektar makes over Portra 160, to 1.12x. Reala 500D's and Superia
Reala's sensitivity plots draw Fujifilm's fourth, cyan-sensitive layer; the
engine has three layers, and it is left out.

**Kodak Royal Gold 100** (E-41) and **Vericolor III 160** (E-26) take more from
their own sheets. Like Agfa's, they plot the spectral density of a midscale
neutral and of D-min instead of single dyes. The D-min, the film's own base and
fog, is the base, where the films above take a donor's base and orange mask.
The dyes are those of agx's colour negative whose measured dyes best fit what
the neutral adds over D-min (`build_profiles.py --donors`): Ektar 100's for
Royal Gold, Kodak's own family, and Fujifilm C200's for Vericolor III, whose
older dyes are broader than any Kodak set agx has. Each dye is moved and
stretched about its peak until the three add up to the sheet's neutral, to
0.010 and 0.012 in density (`build_profiles.neutral_dyes`). agx's spectral
adaptation window, fitted to each of its own stocks' sensitivities, is left
out. Each is metered at its rating on Kodak's absolute axis. A grey card then
reads red 0.84 in Status M on Royal Gold, inside Kodak's 0.80 to 1.00, and 0.69
on Vericolor III, under Kodak's 0.73 to 0.93: its curves' own ISO speed is 126,
and Kodak notes that many photographers expose it at EI 125. They print on the
papers their sheets name, Ektacolor Edge and Portra Endura. Vericolor III's
"moderate color saturation" is Portra 160's chroma, its successor's line;
Royal Gold's sheet says nothing of saturation and puts it in the Gold family,
so it takes Gold 200's on Ektacolor Edge. Vericolor III's sheet plots its MTF,
to which its scatter and inhibitor are fitted, as the slides' are.

**Konica Centuria Super 100** and **Centuria Super 1600** (2002) and **Impresa
50 Professional** (1998, TDSN-501) are built as Royal Gold and Vericolor are,
from Konica's own technical data sheets: characteristic curves, spectral
sensitivities, the spectral density of D-min (the base) and of a midscale
neutral, MTF, and the Centurias' RMS granularity, 4 and 6.
- **Dyes.** None of the three plots single dyes, and one neutral cannot be
  split into three. Each takes Fujifilm X-TRA 400's dyes, moved and stretched
  until the three add up to its sheet's neutral over D-min, to 0.014, 0.010
  and 0.004 in density. Compared as curves after that refinement, X-TRA
  400's fit all three closely with small moves, and the three refined sets
  come out one family, as one maker's films should. The sets that fit Super
  100 closer once refined need larger moves: Kodak's VISION3 cyan moved 14-16
  nm, Fujifilm C200's yellow 22% wider. Ranked as they were, before
  refinement, three different negatives had come first.
- **Sensitivities.** Each continues past its plotted ends at its own final
  gradient, to three decades under its peak, rather than on a donor's tails,
  which carry another film's cross-sensitivities: Ektar's green tail would
  have given Super 1600's green layer 7.6% of its daylight exposure beyond
  the plot, where its own gives 1.5%.
- **Placement.** Each is metered at its rating on its sheet's absolute axis,
  which Konica's exposure tables keep to. The Centurias' curves' own ISO 5800
  speeds are 293 and 2650 (Konica claims "the highest level of actual speed
  in its class"); Impresa's is 51.
- **The sheets against themselves.** Each midscale neutral, read through
  Status M, lands on its sheet's own three curves within 0.02 (Super 100),
  0.08 (Super 1600) and 0.06 (Impresa) decades of one exposure. The Centurias'
  D-min spectra, though, read 0.05 to 0.10 denser through Status M than their
  curves' own D-min; Impresa's agree. The profiles keep the curves' densities
  over the spectral D-min, so they sit that much denser than drawn, which the
  enlarger filtration and print exposure take out.
- **Impresa's plots are bitmaps,** each picture stored upside down, its curves
  drawn in one ink, solid, dashed and dash-dot, and crossing in the spectral
  plot. Each curve is walked along its own stroke, across its dashes' breaks
  and through the crossings (`trace_raster_plot.follow`).
- **Paper.** All three print on Kodak Ektacolor Edge. Of agx's papers it prints
  the Centurias' greys most neutral (within 2.9 and 1.7 a*b* from three stops
  under to two over; Portra Endura 3.8 and 3.4). Impresa's worst step is about
  the same on four papers (5.2 to 5.7), and over its whole wedge Edge holds it
  closest.
- **Colour.** Super 100's sheet promises "brilliantly colored portrayals" and
  calls it "the best selection for outdoor snapshots", what Kodak says of Gold
  200 ("Bright, colorful prints"): Gold 200's chroma on the same paper, 1.19x.
  Super 1600's says nothing of colour and takes Super 100's. Impresa's
  "subtle color tones ... with this much fidelity" is Portra 160's chroma on
  the same paper, 1.15x. On X-TRA 400's coupler strength the three had printed
  1.45x, 1.47x and 1.35x.
- **Grain.** Impresa's sheet claims "the highest level of granularity ... of
  any color print film" and gives no figure. It takes Konica's own step from
  Super 100 to 1600, carried down one stop: 3.6.
- **Character.** Konica draws the blue layer steeper against the green than
  Kodak does. About mid-grey Impresa's is 1.31 times as steep and Super 100's
  1.24, where Kodak's negatives here run 1.07 to 1.16 and Fujifilm's X-TRA 400
  1.24. On Kodak's papers their greys cross over around mid-grey: Impresa's
  run b* +2.7 a stop under and -5.3 a stop over, Super 100's +2.9 and -2.5.
  Super 1600's (1.14) hold within 1.7. On Crystal Archive Type II Impresa's
  hold b* within 1.5, and its shadows lean green instead (a* -5.1 two stops
  under), as X-TRA 400's do on that paper.

**Kodak EXR 50D** (H-1-5245) and **EXR 200T** (H-1-5293), ECN-2 camera
negatives of the EXR generation, take more again from their own sheets, which
both plot curves, spectral sensitivities, the midscale neutral and D-min, MTF
and granularity per layer:
- **Dyes.** EXR 50D's 2003 edition plots its three dyes themselves, which it
  takes as they are. EXR 200T's plots only the neutral; its dyes are its sister
  stock's, 50D's, which add up to 200T's neutral over D-min to 0.014 in density
  without moving, twice as close as any agx negative's (0.030 for Vision3
  250D's).
- **Placement.** Mid-grey goes at Kodak's normal exposure. The 2003 edition
  marks it, camera stop 0 on a scale over 50D's curves: 0.202 lux-seconds at
  EI 50, 10/EI. 200T's sheet draws no scale and goes by the same rule at EI 200
  under tungsten. Normally exposed, the two then read about the same densities,
  R 0.9 G 1.35 B 1.75, as Kodak's LAD aims mean its camera negatives to.
- **Range.** Both plot only to about three stops over normal, a step tablet's
  range, where the film goes on; each curve continues at its final gradient for
  0.8 decades, as the black-and-white negatives' do, so the meter does not take
  the plot's edge for the film's shoulder (5.2 stops of headroom, where
  Eterna's sheets give 5.5).
- **Sharpness and grain.** Their scatter and inhibitor are fitted to their
  green MTF, as the slides' are. Their grain is their own per-layer curves
  (`exr_granularity.py`), which combined as a visual figure read 3.1 for 50D,
  inside its sheet's "less than 5", and 6.1 for 200T, whose sheet gives the
  same "less than 5": its curves and its figure disagree, and the curves are
  kept. Kodak's 2003 edition of 50D's sheet gives the curve in place of a
  figure, measured by a "modified measuring technique".
- **Colour.** No sheet gives a coupler model; theirs is the Vision3 stock's of
  the same balance and speed, which also gives the light: daylight, and for
  200T the 3200 K lamp its own sheet names. Their
  sheets say nothing of saturation, and none is set: on Vision3's coupler
  strength they print 1.09x and 1.12x the scene's chroma on 2383, where Vision3
  50D and 200T print 1.22x and 1.25x, from their own sensitivities and dyes.
- **Paper.** They print on Fujifilm F-CP 3519, the print film of their day
  (below). Its highlights print warmer than 2383's, for every negative: EXR's
  greys reach b* 8.7 and 9.7 at the ends, Vision3's of the same balance 8.2 and
  8.6 on it.

**Fujifilm F-CP 3519** (KB-918E, 1999), **F-CP 3510** (KB-301E, 2003) and
**Eterna-CP 3513DI** (KB-505E, 2005) are print films, the stocks a lab prints a
camera negative onto, built by `build_print_profiles.py` from their own sheets
(`fuji_print_sheets.py`), which plot curves, spectral sensitivities, the three
dyes and a grey, and CTF. Fujifilm's 2008 motion picture manual repeats 3510's
and 3513DI's curves, to 0.003 in density.
- **Dyes.** As plotted the three add up to more than the sheet's grey, by twice
  a smooth floor of 0.03 to 0.08 that the grey holds once, and the yellow and
  magenta lie flat at about 0.05 across 600-700 nm, where neither absorbs: each
  was measured with the film's base under it. Taken off, they add up to the grey
  exactly. Read as plotted, they print 0.65 to 0.75 of 2383's chroma.
- **Curves.** Status A, exposed with 2854 K tungsten through CC-90Y, CC-60M
  and an SC-41, the green and blue moved back by the decade and two the sheet
  shifts them by; put back, the three layers reach the aim density within 0.15
  decades of each other. Each layer's sensitivity level comes from where its
  curve lies under that exposure, the shapes from the sensitivity plot.
- **Placement and light.** As on every agx print stock, log H 0 in each layer
  is its share of a neutral grey of 18.4% luminance under agx's xenon
  projector, and they are printed with agx's lamp: a grey prints as light on
  them as on 2383. Every colour negative's filtration is solved for them in
  agx's pipeline at bake time, as agx's table has its own negatives on its own
  papers.
- **Character.** The same negative prints with less chroma than on 2383 (0.81
  to 0.90 of it), cooler shadows and warmer highlights; 3510 is the softest,
  3513DI the high-contrast stock Fujifilm made it for digital intermediates.
- **Reference papers.** Fujifilm's own motion picture negatives, Eterna 500T,
  Eterna Vivid 160T and Reala 500D, name F-CP 3510 as their paper, and Kodak's
  EXR 50D and 200T name F-CP 3519, where the Vision3 stocks they are built on
  name 2383. Picking a motion picture film moves the print onto its own print
  film, as a lab prints it; picking a still film after one moves the print
  back onto that film's own paper, and a print film chosen for a still film
  stays.

**Kodak 5251 50T** and its print film **Kodak 5385**, Eastman's camera negative
and release print of 1962-68, come from the two papers that introduced them,
W. I. Kisner's in the Journal of the SMPTE of October 1962, whose figures
survive as scans (`kodak_1962_sheets.py`): each figure levelled, calibrated on
its own tick marks, and each solid curve traced with its predecessor's dashed
one (5250's, 5382's) taken out as the short strokes it is.
- **Exposure scale.** Neither paper numbers its log exposure axis. The curves
  are read at the scale of their density, one decade drawn as long as one
  density unit: Kodak's technical sheets draw them so (46.2 points to either on
  EXR's 1999 sheets and Vericolor III's), and Kodak's 1968 paper on 5254,
  "structurally the same" as 5251, marks its axis in decades as long as its
  density units, to 3%. So read, 5251's curves end at a gradient of 0.61 to
  0.63, where 5254's measure 0.61 to 0.62.
- **5251.** Its own curves, spectral sensitivities and equivalent dyes, the
  colored couplers' masking taken into them, their negative lobes kept as agx
  keeps its own negatives'. 3200 K tungsten and EI 50 from Kodak's
  announcement (American Cinematographer, June 1962); mid-grey at Kodak's
  normal exposure, 10/EI, on its ASA/ISO speed point. It predates DIR couplers
  and runs without them, masking only. The BBC's film-only measurement of its
  MTF (C. B. B. Wood, 1966) sets its blur, and never rises over 100% as an
  inhibitor's edge effect would make it. Its base and mask are Vision3 200T's,
  and its grain EXR 200T's per-layer curves: no figure for either survives in
  these sources, and 5251, a generation older, was likely grainier.
- **5385.** Its own curves (integral density, taken as Status A), spectral
  sensitivities and dyes, which add up to its neutral (they carry no floor of
  base). The paper moved each curve along the axis "to illustrate the curve
  shape", so each layer is placed by agx's rule for mid-grey, and its speed
  taken from the sensitivity plot's own levels, where its curve reaches D-min +
  1.0. The curves leave the plot at 3.56, still rising, and go on as agx's
  curve model fitted to them. Kodak's note that labs would add "more yellow" to
  their filter packs for it shows: Vision3 250D prints neutral on it at M 30, Y
  70, against M 51, Y 54 on 2383.
- **Character.** On its own print film 5251 prints more neutral, less
  saturated and harder than Vision3 on 2383. The paper draws its blue layer a
  little steeper than the other two where mid-grey prints (0.59 a decade
  against 0.56 and 0.54), and its greys run b* -4 a stop over mid-grey. In
  daylight through the Wratten 85, which Kodak names for it (EI 32), a grey
  prints neutral.

The print films' CTF is not modelled, Kodak's or Fujifilm's: no print stage
here blurs.

**Polaroid 553**, **Polapan Pro 100** and **Polacolor 559** (Polaroid
Corporation's data sheets for T-53/T-553/T-803, T-54/T-554/T-664/T-804 and the
Polacolor ER films, 1999) are built by `tools/bake/build_instant_profiles.py`
from `polaroid_sheets.py`, which reads their vector charts: each curve a
coloured stroke drawn in pieces, past the frame under a clipping path, with a
legend sample inside the frame, which is left out. The large charts' frames
and ticks disagree by up to 8 points (the small ones' agree to a fifth of a
point); each axis is a line fitted to both, within 0.04 in density of either.
Each takes from its sheet:

- the print's characteristic curves at 55, 70 and 95 F, the cold and hot
  processing the sheet plots, in the pack beside the 70 F print they open at
  (`profiles/processing/`);
- mid-grey where the 70 F curve reads the density the sheets name as their
  reference, "a neutral density of .75", which is what an 18% grey card reads
  on a print: their axes are relative, and their ratings place nothing on them;
- the spectral sensitivity, continued past its plotted ends at its own final
  gradient;
- the black-and-white sheets' MTF of the print, to 4 cycles/mm, fitted with
  the scatter's core and tail: in the engine within 0.6 (553) and 0.9
  (Polapan) points of the sheet. Polacolor gives only its resolving power, 9
  line pairs/mm against 553's 12-15. The two black-and-white sheets show how
  far a print's MTF moves with its resolving power: Polapan's is 553's at 1.33
  times the frequency for 1.67 times the resolution (within 0.8 points), and
  carried to Polacolor's 9 lines that puts its MTF at 553's on 0.80 times the
  frequency, an estimate: 61% at 2 cycles/mm, against 553's 70%.

No sheet gives a granularity (estimates: Polapan and Polacolor 4, 553, at ISO
800, 6.2 by Agfa's step with speed), and their MTFs fall from the start: no
DIR couplers. Three of the sheets' own figures disagree with their plots, and
the plots are used. Type 553's states D-max 1.75, D-min 0.11 and slope 1.64
at 71 F, where its 70 F curve reads 1.88, 0.10 and 1.80. Both black-and-white
sheets print one filter-factor table, which their sensitivities reproduce in
tungsten light (553: Wratten 47, 58, 8 and 25 10.0, 11.9, 1.6 and 2.8,
against 11.2, 11.2, 1.4 and 2.8) but not all in daylight (553's Wratten 58
15.1 and 8 2.8, against 10 and 2). And Polacolor's cold chart is captioned
"55 F/18 C", where 55 F is 13 C, as its own table and the other sheets have
it. Polacolor's dyes: of eleven dye sets, agx's slides and colour papers, none
views its 70 F greys within 10 a*b* (RMS, three stops under mid-grey to two
over), and moved and stretched to the limits the colour negatives' fit allows
they still run a* +4.5 to +7.5: its sheet draws green 0.1 to 0.4 denser than
red and blue. It is built as drawn on the set that views it most neutral,
Crystal Archive, and its greys run magenta, the film's known character.

**HARMAN Phoenix 200**, an experimental C-41 film, has a route of its own:
- **Its plots are bitmaps.** The characteristic curves run against relative log
  exposure, and the spectral sensitivities on a linear "Relative Sensitivity"
  scale. HARMAN's wedge spectrogram is read as plotted, without taking out its
  tungsten light, as HP5 Plus's is.
- **Speed.** Its ISO 200 was measured by the ISO method, so its curves' ISO 5800
  speed point sits at sqrt(2) / 200 lux-seconds. It is metered at EI 125, within
  HARMAN's EI 100 to 200 and the rating its re-release carries.
- **Base and dyes.** It has no masking dyes: its base is its fog and stain, in
  its dyes (G 0.91, the "magenta / purple tint" HARMAN describes). HARMAN
  publishes no dye spectra, so it takes Kodak Gold 200's.
- **No DIR couplers.** Its curves rise about 1.3 in density a decade, twice a
  usual negative's. At agx's coupler strength the inhibition would take more
  than all of that contrast, which no development reaches.
- **Filtration.** Without an orange mask it needs about twice the usual yellow.
  The engine's solve, started at agx's default, misses it on most papers, so
  the bake solves it for every colour paper in agx's pipeline. It is carried in
  the neutral-filter table (`stock_overrides.BAKED_FILTRATION`).
- **Halation.** "Limited antihalation" and "striking halation effects" get agx's
  strongest halation.
- **Contrast.** It prints 2.2 times as hard as Portra 400 about mid-grey. Its
  grades reach R400 rather than R180, as far as Contrast's soft half and Auto
  Contrast go, and it opens at Contrast -30 (R200.5).

A colour negative that agx's neutral-filtration table does not cover, which
Superia 1600 is, has its filtration solved for a neutral mid-grey, as the
black-and-white negatives on colour paper already were. On the pairs the table
does cover, the solve lands within 0.3 of agx's magenta and 1.7 of its yellow.

`tools/bake/build_bw_profiles.py` builds the black-and-white stocks, which need
no donor: one silver layer with a flat spectral density, so the datasheet is
the whole profile. The engine is built for three dye layers, so each profile
carries the one layer three times, each with a third of its density.

- **Kodak Tri-X 400** (F-4017): spectral sensitivity (D = 1.0 over fog) and the
  35 mm D-76 curve at Kodak's 8 minutes, both vector plots.
- **Kodak Tri-X 400 (Push 2)**: the same spectral sensitivity, and a D-76 curve
  for 10 3/4 minutes interpolated between Kodak's 10 and 12 minute ones.
- **Kodak T-Max 100** (F-4016): spectral sensitivity (D = 1.0 over D-min) and
  the small-tank D-76 curve at Kodak's 6 1/2 minutes, interpolated between the
  sheet's 6 and 7 1/2 minute ones: contrast index 0.585, where Kodak's own plot
  of it gives about 0.58. Its MTF, fitted up to 50 cycles/mm, and its RMS
  granularity, 8, are Kodak's.
- **Ilford HP5 Plus**: spectral sensitivity and the characteristic curve,
  vector plots whose labels are drawn as outlines, so they are calibrated on
  their own tick marks and grid.
- **Ilford Delta 100, Delta 400, FP4 Plus, Pan F Plus and Ortho Plus**
  (HARMAN's technical information, 2018-19): the wedge spectrogram and the
  characteristic curve each sheet plots for the developer and time it names,
  bitmaps traced on their own grids: Delta 100 in ID-11 for 8 1/2 minutes,
  Delta 400 in ID-11 for 8 at 24 C, FP4 Plus and Pan F Plus in ILFOTEC HC 1+31
  for 8 and 4, and Ortho Plus in ID-11 for 8, the lower of its two pictorial
  curves. They come out near the normal contrast Ilford sets its times for
  (G-bar 0.62): 0.65, 0.64, 0.68, 0.71, and Ortho Plus's, labelled 0.62, 0.59.
- **Fujifilm Neopan 1600** (AF3-608E): spectral sensitivity and the D-76 curve
  at 7 1/2 minutes, Fujifilm's time for EI 1600, traced off scanned line art
  on its own grid.
- **Agfa APX 25** (Agfa's range sheet): spectral sensitivity and the
  characteristic curve, vector plots labelled in text. On its absolute axis
  ISO 25's mid-grey lies 0.9 decades from where its own speed point puts it,
  so the speed point places it. Agfa publishes its MTF, which its scatter and
  edge effect are fitted to up to 50 cycles/mm, and its RMS granularity, 7.
  Read beside APX 100's own sheet, the range sheet's APX 25 curve is APX
  100's, within 0.011 density from end to end, as the same sheet's APX 400
  MTF is APX 100's: most likely a repeat. Its 0.9 decades are then APX 100's
  speed point, ISO 193, against ISO 25, and the profile carries APX 100's
  curve; APX 25's own sheet would settle it. Its spectral sensitivity and MTF
  are its own.
- **Agfa APX 100 and APX 400** (Agfa-Gevaert's own sheet for each, 08/1995;
  `agfa_bw_sheets.py`): spectral sensitivity (to an equal-energy spectrum, at
  density 1.0 above minimum, as the range sheet says of all its films'
  plots), the density curve and the MTF, vector plots calibrated on their own
  rules and labels. The 1998 range sheet plots the same, read and laid over:
  the editions agree within 0.009 density on the curves and 0.05 decades on
  the sensitivities (at the red edge), but its APX 400 MTF repeats its APX 100
  curve to the third decimal (0.77 at 30 cycles/mm, where APX 400's own sheet
  has 0.65), so each film's MTF is its own sheet's. The curves name no
  developer. APX 400's measures gamma 0.655, the 0.65 the sheet's gamma-time
  plot gives every development the film is rated in (REFINAL 6 minutes, ISO
  400), and its ISO speed point is ISO 440 on Agfa's lux-second axis. APX
  100's measures 0.73, further than its ratings' developments (REFINAL 6
  minutes ISO 125, RODINAL 1+25 8 minutes ISO 100, both 0.65 on its plot), and
  its speed point is ISO 193. Each is taken as plotted and placed by its own
  speed point, as APX 25 is. Their MTFs are fitted up to 50 cycles/mm, within
  2.2% and 1.3%; their RMS granularity is 9 and 14 (REFINAL, 6 minutes).
  Through Kodak's Wratten filters their sensitivities give APX 100 yellow 2.2x,
  orange 4.7x (No. 22) and red 9.5x, APX 400 2.4x, 5.4x and 10.7x; the sheets
  give no factors.
- **Fujifilm Neopan 100 Acros II** (AF3-0258E, 02/2020;
  `fujifilm_bw_sheets.py`): the spectrogram to daylight (5400K) and the curve
  for Microfine at 10 minutes, Fujifilm's time for EI 100 in its own
  developer, in which the sheet also measures its RMS granularity (7) and
  resolving power, bitmaps traced on their own grids. Its ISO speed point is
  ISO 95 on Fujifilm's lux-second axis and its G-bar 0.53, where the sheet's
  time-G plot reads 0.545; the sheet's D-76 curves, at its EI 100 time of
  7 1/4 minutes, would put the speed point at ISO 137. The spectrogram is
  read as Neopan 1600's, divided by CIE daylight at 5400 K: through Kodak's
  Wratten filters it gives yellow 1.6x, orange 2.6x (No. 22) and red 4.2x,
  where the sheet's table says 2, 4 (No. 21) and 8, as Fujifilm's Neopan 1600
  sheet does too; the film is built from the plot. "Orthopanchromatic": it
  sees red up to 650 nm about as strongly as green, and nothing past 660, so
  a deep red prints lighter than a grey of its luminance. Fujifilm publishes
  no MTF; its resolving power, 200 lines/mm at 1000:1 and 60 at 1.6:1, is
  T-Max 100's (200 and 63), so it takes T-Max 100's measured MTF, an
  estimate. No reciprocity correction below 120 seconds, the sheet says; the
  app models none.
- **Ilford Multigrade RC Deluxe**: spectral sensitivity and the curves for each
  filter grade, traced off bitmaps. Traced, the grades read ISO R155, 130, 107
  and 94 for 00 to 2, against Ilford's 160, 130, 110 and 90.
- **Kodak Double-X 5222** (H-1-5222, editions of 10/2003 and 3-22, both read;
  `kodak_bw_cine_sheets.py`): the 2003 edition's vector plots give the
  spectral sensitivity (D = 1.0 over gross fog, drawn from 420 to 660 nm and
  continued to 400 and 670 nm by the D = 0.3 curve's own shape) and the MTF;
  the 2022 edition repeats them as bitmaps, the same to the width of a line.
  The curve is the 2022 edition's for 6 1/2 minutes in D-96 (gamma 0.66, as
  traced and as labelled), traced off a bitmap of five development times
  drawn in one black, which run together in the toe and are told apart by
  their order. The 2003 edition plots one curve, labelled 5 minutes, which
  follows the 2022 edition's 5 minute curve through the toe and lower straight
  line and climbs 0.11 above it at its top. Granularity 14 and its MTF are the
  sheet's.
  Its spectral sensitivity, as plotted, makes daylight 1.254 times as
  effective as 3200 K tungsten at equal illuminance, where its two ratings, 250
  and 200, make it 1.25; through Kodak's Wratten 8 and 25 it gives filter
  factors of 2.0 and 6.5, against the 1.5 and 8 in its table. Its base is
  grey, its antihalation.
- **Kodak 2302** (H-1-2302, 2022): its D-97 curves, its spectral sensitivity
  (D-min + 0.2) and, from its granularity plot, its base, traced off bitmaps;
  projected as agx's 2383, mid-grey printing at 18.4% transmission. The sheet
  develops it to a control gamma of 2.4 to 2.6, and its plot of gamma against
  development time reaches 2.5 at 6.52 minutes: the curve is interpolated
  there, between the sheet's 5 and 7 minute curves. The sheet disagrees with
  itself about it: measured by the formula it states for that gamma, its curves
  come out 0.23 to 0.47 above its plot at every time, this one 2.78, and its 5
  minute curve, 2.53 so measured, would be the other reading, a print about a
  tenth softer. Its curves are plotted net of the base (they start at 0.005 to
  0.016, its net fog); the base, 0.048, is the toe of its granularity plot,
  which draws the density gross, less the net fog. Its MTF and granularity are
  read, not modelled: the app blurs and grains the negative, not the print.

Mid-grey is put where an ISO 2720 meter puts it: 1.007 decades above the ISO
speed point (0.10 over base + fog). Kodak plots against absolute lux-seconds,
and its axis puts the same mid-grey within 0.04 decades of where ISO 400 says it
should be. Neopan 1600's rating is not its ISO speed, so its mid-grey goes
where a meter set to EI 1600 puts it on Fujifilm's absolute axis instead,
0.33 decades lower, and Tri-X Push 2's where one puts it on Kodak's. Double-X,
a motion picture film, goes at Kodak's normal exposure, 10/EI, as Kodak's
colour motion picture films do: its sheet's exposure table, f/5.6 at 200
footcandles at 24 frames a second, works out to 0.050 lux-seconds on an 18%
grey card at EI 200, 10/EI, where a meter's mid-tone would be 8.1/EI, 0.09
decades lower. Past the
last plotted exposure each film curve is continued at its final gradient for 0.8
decades before it rolls off; a black-and-white negative has no shoulder in the
range a datasheet plots.

Camera filters come from `tools/bake/lens_filters.py`, which reads Kodak's
Wratten 2 transmission sheets (see `docs/datasheets.md`): vector plots stored
rotated a quarter turn, calibrated on their own labels; for colour film, Kodak's
upright sheets for the 81 and 85 series, each filter the polyline in its own
colour, its sheet for the 82 series, stored rotated as the single filters' are,
whose four curves are all black and told apart by their line (the 82A dotted,
the 82C dashed, each dot or dash a small filled piece, read at its centre, and
the legend's sample beside each name saying which line is its), and Tiffen's
catalogue for the 812. `bake_packs.py` bakes one exposure LUT per filter for
each film.

Measured through the films, the colour filters do what their curves say. The
81C and 812 are rated a third of a stop and pass about two thirds of the green,
so the app opens up about half a stop; the 85 and 85B, rated two thirds (a
meter's luminous reading), pass under half the green, and it opens up about
one. Kodak's own film sheets rate the 85 at two thirds as well, EXR 200T at EI
125 in daylight against 200 under tungsten and 5251 at 32 against 50; their
green layers make it a stop and 0.9, and the app opens up by those. On Provia
100F a grey card moves a* +3.7 b* +10.6 through the 81C, a plain amber; the
812 lets blue and green through alike and holds back violet, so it adds the
same red with half the yellow, a* +4.3 b* +5.2. The 85 and 85B are rated 5500
K to 3400 K and to 3200 K; Kodak's curves, through the CIE observer, take
daylight to about 3300 K and 2800 K (Tiffen's charts the same, 3300 K and 2900
K), and a tungsten film's own layers see the same. On Kodak EXR 200T in
daylight, decoded for the film's own 3200 K, a grey prints a* -13.8 b* -16.3 on
F-CP 3519 unfiltered; through the 85B a* +5.3 b* +7.2, a little warm and about
as light as the film prints a grey in its own light; through the 85 a* +0.4 b*
-5.1, a little cool. At the film's layers daylight exposes blue 0.96 stops over
green against the film's balance, 0.25 through the 85, and through the 85B
0.24 under, with red 0.25 over.

The 82A and 82C, rated a third and two thirds, open up about half a stop and
three quarters. Kodak's sheet for them states no rating: Tiffen rates its 82A
3000 K to 3200 K (-21 mired), and Fujifilm's data guide pairs the 82A and 82C
with its LBB-2 and LBB-4 (-20 and -40 mired). Kodak's curves, through the CIE
observer, shift a lamp -19 and -48 mired, taking 3000 K to about 3180 K and
2800 K to about 3240 K: the 82A as rated, the 82C a little past Fujifilm's
pairing. On Provia 100F a grey card moves a* -2.0 b* -5.7 through the 82A and
a* -5.1 b* -12.4 through the 82C, where the 81C moves it b* +10.6. On VISION3
500T a grey under a 3000 K lamp prints a* +1.0 b* +5.6 bare and a* -0.7 b*
-0.5 through the 82A, one under a 2800 K lamp a* +2.4 b* +12.4 bare and a*
-2.9 b* -2.2 through the 82C.

Ilford gives its spectral data as a "wedge spectrogram to tungsten light" on an
unlabelled scale. Read literally as tungsten-weighted, HP5 Plus would need a
12–16x red filter, against the 8x photographers use and the 9x Kodak's data
gives Tri-X; read as the film's own relative log sensitivity, 6x. The second is
how it is read, and Ortho Plus's sheet bears it out with numbers of its own:
it rates the film ISO 80 in daylight and 40 in tungsten, and its spectrogram
read this way makes daylight 1.92 times as effective at equal illuminance (2.45
read as tungsten-weighted); its daylight factor for a yellow filter, 2.5, comes
out 2.7 through Kodak's Wratten 8 (5.3). Fujifilm's is a "spectrogram to daylight (5400K)", and read
literally — divided by CIE daylight at 5400 K — it gives Neopan 1600 filter
factors of 2.6x yellow, 4.3x orange (No. 22) and 7.5x red, against Fujifilm's
2x, 4x (No. 21) and 8x for daylight; read as it stands the red is 6.6x.

The extractor was validated by running it over Kodachrome 64, which agx already
has a measured profile for: the extracted dye-density spectra match agx's to
about 0.01 density.
