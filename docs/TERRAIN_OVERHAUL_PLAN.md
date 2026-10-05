> **Status: implemented (historical).** The work below is done; see [FOREVER_WORLDS.md](FOREVER_WORLDS.md) and [TREES.md](TREES.md) for the current behaviour. Kept for design rationale only.

# Terrain-generation overhaul — implementation plan (for Sonnet 5.5)

## Context

Defective-Engine (C, OpenGL 3.3) ships 8 hardcoded biomes, one log + one leaf block shared by all 11 tree species, 5 plant blocks, and a water texture with visible tile seams. Goal: a broad terrain-variety update in 5 areas: tree variety, surface detail, seamless water texture, more biomes, per-biome block rules.

Decisions already made with the user:
- **Compat:** new Forever-Worlds epoch/kernel. Epoch 0 (and all existing worlds) must stay bit-identical.
- **Biomes:** fully data-driven (per-biome JSON incl. surface, plants, trees). The 8 defaults are also expressed as data for the new kernel.
- **Water:** *texture only* — make the water tile seamless (spatial + animation loop). No mesher, LOD, shader, or fluid-sim work.
- **Assets:** ~40 new blocks, generated with the `voxel-asset-forge` skill.

The working tree already has many uncommitted changes (trees, epoch, hydro, forever worlds). Build on top; do not revert or reformat them; do not commit unless asked.

## Facts from exploration (verify before relying on them)

- Blocks: one JSON per block at `mods/base/data/base/blocks/<name>.json`, texture `mods/base/assets/base/textures/block/<name>.png` (square; animated = vertical strip + `<name>.json` `{"fps":N}`). No C change needed for a plain cube or `shape:"cross"` block. Parser: `parse_block_file` `src/registry.c:248`. Fields documented in `docs/MODDING.md` (~194-296).
- Textures: GL_TEXTURE_2D_ARRAY, tile = max source width (≥16, ≤128), REPEAT wrap, mipmaps (`textures_build` `src/registry.c:529`). Cap 1024 layers (currently ~44 used). Layers are independent, so no cross-tile mip bleed.
- Tint is a 2-bit enum (none/grass/foliage/water), colours hardcoded (`src/scene.c:439`, `chunk.vert`). **Do not add tint kinds.** Coloured logs/leaves = baked-colour textures with `tint:"none"`.
- Trees: `mods/base/data/base/trees/*.json`, schema `sdk/schemas/tree.schema.json`, loader `src/trees.c` (`parse_species` ~332 resolves `log`/`leaves` by block name; `trees_is_leaf_state` :64 already handles any leaf block). `selftest_trees.c` requires ≥10 species, `base:oak` = deciduous_broadleaf, `base:palm` = frond. Biome list per species max 8; species bind to biomes via `trees_bind_biomes`.
- Worldgen: `src/gen.c`. Biome pick `biome_index_at` :791 (hardcoded if-chain for defaults; first-match table for custom biomes). Surface `surface_block` :996, subsurface :1040, plants `plant_for` :1142 / `place_plants` :1158 (hardcoded for the 8 defaults, returns air for custom biomes), features `place_features` :1190, `GenBiomeDef` :44, `MAX_WORLDGEN_BIOMES`=32, `GenScratch.biome[]` is u8. Parsing: `parse_biome_array` :241 (a mod `biomes` array *replaces* the defaults). LOD uses the same `surface_block`/`subsurface_block` via `gen_lod_grid` :1790.
- Epochs: `src/epoch.c/.h`, `sdk/schemas/epoch.schema.json`, `tools/epoch_hash.py`. Epoch hash does not cover code, so a new kernel must be gated by an explicit epoch field. `gen.c`/`hydro.c`/`trees.c` compile with `-ffp-contract=off`; keep determinism (no `float` ordering tricks, integer hashes via `hash3`).
- Water texture: `tools/gen_base_assets.py:166-174` draws `sin((x+f*4)*0.8)*cos((y-f*2)*0.6)` on a 16px tile. 0.8·16 and 0.6·16 are not multiples of 2π, so the tile is not periodic in x/y, and the 4-frame loop does not close. That is the visible seam. Shader frame switch is a hard cut at 3 fps (leave it).
- `tools/gen_base_assets.py` regenerates block JSON and `worldgen/default.json`, and its water/lava block calls are out of sync with the checked-in JSON (checked-in water has `level` property, `reach`, `infinite`). **Never run the script end-to-end.** Only call its texture function(s) for water, or write PNGs directly.
- Tests: `./build/dfe --selftest` (groups: worldgen, hydrology, trees, forever-worlds, world-light-mesh…), `--headless --test-forever-worlds`. No CTest/CI. Diag flags: `--dump-trees`, `--dump-tree-shape`, `--tree-bench`, hydro diag.

## Phase 0 — Baseline (do first, nothing else changes)

1. Build (`cmake --build build`) and run `./build/dfe --selftest`; record pass state.
2. Add a golden test in `src/selftest.c` (near `test_gen_determinism` :729): hash block output of ~8 fixed columns at fixed seeds under epoch 0, with the **current** code, and hard-code the digests. This test must stay green through every later phase; it is the guard for "epoch 0 unchanged".

## Phase 1 — Water texture seamless (smallest, independent)

1. In `tools/gen_base_assets.py`, replace the water frame math with integer-period functions of the tile: for tile size S=16 and frame count F=4,
   `v = 0.5 + A·sin(2π(kx·x/S + f/F))·cos(2π(ky·y/S + f/F))` plus a second octave, with integer `kx,ky` (e.g. 2 and 3). Periodic in x, y, and f, so edges and the frame 3→0 wrap match.
2. Write only `water.png` (16×64 strip) + keep `water.json` fps. Do not regenerate any JSON.
3. Same fix for `lava` if it shares the pattern (cheap, optional).
4. Add `tools/check_tileable.py`: loads a PNG strip, reports (a) wrap-edge delta vs mean interior adjacent-pixel delta for x and y per frame, (b) frame-3→0 delta vs mean consecutive-frame delta. Fail if wrap delta > interior mean × 1.25. (Alternative: use `forge.py check`'s seam score.)
5. Verify: run the checker on old (git stash of the PNG or `git show HEAD:` copy) vs new water.png — old must fail, new must pass. Eyeball a 3×3 tiled preview PNG written to the scratchpad.

## Phase 2 — Asset generation with voxel-asset-forge (~40 blocks)

Skill location: `~/.claude/skills/voxel-asset-forge` (`scripts/forge.py`, `references/dsl.md`, `palettes.md`, `recipes_catalog.md`). Rules from the skill: write a spec of family lines (cost scales with families, not assets); never hand-edit PNGs; fix a rejected asset by editing the spec line and rebuilding.

1. Read `references/dsl.md` once, `palettes.md` once, and the "Forest and wood" + "Temperate" + "Desert and mesa" + "Snow and ice" sections of `recipes_catalog.md`. Add `@ramp` palettes in the spec for colours not built in (autumn red/orange/yellow, cherry pink, jungle green, spruce teal, acacia olive, dark-oak brown, birch white, etc.).
2. Create `tools/forge/terrain.voxspec` (new dir). Use `--size 16 --seed <fixed>`; keep `--variants 1` for blocks (the engine uses one texture per face).
3. Families (sweeps keep it to a few lines):
   - **Logs (side + end), ~7 species:** birch, spruce, acacia, dark_oak, jungle, cherry, mangrove/palm. Side = bark pattern (`lib.bark`; vary with `stripe`/`crack` ops per species, e.g. birch white with dark horizontal flecks, acacia grey-orange diagonal). End = ring pattern (`gradient mode=ring`).
   - **Leaves, ~9, each a distinct colour *and* pattern:** birch (yellow-green, sparse), spruce (dark teal, needle `stripe`), autumn_red / autumn_orange / autumn_yellow, cherry_blossom (pink with speckles via `scatter`), acacia (olive, flat clumps), jungle (vivid, dense), willow (pale drooping `stripe nx=0`). Leaves must be cutout (alpha holes): verify how existing `leaves.png` encodes holes and match (alpha <128 = hole; check one pixel read, don't view per-asset).
   - **Flora, ~15 cross-shape textures (alpha cutout):** cornflower, daisy, pink_tulip, lavender, orange_poppy, bluebell, fern, dry_grass, meadow_grass, berry_bush, azalea_bush, cattail, seagrass, heather, snow_shrub. Flora needs masks; add new ASCII `@mask` blocks (the one place hand-authored pixels are allowed) for flower/fern/bush/reed silhouettes, then ramp across colours with one sweep line.
   - **Ground/stone, ~10:** terracotta_red/orange/white, red_sandstone, limestone, tuff, permafrost, peat, dry_grass_block (top+side via `face.grass_side` line from the catalog), cold/warm variants of mossy cobble as needed.
   - Total ≈ 7 + 9 + 15 + 10 ≈ 41.
4. `python scripts/forge.py list --spec …` → show asset count; then `build … --qa --atlas --report`. QA must exit 0.
5. Write `tools/forge_import.py`: reads `manifest.json`, copies chosen assets to `mods/base/assets/base/textures/block/<name>.png`, and emits `mods/base/data/base/blocks/<name>.json` from a small in-script table (shape, layer, tint, hardness, sound, drops, `item:true`). Cross blocks: copy the existing `flower_red.json` / `tall_grass.json` fields (`shape:"cross"`, `layer:"cutout"`, `wind:true`, `hardness:0`, `sound:"grass"`). Logs: copy `log.json` and swap texture ids. Leaves: copy `leaves.json` but `tint:"none"` for baked colours (keep `tint:"foliage"` only for plain green variants that should follow the biome tint). Do not overwrite existing blocks.
6. Check `mods/base/mod.json` for any explicit block/texture listing; add if required.
7. Verify: `./build/dfe --selftest` (registry/content groups), engine boot logs "Texture array: N layers" with N < 1024.

## Phase 3 — New tree species (data only, no C unless a shape is missing)

1. Add `mods/base/data/base/trees/*.json` (~10): birch (new birch log/leaves; shape deciduous_slender), spruce variants using new spruce leaves, dark_oak (broadleaf, dense), cherry (broadleaf, pink leaves), acacia (new colours), jungle (tall deciduous_broadleaf, `density` high), autumn_oak_red/orange/yellow (oak shape, autumn leaves; used in a few biomes), willow (new willow leaves), mangrove/palm (new log). Follow `oak.json` format and `docs/TREES.md`; validate against `sdk/schemas/tree.schema.json`.
2. Existing species files that still point at `base:log`/`base:leaves` stay unchanged (so epoch 0 output is unchanged — species block pick must not change for kernel 0; new species must be unreachable in kernel 0: bind them only to the new biomes, which don't exist under kernel 0).
3. `selftest_trees.c` still asserts ≥10 and oak/palm shapes; add assertions that every species' `log`/`leaves` resolve and that new leaf blocks pass `trees_is_leaf_state`.
4. Verify: `./build/dfe --dump-trees`, `--tree-bench`, selftest trees group.

## Phase 4 — Data-driven biomes, kernel 1 (core C work)

### 4a. Epoch kernel gate
- Add integer `kernel` (default 0) to `EpochParams` and `sdk/schemas/epoch.schema.json`; parse in `epoch.c:parse_params` (:200). Include in the epoch hash input and `tools/epoch_hash.py` only when non-zero, so existing epoch hashes do not change.
- Create epoch folder `epoch 1` (follow `docs/FOREVER_WORLDS.md` layout) with `kernel:1`; make it the default for *new* worlds, keep epoch 0 for old saves. Read `epoch.c` first to see how `epoch_current` selects the epoch for a new world and how `src/save.c` stores it.
- In `gen.c`, every new-behaviour branch is `if (kernel >= 1)`; the legacy branches are not edited.

### 4b. Biome data format
- New `sdk/schemas/biome.schema.json` + loader `src/biome.c/.h`: one file per biome at `data/<ns>/biomes/<name>.json` (extends, not replaces; `parse_biome_array` custom-array path stays for old mods). Fields:
  - `id`, `climate`: `temperature` center+extent, `humidity` center+extent, `height` range, optional `weirdness` range (see 4c), `priority`.
  - `surface`: ordered layers `[{block, depth, when:{slope_max, slope_min, patch:{noise:"a|b", lo, hi}, water_dist_max, near_river, height_min, height_max}}]` replacing hardcoded `surface_block`/`subsurface_block` rules (river bed variants, steep rock, snowline, mud patches).
  - `plants`: `[{block, chance, on:[block ids/tags], habitat: dry|shore|wetland|any, density_noise?}]` — replaces `plant_for`. Multiple entries per biome, evaluated in order with one hash roll per column so total density stays controllable.
  - `ores`/`features`/`structures` references (reuse existing `GenFeatureDef`/`GenOreDef` parsing) plus `tree_density_scale`.
  - `tags` for tree binding (trees already list biome ids).
- Hard limits to keep: `MAX_WORLDGEN_BIOMES`=32 (u8 `biome[]`). Target 8 default + 16 new = 24.
- Express the 8 default biomes as data files under `mods/base/data/base/biomes/` that reproduce the legacy look under kernel 1 (they need not be bit-identical to epoch 0; they only must exist).

### 4c. Biome selection (kernel 1 only)
- Replace the if-chain with a nearest-center selection in (temperature, humidity, height-band) space (Whittaker-style), using the existing jittered climate from `biome_index_at` (`BIOME_JITTER`, `ALT_JITTER`, epoch biases). Add one new low-frequency "weirdness" noise state in `N` (init in `gen_init` :572) for variants (flower_forest vs forest, ice spikes vs tundra). Sea-level/ocean/beach/river handling stays hydro-driven and is not re-derived.
- Deterministic tie-break: lower table index. Float compares only, no `fma`.

### 4d. New biomes (16): drawn from real climates + common modded sets
birch_forest, dark_forest, taiga, snowy_taiga, savanna, jungle (tropical rainforest), badlands (mesa, terracotta strata via `stripe`-like layers in `surface`), meadow, cherry_grove, flower_forest, shrubland (mediterranean), steppe, mangrove_swamp, bog (peat + moss), alpine (snow/rock/gravel above treeline), volcanic_plain (basalt/tuff/ash, optional sparse features).
Each needs: climate box, surface layers using new ground blocks, plant list using new flora, tree species list, and 1-2 distinct features (e.g. fallen logs, boulders via `features`).

### 4e. Wire into gen
- `gen_init`/`registry_load_worldgen_config` (`gen.c:~358`): load biome files after blocks; build `BiomeDef` table; resolve block names once.
- `biome_index_at`, `surface_block`, `subsurface_block`, `place_plants`/`plant_for`: branch on kernel; kernel 1 dispatches into `biome.c` rule evaluators. `gen_lod_grid` calls the same `surface_block`, so LOD stays consistent automatically.
- `place_trees_data`: no change except species binding via the new biome ids; confirm `trees_bind_biomes` runs after biomes load.
- Record biome weights in CGM only if `record_cgm` already does (:1682); keep within the 16-byte `biome_weights` limit (≤16 stored biomes — if >16 biomes, store the 16 highest-weight; check `chunk_generation_metadata.schema.json`).

### 4f. Diagnostics
- Add `--dump-biomes` (in the style of `src/hydro_diag.c` / `tree_diag.c`): prints biome coverage histogram for a seed over N columns, so a bad climate box is easy to spot (every biome ≥ ~1% and none >35% except ocean).

## Phase 5 — Tests, docs

- `selftest.c`/new `src/selftest_biomes.c` (register group in `selftest_run`, add to `CMakeLists.txt` source list like `selftest_trees.c`): biome files parse; all block refs resolve; kernel 0 golden digests (Phase 0) unchanged; kernel 1 determinism (same seed ⇒ same columns across two runs and across worker thread counts); coverage histogram bounds; every plant `on:` block exists; surface layers never emit air.
- Run: `./build/dfe --selftest`, `./build/dfe --headless --test-forever-worlds`, `--dump-biomes`, `--tree-bench`, plus an ASAN build (`build-asan/` exists).
- Docs: `docs/MODDING.md` (biome files, new block/flora notes; fix the stale "planned" line ~815 and ~296), `docs/FOREVER_WORLDS.md` (kernel field, epoch 1), `docs/TREES.md` (new species), `CHANGELOG.md`, a short `tools/README.md` entry for `forge_import.py`/`check_tileable.py`.

## Order and checkpoints

0 → 1 → 2 → 3 → 4 → 5. Phases 1–3 are data/tools only and low risk; Phase 4 is the real C change. After each phase: build, `--selftest`, golden test green. Stop and report if the golden test fails: that means epoch 0 changed.

## Out of scope (state in final report)

Mesher/fluid-height water, LOD river snapping, shader crossfade between water frames, biome-specific tint colours (tint enum is capped at 4), 2-block-tall plants, new tree shape algorithms.

## Verification (end to end)

1. `python ~/.claude/skills/voxel-asset-forge/scripts/forge.py build --spec tools/forge/terrain.voxspec --out <scratchpad>/forge --size 16 --seed 1337 --qa --atlas --report` exits 0.
2. `python tools/check_tileable.py mods/base/assets/base/textures/block/water.png` passes; old water.png fails.
3. `cmake --build build && ./build/dfe --selftest` all groups pass; golden epoch-0 digests unchanged.
4. `./build/dfe --headless --test-forever-worlds` passes.
5. `./build/dfe --dump-biomes --seed 1` histogram sane; `--dump-trees` lists new species.
6. Manual: launch a new world (epoch 1), fly through new biomes: confirm varied logs/leaves, flora, biome-specific ground, tiled water without visible grid lines; load an old save and confirm terrain unchanged at its generated edge.
