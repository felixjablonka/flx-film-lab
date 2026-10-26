# Contributing

FLX Film Lab is a personal project, and its repository takes no issues or pull
requests. It is free software, though: you are welcome to fork it and change it
under its licences.

## Building and testing

Everything builds with the Command Line Tools on macOS 26 or later, on Apple Silicon; see *Build
and run* in the README. These suites cover what a change can break:

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

A fork keeps the licences of what it builds on: the GPLv3 for the code and CC
BY-SA 4.0 for the film and paper data. The app icon is under neither, so a fork
needs its own. Please do not publish the manufacturers' documents, or anything
else you do not have the right to share.
