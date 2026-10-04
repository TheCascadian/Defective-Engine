# Modding Platform Design

## Goal

Make Defective Engine usable as a data- and Lua-driven modding platform without rewriting the renderer, jobs, save, or entity architecture.

## Existing foundations

The current engine already provides namespaced block/state registration, VFS overlays, JSON parsing, Lua sandboxing, mod storage, entities, commands, save metadata, inventory basics, reload stamps, deterministic jobs, and self-tests. `MANUS_BRANCH_CODE` adds a reusable content layer for item definitions, tags, recipes, loot, containers, schemas, and player data.

## Architecture

Adapt the MANUS content layer into the current source tree and expose it through one internal content API plus the stable native/Lua API. Registries use namespaced string IDs and runtime references; tags resolve recursively with cycle/depth validation. Existing block states remain save-compatible, while general items carry an explicit ID, count, durability, metadata, and optional placed state.

Data loading is staged: discover mod manifests and dependencies, parse schemas/definitions, validate all references, register atomically, then activate runtime consumers. Reload builds a candidate registry and swaps it only when validation succeeds. Existing worlds retain old serialized block names and save versions migrate explicitly.

Events remain a small typed layer integrated with current interaction, inventory, entity, container, player, tick, and command paths. Commands use the current console/registration path with structured argument and permission metadata. Lua receives the same capabilities as native plugins through versioned API tables.

## Versioning

Manifests declare `engine_api`, `mod_api`, `content_schema`, and dependency ranges. The engine declares matching constants and rejects incompatible values with mod/file/reason diagnostics. Save headers declare a save format version and provide migration gates.

## SDK

Canonical mod layout:

```text
mod.json
blocks/ items/ entities/ biomes/ worldgen/ structures/
recipes/ loot/ scripts/ textures/ sounds/
```

Legacy `data/<namespace>/...` layouts remain readable during migration. `dfe mod validate` validates without loading a world, `dfe mod test` runs isolated self-tests, and `dfe mod package` emits a deterministic archive after validation.

## Acceptance criteria

Every feature has an input definition, parser, registry/runtime path, persistence where applicable, Lua/native access where applicable, and self-tests. Stress tests cover thousands of definitions, large entity/chunk counts, Lua activity, mod state, dependencies, save/load, and structures; optimization changes require measured evidence and preserve deterministic behavior.
