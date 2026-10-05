# Defective Engine Modding Guide

This guide documents everything a mod can do today. It is the reference for mod authors and the contract the engine keeps: anything described here as stable will not change within mod API 1.

Three tiers are available. Use the lowest tier that does the job.

| Tier | What you write | Sandbox | Use it for |
|------|----------------|---------|------------|
| 1. Data and assets | JSON, PNG and GLSL files | Not applicable, no code runs | Blocks, items, recipes, loot tables, tags, textures, entity types, quality presets, world settings and shader packs |
| 2. Lua scripts | `.lua` files | Yes, bounded memory and instructions | Commands, events, world edits, inventory and item operations, containers and entities |
| 3. Native plugins | A C shared library | No, full privileges | Work that needs native speed or an external library |

Contents

1. [Quick start](#quick-start)
2. [Mod folder layout](#mod-folder-layout)
3. [The manifest, mod.json](#the-manifest-modjson)
4. [Load order, dependencies and errors](#load-order-dependencies-and-errors)
5. [Tier 1: data and assets](#tier-1-data-and-assets)
6. [Tier 2: Lua scripting](#tier-2-lua-scripting)
7. [Events](#events)
8. [The console](#the-console)
9. [Tier 3: native plugins and the C API](#tier-3-native-plugins-and-the-c-api)
10. [The example mods](#the-example-mods)
11. [Developing with hot reload](#developing-with-hot-reload)
12. [Performance and budgets for mod authors](#performance-and-budgets-for-mod-authors)
13. [Limits and what is not available yet](#limits-and-what-is-not-available-yet)
14. [Command-line tools and schemas](#command-line-tools-and-schemas)

---

## Player movement model

* **Collision**: the player is an axis-aligned box (0.6 wide) moved against the voxel grid one axis at a time: the larger horizontal displacement first (exact ties: x), then the other horizontal axis, then vertical. Every overlap test shrinks the box by `PLAYER_COLLISION_EPSILON` (1 mm); a snap to a face leaves twice that gap, so resting contact never snags on a flush seam. Unloaded columns are solid. Chunk boundaries do not matter: queries go through the global block lookup.
* **Ground detection**: `on_ground` is set when the vertical move hits a floor, which tests the whole box footprint. Friction uses `player_ground_probe`, which looks `PLAYER_GROUND_PROBE_DISTANCE` (5 cm) below the feet at every cell under the footprint and averages their friction.
* **Auto-step**: only from the ground (not flying or airborne), and unless auto-jump is off. The box is lifted `PLAYER_STEP_HEIGHT` (one block), the blocked move is repeated, and it settles back down with the normal collision. Walls two blocks high, ceilings and a blocked head all refuse the step. Walking off a ledge of up to one block snaps down instead of going airborne; larger drops fall.
* **Known limitation**: a diagonal move exactly 45 degrees into an outer corner resolves x before z.

## Quick start

A mod is a folder inside the game's `mods` folder. The smallest useful mod is a manifest and one block.

1. Create `mods/hello/mod.json`:

   ```json
   {
     "id": "hello",
     "name": "Hello Block",
     "version": "1.0.0",
     "api": 1,
     "depends": ["base>=1.0"]
   }
   ```

2. Create `mods/hello/data/hello/blocks/marble.json`:

   ```json
   {
     "textures": { "all": "hello:block/marble" },
     "hardness": 1.5
   }
   ```

3. Put a PNG at `mods/hello/assets/hello/textures/block/marble.png`. Any square size works; 16 x 16 matches the base game.

4. Start the game, press the grave key (`` ` ``) to open the console, and run `setblock 0 100 0 hello:marble`.

If something is wrong the game shows a screen that names the mod, the file, the line and what to change. Nothing fails silently.

To add behaviour, add `"script": "scripts/main.lua"` to the manifest and create that file:

```lua
dfe.command("hello", "say hello", function(args)
    dfe.console("hello from " .. dfe.mod .. ", you typed: " .. args)
end)
```

---

## Mod folder layout

```
mods/
  mods.json                  optional: list of disabled mods
  base/                      the base game, itself an ordinary mod
  mymod/
    mod.json                 required manifest
    data/<namespace>/blocks/<name>.json       block definitions
    data/<namespace>/items/<name>.json        item definitions
    data/<namespace>/tags/<registry>/<name>.json  registry tags
    data/<namespace>/recipes/<name>.json      crafting and processing recipes
    data/<namespace>/loot_tables/<name>.json  loot tables
    data/<namespace>/worldgen/default.json    world generation settings
    data/<namespace>/atmosphere/default.json  sky colours, day length, clouds and weather
    data/<namespace>/presets/<id>.json        quality presets
    data/<namespace>/shadows/<id>.json        shadow quality levels
    data/<namespace>/godrays/<id>.json        light-shaft quality levels
    data/<namespace>/fog/<id>.json            near-plane fog quality levels
    data/<namespace>/entities/<id>.json       entity types
    assets/dfe/shaders/<name>.vert|.frag      shader pack: replaces an engine shader
    assets/<namespace>/textures/block/<name>.png
    assets/<namespace>/textures/block/<name>.json   optional animation settings
    scripts/main.lua         entry script named by "script"
    scripts/<module>.lua     modules loaded with require
    plugins/                 native plugin libraries named by "plugin"
```

A namespace is the first part of every id: in `base:stone`, `base` is the namespace and `stone` is the name. The namespace is the folder name under `data/` and `assets/`. A mod normally uses its own id as its namespace. A mod may also write into another namespace, which is how it replaces content (see [Replacing content](#replacing-content)).

---

## The manifest, mod.json

```json
{
  "id": "mymod",
  "name": "My Mod",
  "version": "1.2.0",
  "api": 1,
  "depends": ["base>=1.0", "?othermod"],
  "load_after": ["somemod"],
  "script": "scripts/main.lua",
  "plugin": { "linux": "plugins/libmymod.so", "windows": "plugins/mymod.dll" }
}
```

| Key | Required | Meaning |
|-----|----------|---------|
| `id` | yes | 2 to 31 characters: lowercase letters, digits and underscores, starting with a letter. Unique across installed mods. |
| `version` | yes | Three numbers, `major.minor.patch`. |
| `name` | no | Display name. Defaults to the id. |
| `api` | no | The mod API major version the mod was written for. Defaults to the current one. A mod that targets a different major version is refused with a message. The current version is 1. |
| `schema` | no | Manifest schema version. Defaults to 1; other versions are refused. |
| `depends` | no | Array of dependency strings, at most 16. See below. |
| `load_after` | no | Array of mod ids, at most 8. Load after these if they are installed, without requiring them. |
| `script` | no | Path of the entry Lua script, relative to the mod folder. |
| `plugin` | no | Native library per platform, relative to the mod folder. Loaded only with `--allow-native`. |

Paths in `script` and `plugin` must stay inside the mod folder: no leading slash, no `..`, no drive letters.

### Dependency strings

| Form | Meaning |
|------|---------|
| `"base"` | `base` must be installed, any version. |
| `"base>=1.2"` | Version 1.2.0 or newer. |
| `"base=1.0.0"` | Exactly this version. |
| `"base^1.2"` | Same major version and at least 1.2. |
| `"?gems"` | Optional. If `gems` is installed and active this mod loads after it, otherwise nothing happens. The same operators work after the id, for example `"?gems>=1.1"`. |

A script can test whether an optional dependency is present, for example `if dfe.block_state("gems:ruby_block") then ... end`.

### Disabling mods

Create `mods/mods.json`:

```json
{ "disabled": ["guard", "builder"] }
```

Disabled mods are skipped and so is anything that requires them.

---

## Load order, dependencies and errors

The load order is deterministic: the same installed mods always load in the same order, on every machine.

1. `base` loads first.
2. Every mod loads after the mods it depends on and after the mods named in `load_after`.
3. When the rules above leave a choice, mods are ordered by id.

Later mods win. If two mods provide the same file, the one later in the load order is used. This is the whole override rule and it applies to every kind of file (see [Replacing content](#replacing-content)). Scripts are also run in load order, so a later mod's event handlers run after an earlier mod's.

### What happens on a problem

| Problem | Result |
|---------|--------|
| Missing or too old dependency | The mod is skipped, and so is every mod that requires it. A message names the mod and the dependency. The game continues if `base` loaded. |
| Dependency cycle | The mods in the cycle are skipped and named in a message. |
| Invalid manifest | The mod is skipped. The message gives the line and the fix. |
| Invalid block or world file | The game does not start. A screen lists each problem as `[mod id] file:line: what is wrong and how to fix it`. |
| Script syntax or runtime error while loading | The script is reported with `file:line`. Other mods still load. |
| Script error inside an event handler | Reported once, then that handler is switched off. The game continues. |

The same text goes to the in-game console and the log. Without a window, messages go to standard error.

---

## Tier 1: data and assets

### Blocks

A block is one JSON file at `data/<namespace>/blocks/<name>.json`. Its id is `<namespace>:<name>`. Every key is optional except `textures` (blocks with a visible shape).

```json
{
  "shape": "cube",
  "layer": "opaque",
  "textures": { "up": "base:block/grass_top", "down": "base:block/dirt", "side": "base:block/grass_side" },
  "tint": "grass",
  "tint_faces": ["up"],
  "solid": true,
  "replaceable": false,
  "wind": false,
  "random_tick": false,
  "climbable": false,
  "item": true,
  "friction": 1.0,
  "hardness": 0.6,
  "tool": "shovel",
  "drops": "base:dirt",
  "sound": "grass",
  "light": { "opacity": 15, "emit": [0, 0, 0], "emit_when": { "lit": "on" } },
  "properties": { "lit": ["off", "on"] },
  "defaults": { "lit": "off" },
  "fluid": { "viscosity": 5, "group": "water" }
}
```

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `shape` | string | `cube` | `cube`, `cross` (two crossed quads, for plants), `fluid`, `none` (invisible) or `model` (reserved). |
| `layer` | string | `opaque` | How the block is drawn. `opaque`, `cutout` (each pixel fully visible or fully transparent, for leaves and plants) or `translucent` (blended, for glass and water). |
| `textures` | object | none | Face to texture id. Faces are `east`, `west`, `up`, `down`, `south`, `north`. Shortcuts: `all` for every face, `side` for the four sides, `top` and `bottom` for `up` and `down`. The most specific key wins. A face with no texture is an error. |
| `tint` | string | `none` | Colour from the biome or world: `none`, `grass`, `foliage` or `water`. |
| `tint_faces` | array | all faces | Faces that receive the tint, for example only `up` for grass. |
| `solid` | bool | true for `cube` | Whether the player collides with it. |
| `replaceable` | bool | true for `cross` and `fluid` | Whether placing a block may overwrite it. |
| `wind` | bool | false | Sways in the wind (leaves, grass). |
| `random_tick` | bool | false | Receives random ticks (used by game systems that grow or spread blocks). |
| `climbable` | bool | false | The player can climb it. |
| `item` | bool | true | Whether the block has an inventory item. |
| `friction` | number | 1.0 | Ground movement speed multiplier, clamped to 0.05..1.1 (so `base:ice` at 0.2 is honoured). Lower values slow ground movement; they do not add coasting. When the player's box stands on several blocks, the mean friction of the solid cells under the footprint is used. |
| `hardness` | number | 1.0 | Seconds to break by hand. Negative means unbreakable. |
| `tool` | string | none | Preferred tool, for example `pick`, `axe` or `shovel`. |
| `drops` | string | the block itself | Block id dropped when broken. |
| `sound` | string | `stone` | Sound set name. |
| `light.opacity` | 0 to 15 | 15 for opaque cubes, 2 for fluids, else 0 | How much light the block absorbs per block travelled. 15 stops light completely. |
| `light.emit` | `[r, g, b]` | `[0, 0, 0]` | Coloured light the block gives off, each channel 0 to 15. |
| `light.emit_when` | object | none | One property and value, such as `{"lit": "on"}`. The block emits only in states where the property has that value. The property must be declared under `properties`. |
| `properties` | object | none | State properties. Each key maps to an array of 1 to 16 string values. At most 4 properties per block. |
| `defaults` | object | first value of each | Property values of the default state. |
| `fluid` | object | none | Makes the block a fluid. `viscosity` is ticks per spread step, `group` names the fluid family, `reach` (1 to 14, default 7) is how many blocks it flows from a source, and `infinite` (default false) lets two adjacent sources create a third. A fluid must declare a `level` property with exactly `reach + 2` values (`"0"` is a source, `"1"` to `"reach"` flow, the last is falling). The error message prints the exact array to use. |

Mistakes that name a bad value, such as an unknown `shape` or `tint`, are reported with the file and line and list the valid choices.

`hardness` (seconds to break in survival, negative for unbreakable) and `drops` (the block name given when broken, `""` for nothing) are used by the player. `random_tick: true` makes the block fire the `random_tick` event. `climbable` lets the player climb it. `friction` scales walking and sprinting speed while grounded; `tool` and `sound` are stored for later gameplay systems.

### Block states and properties

A block with properties has one state for every combination of values. A state is written as text:

```
mymod:lamp[lit=on]
mymod:door[half=upper,open=false]
```

Properties may be given in any order, and any property left out takes its default. This text is accepted everywhere a block name is: the `setblock` console command, `dfe.block_state` and `dfe.set_block` in Lua, and `block_state` in the C API. Unknown blocks, properties and values are rejected instead of silently falling back to a default. `dfe.state_name` and `state_string` turn a state number back into text.

Properties currently change gameplay data, not the model: all states of a block share one texture and shape. Light emission is the one thing a property can change per state, through `light.emit_when`.

### Textures

A texture id such as `mymod:block/ruby` names the file `assets/mymod/textures/block/ruby.png`.

* Any square size is allowed. The game uses the largest size found, at least 16 and at most 128 pixels, and scales other textures to it without blurring.
* A texture that is taller than it is wide is an animation: frames are stacked vertically and each frame is as tall as the texture is wide. Add a file next to it with the same name and a `.json` extension to set the speed: `{"fps": 3}`.
* A texture with fully transparent pixels should be used with `"layer": "cutout"`. A texture that is uniformly semi-transparent, like water, should be used with `"layer": "translucent"`.

### PBR maps

Opaque cube blocks can carry surface relief and a specular highlight. It is all done in the terrain shader from two extra texture arrays: no extra geometry, no extra draw pass. A texture without PBR maps or a `"pbr"` key renders exactly as before.

**Companion files.** Next to `<name>.png`, the engine looks for two optional files with the same size and frame layout (a single frame is reused for every frame of an animation):

| File | Channels | Missing means |
|------|----------|---------------|
| `<name>_n.png` | Tangent-space normal, OpenGL convention: red = right in the image (+u), green = up in the image, blue = out of the face. Flat is `(128, 128, 255)`. | flat normal |
| `<name>_r.png` | Red: roughness (0 mirror, 255 matte). Green: height (0 deep, 255 raised, 128 neutral). Blue is ignored. | roughness 0.8, height 0.5 |

Finding either file turns PBR on for that texture.

**Block key.** `"pbr"` is an optional object in a block file. Every key in it is optional:

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `normal` | texture id | `<texture>_n` | Normal map to use for every face texture of this block instead of the companion. A missing file is a data error. |
| `roughness` | number, 0 to 1 | from `_r`, else 0.8 | When given, replaces the `_r` map's roughness channel. Height still comes from the map. |
| `metalness` | number, 0 to 1 | 0 | Tints the highlight by the albedo and darkens the diffuse part. |
| `bump_strength` | number, 0 to 2 | 1 | Scales the height relief (screen-space bump from the `_r` green channel). |

The `"pbr"` key also turns PBR on when there are no companion files. Settings belong to a texture, not a block: when two blocks with `"pbr"` share a texture, the one loaded first wins.

**Limits.** PBR applies only in the near opaque pass on cube faces. Cutout blocks (leaves, plants), translucent blocks (water, glass) and the far terrain keep the plain model. Relief fades out between 16 and 32 blocks from the camera, and normal-mapped pixels take at most 4 shadow samples.

**Worked example** (`examples/mods/pbr_stone`):

```
pbr_stone/
  mod.json
  assets/pbr_stone/textures/block/polished_stone.png
  assets/pbr_stone/textures/block/polished_stone_n.png
  assets/pbr_stone/textures/block/polished_stone_r.png
  data/pbr_stone/blocks/polished_stone.json
```

```json
{
  "textures": {"all": "pbr_stone:block/polished_stone"},
  "hardness": 1.5,
  "tool": "pick",
  "sound": "stone",
  "pbr": {"normal": "pbr_stone:block/polished_stone_n", "roughness": 0.35, "metalness": 0.0, "bump_strength": 0.6}
}
```

Here `"normal"` names the file the companion rule would find anyway; it is spelled out to show the key. `"roughness": 0.35` makes the polished stone shinier than its `_r` map says. `tools/gen_base_assets.py` has `make_normal` and `make_roughness_height`, which derive both maps from a colour PNG (Sobel on blurred luminance, deterministic). `python3 tools/gen_base_assets.py --pbr-only` rebuilds the base game's maps alone.

**Tuning.** `data/<namespace>/pbr.json` (the engine ships `data/dfe/pbr.json`; the last mod to supply the file wins, so a pack should copy and edit it whole) sets every global PBR knob. Every key is optional and clamped. It reloads with the shaders.

| Key | Range | Effect |
|---|---|---|
| `bump_strength` | 0 to 4 | Global multiplier on relief and on each block's `bump_strength`. |
| `height_depth` | 0 to 0.5 | Blocks of relief at full height range (default 0.0625, one texel). Sets how deep contact shadows and cavities read. |
| `normal_strength` | 0 to 4 | Tilt of the normal map. 0 is flat. |
| `fade_start`, `fade_end` | blocks | Relief fades out between these distances. |
| `specular_strength` | 0 to 8 | Brightness of the sun/moon highlight. |
| `roughness_scale`, `roughness_bias` | 0 to 4, -1 to 1 | Applied to every roughness value: `r * scale + bias`. |
| `metalness_scale` | 0 to 2 | Multiplier on block `metalness`. |
| `sky_specular` | 0 to 4 | Sky reflection, which also shows on faces turned from the sun. |
| `diffuse_response` | 0 to 3 | How strongly the normal map changes the diffuse light. 0 ignores it. |
| `sky_lean` | 0 to 3 | How much normals tipped toward the sky catch ambient light. |
| `cavity_ao` | 0 to 1 | Darkening of texels below the neutral height. |
| `shade_floor` | 0 to 1 | Minimum light on PBR surfaces, lifts deep shade. |
| `self_shadow_strength` | 0 to 1 | Darkness of contact shadows that raised texels cast on their neighbours. 0 is off. |
| `self_shadow_reach` | 0 to 8 | Contact shadow length, in multiples of the relief depth. |
| `self_shadow_steps` | 0 to 16 | Ray steps per pixel; 0 is off. More is smoother and slower. |
| `shadow_tap_cap` | 1 to 16 | Soft-shadow samples for PBR pixels. |

Players change the same values in Options > Graphics Settings > Shaders. **Texture Quality** picks Default (this file), Off, Low, Medium, High or Ultra; **Style** (Natural, Clean, Crisp, Realistic, Chunky) scales relief, parallax, bevels, outlines, highlights, contact shadows and colour grading on top of the quality level, saved as `texture_style`. Moving any slider switches to Custom (styles then no longer apply), saved in `settings.json` as `texture_quality` and `shaders`. The Shaders menu also holds the shadow, godray and fog options.

The shadow map is always looked up with the face normal, not the mapped one, so bump detail can no longer cast the block's own shadow onto its shaded side.

**For shader authors.** `chunk.frag` gets `u_tex_normal` (unit 4) and `u_tex_rh` (unit 5), which share `u_tex`'s layer indices, plus `u_bump_strength`. `u_anim` is now RGBA: `.rg` is unchanged, `.b` is 0 for a layer without PBR, otherwise 1 + bump_strength × 127, and `.a` is metalness. A pack that does not declare the new uniforms still works. `shadow.glsl` adds `shadow_visibility_capped(rel, normal, light_dir, max_taps)` and defines `DFE_SHADOW_TAP_CAP`. `chunk.frag` calls it only when that macro is defined, so an older `shadow.glsl` override still compiles.

### World generation settings

`data/<namespace>/worldgen/default.json` sets the sea level, the level where deep stone begins and the blocks terrain is built from. The base game's file is the complete list of keys; copy it and change values:

Epochs, the experimental feature registry and the structure `forever_worlds_policy` key are described in [FOREVER_WORLDS.md](FOREVER_WORLDS.md).

Rivers, lakes and the sea come from one region-scale drainage solve, described in full in [HYDROLOGY.md](HYDROLOGY.md): the query contract, the `"hydrology"` parameter object and its clamps, the `"habitat"` and `"water_distance"` keys for features, structures, ores and entities, and the `--dump-*` diagnostics.

```json
{
  "sea_level": 62,
  "deep_level": 0,
  "blocks": {
    "stone": "base:stone", "deep_stone": "base:deep_stone", "dirt": "base:dirt",
    "grass": "base:grass_block", "sand": "base:sand", "sandstone": "base:sandstone",
    "gravel": "base:gravel", "snow": "base:snow", "mud": "base:mud", "water": "base:water",
    "log": "base:log", "leaves": "base:leaves", "tall_grass": "base:tall_grass",
    "flower_red": "base:flower_red", "flower_yellow": "base:flower_yellow",
    "mushroom": "base:mushroom", "dead_bush": "base:dead_bush",
    "coal_ore": "base:coal_ore", "iron_ore": "base:iron_ore",
    "gold_ore": "base:gold_ore", "diamond_ore": "base:diamond_ore"
  }
}
```

The roles from `stone` to `water` are required. The decoration roles from `log` onward are optional; leaving one out removes that feature. Cave shapes are built into the generator. Biomes are data under the Forever Worlds kernel 1 (see [Biome files](#biome-files)); the original biome layout stays in place for epoch 0. Trees are data: see [TREES.md](TREES.md). A Lua generator hook arrives in a later milestone.

### Biome files

`data/<namespace>/biomes/<name>.json` defines one biome (schema: `sdk/schemas/biome.schema.json`). They are read only when Forever Worlds is on and the current epoch's `biomes.json` has `"kernel": 1` (epoch 1 of the engine assets does). Epoch 0 and worlds without Forever Worlds keep the original generator, bit for bit.

The base mod defines the eight original biomes (`ocean`, `beach`, `desert`, `tundra`, `swamp`, `forest`, `plains`, `mountain`; all are required, and they must not carry `features`, `ores` or `structures`) and 16 more. At most 32 biomes can exist. The table order is the eight originals, then the rest sorted by id.

```json
{
  "id": "base:steppe",
  "role": "land",
  "priority": 2,
  "climate": { "temperature": {"center": 0.3, "extent": 0.35}, "humidity": {"center": -0.55, "extent": 0.35}, "height": [0, 110], "weirdness": [-1, 1] },
  "surface": [
    { "block": "base:clay", "when": { "river_min": 0.55, "patch": {"noise": "b", "lo": 0.3, "hi": 2} } },
    { "block": "base:dry_grass_block", "when": { "patch": {"noise": "a", "lo": -2, "hi": 0.3} } },
    { "block": "base:grass_block" },
    { "block": "base:dirt", "depth": 3 }
  ],
  "plants": [ { "block": "base:dry_grass", "chance": 22, "on": ["base:dry_grass_block", "base:grass_block"] } ],
  "tree_density_scale": 0.15,
  "features": [ { "id": "base:steppe_boulder", "block": "base:andesite", "chance_per_mille": 2, "radius": 1 } ]
}
```

* `role`: `land` (default), `ocean` (chosen wherever the ground is below sea level) or `shore` (the waterline band).
* `climate`: a biome is picked by the nearest climate centre (temperature and humidity, squared distance) among land biomes whose `height` and `weirdness` ranges contain the column. `priority` subtracts `priority * 0.02` from the distance, so higher priority wins close calls; variants such as `flower_forest` and `volcanic_plain` sit on the same climate as a common biome and take over where weirdness is high.
* `surface`: layers with `depth` 0 are the top block; the first whose `when` matches wins and the last must have no `when`. Layers with `depth` n > 0 are subsurface tiers down to n blocks below the surface; entries with the same depth are alternatives; depths do not decrease. A layer cannot be air.
* `when` keys: `slope_min`, `slope_max` (slope >= min and < max), `river_min`, `near_river`, `water_dist_max`, `height_min`, `height_max`, `above_snowline`, `above_treeline`, `patch` (one object or up to three, noise `a` broad, `b` fine or `detail`, matches `lo <= v < hi`) and `stripe` (`period`, `lo`, `hi`: y mod period in [lo, hi), for banded terrain).
* `plants`: one roll per column picks at most one plant. `chance` is a percent in steps of 0.1. The entry counts only if the ground block is in `on`, `habitat` (`dry`, `any`, `river`, `lake`, `shore`, `wetland`) and `water_dist` allow it, and the optional `density_noise` is in range.
* `tree_density_scale` multiplies tree site density. The `treeline` tag makes the biome follow the alpine tree rules. Trees pick biomes by id in their species file (`"biomes"`).
* `features`, `ores`, `structures`: same format as in `worldgen`, limited to this biome. Features also accept `chance_per_mille` (0..1000) for finer odds than whole percent. The engine allows 64 features in all.

Check coverage with `--dump-biomes SEED [N] [STEP] [KERNEL]`: it prints each biome's share and mean height and warns about land biomes under 1% or over 35% of columns.

New blocks in the base mod for this: terracotta (red, orange, white), red sandstone, limestone, tuff, permafrost, peat, mossy cobblestone (cold, warm), tree logs and leaves per species, and flora (cornflower, daisy, pink tulip, lavender, orange poppy, bluebell, fern, dry grass, meadow grass, berry bush, azalea bush, cattail, heather, snow shrub). Plants are single-block cross shapes.

### Atmosphere

`data/<namespace>/atmosphere/default.json` describes the day. The engine interpolates the colour keys and derives the sun, moon, stars, clouds, fog and light colour from them. The base game's file is the complete example.

```json
{
  "day_length": 1200,
  "start_phase": 0.30,
  "clouds": { "altitude": 172, "scale": 0.0035, "speed": 1.6 },
  "weather": { "min_seconds": 300, "max_seconds": 900, "rain_share": 0.4 },
  "keys": [
    { "time": 0.50, "zenith": [0.22, 0.47, 0.86], "horizon": [0.66, 0.80, 0.96], "sky_light": [1, 1, 1], "ambient": 0.05 }
  ]
}
```

| Key | Meaning |
|-----|---------|
| `day_length` | Seconds of game time per day, at least 30. |
| `start_phase` | Phase of a new world at game time 0. Phase 0 is midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset. |
| `clouds` | Cloud layer height in blocks, noise scale (smaller is larger clouds) and drift speed in blocks per second. Clouds are hidden when the camera is above the layer. |
| `weather` | Shortest and longest spell of one weather in seconds, and the chance that a change brings rain instead of overcast sky. |
| `keys` | Two to sixteen keys in increasing `time` order. `zenith` is the colour overhead, `horizon` the colour at the horizon and the distance fog, `sky_light` the colour that sunlit surfaces are multiplied by, `ambient` the minimum brightness. Colours are red, green, blue in 0 to 1. The last key blends into the first, so the cycle repeats. |

Weather darkens and greys these colours and pulls the fog in; a mod does not need to describe it. The console commands `time set` and `weather` change the state, and the options `--time` and `--weather` set it at start.

### Presets

`data/<namespace>/presets/<id>.json` defines a quality preset. The file name without `.json` is the id, and a later mod that provides the same file name replaces it. At most 16 presets are supported. The base game ships `low`, `medium` and `high`; a mod can add more, and they appear in the settings screen and in `--preset NAME`.

```json
{
  "name": "Low",
  "render_distance": 8,
  "far_chunks": 14,
  "clouds": true,
  "stars": true,
  "light_shafts": false,
  "fog": true,
  "fog_quality": "medium",
  "shadows": "low",
  "dynamic_resolution": true,
  "min_scale": 0.6,
  "target_fps": 60
}
```

| Key | Meaning |
|-----|---------|
| `name` | Text shown in the settings screen. Defaults to the id. |
| `render_distance` | Chunks of full detail, 2 to 32. |
| `far_chunks` | Extra chunks of coarse distant terrain beyond the render distance, 0 (off) to 64. |
| `clouds`, `stars` | Draw the cloud layer and the night stars. |
| `light_shafts` | A screen-space shaft effect toward the sun, marched at quarter resolution and applied in the resolve pass. It still needs the scene drawn offscreen, so the base game enables it only on High. |
| `godrays` | The id of a file under `data/<namespace>/godrays/` controlling light-shaft samples and strength. Empty follows `light_shafts`; an unknown id disables the effect. |
| `fog` | Enables near-plane fog that adds local haze and dust independently of the distant sky fog. |
| `fog_quality` | The id of a file under `data/<namespace>/fog/`; empty uses the default quality. |
| `shadows` | The id of a file under `data/<namespace>/shadows/`; empty disables cascaded sun shadows. |
| `dynamic_resolution` | Let the controller change the render scale to hold `target_fps`. |
| `min_scale` | The lowest render scale the controller may choose, 0.4 to 1. Below 0.4 the picture is too soft to play. |
| `target_fps` | The frame rate the controller tries to hold, 15 to 240. |

A preset with a mistake is reported on the error screen and ignored. The player's own choices in `settings.json` (render distance, dynamic resolution, render scale, field of view, vertical sync, shadows, godrays and fog) take precedence over the preset, and a value left at "preset" follows it. Quality level files contain data-driven renderer settings; the base files under `data/base/{shadows,godrays,fog}` are complete examples.

The Graphics Settings screen also exposes Shadow Distance. It defaults to the distance from the selected shadow quality file, or can override it with a bounded 64–512 block choice. The setting is saved as `shadow_distance` in `settings.json`; `0` means follow the preset/mod quality file. Shadow quality files remain the mod-facing way to add resolutions, cascade counts, filtering taps, softness, bias and default distances.

### Entities

`data/<namespace>/entities/<id>.json` defines an entity type. The type id is `<namespace>:<file name>`, so `data/mymod/entities/slime.json` is `mymod:slime`. The base game ships `base:hopper`, `base:chicken` (passive, drops feathers and raw chicken) and `base:pig` (drops raw porkchop).

```json
{
  "name": "Hopper",
  "size": [0.5, 0.6],
  "color": [0.82, 0.74, 0.62],
  "accent": [0.93, 0.88, 0.80],
  "speed": 0.45,
  "wander": true,
  "lifetime": 0
}
```

| Key | Meaning |
|-----|---------|
| `name` | Display name, shown by the `entities` command. |
| `size` | Collision box `[width, height]` in blocks, each 0.1 to 4. Default `[0.6, 0.9]`. |
| `color` | Body colour, red, green, blue in 0 to 1. Default grey. |
| `accent` | Head colour. Defaults to `color`. |
| `speed` | A multiplier of the player's walking speed, 0 to 4. Default 0.4. |
| `wander` | When true the entity walks in random directions, rests, hops over one-block steps, swims up out of water and turns away from walls. When false it stands still. Default true. |
| `lifetime` | Seconds until the entity removes itself, 0 for unlimited. |
| `health` | Hit points, 0 to 100000. 0, the default, means the entity cannot be damaged. |
| `behaviour` | `wander`, `static`, `hostile` or `passive`. Without it, `wander` follows the legacy flag. A hostile entity walks toward the player within `sight` blocks and hurts the player by `attack` on contact. A passive entity walks away from the player within `sight` blocks. |
| `attack` | Damage a hostile entity deals per hit. Default 1. |
| `sight` | Distance in blocks at which the player is noticed. Default 16. |
| `save` | When true the entity is written with the world. Default true. |
| `drops` | Array of `{"item": "ns:item", "min": 1, "max": 2}`, at most 4. Given to the player when the player kills the entity. |

An entity is drawn as a body box and a head box, lit by the world light at its position and fogged like terrain. Its physics is the player's: gravity, water, step-up and collision with blocks. Terrain that is not loaded counts as solid, so an entity beyond the streamed area waits rather than falling out of the world. There are at most 256 entities at once. Entities are spawned from Lua, from a native plugin, or with the console command `spawn`.

The player attacks an entity that has health by aiming at it within reach and pressing the left mouse button. A hit deals the held item's `damage` (at least 1; creative mode kills at once), knocks the entity back and has a 0.4 s cooldown. An entity in front of a block is hit instead of mining the block.

Entities collide with the world, with the player and with each other. Two `static` entities do not push each other. Entities marked `save` are written to `entities.json` in the world folder and restored when the world loads. A record whose type is no longer loaded is kept and written back unchanged, so removing a mod does not delete its entities. Saved entities stay resident while their column is unloaded; the `cx` and `cz` fields in the file only record the column for reference.

#### Entity rendering

Visible entities are culled by fog distance, by the camera frustum and by screen size, sorted nearest first, capped at `entity_max_drawn` and given a level of detail: body and head boxes, one merged box beyond `entity_lod1` blocks, or a camera-facing impostor beyond `entity_lod2` blocks. These three keys are preset keys (see the presets section). With instancing each level is one draw call, so 100 entities cost two or three draws instead of two hundred. The overlay and the benchmark JSON (`entities` object) report draws, instances, culled count, level counts and upload bytes. `--bench-entities N` spawns N entities in front of the benchmark camera, and `--entity-legacy` forces the one-draw-per-part path with no LOD for before and after comparison. Entities do not cast shadows. There is no occlusion culling.

#### Entity scripting

| Lua | Meaning |
|-----|---------|
| `dfe.entity.spawn(type, pos, opts)` | Returns the id, or `nil` and a reason. `pos` is `{x, y, z}` or `{x = , y = , z = }`. `opts` takes the field names of `set`. |
| `dfe.entity.despawn(id)` | Removes an entity. Fires `entity_despawn`, which can be cancelled. |
| `dfe.entity.get(id)` | A table with `type`, `x`, `y`, `z`, `vx`, `vy`, `vz`, `yaw`, `health`, `max_health`, `age`, `behaviour`, `data`, or `nil`. |
| `dfe.entity.set(id, fields)` | Changes the named fields: `x y z vx vy vz yaw health max_health behaviour data`. Health is applied last because it can kill. |
| `dfe.entity.damage(id, amount)` | True when health dropped. Honours the 0.5 s invulnerability timer and the `entity_damage` and `entity_death` events. |
| `dfe.entity.heal(id, amount)` | True when health rose. |
| `dfe.entity.iter()` | Iterator over a snapshot of ids; the loop body may spawn and remove. |
| `dfe.entity.near(pos, radius)` | Array of entity tables, nearest first, each with `distance`. |
| `dfe.entity.on(event, fn)` | Subscribes to `entity_spawn`, `entity_tick`, `entity_damage`, `entity_death` or `entity_despawn`. The event has `entity_id`, and `damage` for `entity_damage`. Return `true` to cancel. |

A cancelled `entity_death` leaves the entity alive with 1 health. Native plugins use the `entity_get`, `entity_set`, `entity_damage`, `entity_heal`, `entity_list` and `entity_near` function pointers added in API 1.3. Entity code runs on the main thread only. Limits: `model` is box-based only, there is no death animation, drops are given only for kills by the player, and entity `data` text is limited to 191 bytes.

A type with a mistake is reported and the previous definition, if any, stays in use, which makes live editing with `--dev` safe.

### Shader packs

Every shader the engine draws with is an ordinary asset, so a mod replaces one by providing a file at the same path. No manifest key is needed.

| File under `assets/dfe/shaders/` | What it draws |
|----------------------------------|---------------|
| `chunk.vert`, `chunk.frag` | All terrain, near and far. The fragment shader is also compiled with `PASS_CUTOUT`, `PASS_TRANSLUCENT` and `LOD` defined. |
| `post.vert`, `post.frag` | The resolve pass that scales the scene to the window. The engine skips it at full scale without shafts to save a full-screen copy, but it always runs while a mod replaces `post.frag`, so a grading shader is applied at every setting. The cost of that pass is paid by every player of the pack. The file is compiled three times: plain, with `SHAFTS` (the resolve, which reads the shaft mask) and with `SHAFT_MASK` (a quarter-resolution march toward the sun that writes one brightness value). A replacement that does not contain the text `SHAFT_MASK` is treated as not supporting light shafts, and they are switched off for it. To keep them, copy the engine's file and edit its final colour step. |
| `entity.vert`, `entity.frag` | Entities. |
| `sky.vert`, `sky.frag` | The sky, stars, sun, moon, clouds and the rain layer, selected by `PASS_SKY` and `PASS_RAIN`. Shaders can `#include` other files through the same virtual filesystem. |
| `shadow.glsl` | Shared shadow sampling code included by terrain and entity shaders. A mod can replace it to change shadow filtering without replacing the complete terrain shader. |
| `ui.vert`, `ui.frag`, `debug_line.vert`, `debug_line.frag` | Menus, the HUD and debug lines. |

Read the engine's file first and keep its `in`, `out` and `uniform` names: the engine sets uniforms by name, and a uniform a shader does not declare is simply skipped. A shader that fails to compile is reported with the compiler's message. At launch the game stops with that message; during a reload the previous program keeps running.

The `warmgrade` example replaces `post.frag` to add a warm tint, a saturation lift and a vignette. It is about 25 lines and is the recommended starting point. Only one mod can replace a given shader; the later one in load order wins. Shader packs run on the GPU of every player, so keep them cheap: the post pass runs for every pixel, and one extra texture fetch per pixel is already a measurable cost on integrated graphics.

### Dynamic resolution

The engine can draw the world at a fraction of the window size and stretch it. The controller is driven by measured GPU time, not frame time, because with vertical sync a fast GPU still reports 16.7 ms frames and fewer pixels cannot help a frame that is limited by the CPU. It changes the scale in steps of 0.05, drops quickly when the frame is over budget and rises slowly when there is headroom, and never goes below the preset's `min_scale`. Where timer queries are unavailable it uses the share of the frame spent waiting on the swap. A mod does not control this, but a preset sets its range and target.

### Replacing content

Because later mods win, replacing base content needs no special syntax: provide a file at the same path.

| To replace | Provide |
|------------|---------|
| The stone texture | `assets/base/textures/block/stone.png` |
| The stone block definition | `data/base/blocks/stone.json` |
| World generation settings | `data/base/worldgen/default.json` |
| The day cycle and weather | `data/base/atmosphere/default.json` |
| A quality preset | `data/base/presets/low.json` |
| Shadow, godray or fog quality | `data/base/{shadows,godrays,fog}/<id>.json` |
| The hopper | `data/base/entities/hopper.json` |
| The final image grade | `assets/dfe/shaders/post.frag` |

The `retexture`, `highsea` and `warmgrade` example mods do exactly this.

### Items, tags, recipes and loot tables

These registries are loaded from each mod's `data/<namespace>/` directory. The file name supplies the id suffix, for example `data/mymod/items/iron_hat.json` defines `mymod:iron_hat`.

An item definition can represent a stackable item, a placeable block item, or equipment:

```json
{
  "type": "armor",
  "slot": "head",
  "durability": 200,
  "protection": 2,
  "max_stack": 1,
  "tags": ["mymod:equipment"]
}
```

`place` names a block placed by the item. `max_stack` is clamped to 1..64; durable and equipped items stack to one. Equipment slots are `head`, `chest`, `legs` and `feet`. Items may also define `damage` and `protection` values.

Tag files live under `data/<namespace>/tags/<registry>/<name>.json`. For example, `data/mymod/tags/items/metal.json` can contain `{"values":["base:iron_ingot","#othermod:metal"]}`. Entries are namespaced ids or references to another tag prefixed with `#`; `replace: true` clears earlier contributions to the same tag.

Recipes live under `data/<namespace>/recipes/`. `type` may be `shaped`, `shapeless` or `processing`; results use an item id and optional count. Shaped recipes use up to three rows of three characters and a `key` map. Shapeless and processing recipes use an `ingredients` array; an ingredient may be an item id or `#tag`, with an optional count. Processing recipes may set `time` in seconds. Loot tables live under `data/<namespace>/loot_tables/`; each `pools` entry names an item and may set `min`, `max` and `chance`.

Definitions are validated before they are registered. If a definition produces a `data_error`—for example, an unknown recipe ingredient, item reference, or worldgen block—the complete definition is discarded; no partially initialized registry entry is kept. The loader continues with other files, so check the data-error log and correct each reported definition.

### Region save integrity and repair

Region files use a versioned `DFER` header with a header CRC32 and a CRC32 for every compressed column. A failed header checksum or unsupported region version is backed up as `r.X.Z.dfr.bak.0` (with older backups rotated through `.bak.1` and `.bak.2`) before the damaged region is regenerated. A failed column checksum preserves the complete region backup and regenerates only that column; other columns remain loadable. Region updates are written through a temporary file and atomically renamed into place.

Older region files without CRCs remain readable with a warning and are upgraded on their next save. To inspect a world and preserve damaged region files before recovery, run `dfe --repair-world NAME`; the command reports verified columns and damaged entries and leaves originals beside each region as `.bak.0`.

The JSON schemas under `sdk/schemas/` are editor aids for these files. Runtime validation remains authoritative and reports errors during startup.

---

## Tier 2: Lua scripting

Scripts run in a sandbox. The language is Lua 5.1 as implemented by LuaJIT, with the JIT compiler switched off so that execution can be counted.

### Entry point and modules

The file named by `"script"` runs once when the game starts, after every mod is mounted. Use it to register commands and event handlers. `require("name")` loads `scripts/name.lua` from the same mod, runs it once and returns the value it returns. Dots in the name become folders, so `require("util.maths")` loads `scripts/util/maths.lua`. A mod cannot require another mod's files.

Every mod has its own global table, so one mod cannot read or overwrite another mod's variables. A mod also has these names, which exist only in its own scripts:

| Name | Meaning |
|------|---------|
| `dfe` | The engine API described below. |
| `dfe.mod` | The id of the running mod, a string. |
| `print(...)` | Writes to the log and console as an info message from your mod. |
| `require(name)` | Loads one of your own modules. |

### What the sandbox provides and removes

Available: `assert error ipairs pairs next pcall xpcall select tonumber tostring type unpack rawget rawset rawequal setmetatable getmetatable`, and the libraries `string`, `table`, `math` and `bit` (bitwise operations).

Not available: `os`, `io`, `package`, `debug`, `ffi`, `jit`, `load`, `loadstring`, `loadfile`, `dofile`, `require` of anything outside your mod, and access to files, the network or other processes. Using one of these produces an error such as `attempt to index global 'os' (a nil value)`.

The `string`, `table` and `math` libraries are shared by all mods. Do not modify them.

### Limits

| Limit | Value | When exceeded |
|-------|-------|---------------|
| Memory | 96 MB in total for all scripts | The allocation fails with an out of memory error in your script. |
| Instructions while loading a script file | 30 million | The script stops with an error naming the file and line. |
| Instructions in one event handler call | 1 million | The handler is stopped, reported, and switched off. |
| Instructions in one command call | 5 million | The command is stopped and reported. |
| Command name | 1 to 23 characters, no spaces, unique | `dfe.command` raises an error. |

Limits count instructions, not time, so a script behaves the same on a fast and a slow machine. The error message for an overrun reads `file:line: script used more than N instructions in one call. Remove the endless loop, or spread the work over several ticks`. The `builder` example shows how to spread work over ticks.

A handler that raises an error is reported with its file and line, then switched off for the rest of the session. Fix the script and restart.

### API reference

All functions are on the `dfe` table. Coordinates are integer block coordinates; numbers with a fraction are rounded down. `y` is the vertical axis. A block position is "unloaded" when its chunk is not in memory; reads return `nil` there and writes return `false`.

#### Logging and console

| Function | Description |
|----------|-------------|
| `dfe.log(level, message)` | Write to the log. `level` is `"debug"`, `"info"`, `"warn"` or `"error"`. Messages are tagged with your mod id. |
| `dfe.console(message)` | Print a line in the in-game console. Use it for command replies. |

#### Blocks and states

A state is an integer that identifies one block with one set of property values. Treat the number as opaque: it can differ between worlds and between runs when mods change. Convert with names whenever you store or compare across sessions.

| Function | Returns | Description |
|----------|---------|-------------|
| `dfe.block_state(name)` | integer or `nil` | The state for a block name, such as `"base:stone"`, or with properties, `"mymod:lamp[lit=on]"`. `nil` if the block, property or value is unknown. |
| `dfe.block_name(state)` | string or `nil` | The block name of a state, without properties. |
| `dfe.state_name(state)` | string or `nil` | The full text of a state, including properties, such as `"mymod:lamp[lit=on]"`. |
| `dfe.get_state(x, y, z)` | integer or `nil` | The state at a position. |
| `dfe.get_block(x, y, z)` | string or `nil` | The block name at a position, without properties. |
| `dfe.set_block(x, y, z, block)` | boolean | Sets the block. `block` is a name or state text (a string) or a state number. Raises an error for an unknown name. Returns `false` if the position is unloaded. Lighting updates automatically. Does not fire `block_place` or `block_break`. |
| `dfe.get_light(x, y, z)` | four integers or `nil` | Sky, red, green and blue light, each 0 to 15. |

#### World, time and player

| Function | Description |
|----------|-------------|
| `dfe.seed()` | The low 32 bits of the world seed as a number. |
| `dfe.time()` | Simulated seconds since the world was created. It advances 0.05 seconds per game tick and is saved with the world. |
| `dfe.player_info()` | Table containing the current player's `id`, `name`, `health`, `dead` and mod `state`. |
| `dfe.player_state()` | Return the player's saved mod state string. |
| `dfe.player_state(value)` | Set the player's saved mod state string. It is shared, not namespaced per mod. |

#### Items, inventory and containers

| Function | Returns | Description |
|----------|---------|-------------|
| `dfe.item_count()` | integer | Number of loaded item definitions. |
| `dfe.item_add(item_id, count)` | integer | Add items to the player inventory; returns the count that did not fit. `count` defaults to 1. |
| `dfe.inventory_count(item_id)` | integer | Count the item or placeable block item in the player inventory. |
| `dfe.recipe_count()` | integer | Number of loaded recipes. |
| `dfe.recipe_craft(recipe_id)` | boolean | Craft one matching recipe from the player's inventory. |
| `dfe.recipe_process(recipe_id)` | boolean, seconds | Process one matching recipe; the second return value is its configured duration. |
| `dfe.loot_roll(table_id)` | integer | Roll a loaded loot table and add the results to the player's inventory; returns the number added. |
| `dfe.container_count(key, item_id)` | integer | Count an item in this mod's save-backed container identified by `key`. |
| `dfe.container_add(key, item_id, count)` | integer | Add items to that container; returns the count that did not fit. `count` defaults to 1. |

Container keys are private to the calling mod and persist in the active world save. The inventory helpers operate on the active player; use the player-related events to react to changes. They do not expose arbitrary player position or movement control.

World metadata includes `schema_version`, which identifies the JSON save-metadata structure and is independent of the existing `version` field. The current value is 2. Saves without this field are treated as legacy schema version 2 and logged with a warning; explicitly older or newer schema versions are rejected with an explanatory error. This prevents an engine from silently interpreting metadata it does not understand.

#### Entities

| Function | Description |
|----------|-------------|
| `dfe.entity_spawn(type, x, y, z)` | Spawn an entity of a type such as `"base:hopper"` with its feet at the position. Returns an integer handle, or `nil` when the type is unknown or the limit of 256 is reached; the reason is written to the log. |
| `dfe.entity_remove(handle)` | Remove an entity. Returns true if it existed. |
| `dfe.entity_position(handle)` | Returns `x, y, z` of the feet, or `nil` when the handle no longer exists (it was removed, its lifetime ended, or it fell out of the world). |
| `dfe.entity_count()` | Number of entities alive. |

Handles are never reused within a session. Spawning needs loaded terrain to stand on; call it from a `tick` handler or later rather than from `world_load`, because the world is still empty then. This example spawns three hoppers on the surface near the origin once the world exists:

```lua
local done = false
dfe.on("tick", function()
  if done then return end
  done = true
  for i = 0, 2 do
    local x, z = 4 + i * 2, 4
    local y = 150
    while y > 0 and dfe.get_block(x, y, z) == "base:air" do y = y - 1 end
    dfe.entity_spawn("base:hopper", x + 0.5, y + 1, z + 0.5)
  end
end)
```

#### Events and commands

| Function | Description |
|----------|-------------|
| `dfe.on(event, fn)` | Subscribe `fn` to an event. Returns a handle. Raises an error for an unknown event name. |
| `dfe.command(name, help, fn)` | Register a console command. `fn` receives the text after the command name as one string, with leading spaces removed. The text of `help` is shown by the `help` command. |

---

## Events

`dfe.on(name, fn)` calls `fn(event)` with a table. The fields that exist depend on the event.

| Event | When | Fields | Cancellable |
|-------|------|--------|-------------|
| `tick` | 20 times per second of game time | `dt` (always 0.05) | No |
| `block_place` | A block is placed on the player's behalf | `x`, `y`, `z`, `state` (the state about to be placed) | Yes |
| `block_break` | A block is removed on the player's behalf | `x`, `y`, `z`, `state` (the state about to be removed) | Yes |
| `world_load` | After a world is opened, before the first tick | none | No |
| `world_unload` | Before a world closes | none | No |
| `random_tick` | Each game tick, a few randomly chosen blocks near the player, for blocks with `random_tick: true` | `x`, `y`, `z`, `state` | No |
| `command` | A console line matched no command | `text`, the full line | Yes: return true to mark it handled |
| `item_use` | The player uses the held item | `x` (inventory slot), `y` (held block state), `text` (item id) | Yes |
| `inventory_change` | An item is added by an engine/mod inventory helper | `text` (item id) | Yes: cancels that addition |
| `container_open`, `container_close` | A mod opens or closes its save-backed container | `text` (`mod:key`) | Yes |
| `player_join`, `player_leave` | A player session starts or ends | `text` (`player`) | No |
| `player_damage`, `player_death`, `player_respawn` | The player is damaged, dies or respawns | `name` | Damage and death events can be cancelled |
| `entity_spawn` | An entity is about to spawn | `text` (entity type id) | Yes |
| `entity_damage`, `entity_death` | An entity is damaged or about to die | `entity_id`, `damage` (damage only) | Yes. A cancelled death leaves 1 health |
| `entity_tick` | An entity is about to be simulated | `entity_id`, `dt` | Yes, the entity skips that update |
| `entity_despawn` | An entity is removed | `entity_id`, `text` (reason) | Yes, except for death and falling out of the world |

Every table also has a `name` field with the event name.

Cancelling: a handler for a cancellable event cancels it by returning `true` (any value other than `false` and `nil` counts). Handlers run in load order and the first one that cancels stops the rest from running. For `command`, returning true prevents the "unknown command" message.

"On the player's behalf" means edits made by the player or by an engine command such as `setblock`. Edits made by a mod through `dfe.set_block` never fire `block_place` or `block_break`. This is deliberate: a handler that edits blocks would otherwise trigger itself and loop.

The engine also fires `block_break` when the new state is air and `block_place` for any other state, so replacing one block with another fires `block_place` only.

---

## The console

Press the grave key (`` ` ``) to open and close the console. Enter runs a line, the up and down arrows recall earlier lines, and Page Up and Page Down scroll. Escape closes it. While it is open, game input is paused. `F3` cycles the debug overlay.

Built-in commands:

| Command | Description |
|---------|-------------|
| `help` | List every command with its help text, including mod commands. |
| `mods` | List installed mods with version, state and load order. |
| `seed` | Show the world seed. |
| `time [set name\|0..1]` | Show game time and the day phase, or set the time of day to midnight, dawn, morning, noon, afternoon, dusk, night or a phase from 0 to 1. |
| `weather [clear\|overcast\|rain] [now]` | Show or change the weather. It blends over about 25 seconds unless `now` is given. |
| `getblock x y z` | Show the state at a position. |
| `setblock x y z block` | Place a block or state, as the player. Fires the cancellable events. |
| `gamemode creative\|survival` | Switch mode. Creative has instant breaking, a block palette and flight (F). Survival uses hardness, drops and consumes placed blocks. |
| `tp x y z`, `teleport x y z` | Move the player to a loaded, unobstructed position and clear their velocity. |
| `give item [count]` | Add an item or block to the inventory, for example `give base:feather 3`. |
| `lua code` | Run a Lua statement or expression in the sandbox and print the result. |
| `spawn type [x y z]` | Spawn an entity three blocks in front of the player, or at the given position. |
| `entities` | List entity types and how many entities are alive. |
| `reload` | Reload shaders, the atmosphere file, presets and entity types now. |

Survival players have 20 health. Hard falls and lava deal damage; a short invulnerability window prevents a single impact from being counted repeatedly. The HUD shows remaining health and a fading hit overlay, and Space respawns at the world's spawn after death. Existing saves without a health field load at full health.

A line that matches no command fires the `command` event before the game prints "unknown command".

---

## Tier 3: native plugins and the C API

Use a native plugin only when Lua cannot do the job. A plugin is a shared library that runs with the full privileges of the game, so the engine loads it only when started with `--allow-native`. Without the flag the engine logs that the plugin was skipped. Only install native mods from authors you trust.

### Building a plugin

A plugin includes only `include/dfe_api.h` and exports two functions:

```c
#include "dfe_api.h"

DFE_PLUGIN_EXPORT int dfe_plugin_init(const dfe_api_t *api, const char *mod_id) {
    api->log(DFE_LOG_INFO, mod_id, "hello");
    return 0;   /* nonzero reports an error and the plugin is unloaded */
}

DFE_PLUGIN_EXPORT void dfe_plugin_shutdown(void) {   /* optional */
}
```

Declare the library in the manifest per platform with `"plugin"`. Build it as a shared library: on Linux `cc -shared -fPIC -I include -o libmymod.so mymod.c`. Use only the `dfe_api_t` table passed to `dfe_plugin_init`. The engine executable has other symbols, but they are internal and change without notice. Keep the pointer; it stays valid until shutdown.

The example plugin builds with CMake: configure with `-DDFE_BUILD_EXAMPLES=ON` and the library is written into `examples/mods/tally/plugins/`.

### Compatibility rules

* Fields are only appended to `dfe_api_t`. Existing fields never change type or meaning.
* `struct_size` tells a plugin how many fields the running engine provides. Check it before using a field that was added after the version you built against.
* `abi_version` changes only for a breaking change, which also changes the major `api` number that manifests declare.
* The Lua bindings are written against this same table, so anything Lua can do a plugin can do.

### dfe_api_t reference

| Member | Description |
|--------|-------------|
| `abi_version` | `DFE_API_VERSION`, currently 1. |
| `struct_size` | `sizeof(dfe_api_t)` as the engine was built. |
| `log(level, mod_id, message)` | Write to the log. Levels: `DFE_LOG_DEBUG`, `INFO`, `WARN`, `ERROR`. |
| `block_state(name)` | State for `"ns:name"` or `"ns:name[prop=value]"`, or `DFE_STATE_UNLOADED` (0xFFFF) if unknown. |
| `block_name(state)` | Block name of a state, or `NULL`. The pointer is valid for the life of the game. |
| `get_state(x, y, z)` | State at a position, or `DFE_STATE_UNLOADED`. |
| `set_state(x, y, z, state)` | Set a block. Returns false if unloaded. Does not fire block events. |
| `get_light(x, y, z, out[4])` | Sky, red, green, blue light. Returns false if unloaded. |
| `world_seed()` | The full 64-bit seed. |
| `game_time()` | Simulated seconds. |
| `subscribe(event, fn, user, mod_id)` | Subscribe to an event. Returns a handle greater than 0, or 0 for an unknown event name. The plugin owns `user`; the engine never frees it. Keep it valid until the subscription is removed and free it after unregistering or during plugin shutdown. |
| `register_command(name, help, fn, user, mod_id)` | Register a console command. Returns 0 on failure. The plugin owns `user`; the engine never frees it. Keep it valid until the command is removed and free it after unregistering or during plugin shutdown. |
| `console_print(message)` | Write a line to the console. |
| `state_string(state, out, size)` | Text of a state such as `"mymod:lamp[lit=on]"`. Added in API 1.1; check `struct_size` before use. Returns false for an invalid state. |
| `entity_spawn(type, x, y, z)` | Spawn an entity. Returns a handle greater than 0, or 0 when the type is unknown or the limit is reached. Added in API 1.2; check `struct_size` before use. |
| `entity_remove(handle)` | Remove an entity. Returns false for an unknown handle. API 1.2. |
| `entity_position(handle, out)` | Write the feet position to `out[3]`. Returns false for an unknown handle. API 1.2. |
| `entity_count()` | Number of entities alive. API 1.2. |
| `entity_get(handle, out)`, `entity_set(handle, in, mask)` | Read or write a `dfe_entity_t`; `mask` uses `DFE_ENTITY_*` bits. API 1.3. |
| `entity_damage(handle, amount)`, `entity_heal(handle, amount)` | Health changes through the events. API 1.3. |
| `entity_list(out, cap)`, `entity_near(x, y, z, radius, out, cap)` | Handles in spawn order, or within a radius nearest first. API 1.3. |

Event handlers have the signature `int fn(const dfe_event_t *ev, void *user)` and return nonzero to cancel a cancellable event. Command handlers have the signature `void fn(const char *args, void *user)`. The `dfe_event_t` fields are `name`, `x`, `y`, `z`, `state`, `dt` and `text`, filled as described in [Events](#events).

All API calls must be made from the game thread, from within `dfe_plugin_init`, an event handler or a command handler. Do not call them from threads you create.

---

## The example mods

Seven example mods are included in `examples/mods`. They are meant to be copied and changed. Copy any of them into `mods/` to install it. The `base` mod in `mods/base` is an eighth, larger example of tier 1 content.

| Mod | Tier | Demonstrates |
|-----|------|--------------|
| `gems` | 1 and 2 | Four new blocks with textures and coloured light, a cross-shaped cutout plant, a block property (`lit`) that controls light emission, and a command that changes the property from Lua. |
| `retexture` | 1 | Replacing base textures by providing files at the same path. No manifest keys beyond the basics. |
| `highsea` | 1 | Replacing a data file, here the world generation settings, to raise the sea level. |
| `builder` | 2 | Commands (`fill`, `sphere`, `builds`, `cancelbuild`), splitting a large job over many ticks to respect the instruction limits, `require` for a shared module, and an optional dependency (`?gems`). |
| `guard` | 2 | Cancelling `block_place` and `block_break` to protect regions, and sub-commands in one command. |
| `warmgrade` | 1 | A shader pack: one replaced engine shader (`post.frag`) that grades the final image. Shows how shader overrides need no manifest keys. |
| `tally` | 3 | A native C plugin with a command and event subscriptions, built through the C API only. |

Trying them in the game:

```
setblock 8 90 8 gems:lamp
lamp 8 90 8 on
sphere 0 100 0 5 gems:ruby_block
fill 0 80 0 15 80 15 base:stone
guard add 0 0 20
setblock 0 90 0 base:stone
tally 0 80 0 15 100 15
```

The last two need `guard` and `tally`. `tally` also needs the engine started with `--allow-native` and the plugin built. The self-test (`dfe --selftest`) loads every example, runs these commands against a generated world and checks the results, so the examples are kept working by the same checks as the engine.

---

## Developing with hot reload

Start the game with `--dev` and the engine checks the modification time and size of every file it has read, twice a second. When one changes it reloads what can safely be reloaded. The `reload` console command and F5 do the same on demand, without `--dev`.

| Reloaded live | Not reloaded, restart the game |
|---------------|--------------------------------|
| Shaders, including shader packs and `#include` files | Block definitions |
| The atmosphere file | Textures |
| Quality presets | Lua scripts and native plugins |
| Entity types (existing entities keep their type) | World generation settings |

The split has a reason. Block ids are stored in every loaded chunk and the texture array is built once, so changing either live would corrupt what is on screen. Scripts hold state that cannot be rebuilt from their files.

A reload that has an error does not break the running game: the previous shader program, atmosphere, presets or entity type stays in use, and the compiler message or the file and line of the mistake is written to the log. Fix the file and save again. A good workflow for a shader pack is to run `dfe --dev --world test`, edit `post.frag` in the mod folder and watch the result change on save.

---

## Performance and budgets for mod authors

The engine targets a 2015 dual-core laptop with an Intel HD 520. A mod is judged by what it costs there.

* Run `dfe --benchmark` with and without your mod (`--mods DIR` selects a mods folder) and compare. The report splits GPU time by pass (`opaque`, `cutout`, `sky`, `water`, `rain`, `ui`, `post`, `entity`), so a shader pack's cost shows in `post` or in the pass it replaced.
* `tools/perf_matrix.py` runs presets and window sizes in one launch and compares against a saved baseline: `tools/perf_matrix.py --baseline perf_results/<earlier>.json -- --mods path/to/mods`.
* Budgets on the reference machine at the Low preset, 720p, render distance 8: 60 fps average, 1% low above 40, no frame above 25 ms, memory below 1.5 GB, cold start under 10 seconds.
* Blocks cost vertices. A block with many model elements or a transparent texture costs more than a cube; prefer cubes and cutouts where the look allows it.
* Entities cost two draw calls each. A few dozen are free; the limit of 256 exists so a runaway script cannot stall the frame.
* Do the minimum in `tick`, which runs 20 times per second, and in `random_tick`. The instruction limit exists to stop a loop, not to make a heavy handler acceptable.

The README explains how to read the report and decide whether a frame is CPU or GPU bound.

---

## Limits and what is not available yet

Stated plainly so that mod authors can plan.

* Lua can read basic player information and per-mod player state, and can add/count inventory items, craft/process recipes, roll loot and use save-backed containers. It cannot read or change arbitrary player position or directly inspect individual inventory slots.
* Lua world generators and Lua-defined ores and structures are planned and are not part of mod API 1. Biome files apply only to worlds with Forever Worlds on and an epoch whose `biomes.json` sets `"kernel": 1`. Block, texture, entity type, preset, atmosphere, shader, biome and world setting data are available now.
* Entities have health, behaviours (`wander`, `static`, `hostile`, `passive`), block collision, Lua/native scripting and are saved with the world. Limits: box-based models only, no death animation, no entity-to-entity collision, at most 256 at once.
* Mods can persist structured state under the active world save through `dfe.storage`. Data is namespaced by mod id and survives save/reload. The value is JSON-backed, so tables, arrays, booleans, numbers and strings are all valid. Block edits and player state persist because the world is saved, and the mod storage is part of the same save stream.
* `dfe.storage` is per-mod: a script in `base` can only read and write keys in `base`, never in another mod's namespace. A key must be a short identifier such as `quest.stage` or `economy.gold`.
* `dfe.seed()` returns only the low 32 bits of the seed. Use the C API for all 64 bits.
* The `string`, `table` and `math` libraries are shared between mods.
* Hot reload covers shaders, the atmosphere file, presets and entity types only; see [Developing with hot reload](#developing-with-hot-reload).
* Only one mod can replace a given shader; the last in load order wins. There is no merging of shader changes.
* Native plugins and the Windows build have not been tested on Windows yet.

---

## Command-line tools and schemas

Run these commands from the repository root:

```
./build/dfe mod validate path/to/mod
./build/dfe mod test path/to/mod
./build/dfe mod package path/to/mod -o mymod.dfe.zip
```

`validate` checks the manifest and JSON registry files. `test` starts the engine's headless runtime: it loads base content and dependencies, creates a deterministic world, runs the mod's Lua entry points and the requested number of fixed ticks, then runs every `tests/*.lua` file in the target mod. A test file passes when it returns without an error; Lua `assert` is the recommended assertion mechanism. The command returns non-zero on content, initialization, tick-handler, or test failures. Use `--json` for CI output, for example:

```
./build/dfe --headless mod test path/to/mod --ticks 100 --seed 123 --json
```

Runtime test options include `--ticks N`, `--seed N`, `--world DIR`, `--timeout SECONDS`, `--verbose`, `--no-cleanup`, and `--allow-native`. The Lua sandbox's deterministic instruction budget protects against infinite scripts; `--timeout` is accepted for CI compatibility, while execution limits remain instruction-based. Headless mode does not create a window, OpenGL context, audio device, or read input. `package` validates first, then creates a zip archive (the `zip` utility must be installed). Schemas in `sdk/schemas/` cover mod manifests, items, recipes, loot tables, tags and save data. The runtime loader may enforce additional semantic rules beyond a JSON schema.

## Status HUD (hud.json, theme, Lua)

Implemented: `assets/dfe/ui/hud.json` (elements) and `assets/dfe/ui/theme/default.json` (colours), parsed once at start and on hot reload; a bad file keeps the previous layout.

`hud.json`: `scale`, `elements[]` with `id`, `anchor` (top_left, top_center, top_right, center, bottom_left, bottom_center, bottom_right), `x`/`y` offset from the anchor, `w`/`h`, `align` ("right" anchors the right edge), `style` ("bar" or "pips"), `source`/`max` (PlayerStatus fields: health, hunger, saturation, stamina, magicka, xp, armor, `max_*`, `xp_next`, or custom names, custom max is `<name>_max`), `pip_value`, `pip_size`, `spacing`, `color` (theme colour name), `modes` (["survival"], ["creative"]; default both), `requires_max`, `hide_when_empty`, `label` ("level").

Theme: `colors` (name -> `#rrggbb` or `#rrggbbaa`), `text_size`. Includes `bar_back` and `text`.

Defaults: survival shows hearts, hunger, armor (when above 0), XP; stamina and magicka show only when their max is above 0. Creative hides all. `settings.json` key `hud_elements` (`{"hearts": true}`) forces an element on or off in any mode.

Lua: `dfe.ui.get_status()`, `dfe.ui.set_status(name, value, max)` (custom bar fields), `dfe.ui.set_element_visible(id, true|false|nil)`, `dfe.ui.element_visible(id)`.

Not implemented yet: effect icons, native plugin UI API, crafting/recipe book/tooltips/sorting, keybind data, accessibility options, hunger/stamina/magicka/XP gameplay (fields hold defaults).

## Screens and widgets

Implemented in `src/screen.c`. A screen is a panel centred in the window; widget `x`/`y` are relative to the panel. Types: `panel`, `label`, `button`, `slot` (frame only), `bar` (`value`/`max`, `text` names a theme colour), `list` (`items`, selectable, scrolls by `scroll`), `scroll` (read-only list). Screens stack (up to 8); `modal` dims and swallows clicks outside; Escape closes when `close_on_escape` (default true) or modal. Tab/Shift-Tab move focus, Up/Down move a focused list, Enter/Space activate. While any screen is open the game does not take movement or mouse-look input.

Lua:
```lua
dfe.ui.register_screen("mymod:demo", {
  title = "Demo", w = 240, h = 120, modal = true,
  on_open = function(screen) end, on_close = function(screen) end,
  widgets = {
    {type = "button", id = "go", text = "Go", x = 10, y = 30, w = 60, h = 20,
     on_click = function(screen, widget, index) dfe.log("info", "clicked " .. widget) end},
    {type = "bar", id = "mana", x = 10, y = 70, w = 100, h = 6, value = 3, max = 10, text = "magicka"},
  }})
dfe.ui.open_screen("mymod:demo")
dfe.ui.set_widget("mymod:demo", "mana", {value = 7})
```
Also `dfe.ui.register_widget(screen, spec)`, `dfe.ui.close_screen()` (pops the top screen), `dfe.ui.is_screen_open([id])`. List `index` is 1-based in Lua. A callback that errors is switched off and reported; scripted screens are removed when scripts shut down.

Native: fill a `ScreenDef` and `Widget`s and call `screen_register`, `screen_add_widget`, `screen_open` (`src/screen.h`). There is no `dfe_api_t` entry yet, so plugins cannot use it.

## Icons

`assets/dfe/ui/icons.json` + `icons.png` (`{"cell":16,"icons":{"heart":[col,row]}}` or `{"x","y","w","h"}` rects; `missing` required, 256 icons max). Mods add `assets/<modid>/ui/icons.json`/`icons.png`; later roots override by name; unknown names draw `missing`. HUD pip elements can use icons (`"icon"`, see the HUD section). `tools/gen_ui_icons.py` regenerates the default png.

### HUD and UI scaling

Four independent settings in `settings.json`:

| Key | Range | Default | Affects |
|---|---|---|---|
| `ui_scale` | -1 (auto), 1 to 4 | -1 | menus, screens: whole screen pixels per art pixel |
| `ui_text_scale` | 0.75 to 2 | 1 | text in menus, screens, widgets and the inventory |
| `hud_scale` | 0.5 to 3 | 1 | multiplies the automatic scale for the hotbar and status HUD |
| `hud_text_scale` | 0.75 to 2 | 1 | text in the HUD |

Pixel precision: every scale that multiplies pixel art is a whole number.
- `ui_gui_scale` is `ui_scale` or the automatic scale (window / 320x360, 1 to 4).
- `ui_hud_scale` is `round(auto * hud_scale)` (1 to 8), then halved (floor, at least 1) because HUD art is drawn at twice the 16 px icon grid. Hotbar, hearts and bars multiply by it. `hud.json` lengths are in these art pixels.
- The inventory uses the same halving of the GUI scale and steps down by whole steps until its panel fits the window.
- Item icons (32 px) and UI icons (16 px) are drawn at exact integer multiples with nearest sampling.
- The UI font (Monocraft) has a pixel grid of 12 px of text size. Text sizes snap to multiples of 12 (`ui_snap_text`), so each font pixel is a whole number of screen pixels.
- Rectangles, images and glyph origins snap to whole pixels (`ui_snap_span`); both edges are rounded so neighbours that meet in layout units meet on screen.

Text scale never shrinks a parent. A label or button grows only when its scaled text no longer fits (`ui_fit`, whole pixels, pad = 4 units).

HUD pip elements can use artwork: set `"icon": "heart"` (a name from `icons.json`) and `pip_size` 16. The empty pip is the icon darkened, the filled part is drawn over it cut on a whole icon pixel (`icons_draw_part`).
