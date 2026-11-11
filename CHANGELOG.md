# Changelog

## 1.5.0 (2026-11-11)

Kodak's black-and-white motion picture pair, two Elite Chrome slides,
Polaroid's peel-apart prints, ovals in Graduated Exposure and a switch for the
mask lines: 55 film stocks and 14 print stocks in all.

### FLX Film Lab for Windows
- **Everything new below,** with Ctrl in place of ⌘: the new films, Polaroid's
  processing temperatures, ovals in Graduated Exposure and Hide Mask Lines
  (Ctrl+Shift+H).

### New
- **Kodak Double-X 5222 and its print film, Kodak 2302:** Kodak's
  black-and-white motion picture negative, printed on its release print film
  and projected, each from Kodak's own data. Picking Double-X moves the print
  onto 2302. The V-Log LUT pack has it too.
- **Polaroid's instant prints:** 553, Polapan Pro 100 and Polacolor 559 (the
  Polacolor ER of 669), Polaroid's 4x5 peel-apart films, each from Polaroid's
  own data sheet. The film is the print: shown as it is, on its own 4x5 frame,
  with a choice of the three processing temperatures its sheet plots, 55, 70
  and 95 F. The V-Log LUT pack has them too, at 70 F.
- **Ovals in Graduated Exposure:** exposure compensation inside an oval, or
  outside it, fading out over its Feather: to give a subject more light or its
  surroundings less, on the film. It stretches, tilts and moves like a Dodge &
  Burn oval, and stacks with the gradients.
- **Hide Mask Lines** (⇧⌘H): the masks' and gradients' lines off the print, to
  judge it without them; they go on acting.
- **Kodak Elite Chrome Extra Color 100 and Elite Chrome 200:** Kodak's E-6
  slides of 2005, each from Kodak's own data, as Kodak drew them: Extra Color
  the most saturated of the consumer slides, and both with greys that run cool
  on the light table, as their sheets put them; an 81C warms them back. The
  V-Log LUT pack has them too.

### Changed
- **A line of films in order of speed:** the Film menu lists Velvia 50 before
  Velvia 100, Kodachrome 25, 64 and 200 in that order, and Vision3 50D before
  200T, 250D and 500T.
- **A slide's LUT names no paper:** Motion's Export LUT titles a slide by its
  film alone and says it is viewed on a light table, where it named the paper
  the slide was never printed on.

## 1.4.0 (2026-11-07)

Curves, a histogram, a loupe and undo, eight films from Ilford and Konica,
and a window that keeps up with a dragged slider: 49 film stocks and 13 print
stocks in all.

### FLX Film Lab for Windows
- **Everything new below,** with Ctrl in place of ⌘: the curves and the
  histogram, the loupe (Ctrl+L), undo and redo (Ctrl+Z, Ctrl+Y), the new films
  and the metadata.
- **Graphics cards with less memory:** where the card is short of it, a
  picture is decoded on the CPU, by the same arithmetic, and an export is
  printed in tiles that come out as the whole print would; the loupe holds
  only its part of the picture on the card.

### New
- **Curves and a histogram:** the finished print's RGB histogram, and point
  curves laid on the print last, one for all three channels and one for each.
- **A loupe** (⌘L): the print at 100%, as the export has it, under a square
  dragged over the print.
- **Undo and redo** (⌘Z, ⇧⌘Z), one step per gesture, with a history for each
  picture and each clip.
- **What made the print, in its metadata:** FLX Film Lab and its version, and
  the film, its camera filter and the paper, in exported pictures and clips.
- **Five Ilford films:** Delta 100, Delta 400, FP4 Plus, Pan F Plus and Ortho
  Plus, each from HARMAN's own data.
- **Three Konica films:** Centuria Super 100 and 1600, and Impresa 50, each
  from Konica's own data.

### Changed
- **The Mac app's window keeps up with a dragged slider:** a slider tick
  redraws only its own part of the inspector, and a new frame only the print
  and what is measured of it. That is less than half the work 1.3.0 did for
  each.
- The Print shows the histogram and the curves in place of the paper's
  characteristic curve.

## 1.3.0 (2026-11-02)

FLX Film Lab for Windows, motion picture films and their print films, and
the filters and white balances for shooting tungsten film: 41 film stocks
and 13 print stocks in all.

### FLX Film Lab for Windows
- **Stills and Motion on Windows,** on the same film engine and film data as
  the Mac app, with a RAW decoder of its own that gives each camera Apple's
  colour. An installer for 64-bit Windows 10 and 11; the source is in
  `windows/`.
- **The Mac remains the main development platform.** The Windows app receives
  limited testing, on a single configuration: Windows 11 with an NVIDIA RTX
  graphics card.

### New
- **Motion picture films:** Kodak EXR 50D and EXR 200T, and Kodak 5251 50T,
  Kodak's camera negative of 1962, each built from its own published data.
- **Print films:** Fujifilm F-CP 3519, F-CP 3510 and Eterna-CP 3513DI, and
  Kodak 5385, 5251's print film.
- **Kodak Wratten 85 and 85B conversion filters,** for tungsten film in
  daylight.
- **Camera filters on every colour film,** where slides alone had the 81C and
  812.
- **Daylight D55 and Tungsten 3200 K white balances,** the films' own lights.
  Daylight and Tungsten are now labelled Daylight D65 and Tungsten 2850 K.
- **V-Log LUT pack:** the new films, and EXR 200T and 5251 converted for
  daylight through an 85, EXR 200T also through an 85B.

### Changed
- Tungsten films are balanced for a 3200 K lamp, as their datasheets say,
  instead of a household bulb: under Film reference a 3200 K lamp now prints
  neutral, and household bulbs warm.
- Picking a motion picture film moves the print onto its own print film, and
  picking a still film moves it back onto its paper.
- Fujifilm's motion picture films print on Fujifilm's F-CP 3510 instead of
  Kodak 2383, in the LUT pack too.
- The 81C is Kodak's Wratten 81C instead of Tiffen's, and prints almost the
  same; the 812 stays Tiffen's.
- The V-Log LUT pack leaves out Portra 800 pushed one and two stops; the app
  keeps them.

## 1.2.1 (2026-10-29)

### Fixed
- Every film stock, and every photo, has its own grain. In 1.2.0 all stocks
  drew the same grain pattern, only stronger or weaker, so HP5, Tri-X and
  Neopan 1600 looked alike.

## 1.2.0 (2026-10-28)

### Added
- Sharpness, Film or Camera, under Film & Paper. Camera keeps the camera
  file's own detail, with the film's colour, halation and grain. Film, the
  default, gives the film's own detail, softer at the finest scale on 35 mm,
  as the film is.

### Changed
- Grain, rebuilt. Each stock's grain is its datasheet's — Fujifilm's and
  Agfa's RMS granularity, Kodak's Print Grain Index, and Kodak's Vision3
  curves for each layer — where every colour stock had the same before.
  It is coarsest where a negative is thin, as film's is, and its dye clouds
  and silver clumps have a real size on the film, so the preview shows the
  grain the export has. No more per-pixel colour speckle.
- 120 is a 6×9 frame, 84 × 56 mm, where it was 56 mm on the long side, so
  grain, halation and edge effects are finer on it, as on the real negative.
- A 4:3 picture — Fuji GFX, Micro Four Thirds — spans the frame's short side,
  24 mm on 35 mm and 56 mm on 120, rather than its long side, so it sits on
  the film as large as it fits, as a 3:2 one does.
- Film format sits under Film & Paper, next to Sharpness.

## 1.1.2 (2026-10-26)

A small update, now for macOS 26 and Apple Silicon.

### Fixed
- The export format in the toolbar shows its name again; in 1.1.1 it was an
  empty field.
- The toolbar is one tone across.

### Changed
- Needs macOS 26 or later and a Mac with Apple Silicon. 1.1.1 remains for
  macOS 14 and 15, and for Intel Macs.

## 1.1.1 (2026-10-25)

A small update.

### Fixed
- Double-clicking a slider resets it again; in 1.1.0 it didn't.

### Changed
- Film format defaults to 120, so grain is finer out of the box.
- Built with Xcode 26: on macOS 26 the app takes the system's current look.

## 1.1.0 (2026-10-25)

Crop and straighten, presets, copy and paste settings, and four new film
stocks: 38 film stocks and 9 print stocks in all.

### New
- **Crop & straighten** (⌘R): aspect presets (3:2, 4:3, 5:4, 65:24, custom,
  free), quarter turns and mirrors, and levelling by slider or by drawing
  along the horizon. The meters read only the crop.
- **Presets:** save settings by name and apply them to any image later.
- **Copy and paste settings** between images (⇧⌘C, ⇧⌘V).
- **New film stocks:** Kodak T-Max 100, Royal Gold 100 and Vericolor III 160,
  and HARMAN Phoenix 200, each built from its own datasheet.
- **Tiffen 81C and 812 warming filters** for slide film.
- **Insta360 I-Log** in the Motion mode and LUT export.
- **V-Log LUT pack:** now 46 tables, with the new stocks.

### Changed
- Sharper preview: 2560 px by default, up to 4096, remembered between launches.
- New images start with Auto Exposure on, and Auto Brightness and Auto Contrast off.
- Toolbar reorganised; in Motion mode the export format now sits beside Export.

### Fixed
- Slides could show as negatives after "Print on paper" had been set on a negative.
- Double-clicking a slider now resets it.

## 1.0.0 (2026-10-23)

The first public release.

- **Stills:** a RAW photo through a spectral film model and a darkroom print, after
  spektrafilm, with NegPy-style print controls. These include grade and split
  grade, shadows and highlights, colour timing, masked dodge and burn, graduated
  exposure, bloom and glow.
- **Motion:** log clips graded through the same model, with playback and export.
  The formats are Nikon N-Log, Panasonic V-Log, Apple Log and Apple Log 2,
  Fujifilm F-Log and F-Log2, Canon Log 2 and 3, and Sony S-Log3.
- **LUTs:** `flxlut` bakes a look into a .cube for each of those log formats, and
  the app's Export LUT… does the same for the open clip.
- **V-Log LUT pack:** every film stock as a Real Time LUT for Lumix cameras,
  and Tri-X through its five filters, attached to the release
  (`tools/lut/vlog_pack.py`).
- **Films:** 34 film stocks, 37 counting the pushed versions, and 9 print stocks:
  seven papers and two cinema print films. 20 of the stocks are built here
  from their manufacturers' datasheets:
  - every slide film;
  - Superia 1600 and Superia Reala;
  - Agfa Optima II and Portrait XPS;
  - Eterna Vivid 160T, Eterna 500T and Reala 500D;
  - the black-and-white films (Tri-X, also pushed two stops, HP5 Plus, Neopan
    1600 and APX 25), printed on Ilford Multigrade.
