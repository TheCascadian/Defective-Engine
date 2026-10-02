#!/usr/bin/env python3
"""Runs dfe --benchmark over presets and window sizes, prints one table and saves the JSON results.

Run from the repository root:
    tools/perf_matrix.py --presets low,medium --sizes 1280x720,1920x1080 --seconds 30 --runs 3
Compare with an earlier run:
    tools/perf_matrix.py --baseline perf_results/20240607-120000.json
"""
import argparse, json, os, statistics, subprocess, sys, tempfile, time

def run_one(exe, preset, size, seconds, extra, label):
    w, h = size.split("x")
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as t:
        out = t.name
    cmd = [exe, "--benchmark", "--bench-seconds", str(seconds), "--preset", preset, "--width", w, "--height", h,
           "--bench-json", out, "--bench-label", label] + extra
    proc = subprocess.run(cmd, capture_output=True, text=True)
    try:
        with open(out) as f:
            return json.load(f)
    except (OSError, ValueError):
        sys.exit("benchmark produced no result; output tail:\n" + "\n".join((proc.stdout + proc.stderr).splitlines()[-15:]))
    finally:
        os.unlink(out)

def median_result(runs):
    """Picks the run with the median average fps so every number in the row comes from one real run."""
    return sorted(runs, key=lambda r: r["fps_avg"])[len(runs) // 2]

def verdict(r):
    ok = r["fps_avg"] >= 60 and r["fps_low1"] >= 40 and r["hitches"] == 0 and r["peak_memory_mb"] < 1536 and r["cold_start_s"] < 10
    return "pass" if ok else "fail"

HEADER = f"{'preset':8}{'size':11}{'rd':>4}{'fps':>8}{'1%low':>8}{'p99ms':>8}{'maxms':>8}{'hitch':>7}{'cpu':>7}{'swap':>7}{'gpu':>7}{'opaque':>8}{'mem MB':>8}{'cold s':>8}  budget"

def row(r, base):
    line = (f"{r['preset']:8}{str(r['width']) + 'x' + str(r['height']):11}{r['render_distance']:>4}{r['fps_avg']:>8.1f}{r['fps_low1']:>8.1f}"
            f"{r['frame_ms']['p99']:>8.1f}{r['frame_ms']['max']:>8.1f}{r['hitches']:>7}{r['cpu_ms']['total']:>7.1f}{r['cpu_ms']['swap']:>7.1f}"
            f"{r['gpu_ms']['total']:>7.1f}{r['gpu_ms']['opaque']:>8.1f}{r['peak_memory_mb']:>8.0f}{r['cold_start_s']:>8.1f}  {verdict(r)}")
    if base:
        line += f"   (baseline {base['fps_avg']:.1f} fps, {r['fps_avg'] - base['fps_avg']:+.1f})"
    return line

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default="build/dfe")
    ap.add_argument("--presets", default="low,medium,high")
    ap.add_argument("--sizes", default="1280x720")
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--runs", type=int, default=1, help="repeat each case and report the median run")
    ap.add_argument("--label", default="")
    ap.add_argument("--baseline", help="JSON file from an earlier run to compare against")
    ap.add_argument("--save", default="perf_results", help="folder for the results file, empty to skip")
    ap.add_argument("extra", nargs="*", help="further dfe arguments, after --")
    a = ap.parse_args()
    base = {}
    if a.baseline:
        with open(a.baseline) as f:
            base = {(r["preset"], r["width"], r["height"]): r for r in json.load(f)}
    results = []
    print(HEADER)
    for preset in a.presets.split(","):
        for size in a.sizes.split(","):
            runs = [run_one(a.exe, preset, size, a.seconds, a.extra, a.label) for _ in range(a.runs)]
            r = median_result(runs)
            if a.runs > 1:
                r["fps_spread"] = statistics.pstdev([x["fps_avg"] for x in runs])
            results.append(r)
            print(row(r, base.get((r["preset"], r["width"], r["height"]))), flush=True)
    print("\n" + results[0]["gl"])
    if a.save:
        os.makedirs(a.save, exist_ok=True)
        path = os.path.join(a.save, time.strftime("%Y%m%d-%H%M%S") + ".json")
        with open(path, "w") as f:
            json.dump(results, f, indent=1)
        print("saved", path)

if __name__ == "__main__":
    main()
