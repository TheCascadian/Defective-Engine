"""Painted-block stylizer: turns a plain tile into a hand-painted-looking one, deterministically.

Pure function of (tile, material class), no randomness, so the same input always gives the same bytes. Steps:
  1. clean    3x3 median removes single-texel noise, so colour reads as patches rather than static
  2. bands    luminance snaps to a few tonal steps (hue kept), the flat-shaded painted look
  3. bake     a fixed top-left light shades every texel by the slope of the tile's own height
  4. cavity   texels lower than their surroundings darken, which gives creases
  5. edge     optional 1 texel lit rim (top, left) and shaded rim (bottom, right), a chiselled block edge
  6. grade    saturation and contrast
The PBR companions (_n, _r) are derived from the result by gen_base_assets.make_pbr_maps, so bevels and creases
appear in the normal and height maps as well.

    python3 tools/stylize_textures.py in.png out.png --class wood      one file
    python3 tools/stylize_textures.py dir/ --class rock                every opaque PNG in a directory, in place
Classes: see CLASSES. Without --class the name picks one (guess_class).
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

# bands: tonal steps; clean: median blend; bake: baked light strength; cavity: crease darkening;
# edge: (lit rim, shaded rim) strength, 0 keeps the tile seamless; sat/contrast: grade.
CLASSES = {
    "rock":    dict(bands=6, clean=0.35, bake=0.35, cavity=0.5, edge=(0.0, 0.0), sat=0.95, contrast=1.10),
    "brick":   dict(bands=6, clean=0.2, bake=0.50, cavity=0.7, edge=(0.12, 0.22), sat=1.0, contrast=1.12),
    "wood":    dict(bands=7, clean=0.2, bake=0.35, cavity=0.5, edge=(0.0, 0.0), sat=1.05, contrast=1.10),
    "planks":  dict(bands=7, clean=0.1, bake=0.35, cavity=0.5, edge=(0.14, 0.26), sat=1.05, contrast=1.12),
    "endgrain": dict(bands=6, clean=0.1, bake=0.30, cavity=0.5, edge=(0.10, 0.18), sat=1.05, contrast=1.10),
    "soil":    dict(bands=5, clean=0.35, bake=0.25, cavity=0.4, edge=(0.0, 0.0), sat=1.0, contrast=1.05),
    "sand":    dict(bands=8, clean=0.3, bake=0.15, cavity=0.2, edge=(0.0, 0.0), sat=1.0, contrast=1.04),
    "plant":   dict(bands=6, clean=0.25, bake=0.30, cavity=0.4, edge=(0.0, 0.0), sat=1.08, contrast=1.08),
    "smooth":  dict(bands=8, clean=0.4, bake=0.10, cavity=0.2, edge=(0.0, 0.0), sat=1.0, contrast=1.02),
    "crystal": dict(bands=5, clean=0.15, bake=0.45, cavity=0.3, edge=(0.20, 0.15), sat=1.1, contrast=1.10),
    "ore":     dict(bands=6, clean=0.2, bake=0.40, cavity=0.4, edge=(0.0, 0.0), sat=1.0, contrast=1.10),
}

# First matching keyword wins.
KEYWORDS = [
    ("cobble", "brick"), ("brick", "brick"), ("planks", "planks"), ("_top", "endgrain"), ("log", "wood"),
    ("crystal", "crystal"), ("_ore", "ore"), ("sandstone_top", "endgrain"), ("sand", "sand"), ("snow", "smooth"),
    ("clay", "smooth"), ("terracotta", "smooth"), ("grass", "plant"), ("moss", "plant"), ("leaves", "plant"),
    ("dirt", "soil"), ("gravel", "soil"), ("mud", "soil"), ("peat", "soil"), ("podzol", "soil"), ("permafrost", "soil"),
]


def guess_class(name):
    for key, cls in KEYWORDS:
        if key in name:
            return cls
    return "rock"


def _shift(a, dx, dy):
    """a sampled at (x + dx, y + dy), wrapping."""
    return np.roll(np.roll(a, -dy, axis=0), -dx, axis=1)


def _median3(a):
    stack = np.stack([_shift(a, dx, dy) for dy in (-1, 0, 1) for dx in (-1, 0, 1)])
    return np.median(stack, axis=0)


def _blur(a):
    k = ((1, 2, 1), (2, 4, 2), (1, 2, 1))
    return sum(k[j][i] * _shift(a, i - 1, j - 1) for j in range(3) for i in range(3)) / 16.0


def stylize(rgb, cls):
    """rgb: float array HxWx3 in 0..1. Returns the painted tile, same shape."""
    p = CLASSES[cls]
    h, w = rgb.shape[:2]
    lum_w = np.array([0.299, 0.587, 0.114])

    med = np.stack([_median3(rgb[..., c]) for c in range(3)], axis=-1)
    rgb = rgb + (med - rgb) * p["clean"]

    lum = rgb @ lum_w
    q = np.round(lum * p["bands"]) / p["bands"]
    q = lum + (q - lum) * 0.7
    rgb = rgb * ((q + 0.02) / (lum + 0.02))[..., None]

    height = _blur(rgb @ lum_w)
    height = (height - height.mean()) / (height.std() + 1e-4)
    slope = height - _shift(height, -1, -1)          # positive when the surface tilts toward the top-left light
    shade = 1.0 + p["bake"] * np.clip(slope * 0.25, -0.5, 0.5)
    cav = np.clip(_blur(_blur(height)) - height, 0.0, 2.0)
    shade *= 1.0 - p["cavity"] * np.clip(cav * 0.18, 0.0, 0.5)

    hi, lo = p["edge"]
    if hi or lo:
        yy, xx = np.mgrid[0:h, 0:w]
        shade = np.where(yy == 0, shade * (1 + hi), shade)
        shade = np.where(xx == 0, shade * (1 + hi), shade)
        shade = np.where(yy == h - 1, shade * (1 - lo), shade)
        shade = np.where(xx == w - 1, shade * (1 - lo), shade)
    rgb = rgb * shade[..., None]

    lum = (rgb @ lum_w)[..., None]
    rgb = lum + (rgb - lum) * p["sat"]
    rgb = (rgb - 0.4) * p["contrast"] + 0.4
    return np.clip(rgb, 0.0, 1.0)


def stylize_file(src, dst, cls=None):
    img = Image.open(src).convert("RGBA")
    a = np.asarray(img).astype(np.float64) / 255.0
    if (a[..., 3] < 1.0).any():
        return False   # cutouts keep their silhouette: not stylized
    cls = cls or guess_class(os.path.splitext(os.path.basename(src))[0])
    out = stylize(a[..., :3], cls)
    res = np.dstack([np.round(out * 255), a[..., 3] * 255]).astype(np.uint8)
    Image.fromarray(res, "RGBA").save(dst)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--class", dest="cls", choices=sorted(CLASSES))
    args = ap.parse_args()
    if os.path.isdir(args.src):
        for f in sorted(os.listdir(args.src)):
            if f.endswith(".png") and not f.endswith(("_n.png", "_r.png")):
                path = os.path.join(args.src, f)
                print(f, "stylized" if stylize_file(path, path, args.cls) else "skipped (alpha)")
    else:
        if not args.dst:
            sys.exit("need a destination file")
        stylize_file(args.src, args.dst, args.cls)


if __name__ == "__main__":
    main()
