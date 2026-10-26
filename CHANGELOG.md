# Changelog

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
