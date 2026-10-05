#!/usr/bin/env python3
"""Compares two `dfe --headless --forever-bench --output FILE` results.

    ./build/dfe --headless --forever-bench --output forever_worlds_before.json
    ./build/dfe --headless --forever-bench --output forever_worlds_after.json
    tools/forever_perf.py --baseline forever_worlds_before.json --current forever_worlds_after.json

Exit status 1 when the feature-off cost grew by more than --tolerance (default 5%) or a blended column costs more than
--blend-limit times a plain one (default 4x).
"""
import argparse, json, sys

ap = argparse.ArgumentParser()
ap.add_argument("--baseline", required=True)
ap.add_argument("--current", required=True)
ap.add_argument("--tolerance", type=float, default=0.05)
ap.add_argument("--blend-limit", type=float, default=4.0)
a = ap.parse_args()
b = json.load(open(a.baseline))["ms_per_column"]
c = json.load(open(a.current))["ms_per_column"]
bad = False
print(f"{'case':22} {'baseline':>10} {'current':>10} {'change':>8}")
for k in c:
    base = b.get(k)
    chg = (c[k] / base - 1) * 100 if base else float("nan")
    print(f"{k:22} {base if base is not None else float('nan'):10.3f} {c[k]:10.3f} {chg:+7.1f}%")
if b.get("feature_off") and c["feature_off"] > b["feature_off"] * (1 + a.tolerance):
    print("FAIL: feature-off generation is slower than the baseline"); bad = True
for k in ("blend_two_epochs", "blend_three_epochs"):
    if c[k] > c["feature_off"] * a.blend_limit:
        print(f"FAIL: {k} is more than {a.blend_limit}x a plain column"); bad = True
sys.exit(1 if bad else 0)
