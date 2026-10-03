# Defective Engine Modding Guide

This guide documents everything a mod can do today. It is the reference for mod authors and the contract the engine keeps: anything described here as stable will not change within mod API 1.

Three tiers are available. Use the lowest tier that does the job.

| Tier | What you write | Sandbox | Use it for |
|------|----------------|---------|------------|
| 1. Data and assets | JSON, PNG and GLSL files | Not applicable, no code runs | New blocks, textures, entity types, quality presets, world settings, shader packs, replacing base content |
| 2. Lua scripts | `.lua` files | Yes, bounded memory and instructions | Commands, reacting to events, editing the world, spawning entities |
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

---

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
    data/<namespace>/worldgen/default.json    world generation settings
    data/<namespace>/atmosphere/default.json  sky colours, day length, clouds and weather
    data/<namespace>/presets/<id>.json        quality presets
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
| `friction` | number | 1.0 | Ground movement speed multiplier, clamped to 0.5..1.1. Lower values slow ground movement; they do not add coasting. |
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

### World generation settings

`data/<namespace>/worldgen/default.json` sets the sea level, the level where deep stone begins and the blocks terrain is built from. The base game's file is the complete list of keys; copy it and change values:

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

The roles from `stone` to `water` are required. The decoration roles from `log` onward are optional; leaving one out removes that feature. Biome layout, cave shapes and tree shapes are built into the generator. Data-driven definitions for them and a Lua generator hook arrive in a later milestone.

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
| `dynamic_resolution` | Let the controller change the render scale to hold `target_fps`. |
| `min_scale` | The lowest render scale the controller may choose, 0.4 to 1. Below 0.4 the picture is too soft to play. |
| `target_fps` | The frame rate the controller tries to hold, 15 to 240. |

A preset with a mistake is reported on the error screen and ignored. The player's own choices in `settings.json` (render distance, dynamic resolution, render scale, field of view, vertical sync) take precedence over the preset, and a value left at "preset" follows it.

### Entities

`data/<namespace>/entities/<id>.json` defines an entity type. The type id is `<namespace>:<file name>`, so `data/mymod/entities/slime.json` is `mymod:slime`. The base game ships `base:hopper`.

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

An entity is drawn as a body box and a head box, lit by the world light at its position and fogged like terrain. Its physics is the player's: gravity, water, step-up and collision with blocks. Terrain that is not loaded counts as solid, so an entity beyond the streamed area waits rather than falling out of the world. There are at most 256 entities at once. Entities are spawned from Lua (`dfe.entity_spawn`), from a native plugin, or with the console command `spawn`. They are not saved with the world.

A type with a mistake is reported and the previous definition, if any, stays in use, which makes live editing with `--dev` safe.

### Shader packs

Every shader the engine draws with is an ordinary asset, so a mod replaces one by providing a file at the same path. No manifest key is needed.

| File under `assets/dfe/shaders/` | What it draws |
|----------------------------------|---------------|
| `chunk.vert`, `chunk.frag` | All terrain, near and far. The fragment shader is also compiled with `PASS_CUTOUT`, `PASS_TRANSLUCENT` and `LOD` defined. |
| `post.vert`, `post.frag` | The resolve pass that scales the scene to the window. The engine skips it at full scale without shafts to save a full-screen copy, but it always runs while a mod replaces `post.frag`, so a grading shader is applied at every setting. The cost of that pass is paid by every player of the pack. The file is compiled three times: plain, with `SHAFTS` (the resolve, which reads the shaft mask) and with `SHAFT_MASK` (a quarter-resolution march toward the sun that writes one brightness value). A replacement that does not contain the text `SHAFT_MASK` is treated as not supporting light shafts, and they are switched off for it. To keep them, copy the engine's file and edit its final colour step. |
| `entity.vert`, `entity.frag` | Entities. |
| `sky.vert`, `sky.frag` | The sky, stars, sun, moon, clouds and the rain layer, selected by `PASS_SKY` and `PASS_RAIN`. Shaders can `#include` other files through the same virtual filesystem. |
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
| The hopper | `data/base/entities/hopper.json` |
| The final image grade | `assets/dfe/shaders/post.frag` |

The `retexture`, `highsea` and `warmgrade` example mods do exactly this.

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

#### World and time

| Function | Description |
|----------|-------------|
| `dfe.seed()` | The low 32 bits of the world seed as a number. |
| `dfe.time()` | Simulated seconds since the world was created. It advances 0.05 seconds per game tick and is saved with the world. |

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
| `give block [count]` | Add items to the inventory. |
| `lua code` | Run a Lua statement or expression in the sandbox and print the result. |
| `spawn type [x y z]` | Spawn an entity three blocks in front of the player, or at the given position. |
| `entities` | List entity types and how many entities are alive. |
| `reload` | Reload shaders, the atmosphere file, presets and entity types now. |

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
| `subscribe(event, fn, user, mod_id)` | Subscribe to an event. Returns a handle greater than 0, or 0 for an unknown event name. |
| `register_command(name, help, fn, user, mod_id)` | Register a console command. Returns 0 on failure. |
| `console_print(message)` | Write a line to the console. |
| `state_string(state, out, size)` | Text of a state such as `"mymod:lamp[lit=on]"`. Added in API 1.1; check `struct_size` before use. Returns false for an invalid state. |
| `entity_spawn(type, x, y, z)` | Spawn an entity. Returns a handle greater than 0, or 0 when the type is unknown or the limit is reached. Added in API 1.2; check `struct_size` before use. |
| `entity_remove(handle)` | Remove an entity. Returns false for an unknown handle. API 1.2. |
| `entity_position(handle, out)` | Write the feet position to `out[3]`. Returns false for an unknown handle. API 1.2. |
| `entity_count()` | Number of entities alive. API 1.2. |

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

* Mods cannot read or change the player or inventory from Lua yet. `block_place` and `block_break` fire for the player's own edits and for the `setblock` command.
* Lua world generators, data-driven biomes, ores and structures, and custom screens are planned and are not part of mod API 1. Block, texture, entity type, preset, atmosphere, shader and world setting data are available now.
* Entities have no scripted behaviour, no health, no models beyond the two-box shape and no collision with the player or each other, and they are not saved with the world. Their movement is a wander; a mod that wants other behaviour can move them by removing and respawning, or wait for the behaviour API.
* Mods have no persistent storage. State kept in Lua variables is lost when the game closes. Block edits persist because the world is saved.
* `dfe.seed()` returns only the low 32 bits of the seed. Use the C API for all 64 bits.
* The `string`, `table` and `math` libraries are shared between mods.
* Hot reload covers shaders, the atmosphere file, presets and entity types only; see [Developing with hot reload](#developing-with-hot-reload).
* Only one mod can replace a given shader; the last in load order wins. There is no merging of shader changes.
* Native plugins and the Windows build have not been tested on Windows yet.
