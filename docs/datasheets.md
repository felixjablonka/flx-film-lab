# Manufacturer datasheets

The film and paper profiles in `profiles/` are measured off the manufacturers'
published datasheets: characteristic curves, spectral sensitivities, dye spectra
and MTF curves, read from the sheets' vector plots. The sheets themselves are the
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
| Fujifilm | REALA 500D (8592/8692): product information | product sheet | `REALA500D_product_info.pdf` | reference only |
| Fujifilm | NEOPAN 1600 SUPER PRESTO | AF3-608E | `datasheet_neopan1600superpresto_en_01.pdf` | `build_bw_profiles.py` |
| Fujifilm | Professional Film Data Guide | AF3-207U | `Fuji_ProfessionalFilmDataGuide.pdf` | reference only |
| Kodak Alaris | KODAK PROFESSIONAL EKTACHROME Film E100 | E-4000 | `e4000_ektachrome_100.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| Kodak Alaris | KODAK PROFESSIONAL EKTAR 100 Film | E-4046 | `e4046_ektar_100.pdf` | reference only |
| Kodak Alaris | KODAK PROFESSIONAL TRI-X 320 and 400 Films | F-4017 | `Kodak Tri-X 320 and 400 F-4017.pdf` | `build_bw_profiles.py`, `bw_mtf.py` |
| Eastman Kodak | KODACHROME 25, 64 and 200 Professional Film | E-55 | `Kodachrome Spec Sheet.pdf` | `build_profiles.py`, `slide_mtf.py` |
| Kodak Alaris | KODAK PROFESSIONAL T-MAX 100 Film | F-4016 | `f4016_tmax_100-2018.pdf` | `build_bw_profiles.py`, `bw_mtf.py` |
| Eastman Kodak | KODAK ROYAL GOLD 100 Film | E-41 | `e41-1998_02_Kodak Royal Gold 100.pdf` | `build_profiles.py`, `sheet_readers.py` |
| Eastman Kodak | KODAK VERICOLOR III Professional Film | E-26 | `e26-Vericolor_III.pdf` | `build_profiles.py`, `sheet_readers.py`, `slide_mtf.py` |
| Kodak | KODAK WRATTEN 2 Optical Filter No. 8, 22, 25, 47 and 58 | filter data | `Kodak Wratten 2 Filter 8.pdf (and 22, 25, 47, 58)` | `lens_filters.py` |
| Tiffen | Filter Specifications: 81 series (p. 13), 812 Warming (p. 16) | catalogue | `Tiffen Filter Specifications.pdf` | `lens_filters.py` |
| Agfa | Technical Data: Agfa range of films, Professional | range sheet | `agfa_films_data.pdf` | `build_bw_profiles.py`, `build_profiles.py`, `bw_mtf.py`, `sheet_readers.py` |
| HARMAN technology (Ilford) | ILFORD HP5 Plus | technical information | `Ilford HP5 Plus.pdf` | `build_bw_profiles.py` |
| HARMAN technology (Ilford) | ILFORD MULTIGRADE RC Deluxe | technical information, 2020 | `Ilford Multigrade RC Deluxe.pdf` | `build_bw_profiles.py` |
| HARMAN technology | HARMAN Phoenix 200 | technical information, Sep 2024 | `HARMAN Phoenix Technical Data sheet 0924.pdf` | `build_profiles.py`, `sheet_readers.py` |

Each profile records which sheet each of its parts came from in its own
`datasource` field, and `NOTICE.md` lists what is inherited from spektrafilm
rather than measured.
