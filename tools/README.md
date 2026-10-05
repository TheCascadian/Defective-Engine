# Tools

Utility scripts for the Defective Engine repository.

## repomix

Regenerate minimal repository structure metadata (excludes build artifacts, generated files, and non-essential directories).

**Bash version:**
```bash
./tools/remake-repomix.sh [output-file]
```

**Python version:**
```bash
python tools/remake_repomix.py [--output FILE] [--style {xml,markdown}]
```

**Prerequisites:**
```bash
npm install -g repomix
```

**Examples:**
```bash
# Generate default XML output
./tools/remake-repomix.sh

# Generate to custom file
./tools/remake-repomix.sh my-repomix.xml

# Python: generate markdown
python tools/remake_repomix.py --style markdown --output repo.md
```

Both scripts create/update `.repomixignore` with sensible defaults and generate minimal output by excluding:
- Build artifacts and compiled objects
- Generated CMake/build files
- Tool metadata directories
- Test/example directories
- Generated data files

## Dependencies

`gen_base_assets.py` needs Pillow; `check_tileable.py` needs `numpy` and Pillow (`pip install numpy Pillow`). `forge_replace.py` imports `gen_base_assets` (run it from `tools/` or with `tools/` on `PYTHONPATH`), so it needs Pillow too. `forge_import.py` uses only the standard library, but needs forge output built from `tools/forge/terrain.voxspec` by the voxel-asset-forge skill. Run all scripts from the repository root.

## forge_import.py

Imports voxel-asset-forge output built from `tools/forge/terrain.voxspec` into the base mod: copies textures to `mods/base/assets/base/textures/block/` and writes block JSON to `mods/base/data/base/blocks/`. Existing files are never overwritten.

```bash
python tools/forge_import.py <forge_out_dir> [--dry-run]
```

## check_tileable.py

Seam check for a square-tile vertical animation strip (or a single tile): compares wrap-edge pixel deltas to the interior mean and exits 1 when a seam exceeds 1.25x.

```bash
python tools/check_tileable.py <strip.png>
```

## stylize_textures.py

Deterministic "painted block" pass for any tile: median clean-up, tonal banding, baked top-left light, cavity darkening, optional chiselled rim, grade. `gen_base_assets.py` runs it on every `PBR_BLOCKS` texture before deriving the `_n`/`_r` maps, so the baked detail is also in the normal and height maps. For new or mod textures: `python3 tools/stylize_textures.py in.png out.png --class wood` (classes in the script; the file name picks one when `--class` is omitted).
