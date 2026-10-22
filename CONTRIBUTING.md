# Contributing

Thank you for looking. FLX Film Lab is a personal project, so issues and pull
requests are read as time allows.

## Building and testing

Everything builds with the Command Line Tools on macOS 14 or later; see *Build
and run* in the README. Before a pull request, run the suites that cover what you
changed:

```bash
./build.sh checks          # invariants of the engine: exposure, tone, colour, grain
./build.sh parity          # the Metal engine against spektrafilm's numbers
./build.sh apptest         # opening, editing and exporting photographs
./build.sh lutchecks       # the LUT baker and the log curves
./build.sh motiontest      # the Motion mode's video path, on synthetic clips
./build.sh motionapptest   # the Motion mode's model: open, grade, switch, export
```

They need a Mac with Metal. A check that needs one of the author's own RAW files
skips itself when the file is missing.

## Film and paper data

The baked packs in `FLXFilmLab/Resources/packs/` are generated: change the
profiles or `tools/bake/`, then rebake (*Rebuilding the data packs* in the
README). Rebuilding the profiles needs the manufacturers' datasheets, which are
not included; `docs/datasheets.md` lists them. A profile should follow its own
datasheet; where a sheet leaves something open, say what was chosen and why in
`NOTICE.md`.

## Licences

Code contributions are accepted under the GPLv3 and data contributions under CC
BY-SA 4.0, the licences of what they join. Please do not add the manufacturers'
documents themselves, or anything else you do not have the right to share.
