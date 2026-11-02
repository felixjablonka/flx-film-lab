"""Add cameras measured one by one (calibrate.py) to the port's table,
engine/data/apple-colour.json, by calibrate_all.py's rules: a camera's colour
from its file with the most colour in it, and Apple's crop for each mosaic
size (mode) its files have.

    add_to_table.py <calibration.json> ... [--manifest MANIFEST ...]

Each MANIFEST, a sample set's manifest.json, gives Apple's names for each
camera (those whose file sat in a folder named as the calibration's) and
the sample's source and licence; the last one that has the folder, the
source.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from calibrate_all import TABLE, rounded  # noqa: E402


def main():
    args = sys.argv[1:]
    manifests = []
    while "--manifest" in args:
        i = args.index("--manifest")
        manifests.append(json.load(open(args[i + 1])))
        del args[i:i + 2]
    table = json.load(open(TABLE))
    cameras = table["cameras"]
    for path in args:
        p = json.load(open(path))
        folder = os.path.basename(os.path.dirname(os.path.abspath(path)))
        names = sorted({a for m in manifests for a, e in m.items() if os.path.dirname(e["file"]) == folder})
        sources = [e for m in manifests for e in m.values() if os.path.dirname(e["file"]) == folder]
        if sources:
            p["source"] = {k: sources[-1].get(k, "-") for k in ("source", "licence", "mode")}
        key = f"{p['make']}|{p['model']}"
        old = cameras.get(key)
        modes = {f"{m['mosaic'][0]}x{m['mosaic'][1]}": m for m in (old or {}).get("modes", [])}
        modes[f"{p['mosaic'][0]}x{p['mosaic'][1]}"] = {"mosaic": p["mosaic"], "crop": p["crop"]}
        entry = p if not old or p["quality"]["colourVariety"] > old["quality"]["colourVariety"] else old
        entry = dict(entry, modes=sorted(modes.values(), key=lambda m: m["mosaic"]),
                     appleNames=sorted(set((old or {}).get("appleNames", [])) | set(names)))
        entry.setdefault("source", {"source": "-", "licence": "-", "mode": "-"})
        cameras[key] = rounded(entry)
        print(f"{key}: {'added' if not old else 'updated'}, colour from {entry['input']}, "
              f"{len(entry['modes'])} mode(s), Apple's names {entry['appleNames']}")
    json.dump(table, open(TABLE, "w"), separators=(",", ":"), sort_keys=True)
    print(f"{len(cameras)} cameras in {TABLE}")


if __name__ == "__main__":
    main()
