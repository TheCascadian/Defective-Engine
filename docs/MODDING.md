# Defective Engine Modding Guide

This guide documents everything a mod can do today. It is the reference for mod authors and the contract the engine keeps: anything described here as stable will not change within mod API 1.

Three tiers are available. Use the lowest tier that does the job.

| Tier | What you write | Sandbox | Use it for |
|------|----------------|---------|------------|
| 1. Data and assets | JSON and PNG files | Not applicable, no code runs | New blocks, textures, world settings, replacing base content |
| 2. Lua scripts | `.lua` files | Yes, bounded memory and instructions | Commands, reacting to events, editing the world |
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
11. [Limits and what is not available yet](#limits-and-what-is-not-available-yet)

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
| `friction` | number | 1.0 | Surface friction multiplier. |
| `hardness` | number | 1.0 | Seconds to break by hand. Negative means unbreakable. |
| `tool` | string | none | Preferred tool, for example `pick`, `axe` or `shovel`. |
| `drops` | string | the block itself | Block id dropped when broken. |
| `sound` | string | `stone` | Sound set name. |
| `light.opacity` | 0 to 15 | 15 for opaque cubes, 2 for fluids, else 0 | How much light the block absorbs per block travelled. 15 stops light completely. |
| `light.emit` | `[r, g, b]` | `[0, 0, 0]` | Coloured light the block gives off, each channel 0 to 15. |
| `light.emit_when` | object | none | One property and value, such as `{"lit": "on"}`. The block emits only in states where the property has that value. The property must be declared under `properties`. |
| `properties` | object | none | State properties. Each key maps to an array of 1 to 16 string values. At most 4 properties per block. |
| `defaults` | object | first value of each | Property values of the default state. |
| `fluid` | object | none | Makes the block a fluid. `viscosity` is ticks per spread step, `group` names the fluid family. |

Mistakes that name a bad value, such as an unknown `shape` or `tint`, are reported with the file and line and list the valid choices.

The keys `hardness`, `tool`, `drops`, `sound`, `random_tick`, `friction` and `climbable` are stored with the block and read by the interaction and tick systems as those arrive. Today they have no visible effect on their own.

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
    "gravel": "base:gravel", "snow": "base:snow", "mud": "base:mud", "water": "base:water"
  }
}
```

Biomes, caves, ores and structures are currently built into the generator. Data-driven definitions for them and a Lua generator hook arrive with the world generation milestone.

### Replacing content

Because later mods win, replacing base content needs no special syntax: provide a file at the same path.

| To replace | Provide |
|------------|---------|
| The stone texture | `assets/base/textures/block/stone.png` |
| The stone block definition | `data/base/blocks/stone.json` |
| World generation settings | `data/base/worldgen/default.json` |

The `retexture` and `highsea` example mods do exactly this.

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
| `time` | Show simulated time. |
| `getblock x y z` | Show the state at a position. |
| `setblock x y z block` | Place a block or state, as the player. Fires the cancellable events. |
| `lua code` | Run a Lua statement or expression in the sandbox and print the result. |

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

Event handlers have the signature `int fn(const dfe_event_t *ev, void *user)` and return nonzero to cancel a cancellable event. Command handlers have the signature `void fn(const char *args, void *user)`. The `dfe_event_t` fields are `name`, `x`, `y`, `z`, `state`, `dt` and `text`, filled as described in [Events](#events).

All API calls must be made from the game thread, from within `dfe_plugin_init`, an event handler or a command handler. Do not call them from threads you create.

---

## The example mods

Six example mods are included in `examples/mods`. They are meant to be copied and changed. Copy any of them into `mods/` to install it. The `base` mod in `mods/base` is a seventh, larger example of tier 1 content.

| Mod | Tier | Demonstrates |
|-----|------|--------------|
| `gems` | 1 and 2 | Four new blocks with textures and coloured light, a cross-shaped cutout plant, a block property (`lit`) that controls light emission, and a command that changes the property from Lua. |
| `retexture` | 1 | Replacing base textures by providing files at the same path. No manifest keys beyond the basics. |
| `highsea` | 1 | Replacing a data file, here the world generation settings, to raise the sea level. |
| `builder` | 2 | Commands (`fill`, `sphere`, `builds`, `cancelbuild`), splitting a large job over many ticks to respect the instruction limits, `require` for a shared module, and an optional dependency (`?gems`). |
| `guard` | 2 | Cancelling `block_place` and `block_break` to protect regions, and sub-commands in one command. |
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

## Limits and what is not available yet

Stated plainly so that mod authors can plan.

* There is no player, inventory or interaction API yet. `block_place` and `block_break` currently fire for the `setblock` console command and will fire for the player when interaction is added, with no change to the mod API.
* Lua world generators, data-driven biomes, ores and structures, custom screens, entities and shader packs are planned and are not part of mod API 1. Block, texture and world setting data are available now.
* Mods have no persistent storage. State kept in Lua variables is lost when the game closes. Block edits persist because the world is saved.
* `dfe.seed()` returns only the low 32 bits of the seed. Use the C API for all 64 bits.
* The `string`, `table` and `math` libraries are shared between mods.
* Hot reload is not implemented. Restart the game after changing a mod.
* Native plugins and the Windows build have not been tested on Windows yet.
