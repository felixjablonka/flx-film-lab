# Building a film profile from its datasheet: the rules

What every new film stock in `profiles/` is held to, and what the builds so
far have taught. The README's "Building profiles from datasheets" says how
each stock was built; this is the checklist for the next one.

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
  curve walked along its own stroke (`trace_raster_plot.follow`).

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
- **Sharpness.** The sheet's MTF, fitted in the bake (`slide_mtf.py`).

## Judge by eye

Show the curves, the wedges and the renders, side by side or overlaid. Quote
averages only as support.
