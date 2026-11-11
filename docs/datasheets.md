# Manufacturer datasheets

The film and paper profiles in `profiles/` are measured off the manufacturers'
published datasheets: characteristic curves, spectral sensitivities, dye spectra
and MTF curves, read from the sheets' vector plots. Every stock's grain is its
sheet's RMS granularity or Print Grain Index (`tools/bake/grain.py`, which
lists each figure and where it is from), and Kodak's Vision3 and EXR
granularity curves give the colour negatives theirs against density. The sheets themselves are the
manufacturers' documents, so they are not part of this source. The app and its
tests do not need them; only rebuilding the profiles does.

To rebuild, download each sheet from its maker (most are still on their websites,
the discontinued ones on archive sites) and save it in a `film spec sheets/`
folder at the top of the repository, under the file name below. The build
scripts look for these names.

| Maker | Document | Number | File name | Read by |
|---|---|---|---|---|
| Fujifilm | FUJICHROME PROVIA 100F Professional [RDP III] | AF3-036E | `films_provia-100f_datasheet_01.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| Fujifilm | FUJICHROME PROVIA 400X Professional [RXP] | AF3-0213E | `Fujichrome-Provia-400X-Professional-RXP_AF3-0213E.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| Fujifilm | FUJICHROME ASTIA 100F Professional [RAP100F] | AF3-149E | `astia_100f_datasheet.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| Fujifilm | FUJICHROME Velvia 50 Professional [RVP 50] | AF3-0221E2 | `VELVIA 50 Spec Sheet.pdf` | `build_profiles.py`, `slide_mtf.py` |
| Fujifilm | FUJICHROME Velvia 100 Professional [RVP 100] | AF3-202E | `films_velvia-100_datasheet_01.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| Fujifilm | FUJICOLOR SUPERIA 1600 [CU] | AF3-145E | `superia_1600_datasheet.pdf` | `build_profiles.py` |
| Fujifilm | FUJICOLOR SUPERIA REALA [CS] | AF3-967E | `fujicolor_superia_reala.pdf` | `build_profiles.py`, `sheet_readers.py` |
| Fujifilm | ETERNA 500T (8573/8673) | data sheet | `fujifilm_ETERNA_500t.pdf` | `build_profiles.py`, `sheet_readers.py` |
| Fujifilm | ETERNA Vivid 160 (8543/8643) | KB-0701E | `fujifilm_eterna_vivid160.pdf` | `build_profiles.py`, `sheet_readers.py` |
| Fujifilm | REALA 500D (8592/8692): characteristic curves | curve sheet | `Reala500D_curve.pdf` | `build_profiles.py`, `sheet_readers.py` |
| Fujifilm | FUJICOLOR POSITIVE FILM F-CP, 35mm Type 3519 / 16mm Type 3619 | KB-918E (1999) | `fcp_type3519.pdf` | `build_print_profiles.py`, `fuji_print_sheets.py` |
| Fujifilm | FUJICOLOR POSITIVE FILM F-CP, 35mm Type 3510 / 16mm Type 3610 | KB-301E (2003) | `fcp_type3510.pdf` | `build_print_profiles.py`, `fuji_print_sheets.py` |
| Fujifilm | FUJICOLOR POSITIVE FILM ETERNA-CP, 35mm Type 3513DI / 16mm Type 3613DI | KB-505E (2005) | `eterna_cp_type3513di.pdf` | `build_print_profiles.py`, `fuji_print_sheets.py` |
| Fujifilm | FUJIFILM MOTION PICTURE FILM MANUAL: 3510 and 3513DI curves (pp. 25-28), a check on their sheets | KB-0707E (2008) | `076_fujifilm_motion_picture_film_manual.pdf` | `fuji_print_sheets.py` |
| Fujifilm | REALA 500D (8592/8692): product information | product sheet | `REALA500D_product_info.pdf` | reference only |
| Fujifilm | NEOPAN 1600 SUPER PRESTO | AF3-608E | `datasheet_neopan1600superpresto_en_01.pdf` | `build_bw_profiles.py` |
| Fujifilm | Professional Film Data Guide | AF3-207U | `Fuji_ProfessionalFilmDataGuide.pdf` | reference only |
| Kodak Alaris | KODAK PROFESSIONAL EKTACHROME Film E100 | E-4000 | `e4000_ektachrome_100.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| Eastman Kodak | KODAK PROFESSIONAL ELITE Chrome Extra Color 100 Film | E-126E (4-05) | `e126e-Elitechrome_100ec.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| Eastman Kodak | KODAK PROFESSIONAL ELITE Chrome 200 Film | E-148E (4-05) | `e148e-Elite_chrome_200.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| Polaroid Corporation | T-53, T-553 and T-803 B&W High Speed Instant Peel-Apart Coaterless Film, Film Data Sheet | T-553 (1999) | `553fds.pdf` | `polaroid_sheets.py`, `build_instant_profiles.py`, `bw_mtf.py` |
| Polaroid Corporation | Polapan Pro 100 B&W T-54, T-554, T-664 and T-804, Film Data Sheet | T-554 (1999) | `554fds.pdf` | `polaroid_sheets.py`, `build_instant_profiles.py`, `bw_mtf.py` |
| Polaroid Corporation | Polacolor 559, and Technical Data for T-88, T-108, T-669, T-669S, T-559, T-559S, T-59 and T-809 Instant Color Peel-Apart Films | T-559 (1999) | `559fds.pdf` | `polaroid_sheets.py`, `build_instant_profiles.py`, `slide_mtf.py` |
| Kodak Alaris | KODAK PROFESSIONAL EKTAR 100 Film | E-4046 | `e4046_ektar_100.pdf` | reference only |
| Kodak Alaris | KODAK PROFESSIONAL TRI-X 320 and 400 Films | F-4017 | `Kodak Tri-X 320 and 400 F-4017.pdf` | `build_bw_profiles.py`, `bw_mtf.py` |
| Eastman Kodak | KODACHROME 25, 64 and 200 Professional Film | E-55 | `Kodachrome Spec Sheet.pdf` | `build_profiles.py`, `slide_mtf.py` |
| Kodak Alaris | KODAK PROFESSIONAL T-MAX 100 Film | F-4016 | `f4016_tmax_100-2018.pdf` | `build_bw_profiles.py`, `bw_mtf.py` |
| Kodak Alaris | KODAK PROFESSIONAL T-MAX 400 Film | F-4043 | `f4043_tmax_400.pdf` | none: its RMS granularity, 10, is Delta 400's (`bake_packs.py`) |
| Eastman Kodak | EASTMAN DOUBLE-X Negative Film 5222/7222 | H-1-5222 (10/2003) | `5222-Double-X_v1.pdf` | `kodak_bw_cine_sheets.py`, `build_bw_profiles.py`, `bw_mtf.py` |
| Eastman Kodak | EASTMAN DOUBLE-X Negative Film 5222/7222, Technical Data | H-1-5222 (3-22) | `DOUBLEX-Technical-Data-EN_v2.pdf` | `kodak_bw_cine_sheets.py`, `build_bw_profiles.py` |
| Eastman Kodak | KODAK Black-and-White Print Film 2302/3302, Technical Data | H-1-2302 (2022) | `Kodak 2302.pdf` | `kodak_bw_cine_sheets.py`, `build_bw_profiles.py` |
| Eastman Kodak | KODAK ROYAL GOLD 100 Film | E-41 | `e41-1998_02_Kodak Royal Gold 100.pdf` | `build_profiles.py`, `sheet_readers.py` |
| Eastman Kodak | KODAK VERICOLOR III Professional Film | E-26 | `e26-Vericolor_III.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| Kodak | KODAK WRATTEN 2 Optical Filter No. 8, 22, 25, 47 and 58 | filter data | `Kodak Wratten 2 Filter 8.pdf (and 22, 25, 47, 58)` | `lens_filters.py` |
| Kodak | KODAK WRATTEN 2 Optical Filter / Light Balancing 81 Series (81C) | filter data | `Kodak Wratten 2 Filters 81 Series.pdf` | `lens_filters.py` |
| Eastman Kodak / SMPTE, BBC | W. I. Kisner, "A New Color Negative Film for Better Picture Quality" and "A Higher Speed Color Print Film", Journal of the SMPTE 71(10), 1962, Figs. 1-3 each; C. B. B. Wood, BBC Research Department Report 1966/31, Fig. 1 (film-only MTF): EASTMAN Color Negative 5251, Color Print 5385, scanned figures compiled | papers | `Kodak 50T_5251_and 5385 print film.pdf` | `kodak_1962_sheets.py`, `build_profiles.py`, `build_print_profiles.py` |
| Eastman Kodak / SMPTE | R. L. Beeler, R. A. Morris, C. W. Simonds, "A New, Higher Speed Color Negative Film" (5254, "structurally the same" as 5251), Journal of the SMPTE 77(9), 1968, pp. 988-990 | paper | `SMPTE 1968 Kodak 5254.pdf` | the exposure-axis convention, 5251's masking (`kodak_1962_sheets.py`) |
| American Cinematographer | N. L. Simmons (Kodak), "The New Eastman Color Negative and Color Print Films", June 1962, pp. 362-363 and 385: 5251's EI 50 tungsten, EI 32 daylight with a Wratten 85, its 3200 K balance | article (online, Media History Digital Library) | — | `build_profiles.py` |
| Kodak | KODAK WRATTEN 2 Optical Filter / Conversion 85 Series (85, 85B) | filter data | `Kodak Wratten 2 Filters 85 Series.pdf` | `lens_filters.py` |
| Eastman Kodak | EASTMAN EXR 50D Color Negative Film 5245/7245 | H-1-5245 (3-99) | `Kodak EXR 50D 5245-1999.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py`, `exr_granularity.py` |
| Eastman Kodak | EASTMAN EXR 50D Film 5245/7245 | H-1-5245t (5-03) | `Kodak EXR 50D 5245.pdf` | `build_profiles.py`, `sheet_readers.py` |
| Eastman Kodak | EASTMAN EXR 200T Color Negative Film 5293/7293 | H-1-5293 (3-99) | `Kodak EXR 200T 5293-1999.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py`, `exr_granularity.py` |
| Tiffen | Filter Specifications: 812 Warming (p. 16) | catalogue | `Tiffen Filter Specifications.pdf` | `lens_filters.py` |
| Agfa | Technical Data: Agfa range of films, Professional | range sheet | `agfa_films_data.pdf` | `build_bw_profiles.py`, `build_profiles.py`, `bw_mtf.py`, `sheet_readers.py` |
| Konica | Konica Color CENTURIA SUPER 100 | technical data sheet, 2002 | `konica_csuper100.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| Konica | Konica Color CENTURIA SUPER 1600 | technical data sheet, 2002 | `konica_csuper1600.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| Konica | Konica Color IMPRESA 50 Professional Film | TDSN-501 (1998) | `konica_IMP50.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| HARMAN technology (Ilford) | ILFORD HP5 Plus | technical information | `Ilford HP5 Plus.pdf` | `build_bw_profiles.py` |
| HARMAN technology (Ilford) | ILFORD DELTA 100 PROFESSIONAL | technical information, Nov 2018 | `Delta-100_201811.pdf` | `build_bw_profiles.py` |
| HARMAN technology (Ilford) | ILFORD DELTA 400 PROFESSIONAL | technical information, Nov 2018 | `Delta-400_201811.pdf` | `build_bw_profiles.py` |
| HARMAN technology (Ilford) | ILFORD FP4 PLUS | technical information, Nov 2018 | `FP4-Plus_201811.pdf` | `build_bw_profiles.py` |
| HARMAN technology (Ilford) | ILFORD PAN F PLUS | technical information, Dec 2018 | `Pan-F-Plus_201812.pdf` | `build_bw_profiles.py` |
| HARMAN technology (Ilford) | ILFORD ORTHO PLUS | technical information, Oct 2019 | `Ortho-Plus_201910.pdf` | `build_bw_profiles.py` |
| HARMAN technology (Ilford) | ILFORD MULTIGRADE RC Deluxe | technical information, 2020 | `Ilford Multigrade RC Deluxe.pdf` | `build_bw_profiles.py` |
| HARMAN technology | HARMAN Phoenix 200 | technical information, Sep 2024 | `HARMAN Phoenix Technical Data sheet 0924.pdf` | `build_profiles.py`, `sheet_readers.py` |
| Kodak | KODAK VISION3 50D Color Negative Film 5203/7203 | K-1165, 2025 | `KODAK-VISION3-50D-5203-7203-brochure.pdf` | `vision3_granularity.py` |
| Kodak | KODAK VISION3 250D Color Negative Film 5207/7207 | K-1166, 2025 | `KODAK-VISION3-250D-5207-7207-brochure.pdf` | `vision3_granularity.py` |
| Kodak | KODAK VISION3 500T Color Negative Film 5219/7219 | K-1168, 2025 | `KODAK-VISION3-500T-5219-7219-brochure.pdf` | `vision3_granularity.py` |

Each profile records which sheet each of its parts came from in its own
`datasource` field, and `NOTICE.md` lists what is inherited from spektrafilm
rather than measured.
