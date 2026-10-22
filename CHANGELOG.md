# Changelog

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
- **Films:** 34 film stocks, 37 counting the pushed versions, and 9 print stocks:
  seven papers and two cinema print films. 20 of the stocks are built here
  from their manufacturers' datasheets:
  - every slide film;
  - Superia 1600 and Superia Reala;
  - Agfa Optima II and Portrait XPS;
  - Eterna Vivid 160T, Eterna 500T and Reala 500D;
  - the black-and-white films (Tri-X, also pushed two stops, HP5 Plus, Neopan
    1600 and APX 25), printed on Ilford Multigrade.
