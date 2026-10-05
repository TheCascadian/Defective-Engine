#!/usr/bin/env python3
"""Install voxel-asset-forge block textures over the base mod's, then rebuild their PBR maps.

Reads <forge_out>/manifest.json; every variant-0 asset whose family is a base texture name (see PBR_BLOCKS in
gen_base_assets.py for the ones that get _n/_r maps) replaces textures/block/<family>.png. Run after
gen_base_assets.py, which regenerates the plain tiles.

usage: forge_replace.py <forge_out_dir>
"""
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_base_assets as g


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    manifest = json.load(open(os.path.join(src, "manifest.json")))
    n = 0
    for a in manifest["assets"]:
        if a["variant"] != 0 or "." in a["family"]:
            continue
        shutil.copyfile(os.path.join(src, a["file"]), os.path.join(g.TEX, a["family"] + ".png"))
        n += 1
    g.make_pbr_maps()
    print(f"installed {n} textures, pbr maps rebuilt")


if __name__ == "__main__":
    main()
