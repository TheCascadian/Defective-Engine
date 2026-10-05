#!/usr/bin/env python3
"""Seam check for a square-tile vertical animation strip.

Wrap-edge delta (last/first column, last/first row) against the mean delta of interior adjacent pixels, per frame
(reported) and averaged over frames (judged: one 16px seam is too few samples to judge alone).
Across frames: frame N-1 -> 0 delta against the mean consecutive-frame delta.
Fails (exit 1) when a wrap delta exceeds interior mean * 1.25.
"""
import sys

import numpy as np
from PIL import Image

LIMIT = 1.25


def check(path):
    img = np.asarray(Image.open(path).convert("RGBA"), dtype=np.float64)
    size = img.shape[1]
    frames = img.shape[0] // size
    ok = True
    sums = np.zeros(4)
    for f in range(frames):
        t = img[f * size:(f + 1) * size]
        ix = np.abs(np.diff(t, axis=1)).mean()
        iy = np.abs(np.diff(t, axis=0)).mean()
        wx = np.abs(t[:, 0] - t[:, -1]).mean()
        wy = np.abs(t[0] - t[-1]).mean()
        sums += (wx, ix, wy, iy)
        print(f"frame {f}: x wrap {wx:.2f} vs {ix:.2f}, y wrap {wy:.2f} vs {iy:.2f}")
    wx, ix, wy, iy = sums / frames
    bad = wx > ix * LIMIT or wy > iy * LIMIT
    ok &= not bad
    print(f"mean: x wrap {wx:.2f} vs {ix:.2f}, y wrap {wy:.2f} vs {iy:.2f}{'  FAIL' if bad else ''}")
    if frames > 1:
        tile = [img[f * size:(f + 1) * size] for f in range(frames)]
        mean = np.mean([np.abs(tile[f + 1] - tile[f]).mean() for f in range(frames - 1)])
        loop = np.abs(tile[0] - tile[-1]).mean()
        bad = loop > mean * LIMIT
        ok &= not bad
        print(f"loop {frames - 1}->0: {loop:.2f} vs mean consecutive {mean:.2f}{'  FAIL' if bad else ''}")
    return ok


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: check_tileable.py <strip.png>")
    sys.exit(0 if check(sys.argv[1]) else 1)
