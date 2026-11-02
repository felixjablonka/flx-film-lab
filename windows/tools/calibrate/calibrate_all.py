"""Calibrate every camera of a sample set (tools/calibrate/calibrate.py) and
gather the profiles into the port's table, engine/data/apple-colour.json.

    calibrate_all.py <samples> <workdir> [--jobs N] [--retry] [--extra RAW ...]

<samples> holds one folder per sample file and manifest.json: Apple's camera
name -> the file, its source and licence. Each file is calibrated once, into
<workdir>/<folder>/calibration.json, so a run can stop and go on later. The
pictures each measures from (most of a gigabyte) go into a scratch folder per
worker, <workdir>/.scratch-<n>, which the next camera's overwrite and the end
of the run moves to the Trash; nothing is deleted. Files that fail are listed
with the reason in <workdir>/failures.json, and not tried again unless
--retry. --extra adds raw files of one's own (into <workdir>/extra).
Where siblings.py has run, the table follows its <workdir>/siblings.json.

The table has an entry per make and model as the port's decoders name them:
the colour of its file with the most colour in it, and Apple's crop for each
of its files' mosaic sizes, which are the camera's modes.
"""
import concurrent.futures
import json
import os
import shutil
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calibrate  # noqa: E402
import siblings  # noqa: E402

TABLE = os.path.join(calibrate.ROOT, "engine", "data", "apple-colour.json")


def rounded(value):
    """7 significant digits: more than the measurement resolves"""
    if isinstance(value, float):
        return float(f"{value:.7g}")
    if isinstance(value, dict):
        return {k: rounded(v) for k, v in value.items()}
    if isinstance(value, list):
        return [rounded(v) for v in value]
    return value


def one(raw, workdir, scratch_root):
    done = os.path.join(workdir, "calibration.json")
    if os.path.exists(done):
        return json.load(open(done)), None
    try:
        # this worker's scratch folder, overwritten camera after camera
        scratch = os.path.join(scratch_root, f".scratch-{os.getpid()}")
        return calibrate.calibrate(raw, workdir, scratch=scratch), None
    except Exception as e:                  # noqa: BLE001 - every failure is reported
        return None, f"{type(e).__name__}: {e}" if str(e) else traceback.format_exc(limit=1)


def main():
    samples, workdir = sys.argv[1], sys.argv[2]
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 3
    manifest = json.load(open(os.path.join(samples, "manifest.json")))
    files = {}
    for camera, entry in manifest.items():
        files.setdefault(entry["file"], {"cameras": [], **entry})["cameras"].append(camera)
    files = {f: e for f, e in files.items() if os.path.exists(os.path.join(samples, f))}
    print(f"{len(files)} files for {sum(len(e['cameras']) for e in files.values())} of Apple's cameras", flush=True)
    failed_before = os.path.join(workdir, "failures.json")
    failed_before = json.load(open(failed_before)) if os.path.exists(failed_before) and "--retry" not in sys.argv else {}
    extra = sys.argv[sys.argv.index("--extra") + 1:] if "--extra" in sys.argv else []
    jobs_list = [(os.path.join(samples, f), os.path.join(workdir, os.path.dirname(f)), f) for f in sorted(files)
                 if f not in failed_before]
    jobs_list += [(os.path.abspath(r), os.path.join(workdir, "extra", os.path.splitext(os.path.basename(r))[0]),
                   "extra/" + os.path.basename(r)) for r in extra]

    profiles, failures = {}, dict(failed_before)
    with concurrent.futures.ProcessPoolExecutor(jobs) as pool:
        futures = {pool.submit(one, raw, out, workdir): f for raw, out, f in jobs_list}
        for n, future in enumerate(concurrent.futures.as_completed(futures), 1):
            f = futures[future]
            result, error = future.result()
            if error:
                failures[f] = {"error": error, "cameras": files.get(f, {}).get("cameras", [])}
                print(f"{n}/{len(futures)} FAILED {f}: {error}", flush=True)
                continue
            entry = files.get(f, {"source": "one's own", "licence": "-", "mode": "-", "cameras": []})
            result["source"] = {k: entry[k] for k in ("source", "licence", "mode")}
            result["appleNames"] = sorted(entry["cameras"])
            profiles[f] = result
            print(f"{n}/{len(futures)} {calibrate.summary(result)}", flush=True)

    # cameras whose sample had too little colour take a sensor sibling's,
    # where siblings.py found Apple's the same on their own picture
    decisions = os.path.join(workdir, "siblings.json")
    decisions = json.load(open(decisions)) if os.path.exists(decisions) else {}
    by_folder = {os.path.dirname(f): p for f, p in profiles.items()}
    for f, p in profiles.items():
        donor = decisions.get(os.path.dirname(f), {}).get("donor")
        if donor in by_folder:
            p.update(siblings.scaled(by_folder[donor], p))
            p["colourFrom"] = f"{by_folder[donor]['make']} {by_folder[donor]['model']}"

    # the table: one entry per make and model as the port's decoders name
    # them, with the colour of the file with most colour, and Apple's crop for
    # each mosaic size the camera's files have
    table, modes = {}, {}
    for f, p in sorted(profiles.items()):
        key = f"{p['make']}|{p['model']}"
        modes.setdefault(key, {})[f"{p['mosaic'][0]}x{p['mosaic'][1]}"] = {"mosaic": p["mosaic"], "crop": p["crop"]}
        if key not in table or p["quality"]["colourVariety"] > table[key]["quality"]["colourVariety"]:
            table[key] = dict(p)
    for key, p in table.items():
        p["modes"] = sorted(modes[key].values(), key=lambda m: m["mosaic"])
        p["appleNames"] = sorted({n for q in profiles.values() if f"{q['make']}|{q['model']}" == key for n in q["appleNames"]})
    os.makedirs(os.path.dirname(TABLE), exist_ok=True)
    json.dump({"about": "Apple's camera profiles as macOS's RAW decoder applies them, measured by "
                        "tools/calibrate from public sample files (raw.pixls.us)",
               "cameras": rounded(table)}, open(TABLE, "w"), separators=(",", ":"), sort_keys=True)
    json.dump(failures, open(os.path.join(workdir, "failures.json"), "w"), indent=1, sort_keys=True)
    print(f"{len(table)} cameras in {TABLE}; {len(failures)} files failed", flush=True)
    # the scratch folders to the Trash, to be emptied
    for name in sorted(os.listdir(workdir)):
        if name.startswith(".scratch-"):
            shutil.move(os.path.join(workdir, name),
                        os.path.expanduser(f"~/.Trash/flx-calibration-{name[1:]}-{time.strftime('%H%M%S')}"))


if __name__ == "__main__":
    main()
