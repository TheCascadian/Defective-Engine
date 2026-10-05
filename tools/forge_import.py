#!/usr/bin/env python3
"""Import voxel-asset-forge output (tools/forge/terrain.voxspec) into the base mod.

Reads <forge_out>/manifest.json, copies textures to mods/base/assets/base/textures/block/<name>.png and writes
mods/base/data/base/blocks/<name>.json. Existing files are never overwritten.

usage: forge_import.py <forge_out_dir> [--dry-run]
"""
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "mods" / "base"
TEX = ROOT / "assets" / "base" / "textures" / "block"
BLK = ROOT / "data" / "base" / "blocks"

LOGS = ["birch", "spruce", "acacia", "darkoak", "jungle", "cherry", "mangrove"]
LEAVES = ["birch", "spruce", "autumn_red", "autumn_orange", "autumn_yellow", "cherry", "acacia", "jungle", "willow"]
FLORA = ["cornflower", "daisy", "pink_tulip", "lavender", "orange_poppy", "bluebell", "fern", "dry_grass", "meadow_grass",
         "berry_bush", "azalea_bush", "cattail", "seagrass", "heather", "snow_shrub"]
STONE = {"terracotta_red": "pick", "terracotta_orange": "pick", "terracotta_white": "pick", "red_sandstone": "pick",
         "limestone": "pick", "tuff": "pick", "permafrost": "pick", "peat": "shovel", "mossy_cobble_cold": "pick",
         "mossy_cobble_warm": "pick"}

# forge asset family -> texture name copied into the mod
TEXTURES = {}
for s in LOGS:
    TEXTURES[f"logside.{s}"] = f"{s}_log_side"
    TEXTURES[f"logend.{s}"] = f"{s}_log_top"
for s in LEAVES:
    TEXTURES[f"leaf.{s}"] = f"{s}_leaves"
for s in FLORA:
    TEXTURES[f"flora.{s}"] = s
for s in STONE:
    TEXTURES[f"ground.{s}"] = s
TEXTURES["ground.dry_grass_top"] = "dry_grass_top"
TEXTURES["face.grass_side.dry"] = "dry_grass_side"


def tex(name):
    return f"base:block/{name}"


def blocks():
    out = {}
    for s in LOGS:
        out[f"{s}_log"] = {"textures": {"up": tex(f"{s}_log_top"), "down": tex(f"{s}_log_top"),
                                        "side": tex(f"{s}_log_side")},
                           "hardness": 2.0, "tool": "axe", "sound": "wood"}
    for s in LEAVES:
        out[f"{s}_leaves"] = {"textures": {"all": tex(f"{s}_leaves")}, "layer": "cutout", "tint": "none", "wind": True,
                              "light": {"opacity": 1}, "hardness": 0.2, "sound": "grass"}
    for s in FLORA:
        out[s] = {"shape": "cross", "layer": "cutout", "wind": True, "textures": {"all": tex(s)},
                  "hardness": 0.0, "sound": "grass"}
    for s, tool in STONE.items():
        out[s] = {"textures": {"all": tex(s)}, "hardness": 0.6 if tool == "shovel" else 1.5, "tool": tool,
                  "sound": "mud" if s == "peat" else "stone"}
    out["dry_grass_block"] = {"textures": {"up": tex("dry_grass_top"), "down": "base:block/dirt",
                                           "side": tex("dry_grass_side")},
                              "hardness": 0.6, "tool": "shovel", "drops": "base:dirt", "sound": "grass"}
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    if len(args) != 1:
        sys.exit(__doc__)
    src = Path(args[0])
    manifest = json.loads((src / "manifest.json").read_text())
    files = {a["family"]: src / a["file"] for a in manifest["assets"] if a["variant"] == 0}
    copied = written = skipped = 0
    for family, name in TEXTURES.items():
        if family not in files:
            sys.exit(f"missing forge asset for {family}")
        dst = TEX / f"{name}.png"
        if dst.exists():
            skipped += 1
        elif not dry:
            shutil.copyfile(files[family], dst)
            copied += 1
    for name, data in blocks().items():
        dst = BLK / f"{name}.json"
        if dst.exists():
            skipped += 1
        elif not dry:
            dst.write_text(json.dumps(data, indent=2) + "\n")
            written += 1
    print(f"textures copied {copied}, blocks written {written}, skipped existing {skipped}")


if __name__ == "__main__":
    main()
