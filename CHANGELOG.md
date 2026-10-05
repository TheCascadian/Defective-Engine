# Changelog

All notable changes to the engine and the base game, built from the git history. There are no tagged releases yet, so
everything is listed under one heading, newest work first, grouped by area. Short hashes point at the commits.

## Unreleased

### PBR block shading
- Shader-only surface depth for opaque cube blocks: optional `<name>_n.png` (normal) and `<name>_r.png` (roughness, height) companions and a per-block `"pbr"` key (`normal`, `roughness`, `metalness`, `bump_strength`). They are loaded into two texture arrays beside the colour array. The shading is a normal map, a screen-space height bump, sky-from-above ambient, one Blinn-Phong lobe with Schlick Fresnel (shared with the water Fresnel), and at most 4 shadow taps on mapped pixels. See "PBR maps" in docs/MODDING.md and `examples/mods/pbr_stone`.
- Base maps for stone, deep stone, cobblestone, limestone, planks, gravel and the ores. Generate them with `tools/gen_base_assets.py --pbr-only`.
- Rulings: this is additive, so the mod API stays 1 (`u_anim` grows from RG8 to RGBA8 and `.rg` is unchanged). PBR runs only in the near, non-LOD opaque pass. The tangent frame is derived from the face normal in `chunk.frag`, with no new varyings: varyings cost every opaque pixel about 17% in measurement, and this way `chunk.vert` is unchanged. JSON `roughness` overrides the `_r` red channel, and height comes only from the map. The first block with `"pbr"` that uses a texture owns that texture's settings. The PBR result is blended back to the flat result between 16 and 32 blocks from the camera.

### Terrain overhaul
- Data-driven biomes (`data/<ns>/biomes/*.json`, schema `sdk/schemas/biome.schema.json`): climate boxes, surface layers with slope, river, height, snowline and noise-patch rules, plants and per-biome features. 24 base biomes. Active only under Forever Worlds epoch 1 (`"kernel": 1` in `biomes.json`); epoch 0 is byte-identical, guarded by the golden digest test.
- 11 new tree species, 50 new block textures and 42 new blocks (terracotta, limestone, tuff, peat, flora, per-species logs and leaves), a seamless water texture, `chance_per_mille` for features.
- Added `--dump-biomes`, `tools/forge_import.py`, `tools/check_tileable.py`, the `biomes` self-test group and `DFE_DIAG_FW=1` for the tree diagnostics.

### Trees
- Replaced the single tree shape with data-driven species (`data/<ns>/trees/*.json`, schema in `sdk/schemas/tree.schema.json`), 11 base species, 12 shape types, chunk-independent placement from biome, climate, substrate, elevation, slope and hydrology, understory and deadwood. See [docs/TREES.md](docs/TREES.md).
- Added `--dump-trees`, `--dump-tree-shape`, `--tree-bench` and the `trees` self-test group.

### Forever Worlds (experimental)
- Added the Experimental Features menu and Forever Worlds: versioned generation epochs with a hashed registry, per-column
  chunk generation metadata, progressive blending of terrain, biomes, caves and structures across epoch borders, seam
  carving and a structure policy. Off by default and bit-identical to the old generator when off. See
  [docs/FOREVER_WORLDS.md](docs/FOREVER_WORLDS.md).
- Added `--headless --test-forever-worlds`, `--forever-bench` and `tools/forever_perf.py`.

### Menus and UI
- Redesigned the title, world select, create, edit, delete, options and pause screens in Minecraft's visual language:
  bevelled grey buttons, a tiled dirt backdrop, a stone-block logo and shadowed text, with an integer GUI scale taken
  from the window size. (d0b99f7)
- Worlds can be renamed or deleted from the world list, which also shows each world's seed and last-played date. Delete
  asks for confirmation and never follows symlinks. (d0b99f7)
- Fixed the Quit button on the title screen, which did nothing because `window_poll` reset the close flag every frame.
  (d0b99f7)
- Switched the UI font to Monocraft (SIL OFL, license included). Menu text now draws one drop shadow instead of two.
  (d0b99f7)
- Added the title screen with the world list, the pause menu, the settings screen and data-driven entities. (1867b8e)
- Added the HUD together with the player, inventory and interaction. (1627edb)

### Gameplay
- Wired player damage, death, teleport and the HUD. (2c605e2)
- Added the survival core with direct ground controls. (d196236)
- Applied snow friction. (09fc322)
- Tightened braking on foot, made movement grounded and crouch-aware, and fixed fixed-step time loss. (cff71e7, b59c004,
  4723d5b)
- Raised the step height to 1.0 and made the player snap down stairs while grounded. (c7036b9)
- Added the player, inventory, interaction, server queue and fluids. (1627edb)

### World generation
- Biomes, ores, features and structures can be defined by mod data, with the built-in defaults kept as the fallback.
  (c7036b9)
- Added ores, plants and trees. (1769fda)
- Added the data-driven terrain generator and a rolling-hills noise layer. (2d0412c, 1f4ddb9)
- Warn when the spawn search finds no land, and test spawn under a raised sea level. (340567c)

### Modding
- Mods can persist namespaced JSON under the active world save through `dfe.storage` and the native `mod_storage_*`
  calls. (c7036b9)
- Each mod gets its own string, table and math libraries in the Lua sandbox. (ebe7fd7)
- Added the mod loader, sandboxed Lua, the event bus, the console and load-error screens. (469a7e4)
- Mods can select block states, gate emission by state and route edits through events. (69a6a12)
- Added the versioned native plugin API header and the mod loader interfaces. (1dc9fed)
- Added example mods, the modding guide and an end-to-end example test. (a5ee4ba)
- Documented fluid keys, random_tick, game modes and generation roles, then presets, entities, shader packs, hot reload
  and performance testing, then movement and survival controls. (dd763fe, 2f87963, 2c44665)

### Rendering
- Added the data-driven day cycle, sky, clouds, weather and fog. (715ec98)
- Added data-driven presets, saved settings, dynamic resolution and light shafts, which are marched at quarter
  resolution. (cb81e68, 16b45e3)
- Added hot reload for shaders, the atmosphere file and presets. (6eabcda)
- Added the warmgrade shader pack and run the post pass when a mod replaces `post.frag`. (740a872)
- Added the wireframe debug view (`--wireframe`, F4). (647887b)
- Added the greedy chunk mesher with baked AO, smooth light and connectivity, and the arena-based chunk renderer.
  (2aa3c5f, 60d9e48)
- Added multi-channel flood-fill light. (650f64a)
- Far terrain went from a heightmap to flat-topped columns and then to voxel LOD tiles. (512e808, 153527b, c08f45d)
- Fixed the ocean seam and water colour, and brightened the sea. Deep water is opaque, so lit near beds and dim far beds
  no longer show as rectangles. (da61c0e, ab607dd, 4870e7b)

### Saves and storage
- Added region-file saves with an LZ4 column format and a block name table. (ffa02af)
- Serialisation is capped at 16 columns per call, and `world_save_all` writes every edited column on exit. (85879b1)
- Reading a directory as a file is now refused. (109298d)

### Performance tooling
- Added GPU pass timers, JSON and CSV benchmark output and a preset matrix runner, now run inside one launch. Matrix
  cases keep the requested size and stop reporting process-wide figures. (4906bdc, 52aa8e2, 3c8c006)
- Added world and job pages to the debug overlay, a 24-bit depth buffer request and `--workers`. (7bfd34c, 841b4d0)

### Build and CI
- Added an optional AddressSanitizer and UBSan build (`DFE_ENABLE_SANITIZERS`), a GitHub Actions self-test workflow, and
  ignore rules for scratch worlds and local archives. (c7036b9)
- Corrected the `-fwrapv` comment: FastNoiseLite needs it, `hash3` does not. (a418136)
- Vendored the dependencies and added the CMake build. (fcb8e3c)
- Moved the engine shaders and font under `assets/dfe`. (df96187)

### Foundation
- Initial commit. (02dae6a)
- Added the base utilities, job system and layered virtual filesystem. (35fb2a4)
- Added the window, GL helpers, camera, 2D batch and debug overlay. (b0e9e10)
- Added the lenient JSON parser and writer. (ec69e18)
- Added the block registry, data loading and texture array stitching. (afa9a0a)
- Added palette-compressed chunk storage and the streaming world. (33f4613)
- Added the base mod block set and the procedural asset generator. (0d15724)
- Wired world, streaming and renderer into the run modes and extended the self-tests. (19fca4a)
