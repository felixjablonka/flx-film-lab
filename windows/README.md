# FLX Film Lab for Windows

FLX Film Lab on Windows: the Mac app's film engine and both its modes,
Stills and Motion, ported to C++20 and Qt 6.12 (QRhi compute), and checked
against the Mac app's own engine. The Mac app stays the main version and is
never changed for the port, which follows its releases (now 1.3.0) and reads
its tree as it is: this folder's parent in the public repository, the
submodule `mac/` in the port's own.

## How the port is checked

The Mac engine is the reference. `flxref` is the Mac app's engine, built from
its own sources, with a main that renders a picture and writes out
everything its GPU kernels read and make: the decoded picture, the render
setup (uniforms, spectral tables, curves, LUTs) and the stages in between
(`log_e_film`, `cmy_film`, the linear print `rgb_out`, the 8-bit screen
image). The port renders the same setup on a GPU backend, and the two are
compared stage by stage. What differs is then the port's doing alone.

    git submodule update --init                       # the third-party libraries
    tools/mac-reference/run.sh <raw> out/<name> --size 3000 --plain
    cmake -S . -B build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)"
    cmake --build build/mac
    build/mac/flxport-render out/<name> --backend metal
    python3 tools/compare/compare.py out/<name>       # numpy
    build/mac/flxport-setup out/<name> --backend metal  # the port's own setup
    build/mac/flxport-checks                          # the film picker's papers, as tests/app

On Windows the same tools build into `build\win` (*Building on Windows*) and
run on the Mac's dumps with `--backend d3d12` or `d3d11`.

`flxport-setup` makes the port's own setup from the film packs (the Mac app's)
and the recipe the dump started from (`recipe.json`, in the Mac app's own
format) on the dump's picture, meters and all, compares every uniform, table
and LUT with the Mac's, and renders it into `port-<backend>-ownsetup/`.

flxref renders as the app does unless told otherwise: `--recipe FILE` loads a
saved recipe, and `--plain` turns
halation, the DIR couplers and grain off, `--no-halation`, `--no-couplers`
and `--no-grain` one each, `--film`, `--paper` and `--format 35mm|120` set
the stocks and the frame. `flxport-render --blur measured` blurs with the
kernels flxref measured from Metal Performance Shaders, into
`port-<backend>-measured/`, so the difference between the two runs is the port's blur's alone.
`tools/compare/sheet.py` makes a comparison sheet: the two renders, the ΔE
map and 1:1 crops where they differ most. `out/` holds the dumps (about
600 MB a picture at 3000 px) and is not in git.

## Status

The film engine for a frame, every stage of Renderer.render: filming.expose,
scatter and halation, filming.develop, the DIR couplers, the grain, the
print and scan, the grain's mean correction and the screen's encoding, 16
kernels and about 42 passes. At 3000 px, the Mac engine against the port on
the RTX 5070 Ti (Direct3D 12); Metal on the M4 Pro gives the same numbers:

| Picture, stocks | ΔE2000, largest / mean | Pixels over ΔE 0.1 | 8-bit values off by one | Direct3D 12 | Metal | Mac engine |
|---|---|---|---|---|---|---|
| DSC_0871, Portra 400 on Endura | 0.062 / 0.0004 | none | 0.05% | 19.7 ms | 77 ms | 97 ms |
| DSC_0871, Tri-X on Multigrade | 0.041 / 0.0006 | none | 0.17% | 21.5 ms | 76 ms | 94 ms |
| DSC_0871, Provia 100F as a slide | 0.031 / 0.0003 | none | 0.07% | 18.8 ms | 76 ms | 79 ms |
| DSC_0889, Ektar 100 on 35 mm | 0.038 / 0.0007 | none | 0.14% | 22.4 ms | 86 ms | 86 ms |
| DSC_0567, Portra 400 on Endura | 0.009 / 0.0001 | none | 0.01% | 19.9 ms | 76 ms | 92 ms |

ΔE 1 is a just-noticeable step; no 8-bit value is ever off by more than one.
All of that difference is halation's, from the blur (below): the couplers
alone match to 1.7e-6 in the linear print, the grain alone to 3e-5, its
random field bit for bit. Without halation, couplers and grain the port
matches to 1.9e-6 (ΔE 0.00026), on Direct3D 11 too, and renders in 3.2 ms on
Direct3D 12, 11 ms on Metal, against the Mac engine's 19.

The render setup is the port's own too (`engine/SetupBuilder`, `ToneCurve`,
`FilmMath`, `Packs`, `Recipe`): from the packs and a recipe, Auto Exposure,
Auto Brightness and Auto Contrast metered on the picture as the Mac does.
On five recipes that use every tone control (split grade, toe, shoulder,
snap, the RGB trims, regional colour, filter shifts, preflash, the density,
a graduated exposure, the meters), a black-and-white film through a red
filter, a slide, 35 mm and Camera sharpness, every table and LUT comes out
identical to the Mac's on the Mac, and within a float step or two (2e-6 at
most) on Windows, whose maths library rounds `erf`, `pow` and `exp` a little
differently; the uniforms differ by a float step where a 3x3 matrix is
inverted. Rendered from its own setup, the port matches the Mac's prints as
closely as from the Mac's setup: ΔE2000 0.006-0.049 at most, on Metal and
Direct3D 12 alike. The setup takes 1.4 ms on the Mac, 2.6 ms on the PC, and
11 and 25 ms with the meters, which run once a picture.

Followed to the Mac's 1.3.0: its new stocks and print films come with the
packs, and the port mirrors the rest, the 85 and 85B's print time set in
daylight, a motion picture film's print film, and the 3200 K lamp and its
white balances. On four more recipes at 1500 px (EXR 200T through the 85B on
F-CP 3519, Vision3 500T through the 85 on 2383, 5251 through the 85 on 5385,
Eterna 500T on F-CP 3510), and the five above, every value matches the Mac's
within 9e-7 of its largest, and the Metal renders within ΔE2000 0.09, no
pixel over 0.1. `flxport-checks` runs the Mac's own film-picker cases. On
the RTX 5070 Ti, Direct3D 12 and 11 match as closely, once Direct3D 11 no
longer read and wrote one texture in a dispatch: with Camera sharpness the
halation did, which Direct3D 11 does not allow.

Followed to the Mac's 1.5.0: Double-X and 2302, the two Elite Chromes and
Polaroid's three instant prints come with the packs. The port mirrors the
instant print: seen as it is whatever Print on paper says, laid on its own
4x5 frame (117.5 x 88.9 mm, which sets the pitch), and processed at the
temperature its sheet plots nearest the one chosen (55, 70 or 95 F),
nothing between. It also mirrors the ovals in Graduated Exposure (three
vec4 a gradient in `film_expose.comp`), Hide Mask Lines (Ctrl+Shift+H)
and the films in the menu's order. From DSC_0871 at 3000 px, the port's own
setup is identical to the Mac's (5e-7 at most), and its renders on Metal,
Direct3D 12 and 11 alike are within ΔE2000 0.014 with three gradients, two
of them ovals; 0.08 on Elite Chrome Extra Color; 0.21 on Polaroid 553 at
55 F and Polacolor at 95 F; 0.16 on Double-X on 2302. Never more than one
8-bit level off. The Polaroids' residual is the blur's, as above.
Double-X's is in its development's edge effects: the same code as Tri-X's,
but stronger (0.39 against 0.27) and wider, so it carries the float steps
further.

Followed to the Mac's 2.0.0: Fujifilm Neopan 100 Acros II,
Agfa APX 100 and 400, and Kodak's Wratten 82A and 82C cooling filters on
every colour film come with the packs. The port mirrors Settings' Films
tab (`filmmenu` in `engine/Packs`, the hidden films kept under the Mac's
key, `hiddenFilms`, and the one in use always listed) and the filters'
help. From DSC_0871 at 2000 px, the port's own setup matches the Mac's to
2e-6 of its largest value, and its renders on Metal and Direct3D 12 are
within ΔE2000 0.023 on Acros II, 0.036 on APX 100 and 0.035 on Provia 100F
through the 82C, never more than one 8-bit level off.

Past 12 MP the intermediates are half precision, as the Mac's. The shaped
print (`apply_geometry`), dodge & burn (MaskRasterizer) and Glow's effects
(Core Image kernels on the Mac) are ported with the app (below), and the RAW
decoder (below) runs on the CPU and on the GPU.

## The app

`app/` is FLX Film Lab for Windows: both halves of the Mac app, Stills and
Motion, in Qt Widgets on the port's engine, switched in the toolbar's centre
or with Ctrl+1 and Ctrl+2; a movie opened or dropped goes to Motion,
anything else to Stills. In Stills a RAW file opens (File > Open, or dropped on the
window), decodes on the GPU at the preview's size, and prints through the
film and paper the inspector chooses, with the Mac's controls, their two
vocabularies, help texts and defaults; Copy and Paste Settings, presets in
the Mac's own JSON (a preset file works in either app), the surround, and
export at full size: 16-bit TIFF, JPEG or PNG, the output profile embedded.
Rendered pictures (JPEG, PNG) open too, read colour-managed into the working
space as the Mac reads them. Crop & Straighten is the Mac's: turns, mirrors,
levelling (by the slider, or a line drawn along the horizon), crops of fixed
or free proportions dragged on the whole frame, the film exposed and
developed whole and the print made at the crop's own size with its grain
drawn after (`apply_geometry`), the meters reading the upright crop.
Graduated exposure and dodge & burn are the Mac's too: gradients and ovals given to
the film at capture, masks (polygons, ovals, gradient cards, feathered,
inverted, with a local grade) rasterised on the CPU into the 1024-pixel map
the print kernel reads, each mask's coverage kept while only its strength
moves; all drawn and dragged on the canvas through the print's shape.
Bloom & Glow are Glow's kernels on the GPU (`effects_*.comp`): bloom's
highlights selected in display values, blurred with their selection and
blended back, its haze, glow and Glow's halation added in linear light,
their sizes the frame's long edge's. Exports carry the photograph's EXIF
as the Mac's do: the camera and lens (a lens the file does not name from
its LensInfo, as ImageIO names it), the exposure, the date, the credits and
a GPS position where there is one, the orientation reset to upright; in a
TIFF's own directories, a JPEG's APP1 and a PNG's eXIf, each passing
exiftool's validation.

Motion is the Mac's Motion mode (`MotionDocument`, `MotionEngine`,
`MotionCanvas`, `MotionInspector`, `TransportBar`). A log clip opens and
plays, its frames read with FFmpeg as recorded (`engine/Clip`) and graded
on the GPU (`engine/Motion`, FramePipeline) through the print baked from
the film and paper, a 65-point LUT made again in about 15 ms when a control
moves, with Glow's bloom, glow and halation on top, at the size the frame
is shown. The format comes from the clip's tags or its maker (N-Log, V-Log,
Apple Log and Apple Log 2, F-Log and F-Log2, Canon Log 2 and 3, S-Log3 in
both gamuts, I-Log), the exposure is metered once across the clip, and the
pointer reads the code values, IRE and stops beneath it. Export
(`engine/ClipExport`) writes every frame at full size into ProRes 422 HQ or
HEVC Main 10 (on NVIDIA's, AMD's or Intel's encoder; *FFmpeg*), tagged
Rec.709 and written with the curve its displays decode: sRGB's for Windows
displays (the default here, as Windows shows video on the screen as it is),
a 1.961 power for Apple displays, 2.4 for a BT.1886 monitor; the sound
copied and the rotation and the camera's metadata kept; Export LUT writes
the print as a .cube for Resolve. Against the Mac's own Motion pipeline
(`tools/mac-reference/motion.sh`) on iPhone Apple Log, Zf N-Log and Lumix
V-Log frames, the port grades to within 7e-7 in display light, ΔE2000 0.03
with bloom and glow on; a ProRes export decodes to within ΔE 0.2 median per
pixel. Clips are decoded on the GPU's video decoder where there is one
(VideoToolbox, Direct3D 11, NVIDIA's), bit for bit as in software, so 4K
60p plays at its own rate on the M4 Pro. The sound plays with the picture
(`engine/ClipAudio`, decoded with FFmpeg to the speakers' format and played
through Qt Multimedia's audio output, `AudioPlayback`), started, stopped
and moved with it; the two clocks agree to a few milliseconds.

A folder's pictures, or in Motion its clips, line the left of the window, as
in the Mac app (`FolderBrowser`, `FolderSidebar`): File > Open Folder
(Ctrl+Shift+O), a folder dropped or handed over from Explorer (it opens in
the half that has its kind of file), or opening a picture lists its folder.
Rows of 84 x 56 prints with the name beside them in small type, the picture
on screen and the selection outlined in the accent colour; click, Ctrl- and
Shift-click, Up and Down (Shift extends), Ctrl+A, Ctrl+Up and Ctrl+Down from
anywhere, View > Sidebar (Ctrl+Shift+S). P picks, X rejects and U unflags,
as do the flag and the cross a row shows under the pointer (on its picture,
or the selection it is in); All, Picks or No Rejects; a reject is dimmed and
left out of batches, never deleted. A row's picture is the camera's own preview, the smallest the RAW
embeds that fills it (LibRaw's list of them); a picture with settings of its
own shows its print, taken from the screen when it is left or made on an
engine of its own in the background, and a clip its frame as a player shows
log footage, graded with its look once it has one of its own; prints are
cached on disk by file and settings. The pictures either side of the one on
screen are decoded ahead on an engine of their own, so stepping to one shows
it at once, and the toolbar's states, the status line's notes and the white
balance's "decoding…" wait 0.3 s before showing, so stepping does not flash
them. Only the last 30 pictures visited keep their undo history.

Each picture's settings and flag, and each clip's look and flag, are kept
between launches in a library in `%APPDATA%\FLX Film Lab\Library`, one file
per folder (`ImageLibrary`); a folder moved or renamed is found again by its
files' names and sizes. With Save settings beside images (Settings) each
record also goes into `DSC_0871.NEF.flx` beside its picture, the
Mac app's own form, so a folder's settings travel between the two apps both
ways (`flxport-checks` reads sidecars the Mac app wrote, and the Mac's own
decoder reads the port's); a sidecar is read wherever there is one, and of
it and the library the newer counts. Copy Settings and Paste Settings to
Selected (each keeps its own masks, gradients and crop, the paste a step of
its own history), Copy Look and Paste Look in Motion (Ctrl+Shift+C and V),
and Export Selected (Ctrl+Shift+E): each picture printed at full size on an
engine of its own, metered on its own preview-sized decode, one after
another, or each clip written with its own look, a picture or clip never
opened taking the look on screen; progress and Stop in the sidebar.

The previews are colour-managed for the monitor the window is on, as macOS
manages every window of the Mac app (`DisplayColour`). Windows shows an app's
pixels as they are, unless it manages colour itself on that monitor: with
Windows 11's automatic colour management on (wide colour on an SDR display),
or with HDR, it maps an app's sRGB to the display on its own, and FLX leaves
its previews in sRGB. Otherwise a monitor with a profile of its own (a
wide-gamut panel's, a calibration's) gets the print, the loupe, Motion's
frame, the sidebar's pictures and the surround converted to that profile,
the print straight from its output space, so a Display P3 or ProPhoto print
is not clipped to sRGB on the way. Windows' own sRGB profile, or one Qt
cannot read, changes nothing. The monitor's state is read again when the
window moves to another monitor, the screens change or the app comes to the
front; Settings says what the previews do, and `--display-colour FILE`
writes every monitor's colour mode and profile. The conversion runs on the
render threads (2.6 ms for a 2560 px preview on the M4 Pro); exports are
untouched, as they carry their own profile. HDR is left to Windows for now.

File > Settings (Ctrl+,) is the Mac's Settings window (`SettingsDialog`):
what belongs to the app rather than to a picture, kept between launches and
applied at once. Text size (Standard, Larger, Largest) makes the inspectors
and the sidebars 15 or 30 % larger as a whole, their type, controls, widths
and spacing together (`theme::px`, the panel fonts), as the Mac's does; the
toolbar, the canvas, the transport bar and the dialogs keep their size. The
panels are made again at the new size in their places, each section open or
closed as it was left, so nothing they show is lost. Control names
(Photographic or Darkroom, the Stills sliders; Motion's keep the
photographic names, as on the Mac), Preview size, Save settings beside
images, and the monitor's colour are there too; the inspector's Output
section keeps the picture's own colour space and gamut compression.

Original (`\`, in the View menu and the toolbar) shows the picture before film
and paper, the RAW as macOS renders it by default, as shot, in the print's
place, as the Mac's Original does; again, the print. Windows has no Core
Image, so the port renders it as Apple does (`engine/AppleRendering`),
measured on the Mac from CIRAWFilter at its defaults against its linear
decode: the decode with the camera's own white balance, at the preview's
size; Apple's shadow bias, which takes s x 2^baselineExposure off and scales
by 1 / (1 - s), s being 5/1000 for DNG files and 3/1000 for the cameras'
own formats; and Apple's tone curve, one table for every camera, on each
channel of linear Rec.2020 alike. Apple's gamut mapping, sharpening and
noise reduction are left out. The original is decoded the first time it is asked for, for the
picture on screen alone, and runs through the print's own geometry map,
so it is turned, levelled and cropped as the print is and a toggle moves
nothing; nothing is decoded until then. Against the Mac's Original on 15
pictures, colour and tone over 80-pixel blocks are within ΔE2000 0.2-1.0
median per picture on the Nikon Z 7 and Z f, the Leica M, the Ricoh GR IIIx
and a Canon EOS 400D, 1.8 on a tungsten-lit portrait from the Canon. Two
things differ: light sources past sRGB, which Apple's gamut mapping keeps
redder (the taxi's tail lights), and a Fujifilm frame shot with an extreme
custom white balance, which Apple reads as 100000 K. Windows' originals
equal the Mac build's within one 8-bit level.

    cmake --build build/mac --target FLXFilmLab       # the Mac build, for development
    tools\pc\build.cmd                                # Windows: build\win\FLXFilmLab.exe, Qt deployed beside it
    tools\pc\package.cmd                              # Windows: dist\FLX Film Lab and its installer

The package (`cmake --install`, by `tools/pc/package.cmd`) is the program
with Qt's libraries and plugins (windeployqt; of Qt Multimedia its library
alone, not its media plugins or Qt's own FFmpeg), the Visual C++ runtime and
FFmpeg's libraries beside it, the film packs, Apple's colour per camera,
Lensfun's database, RawSpeed's camera list, and `licenses/`. The installer
(`tools/pc/installer.iss`, Inno Setup 7) installs it for the person running
it, without administrator rights, or for everyone if chosen; it adds the
Start menu entry and offers the app in Explorer's Open with for RAW, JPEG,
TIFF and movie files, never as their default. A file opened while the app
runs goes to the window already open. The icon is Felix's artwork, masked
to its rounded square (`tools/pc/make_icon.py`), under the artwork's
reservation.

`DocumentModel` and `RenderEngine` are ported as they are: the render runs
on a thread with its own GPU device, a drag renders its latest value rather
than queueing every tick, a white balance change decodes again after a
120 ms settle, and each image keeps its edits for the session. The renderer
keeps the picture, its textures and the kernels between renders, reuses a
setup's buffers and bindings when the next one has the same shape, and,
as the Mac's, skips the film stage when only the print's controls moved.

For testing without a screen, `--screenshot PNG` writes the window once the
file has rendered, `--save-preview PNG` the preview itself, `--export PATH`
exports, `--recipe JSON` puts a Mac recipe on the file first, `--bench N`
times N renders, `--size WxH` and `--preview N` set the window and the
preview. With a movie, `--at SECONDS` shows that frame, `--play SECONDS`
plays first and counts the frames shown, `--codec prores|hevc` chooses
what `--export` writes, and `--export-lut PATH` writes the look's .cube.
`flxport-export` writes a clip as the app's Export does. `--save-original
PNG` writes the original in the print's place. The file may be a
folder, and `--screenshot` then waits for the sidebar's pictures too;
`--library DIR` keeps the library, the cached prints and the settings (an INI
file) in DIR rather than the app's own, so a test run never reads or changes
the person's. `--self-test RAWS --library DIR [--self-test-clips CLIPS]` drives
the folder workflow in the running app, as the Mac's app tests do: four of
RAWS' files copied into DIR and opened as a folder, decoded ahead, stepped
through without a load showing, settings and flags kept and found again by a
new library, the print kept from the screen, Paste Settings to Selected and
its undo step, Export Selected, the sidecars; and the two smallest of CLIPS
copied into DIR as a folder of clips, a look kept, its graded frame, Paste
Look and Export Selected; and the previews on a
wide-gamut monitor (`FLX_DISPLAY_PROFILE` names a profile to use as the
monitor's): the print, the surround, the sidebar's pictures and Motion's
frame converted, a Display P3 print reaching it unclipped; and Settings:
each text size's panels made again at their widths and type, a section left
open kept open, the sidebar made again with the folder and its selection,
Control names, Preview size and Save settings beside images reaching what
they change, the monitor's colour said, and on Windows every film's and
paper's name whole on its menu at each text size; and Original: its menu
item and key in Stills alone, the original at the print's size, not the
print, turned with it and let go; a row's pick and reject buttons, which
flag its picture alone and leave the selection and the list where they
were, a double click on one counting once; the toolbar's Stills/Motion
switch, in the middle of the window in both halves; and the menus, which
the wheel does not change and which open with the choice ticked, and the
status saying the photograph, and the film menu one column that scrolls;
and Settings' Films tab: films left out of the Stills and Motion film menus,
a hidden film in use kept in them, a group hidden and every film shown again
(83 checks, all passing on Direct3D 12; the Mac build, whose type is wider
than Segoe UI, runs the other 79, all passing on Metal, and only reports
the menus). `--text-size
SIZE` shows the panels at a text size for one run, without keeping it.

A slider's tick at the 2560 px preview, all of it (meters, setup, GPU,
readback, the image), on DSC_0871 with Portra 400 on Endura:

| | Print control (film stage kept) | Exposure (film stage again) | First render |
|---|---|---|---|
| RTX 5070 Ti, Direct3D 12 | 20 ms | 27 ms | 64 ms |
| M4 Pro, Metal | 47 ms | 68 ms | 90 ms |

The Mac app itself takes 23 and 40 ms on the M4 Pro. On Windows the kernels
are compiled when the app is built (fxc), and the decoder keeps them between
decodes: opening a 24 MP file decodes in 0.45 s on the PC, and a white
balance change, which decodes again as on the Mac, in 0.32 s. Exports of the 45 MP
file match between Metal and Direct3D 12 to 0.2 in 65535 on average, half
an 8-bit level at most, the half-precision intermediates past 12 MP
included. Against the Mac app's own render of the same file (its decode, its
engine, at 2560 px), the app's preview is within ΔE2000 0.8 median, 1.7 at
the 90th percentile, over 8 x 8 blocks (turned, levelled 3.5° and cropped
to 4:3: the same size to the pixel, 0.7 and 1.5; with a gradient, an oval
dodge with a softer local grade, a polygon burn and a gradient card: 0.78
and 1.6; with bloom, haze and glow: 0.81 and 1.7); per pixel the two
decoders' detail
differs (sharpening, noise, the lens, the highlights' rebuild). The port's
decode of this Z 7 file is 0.065 EV brighter than Apple's, a little more in
blue: Apple's exposure for it is not the 12-bit sample's it was measured on.

## RAW decoding

The Mac app decodes with Apple's decoder (CIRAWFilter, set up in
`RawLoader`); the port has its own, in `engine/RawDecoder`, on the CPU:

- **Unpacking.** RawSpeed (darktable's decoder) reads the sensor data, and
  LibRaw reads whatever RawSpeed can't: CR3, and cameras missing from
  RawSpeed's list. Both are compiled in from their sources
  (`cmake/RawSpeed.cmake`, `cmake/LibRaw.cmake`). They read the same
  values; RawSpeed's camera list has each camera's measured clipping point,
  and it decodes in half the time.
- **The port's own steps:**
  - black and white levels;
  - the white balance;
  - the demosaic: Malvar-He-Cutler on Bayer sensors, and a colour-difference
    one on X-Trans, right in colour but softer than Apple's;
  - clipped highlights rebuilt (below);
  - Apple's colour and crop (below);
  - false colour, colour noise and the capture's softness (below);
  - the lens's distortion corrected (below);
  - turning upright;
  - Lanczos scaling as RawLoader's.

`flxport-decode` decodes with the port (`--gpu` on the GPU), and
`tools/mac-reference/flxdecode` with Apple's decoder as RawLoader sets it up. In both, the sharpening, the colour
noise reduction, the false-colour (moiré) suppression and the lens
correction can each be switched off: `--no-sharpening`, `--no-colour-nr`,
`--no-moire`, `--no-lens-correction`.

**What it cannot read.** Tested (2026-11-12) with raw.pixls.us's samples of
each mode, against Apple's decoder on macOS 27.0.1, which opens them all:

| Camera | Mode | The port |
|---|---|---|
| Nikon Z f | Lossless compressed | reads it (RawSpeed) |
| Nikon Z f | High Efficiency (HE), HE* | cannot |
| Canon EOS R6 Mark II | RAW, C-RAW, full frame and 1.6x crop | reads them (LibRaw) |
| Sony A7R V | uncompressed, compressed, lossless compressed L | reads them (RawSpeed) |
| Sony A7R V | lossless compressed M, S | cannot |

- Nikon's High Efficiency RAW, on the Z 8, Z 9, Z f, Z6III and later, codes
  the mosaic in JPEG XS (intoPIX's TicoRAW). Neither decoder reads it: LibRaw
  0.22.3 identifies it and refuses it ("not supported yet" in its camera
  list), and RawSpeed fails on it. The port identifies it by the NEF's own
  compression (13 HE, 14 HE*), so cameras LibRaw does not list count too,
  and says so, with what to do: shoot Lossless compressed, or convert the file
  with Adobe DNG Converter (free), whose DNG is a mosaic again and opens with
  the camera's Apple colour (tested with DNG Converter 17.4.1 on HE*).
- Sony's lossless compressed RAW at M and S size (A1, A7 IV, A7R V and
  later) holds YCbCr colour, interpolated in the camera, not the sensor's
  mosaic. LibRaw reads it as colour pixels, which the port's pipeline, from
  the mosaic on, does not take; RawSpeed refuses it. Adobe DNG Converter
  turns it into a Linear Raw DNG, not a mosaic either. The port says so; the
  L size opens.
- `UnreadableFormat` (`engine/RawUnpack.h`) carries those messages in place
  of the decoders' own, and the canvas wraps a long error onto more lines.

A camera without Apple's colour measured (the Sony A7R V and A7R VI among
them: the A7R V's sample on raw.pixls.us is an M-size file) decodes with the
colour matrix RawSpeed's camera list gives it, Adobe's, close to Apple's but
not the same.

### Apple's colour

Apple colours each camera with a profile of its own. Adobe's profile for the
Zf, through the DNG pipeline, misses Apple's decode by ΔE 3-5. At a white
balance, Apple's colour is a 3x3 transform of the camera's RGB:

    Rec.2020 = C diag(1 / w) camera RGB

- w is the camera's neutral for the light, with its largest channel 1.
- C is the colour. On most cameras it follows a two-illuminant DNG profile,
  blended by 1/T between 2856 K and 6504 K and the same at any tint, but not
  on all. Some Fujifilms' depends on the tint too.
- The transform includes Apple's baseline exposure for the camera, which
  Apple varies per file. Where a camera's files at some ISO values get
  another, the profile lists them (`isoExposure`, below) and the decode adds
  the difference; a DNG gets its own BaselineExposure, as Apple applies it,
  with a profile or without. On Fujifilm it also follows the file's raw
  exposure shift (DR100/200/400), though not by a rule one sample per camera
  can pin down.
- Temperatures and tints are the DNG SDK's (Robertson's lines, tint -3000 to
  a unit of uv).

`tools/calibrate` measures the profile per camera on the Mac, from one raw
file:

1. `flxmeasure` renders the file through Apple's decoder, with everything
   that isn't colour switched off, at three sets of white balances:
   - the app's own: the films' references, Daylight, Tungsten, As shot;
   - a grid of 9 temperatures by 7 tints;
   - 8 probes off the grid.

   That is 79 pictures in about 5 s.
2. `calibrate.py` lines up the port's decode of the same file, finds Apple's
   crop of the mosaic to a fraction of a pixel, and fits Apple's transform at
   each white balance.
3. The port uses:
   - **the app's own white balances:** the measured transforms;
   - **Custom:** the grid's transforms, interpolated whole in mired and tint;
   - **As shot:** the grid's transform where the grid has the camera's own
     neutral, with that neutral put in.

   The transform is interpolated whole, not split into C and w, because at
   some lights a camera has no neutral (the grey takes a negative channel)
   while the transform stays smooth. The probes check the interpolation.
4. `calibrate_all.py` does this for a whole sample set and writes the table
   the port reads, `engine/data/apple-colour.json`.
5. `siblings.py`: a sample with little colour pins down little of the 3x3
   (the X-T30's). Such a camera takes the colour of a same-sensor camera with
   a better sample, but only where that profile does as well on the camera's
   own picture as its own does.
   - The X-T3's predicts the X-T30's decodes to ΔE 0.05, against its own
     profile's 0.22.
   - The X-T4, X-S10 and X100V share the sensor, yet Apple's colour for them
     is different.

One picture is enough: profiles measured from three Zf pictures predict each
other's decodes within ΔE 0.02 of their own. With a profile from another
picture, the port's decode matches Apple's to ΔE 0.03-0.34 median at every
white balance the app has: Film reference, As shot, Daylight, Tungsten and
Custom. Cameras without a profile fall back to their daylight matrix,
ΔE 0.6-2.4 from Apple.

    tools/mac-reference/build.sh                      # flxdecode, flxmeasure
    $PY -I tools/calibrate/calibrate.py <raw> <workdir>
    $PY -I tools/calibrate/calibrate_all.py <samples> <workdir> --jobs 4
    $PY -I tools/calibrate/siblings.py <samples> <workdir> --jobs 4
    $PY -I tools/calibrate/calibrate_all.py <samples> <workdir>   # the table again, with the siblings
    $PY -I tools/calibrate/calibrate.py <raw> <dir>/<camera>       # a camera added later:
    $PY -I tools/calibrate/add_to_table.py <dir>/*/calibration.json --manifest <samples>/manifest.json

`$PY` needs numpy. The samples are public, from raw.pixls.us (a folder per
file and a `manifest.json`). They take 11 GB, so they are fetched when the
table is measured again and not kept.

Findings:

- Apple's output is linear far above 1 and below 0: a strong white balance
  takes channels there, and they stay linear. Its only nonlinearity is the
  highlight reconstruction, from about 0.85 of the sensor's white.
- Core Image counts rows from the bottom. Where a scaled height has a
  fraction of a row, RawLoader's crop to whole pixels drops it at the top.
- RawSpeed moves a Bayer pattern with its crop, but leaves an X-Trans pattern
  at the mosaic's origin.
- LibRaw opens some cameras it has no data for, with no colour matrix and a
  guessed pattern; the port refuses those.
- Clipped highlights. Apple's decoder leaves a pixel's unclipped channels as
  they are and rebuilds the clipped ones, in the camera's RGB balanced for
  the light. The port rebuilds them as follows:
  - **All three clipped:** the neutral at the pixel's brightest channel's
    level, that is where the camera's last channel would clip. This is
    Apple's to ΔE 0.02, where the port drew magenta before (ΔE 29).
  - **Two clipped:** the third channel's level.
  - **One clipped:** the colour of the unclipped pixels around.

  On a Zf picture under D55 that comes to ΔE 3.2 and 8.9 from Apple, where
  it was 26 and 13. Apple's rebuild of those starts below the clip, from
  about 0.75 of white, lifting only the channel near it. A soft blend like
  that gained little on one picture; more pictures with clipped skies would
  pin it down.
- Where a light's neutral has a channel near zero, a balance from it starves
  the demosaic of that channel. The port demosaics with daylight's balance
  there; the transform undoes either.
- Keep `build/` and `out/` outside a folder iCloud syncs (they are links to
  one on this Mac). With the disk nearly full, iCloud evicts files, and macOS
  kills a program whose binary it evicted.

### On the GPU

`planDecode` lays a decode out on the CPU: the unpacking, the image area,
the white balance, the colour transform, the lens's distortion and the
size, and the mosaic with black and white off. Its pixels are then worked
on the CPU (`decodePlan`) or on the GPU (`GpuDecoder`, 20 kernels in
`engine/shaders/raw_*.comp`, QRhi compute), by the same steps and the same
arithmetic:

- the demosaic, the highlights and the colour transform;
- the detail steps;
- the lens's warp;
- turning upright and Lanczos scaling.

Two things need the whole picture, the highlights' block colours and the
noise's fit. The CPU works them out between the GPU's three frames, from
small readbacks.

On Metal, Direct3D 12 and Direct3D 11 alike, the GPU's decode matches the
CPU's to a median of 4e-6 of each value. At the 99.99th percentile it is
within 5e-4, and those are pixels on a threshold. The two CPUs, the Mac's
and the PC's, decode bit for bit alike.

| Picture | CPU | GPU, its frames | GPU, the whole decode |
|---|---|---|---|
| 24 MP, Mac (M4 Pro) | 1.2 s | 0.24 s | 0.4 s |
| 45 MP, Mac | 2.3 s | 0.39 s | 0.7 s |
| 24 MP, PC (RTX 5070 Ti, Direct3D 12) | 2.7 s | 0.08 s | 1.1 s |
| 45 MP, PC | 4.8 s | 0.13 s | 1.1 s |

All at 3000 px with every step on. Most of the rest of a decode on the GPU
is the unpacking, and loading the colour table and Lensfun once a run.

The PC's build has no Vulkan (no SDK), as with the film tools. The X-Trans
demosaic, and the lens's tabulated and DNG warps, run on the GPU untested,
for want of samples at hand.

    build/mac/flxport-decode <raw> <out.f32> --gpu   # --backend metal|d3d12|d3d11|vulkan

### Detail

The port follows the Mac app's principle, not Apple's filters.
RawLoader leaves Apple's sharpening, colour noise reduction and moiré
reduction at their defaults and switches luminance noise reduction off, as
the film's grain is the texture. So the port, by default:

- **suppresses false colour**: R - G and B - G each through a 3x3 median,
  once (Freeman's method);
- **reduces colour noise**: a guided filter (He, Sun and Tang) on the chroma
  of the channels' cube roots, guided by their luminance, so the chroma
  follows the luminance's edges and is evened out between them. It runs at
  half size, as in their fast variant, over some 10 x 10 pixels of a 24 MP
  picture, and an edge is anything over three times the noise;
- **sharpens the capture**: Richardson-Lucy deconvolution of the luminance,
  a Gaussian of 0.7 px, 10 times, applied to the three channels as a ratio.
  The result stays within each pixel's 3x3 neighbours, so there are no
  halos, and only detail over three to six times the noise is sharpened;
- **doesn't touch luminance noise.**

The steps are the port's own code (`engine/RawDetail`), on linear Rec.2020
after the colour transform, so the colour, and with it the parity with
Apple, is the same with them or without. The noise they gauge themselves
by is measured once on the picture, after the colour transform: its
variance against the level, a Y + b, from every fourth pixel each way. Levels
where the pixels hardly differ are left out of the fit. They are flat,
clipped highlights most often, with no noise to measure, and would pull the
fit to nought; a GPU rounding the mean by a hair did just that.

First, a pixel without light amid well-lit ones takes its eight neighbours'
median. Such pixels come from the demosaic overshooting beside a point of
light, on sensors without a low-pass filter. Taking their false colour
would leave them black. Pixels in the shadows' noise, where many sit below
nought, aren't touched: only those whose neighbours' median is over four
times the noise. Clamping the demosaic instead would change a sixth of all
pixels and the calibrated colour.

The steps take 1.1 s on 24 MP on the Mac's CPU and 2 s on 45 MP; the PC's
CPU takes about twice that. The Mac and the PC compute them bit for bit
alike: the cube roots and powers are taken in double, where the two
libraries agree.

### Lens correction

The port corrects the lens's distortion, and nothing else, as the Mac app
has Apple's decoder do. Here the Mac's decode is no reference: the port
follows the cameras' data and open-source practice, and Apple's decodes
only check that each camera's numbers are read right. In
`engine/LensCorrection`:

- **The camera's own data first**, read by readers of the port's own,
  written from public descriptions (exiftool's tag tables, the DNG
  specification):
  - Nikon: NikonNEFInfo's DistortionInfo;
  - Sony: DistortionCorrParams;
  - Fujifilm: GeometricDistortionParams;
  - Panasonic: DistortionInfo;
  - Olympus and OM System: the makernote's ImageProcessing 0x150a;
  - DNG: OpcodeList3's WarpRectilinear, as the specification has it, one
    warp per colour where the file has three.
- **Else Lensfun's database** (`third_party/lensfun`, its XML alone, under CC
  BY-SA 3.0). The port reads it with pugixml and evaluates Lensfun's models
  itself (ptlens, poly3, poly5), interpolated between calibrated focal
  lengths as Lensfun does; Lensfun's library isn't used.
  - A lens counts only where its name matches the file's: the same focal
    lengths, apertures and other words, except the maker's name. An adapted
    lens the camera calls "28mm f/2" is none of Lensfun's.
  - A compact camera takes the fixed lens on its own mount, as most compacts
    don't name their lens, but not a profile calibrated with a converter.
    Canon's compacts are corrected this way; their raws hold no numbers the
    port can read.
- **Neither:** no correction.

The scale: data with a scale of its own keeps it (Nikon's, Sony's,
Panasonic's, Olympus's), scaled up only as far as keeps every pixel of the
frame's edge from outside the picture. Fujifilm's and Lensfun's have none,
and fill the frame. DNG's warp is as the specification has it.

How each camera's numbers read, worked out against Apple's decodes of
public samples:

| Data | Source radius at output radius r | Check |
|---|---|---|
| Nikon | r (1 + k3 r² + k2 r³ + k1 r⁴) / (1 + c0), r over the readout's half-diagonal | 23 pictures, 0.03-0.14 px |
| Sony | r (1 + v(r) / 2¹⁴), v linear through n values evenly to the corner | RX100 VII, A7 IV: 0.1-0.5 ‰ |
| Fujifilm | r (1 + v(r) / 100), v linear through its radii, flat inside the first | X-T4: 0.15 ‰ |
| Panasonic | inverse: r = s · src (1 + p8 src² + p4 src⁴ + p11 src⁶) | LX100 II, ZS200, G9: 0.13 ‰ |
| Olympus | s r (1 + k1 q² + k2 q⁴ + k3 q⁶), q = s r over the 4:3 frame's half-diagonal | E-M10 IV, TG-6: 0.1 ‰ |

The Nikon coefficients are exiftool's 1-3, c0 the number at 0x40; for
Panasonic, exiftool's DistortionParam02-11 and DistortionScale s; for
Olympus, 0x150a's four numbers, k1-k3 and s. The ‰ are of the radius; the
px are at 3000 px. No other order or set of powers comes within 1 ‰ for
Nikon, or within 0.3 ‰ for Panasonic. Olympus's frame is the sensor's
whole 4:3, also for a picture cropped to 3:2 or 16:9.

    build/mac/flxport-decode <raw> <out.f32> --no-lens-correction   # or --lensfun-only
    build/mac/flxport-decode --lens <maker> <model> <lens> <focal>  # what Lensfun gives

## Findings

- Metal Performance Shaders' Gaussian blur (MPSImageGaussianBlur), which the
  Mac blurs with, is two algorithms. Below a sigma of 1 it is an exact
  sampled Gaussian over round(3.5 sigma) pixels each side, rounded half up,
  the same everywhere. From 1 on it is Apple's own approximation: within
  1e-3 of a pixel-integrated Gaussian, and not the same at every position of
  a large picture, by up to 1% of the kernel, so no one kernel reproduces
  it. The port blurs with the exact Gaussian below 1, as MPS, and the
  pixel-integrated one above. Halation's blurs are 2-10 pixels, where in
  dark pixels next to bright ones it is much of the light, and there the
  Mac's render differs from the port's by up to 2.5% of the exposure and ΔE
  0.06 at most.
- Uniform blocks hold whole `vec4`s only, so std140, Direct3D's constant
  buffers and Metal lay them out alike; `float3` and `float3x3` differ
  between them.
- QRhi's Direct3D backends take only Dynamic uniform buffers.
- Direct3D 11 reads zeros from a texture still bound as the last kernel's
  output, and QRhi binds a kernel's inputs before it unbinds the last one's
  outputs: there each kernel runs in a compute pass of its own.
- Its offscreen frames return before the GPU is done, even after
  `QRhi::finish()`, so they cannot be timed that way.
- `encode_display` rounds to the 8-bit levels itself: left to Direct3D's
  conversion, 4% of the values came out one level off Metal's.
- `active` and `shared` are reserved words in GLSL.
- Apple's baseline exposure goes by the file's ISO: for each camera a table of
  ISO values that get another, read in nothing but the ISO tags (copies of one
  file differing only in them read differently, and as files really taken at
  that ISO). On the Nikon D700, +0.1 EV, but -0.7 at ISO 100 (Lo 1), -0.57 at
  125, -0.23 at 160 and +0.2 from 25600. Its profile carried -0.7, the value
  Apple gives at ISO 100 alone, so the raw.pixls.us file it was measured from
  was most likely taken there (the file itself is gone), and the port decoded
  every other D700 file 0.8 EV darker than Apple. On the Z 7 and Z f, +0.35,
  but +1.35 from ISO 82 to 97 and +1 from 103 to 122, where auto ISO's sixth
  stops fall (ISO 90, 110), and less at their Lo settings; nothing above 163
  differs but the top Hi settings. Active D-Lighting changes nothing.
  `tools/calibrate/iso_exposure.py` reads a camera's table from one file of it
  (every whole ISO to 200, the third and sixth stops above) through Apple's
  decoder (`flxbaseline`) and writes it to the camera's profile, scaling the
  profile's transforms where its file was taken at an exception; done for the
  D700, Z 7 and Z f, whose files were at hand. A DNG carries its own
  BaselineExposure, which Apple applies: the port now does too, for a camera
  with a profile or without (the Ricoh GR IIIx's, -0.5 EV, had been left out).
  Against Apple's linear decode the D700's and the GR IIIx's are now within 1%
  (they were 0.8 and 0.5 EV off), and the Z 7 at ISO 90 within 0.1 EV (0.9).
- The C++ reproduces the Mac's Swift bit for bit, on the Mac, when it fuses
  no multiply and add (`-ffp-contract=off`, as Swift) and stays in float:
  `std::pow(10, x)` is double, `std::pow(10.0f, x)` float.

## Building on Windows

Visual Studio Build Tools 2026 (clang-cl, CMake and Ninja), Qt 6.12.0 for
MSVC 2022 64-bit with Qt Shader Tools and Qt Multimedia in
`C:\Qt\6.12.0\msvc2022_64` (`QT_DIR` overrides it), and Inno Setup 7 for
the installer. FFmpeg is the app's own build, made on a Mac by
`tools/ffmpeg/build.sh` (below) into `build/ffmpeg` (`FLX_FFMPEG` points
elsewhere), as the release workflow makes it on GitHub's macOS image.

    tools/ffmpeg/build.sh                             # on a Mac: build/ffmpeg, then copied over
    git submodule update --init                       # the third-party libraries
    tools\pc\build.cmd                                # build\win: the app and the tools
    tools\pc\package.cmd                              # dist\FLX Film Lab, and FLX Film Lab <version> Setup.exe

The port was developed on a Mac, which built and ran it on a PC over SSH;
that tooling stays in the port's own repository.

### FFmpeg

Motion reads and writes clips with FFmpeg, and the app carries its own
build of it, made by `tools/ffmpeg/build.sh`: FFmpeg 8.1's release source
under the LGPL (configured without `--enable-gpl`), cross-compiled with
mingw-w64 into five DLLs that need only Windows' own libraries, with no more
in it than the app uses. That is the containers Motion opens (MOV and MP4,
MXF, Matroska, AVI, MPEG transport and program streams), their video
(HEVC, H.264, ProRes, DNxHD and DNxHR, MPEG-2) and audio (AAC, AC-3, MP2, MP3,
ALAC, PCM) decoders, the GPU's video decoders (Direct3D 11, DXVA2, NVIDIA's),
and for export ProRes and the GPU's 10-bit HEVC encoders, NVIDIA's, AMD's and
Intel's, the last through Intel's libvpl. A PC with none of these exports
ProRes. Its sources are pinned by hash; `SOURCE.txt` beside the DLLs, which
the installer puts in `licenses\`, names each and how it was built, and
`NOTICES.txt` carries the licences of what is compiled in with FFmpeg.
