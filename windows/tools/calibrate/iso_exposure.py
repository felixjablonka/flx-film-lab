"""Apple's baseline exposure by ISO for one camera, into the port's table.

Apple's decoder does not give a camera one exposure. For some ISO values it
gives another: on the Nikon D700 -0.7 EV at ISO 100 (Lo 1) against +0.1 from
ISO 200 up; on the Z 7 and Z f +1 EV from ISO 82 to 97, the auto ISO's ISO 90
among them. It goes by the ISO the file says: copies of one RAW file that
differ only in their ISO tags read differently, and the same as a file
really taken at that ISO (the Z 7's DSC_2063, at 90, reads 1.35 either way).

This reads Apple's value for copies of RAW at every whole ISO from 25 to 200,
where the exceptions found so far all are, and at the third- and sixth-stop
values above, up to 409600; then stores the camera's own value, the one most
ISO values have, and the exceptions, as ranges of ISO, in its entry of
engine/data/apple-colour.json:

    "baselineExposure": EV       the camera's own
    "isoExposure": [[from, to, EV], ...]   where Apple gives another
    "isoExposureFrom": the file they were read from

The decoder applies the exception for a file's ISO (RawDecoder). A profile
measured from a file at an exception's ISO carries that file's exposure in its
transforms (the D700's sample, most likely at ISO 100): they are scaled to the
camera's own value.

The ISO tags written are every one exiftool writes as ISO, and on a Nikon
its maker note's ISO and ISO2: the cameras read this way so far are Nikon's
(D700, Z 7, Z f).

It writes them with exiftool, on APFS clones of RAW in a temporary folder
(plain copies where RAW is on another volume), removed after.

    python3 -I iso_exposure.py RAW "Make|Model"

Needs build/mac-reference/flxbaseline (tools/mac-reference/build.sh).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TABLE = os.path.join(ROOT, "engine", "data", "apple-colour.json")
BASELINE = os.path.join(ROOT, "build", "mac-reference", "flxbaseline")
BATCH = 12

# every whole ISO up to 200; above, the third and sixth stops cameras write
THIRDS = [250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000, 2500, 3200, 4000, 5000, 6400, 8000, 10000, 12800,
          16000, 20000, 25600, 32000, 40000, 51200, 64000, 80000, 102400, 128000, 160000, 204800, 256000, 320000,
          409600]
SIXTHS = [220, 280, 360, 450, 560, 720, 900, 1100, 1400, 1800, 2200, 2800, 3600, 4500, 5600, 7200, 9000, 11000,
          14000, 18000, 22000, 28000, 36000, 45000, 56000, 72000, 90000, 110000, 140000, 180000, 220000, 280000,
          360000]
ISOS = sorted(set(range(25, 201)) | set(THIRDS) | set(SIXTHS))


def rounded(value):
    """7 significant digits, as calibrate_all.py writes the table"""
    if isinstance(value, float):
        return float(f"{value:.7g}")
    if isinstance(value, dict):
        return {k: rounded(v) for k, v in value.items()}
    if isinstance(value, list):
        return [rounded(v) for v in value]
    return value


def scaled(value, k):
    if isinstance(value, list):
        return [scaled(v, k) for v in value]
    return value * k


def read(raw, nikon, scratch):
    values = {}
    for start in range(0, len(ISOS), BATCH):
        batch = ISOS[start:start + BATCH]
        copies = []
        for iso in batch:
            copy = os.path.join(scratch, f"{iso}{os.path.splitext(raw)[1]}")
            if subprocess.run(["cp", "-c", raw, copy], capture_output=True).returncode != 0:
                shutil.copyfile(raw, copy)
            tags = [f"-ISO={iso}"]
            if nikon:
                tags += [f"-Nikon:ISO={iso}", f"-ISO2={iso}"]
            subprocess.run(["exiftool", "-q", "-q", "-overwrite_original", *tags, copy], check=True,
                           capture_output=True)
            copies.append(copy)
        out = subprocess.run([BASELINE, *copies], check=True, capture_output=True, text=True).stdout
        for line, iso in zip(out.splitlines(), batch):
            ev = line.split("\t")[1]
            if ev == "-":
                raise SystemExit(f"Apple's decoder did not open {raw} at ISO {iso}")
            values[iso] = float(ev)
        for copy in copies:
            os.remove(copy)
    return values


def ranges(values, own):
    """the exceptions, as runs of ISO values read in a row with one value"""
    out = []
    for iso in ISOS:
        ev = values[iso]
        if ev == own:
            continue
        if out and out[-1][2] == ev and ISOS.index(out[-1][1]) == ISOS.index(iso) - 1:
            out[-1][1] = iso
        else:
            out.append([iso, iso, ev])
    return out


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    raw, key = os.path.abspath(sys.argv[1]), sys.argv[2]
    table = json.load(open(TABLE))
    entry = table["cameras"].get(key)
    if entry is None:
        raise SystemExit(f"{key} is not in {TABLE}")
    scratch = tempfile.mkdtemp(prefix="iso-exposure-")
    try:
        values = read(raw, key.startswith("Nikon|"), scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    own = Counter(values.values()).most_common(1)[0][0]
    exceptions = ranges(values, own)
    measured = entry["baselineExposure"]
    if abs(measured - own) > 1e-6:
        # the profile was measured from a file at an exception's ISO
        k = 2 ** (own - measured)
        for fixed in entry["fixed"].values():
            fixed["transform"] = scaled(fixed["transform"], k)
        entry["grid"]["transforms"] = scaled(entry["grid"]["transforms"], k)
        print(f"{key}: profile measured at {measured:+.2f} EV, scaled to the camera's own {own:+.2f}")
    entry["baselineExposure"] = own
    entry["isoExposure"] = exceptions
    entry["isoExposureFrom"] = os.path.basename(raw)
    table["cameras"][key] = rounded(entry)
    json.dump(table, open(TABLE, "w"), separators=(",", ":"), sort_keys=True)
    print(f"{key}: {own:+.2f} EV at {sum(v == own for v in values.values())} of {len(values)} ISO values; "
          f"{len(exceptions)} exceptions: {exceptions}")


main()
