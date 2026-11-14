# Building a film profile from its datasheet: the rules

What every new film stock in `profiles/` is held to, and what the builds so
far have taught. `docs/profiles.md` says how each stock was built; this is the
checklist for the next one.

## Take everything the sheet gives

- **Read the whole sheet first:** every page, every plot, every table and
  every sentence. Exposure tables, filter factors, ratings for other light,
  speed claims, the layer diagram (an anti-halation layer, a fourth layer),
  resolving power and the colour words all place or check a build.
- **Write down where each part of the profile comes from.** For each part,
  note whether it is from the sheet, from the maker's sister sheet (same
  maker, generation and process), or from a donor. If from a donor, say why
  the sheet cannot supply it. The split goes in the build's comment and in the
  profile's `datasource`.
- **A donor supplies only what no sheet of the film or its sisters has.** Today
  that is:
  - single-dye spectra, when a sheet plots only a midscale neutral and D-min;
  - the DIR coupler model, whose strength is then set by the sheet's colour
    words (`stock_overrides.COLOUR`).

  Everything else is the sheet's own:
  - curves;
  - spectral sensitivities, including past their plotted ends (`own_tails`:
    each continues at its own final gradient, not on another film's
    cross-sensitivities);
  - base, from the D-min spectral density (`sheet_base`);
  - speed and placement;
  - MTF;
  - granularity.

  agx's per-stock adaptation window is fitted to agx's own stocks, so it is
  left out (`no_window`).

## Choose a donor by its curves, not its RMS

- `build_profiles.py --donors` ranks agx's negatives by how well their dyes,
  as they are, add up to the sheet's neutral. It is a starting list, not the
  answer. Its RMS is for the dyes before the refinement the build applies
  (`neutral_dyes`: each dye moved by up to 20 nm and stretched by up to 25%),
  and one number hides where the misfit sits.
- **Refine every plausible candidate as the build will.** Plot each refined
  sum against the sheet's neutral, with the residual, and compare the curves:
  - **The fit:** whether the misfit falls on a peak, in a trough or where two
    dyes overlap.
  - **The moves:** how far the dyes had to move. Small moves mean the family
    is right. Moves near the bounds mean the family is wrong, even when they
    fit closely.
  - **The family:** whether one donor serves the maker's sister sheets as one
    family. One maker's films of one generation and process should come out
    with one dye family; a new film's refined dyes can be plotted against its
    sisters' to check.
- Example: the three Konica sheets, ranked raw, put three different donors
  first. Refined, Fujifilm X-TRA 400's dyes fit all three closely with small
  moves, and come out one family. The sets that fit one of them closer do so
  only by larger moves: Kodak's VISION3 with their cyan moved 14–16 nm.

## Check the build against its sheet, and the sheet against itself

- **Plot the built profile's curves over the traced ones,** in the sheet's own
  densitometry. Compare absolute densities, not only density over D-min. The
  readback figure alone missed that the Konica CENTURIA sheets' spectral D-min
  reads 0.05–0.10 denser through Status M than their own curves' D-min.
- **Cross-check the sheet's parts against each other.** The midscale neutral,
  read through the sheet's densitometry, should land on its own three curves
  at one exposure. The curves' ISO 5800 speed should agree with the rating and
  the exposure table, or the sheet should say why not (Konica's "actual
  speed").
- **Where two of the sheet's own figures disagree, show both and say so**
  (README, `NOTICE.md` and the build comment). Never present a computed
  figure as the maker's.
- **Overlay every trace on the plot image and look at it,** every curve end to
  end, before using it. A bitmap with dashed curves that cross needs each
  curve walked along its own stroke (`trace_raster_plot.follow`). Curves drawn
  in one ink that never cross but run together (Double-X's development series)
  are told apart by their order: each column's runs shared out top to bottom,
  the split that moves them least from the column before. Where a sheet draws
  each curve in one stroke, read it one stroke at a time
  (`vector_plots.runs`): chaining strokes by their ends (`joined`) runs two
  curves into one where they end at the same point, as Elite Chrome's green
  and red curves, and its yellow and magenta dyes, do.
- **Calibrate a chart on its frame and its ticks together, and read every
  curve to the frame's edge.** Polaroid's large charts put the frame's edges
  up to 8 points from where the inner ticks would (their small ones agree to
  a fifth of a point): each axis is a line fitted to both. Their curves run on
  past the frame under a clipping path, so a trace is clipped to it, with half
  a point's slack: without it Polapan's MTF lost its last half cycle per mm,
  where it falls to 50%. A legend sample drawn inside the frame in the
  curve's own colour is left out by its box.
- **Offer only what the sheet plots.** Polaroid's sheets give each print at
  three processing temperatures: the app offers those three, each its own
  curves, and nothing between them (Felix, 2026-11-11).
- **Place a print on its sheet's reference density.** A sheet that plots a
  print against relative log exposure, with a rating its axis does not place,
  can still name the density it measures at: Polaroid's "neutral density of
  .75", which is what an 18% grey card reads on a print. Mid-grey goes there.
- **Before taking a cast for the film's, see whether any dyes could undo it.**
  Polacolor's sheet plots no dyes, and draws green 0.1 to 0.4 denser than red
  and blue: with every one of eleven dye sets, and with the best moved and
  stretched to the fit's limits, its greys still run magenta. That is the
  sheet's, and the film's: Polacolor's magenta shadows are its known
  character (Felix, 2026-11-11). The profile keeps it.
- **Take a caption for what the plot draws.** Elite Chrome Extra Color's dye
  plot is captioned "for a midscale neutral subject and D-min" but draws the
  three dyes and a neutral only. A slide with no D-min spectrum takes its base
  from its curves' own D-min, in its own dyes (`unmasked_base`).
- **Where a sheet comes in editions, read them all and lay them over each
  other.** Take each part from the edition that measures it best (a vector
  plot over a bitmap of the same data), and the curve from the edition and
  development the rating names. Double-X's 2003 and 2022 editions share their
  spectral sensitivity and MTF, but not their curves.
- **Measure a stated contrast on the sheet's own curves, by the sheet's own
  formula,** and set it beside any plot of it. 2302's curves measure 0.23 to
  0.47 above its gamma-against-time plot at every time. Show both readings and
  their prints, say so, and let the choice be made: for 2302 Felix took the
  plot's time for the control gamma, the harder of the two.
- **Check where a curve's D-min lies against what its base can read.** 2302's
  curves start at 0.005 to 0.016, which no polyester base reads: they are
  plotted net of the base, and the base comes from another of its figures (its
  granularity plot, drawn gross).
- **Check a slide's grey balance against its spectral sensitivity too.** A
  sensitivity plot is absolute: integrated against daylight, it gives the
  exposure at which each layer reaches the plot's density, and that should
  agree with where the curves put each layer. Kodak's Elite Chrome sheets put
  the blue layer 0.07 and 0.10 decades ahead of the green on their curves, and
  0.12 and 0.10 by their sensitivities in D55: their greys view blue, and are
  kept so (`as_drawn`). A dye plot's visual neutral ("normalized dyes to form
  a visual neutral density of 1.0") is how the dyes add up to a grey, not how
  the film renders one.
- **Lay a maker's films over each other, plot by plot.** A range sheet that
  plots several films side by side can repeat one film's plot for another:
  Agfa's 1998 range sheet gives APX 400 APX 100's MTF to the third decimal,
  and APX 25 APX 100's density curve within 0.011. Where a film has a sheet
  of its own, read it, lay the two over each other, and take each plot from
  the film's own.
- **A filter-factor table can be the maker's boilerplate.** Fujifilm's Acros
  II table (2, 4 and 8 in daylight) is its Neopan 1600 sheet's, where Acros
  II's own spectrogram gives 1.6, 2.6 and 4.2. Check a table against the
  spectral plot, say where they disagree, and build from the plot.
- **Where a sheet plots several developers, take the maker's own at its rated
  time, the one its grain and resolving power are measured in,** and check
  its speed point against the rating: Acros II's Microfine curve at 10 minutes
  reads ISO 95 for its EI 100, its D-76 curve at the rated 7 1/4 minutes
  ISO 137.
- **Measure an unlabelled curve's contrast against the sheet's gamma-time
  plot.** APX 100's density curve measures gamma 0.73, where that plot puts
  every development the film is rated in at 0.65: it is developed further
  than the ratings. It is taken as plotted and placed by its own speed point,
  and the difference said.
- **Use the sheet's ratings to check its spectral sensitivity.** A daylight and
  a tungsten rating fix how much more one light exposes the film than the
  other at equal illuminance, more exactly than a filter-factor table rounded
  to practical numbers: Double-X's plot gives 1.254, its EI 250 and 200 1.25.
- **Place a motion picture film at the maker's normal exposure, and check it
  against its exposure table.** Kodak's is 10/EI; Double-X's footcandle table,
  worked through for an 18% grey card, gives exactly that.

## Never move the curves

A grey that drifts or a cast that shows is an error in reading or
transferring the data, or in the engine. Find it; never rescale, reshape or
shift a layer's curve to hide it.

## Colour, paper, grain, sharpness

- **Colour.**
  - The inter-image strength is set so the print's ColorChecker chroma
    matches what the sheet's words imply:
    - "natural" or "restrained": Portra 160's;
    - "high saturation": Ektar 100's;
    - a consumer film's "bright, colorful": Gold 200's.
  - Measure the anchor film on the new film's own paper, so film is compared
    with film.
  - A sheet without colour words takes its sister stock's colour.
  - First see what the film's own data give on its donor's coupler strength,
    and judge it by eye on real pictures. Elite Chrome Extra Color's "highest
    color saturation ... in a 100-speed consumer film", matched to Velvia
    100's chroma, looked extreme; on E100's strength its own dyes and curves
    already put it above every other consumer slide here.
- **Paper.**
  - The maker's own, if agx has it.
  - Otherwise the paper that prints the film's grey wedge most neutral, three
    stops under to two over (`print_neutral.wedge`).
  - Where papers tie, compare the whole wedge, and say which was picked and
    why.
- **Grain.**
  - The sheet's RMS granularity or Print Grain Index.
  - Without one, the maker's own speed steps, or the class analogue stepped
    by the maker's own range. Say it is an estimate.
- **Sharpness.** The sheet's MTF, fitted in the bake (`slide_mtf.py`; for
  silver, `bw_mtf.py`, which fits the scatter's tail as well as its core when
  the core alone misses a sheet's curve by more than 5%, as on Double-X and
  APX 100 and 400). Without one, the film of the same resolving power: Acros
  II, 200 and 60 lines/mm, takes T-Max 100's (200 and 63). Say it is an
  estimate.

## Judge by eye

Show the curves, the wedges and the renders, side by side or overlaid. Quote
averages only as support.
