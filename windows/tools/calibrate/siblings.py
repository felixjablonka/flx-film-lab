"""Sensor siblings: a camera whose sample has little colour in it takes Apple's
colour from a camera with the same sensor whose sample has more, where Apple
uses one profile for both (as for the X-T3 and the X-T30).

    siblings.py <samples> <workdir> [--jobs N]

For each camera of <workdir> (calibrate_all.py's) whose sample's colour
variety is under 0.05, and each better-sampled camera of the same make and
mosaic size: Apple renders the camera's own picture again, and the other's
profile, its exposure and levels made the camera's, must do on it as well as
the camera's own does, which was fitted on that very picture (within 0.15
ΔE at the app's white balances, 0.1 over everything). Of those that do, the
best-sampled gives the colour. Writes <workdir>/siblings.json, which
calibrate_all.py's table follows; scratch folders go to the Trash after.
"""
import concurrent.futures
import json
import os
import shutil
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calibrate as c  # noqa: E402

WEAK, DONOR = 0.05, 0.08


def scaled(donor, camera):
    """The donor's transforms for the camera's RGB: its levels and Apple's exposure"""
    s = ((camera["white"] - camera["black"][0]) / (donor["white"] - donor["black"][0])
         * 2 ** (camera["baselineExposure"] - donor["baselineExposure"]))
    grid = dict(donor["grid"], transforms=[[(np.array(a) * s).tolist() for a in row] for row in donor["grid"]["transforms"]])
    fixed = {n: dict(e, transform=(np.array(e["transform"]) * s).tolist()) for n, e in donor["fixed"].items()}
    return {"grid": grid, "fixed": fixed}


def check(raw, camera, donors, scratch):
    """ΔE on the camera's own picture: its own profile's, and each donor's"""
    apple_dir = os.path.join(scratch, "apple")
    c.run([c.FLXMEASURE, raw, apple_dir, "--size", "1200",
           *[a for t, n in c.PROBES for a in ("--probe", f"{t}:{n}")]])
    info = json.load(open(os.path.join(apple_dir, "apple.json")))
    port_path = os.path.join(scratch, "port.f32")
    c.run([c.FLXPORT_DECODE, raw, port_path, "--camera-rgb", "--sensor-frame", "--size", "1200",
           "--crop", *map(str, camera["crop"])])
    pj = json.load(open(port_path[:-4] + ".json"))
    port = c.read_dump(port_path)[..., :3].astype(np.float64)
    sensor = port.reshape(-1, 3) / np.array(pj["whiteBalance"])
    smooth = c.smoothness(port)
    names = c.FIXED + ["as-shot"] + [f"probe-{t}-{n}" for t, n in c.PROBES]
    pictures = {}
    for name in names:
        a = c.read_dump(os.path.join(apple_dir, name + ".f32"))[..., :3].astype(np.float64).reshape(-1, 3)
        pictures[name] = (a, c.usable(sensor, a, smooth), info["dumps"][name])

    def score(profile):
        out = {}
        for name, (a, k, e) in pictures.items():
            if name in c.FIXED:
                t = np.array(profile["fixed"][name]["transform"])
            elif name == "as-shot":
                t = c.transform_for_neutral(profile, np.array(pj["asShotNeutral"]))
            else:
                t = c.transform(profile, e["temperature"], e["tint"])
            out[name] = c.de_stats(a, sensor @ t.T, k)[0]
        return out
    own = score(camera)
    return own, {name: score(scaled(d, camera)) for name, d in donors.items()}


def one(job):
    folder, raw, camera, donors, scratch_root = job
    scratch = os.path.join(scratch_root, f".scratch-{os.getpid()}")
    try:
        own, theirs = check(raw, camera, donors, scratch)
    except Exception as e:                  # noqa: BLE001 - reported, the camera keeps its own
        return folder, None, str(e)
    fixed = lambda s: max(s[n] for n in c.FIXED)
    median = lambda s: float(np.nanmedian(list(s.values())))
    good = [d for d, s in theirs.items() if fixed(s) <= fixed(own) + 0.15 and median(s) <= median(own) + 0.1]
    best = max(good, key=lambda d: donors[d]["quality"]["colourVariety"]) if good else None
    return folder, {"donor": best, "own": {"fixedWorst": fixed(own), "median": median(own)},
                    "donors": {d: {"fixedWorst": fixed(s), "median": median(s), "matches": d in good}
                               for d, s in theirs.items()}}, None


def main():
    samples, workdir = sys.argv[1], sys.argv[2]
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 4
    manifest = json.load(open(os.path.join(samples, "manifest.json")))
    folder_file = {os.path.dirname(e["file"]): e["file"] for e in manifest.values()}
    profiles = {f: json.load(open(os.path.join(workdir, f, "calibration.json"))) for f in sorted(os.listdir(workdir))
                if os.path.exists(os.path.join(workdir, f, "calibration.json"))}
    work = []
    for folder, p in profiles.items():
        if p["quality"]["colourVariety"] >= WEAK or folder not in folder_file:
            continue
        donors = {f: q for f, q in profiles.items() if f != folder and q["make"] == p["make"]
                  and q["mosaic"] == p["mosaic"] and q["quality"]["colourVariety"] >= DONOR}
        if donors:
            work.append((folder, os.path.join(samples, folder_file[folder]), p, donors, workdir))
    print(f"{len(work)} cameras to check against {sum(len(w[3]) for w in work)} possible siblings", flush=True)
    decisions = {}
    with concurrent.futures.ProcessPoolExecutor(jobs) as pool:
        for n, (folder, result, error) in enumerate(pool.map(one, work), 1):
            p = profiles[folder]
            if error:
                print(f"{n}/{len(work)} {p['model']}: {error}", flush=True)
                continue
            decisions[folder] = result
            d = result["donor"]
            print(f"{n}/{len(work)} {p['make']} {p['model']} ({p['quality']['colourVariety']:.3f}): own "
                  f"{result['own']['fixedWorst']:.2f} / {result['own']['median']:.2f}; "
                  + ", ".join(f"{profiles[x]['model']} {s['fixedWorst']:.2f} / {s['median']:.2f}{' *' if x == d else ''}"
                              for x, s in result["donors"].items()), flush=True)
    json.dump(decisions, open(os.path.join(workdir, "siblings.json"), "w"), indent=1, sort_keys=True)
    print(f"{sum(1 for r in decisions.values() if r['donor'])} of {len(decisions)} take a sibling's colour", flush=True)
    for name in sorted(os.listdir(workdir)):
        if name.startswith(".scratch-"):
            shutil.move(os.path.join(workdir, name),
                        os.path.expanduser(f"~/.Trash/flx-siblings-{name[1:]}-{time.strftime('%H%M%S')}"))


if __name__ == "__main__":
    main()
