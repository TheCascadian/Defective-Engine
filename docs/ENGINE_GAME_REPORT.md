# Defective Engine: Data and Runtime Report

## At a glance

Defective Engine is a C17/OpenGL 3.3 voxel engine. The shipped game is the `base` mod: engine code supplies simulation, storage, streaming, rendering and APIs; data, assets and scripts supply most game content.

## Data structure

- **Mods and VFS:** Each mod has `mod.json`, namespaced `data/`, `assets/`, optional Lua entry script and optional native plugin. Dependencies produce a deterministic load order; later mods override earlier files. `engine_assets` is mounted as `dfe`.
- **Registries:** JSON is loaded into namespaced registries for blocks, items, recipes, loot, tags, entities, world generation, atmosphere, presets, fog, shadows and godrays. Blocks receive numeric runtime IDs and can expand into property-based states (`namespace:block[prop=value]`); saves use names, not transient IDs.
- **Block data:** A block describes shape, render layer, textures, tint, collision/replaceability, friction, hardness/tool/drop, sound, light opacity/emission, properties and fluid behavior. Textures are stitched into a GL texture array.
- **World:** Space is divided into 32×32×32 chunks, grouped into X/Z columns over a generated vertical band. A chunk stores block states using uniform, palette-compressed (1/2/4/8-bit) or direct 16-bit storage, plus uniform or per-voxel packed sky/RGB light. Flags track generation, dirtiness, pending work, persistence and meshes.
- **Runtime content:** The player stores position/velocity/view, health, movement/environment state and mod-owned state. Entities use registered type data (size, color, speed, wandering and lifetime), with bounded handles. Inventory/equipment use named items plus metadata, counts and durability. Lua/native mods use namespaced JSON storage in the world save.
- **Persistence:** `saves/<world>/` stores metadata (seed, player, time, inventory, equipment), a block-name table, mod storage, and compressed region files containing serialized columns/chunks. LZ4 compression and asynchronous writes avoid long frame stalls; dirty columns autosave and all remaining data saves on exit.

## How the engine works

1. **Boot:** Parse options, locate assets/mods, discover and resolve mods, mount the layered VFS, create the window/GL context, start workers, load registries/textures/shaders, load scripts and optionally native plugins.
2. **World start:** Open/create a save (interactive play), choose its seed, initialize deterministic generation, entities, atmosphere, server/game systems and inventory, then load the player and initial terrain.
3. **Generation:** Per-coordinate seeded noise/hash functions produce height, biomes, caves, ores, fluids, vegetation and structures. Generation workers fill column state/light buffers. A deterministic seed means the same seed and content reproduce the same world.
4. **Streaming:** Each frame, `world_stream` prioritizes columns around the camera, loads saved columns or schedules generation, schedules lighting and meshing, uploads completed mesh results within a time budget, and unloads distant data. Distant terrain uses lower-resolution analytic LOD tiles.
5. **Simulation:** Input drives player collision, gravity, jumping, crouching, sprinting, swimming, lava/fall damage and creative flight. Entities update continuously. A fixed 20 Hz game tick fires mod events and runs server-side systems such as fluids/random ticks; menus pause world time and gameplay commands.
6. **Editing/events:** Ray interaction breaks, places or picks blocks. Edits update palette storage, lighting, neighboring mesh dirtiness, inventory and save dirtiness. The event bus lets scripts/plugins observe or cancel relevant actions and register commands.
7. **Rendering:** Visible chunk meshes are frustum/visibility culled and drawn in opaque, cutout and translucent passes. The scene also renders shadows, sky/atmosphere, water, entities, rain/godrays, post-processing, HUD/UI and debug overlays. Dynamic resolution can adjust render scale to a frame-time target.
8. **Frame services:** Worker completions are pumped under a budget, autosave runs periodically, hot reload can refresh shaders and selected data in development, and benchmark mode uses deterministic camera/time and never touches a save.

## Extension model

1. **Data/assets:** Safest tier; add or override content with JSON, PNG and shader files.
2. **Lua:** Sandboxed commands, events, world edits, inventory, containers, entities and mod storage; handlers have bounded execution and failing handlers are disabled.
3. **Native C plugins:** Stable API in `include/dfe/dfe_api.h` for logging, block/world access, events, commands, entities and storage. Unsandboxed and loaded only with `--allow-native`.

## Main boundaries

`main.c` owns lifecycle and the frame loop; `registry.c`/`content.c` own definitions; `gen.c` generates terrain; `world.c` streams and stores chunks; `light.c` propagates light; `mesher.c` builds geometry; `scene.c`/`render.c` draw; `player.c`, `entity.c`, `server.c` simulate; `save.c` persists; `mods.c` and `script.c` expose mod behavior; `menu.c`, `hud.c` and `ui_widgets.c` provide the game interface.

## Important constraints

Block/state IDs are frozen after content load, so block data/textures/scripts require a restart; development reload is intended for shaders, atmosphere, quality files and entity types. Native plugins are fully trusted. The engine is primarily single-main-thread for world state, with worker jobs restricted to private generation, lighting, meshing, far terrain and save work.
