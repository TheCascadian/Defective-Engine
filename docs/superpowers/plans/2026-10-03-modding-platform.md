# Modding Platform Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Deliver a validated, persistent, Lua-accessible modding platform using the current engine and MANUS content implementation.

**Architecture:** Adapt `MANUS_BRANCH_CODE/content.c` and schemas into focused current-engine modules, then wire runtime consumers through existing block, entity, player, save, console, reload, and Lua paths. Registry reload is candidate-build/atomic-swap; incompatible definitions never replace active content.

**Tech Stack:** C17, CMake, vendored LuaJIT, existing JSON/VFS/save/job/entity systems, self-test driver, shell tooling.

**Spec:** `docs/superpowers/specs/2026-10-03-modding-platform-design.md`

## Global Constraints

- Preserve renderer, job system, save system, and entity architecture unless measurements prove a defect.
- Namespaced IDs are canonical and persisted references must not use unstable numeric registry indices.
- No production code without a failing self-test first.
- Reload swaps only fully validated candidate registries.
- Native API fields are append-only and guarded by `struct_size`.

## Review Focus

- Duplicate IDs and cyclic tags: validation rejects them without partial registration.
- Missing references and dependency cycles: diagnostics identify mod, file, and dependency.
- Save data with removed/changed content: load is deterministic and reports migrations.
- Lua errors and budget exhaustion: only the offending handler is disabled.
- Large registries and inventories: lookup and serialization remain bounded and deterministic.

### Task 1: Adapt unified content registries

**Files:** create/modify `src/content.c`, `src/dfe.h`, `CMakeLists.txt`; adapt `MANUS_BRANCH_CODE/content.c`.

**Tests:** duplicate IDs, tags, item definitions, recipe/loot references, atomic reload, 2000+ registrations.

### Task 2: General items and inventory

**Files:** `src/inventory.c`, `src/dfe.h`, `src/save.c`, `src/script.c`, `include/dfe_api.h`.

**Tests:** metadata/durability/equipment, stacking, serialization round trip, Lua item access, legacy block inventory migration.

### Task 3: Containers

**Files:** content/inventory/save/script integration; container runtime module if needed.

**Tests:** transfer rules, save/load, open/close interaction, Lua access, invalid item handling.

### Task 4: Events and interaction

**Files:** `src/interact.c`, `src/entity.c`, `src/inventory.c`, `src/player.c`, `src/server.c`, `src/script.c`, API header.

**Tests:** cancellation and ordering for block, item, entity, inventory, container, tick, and player events.

### Task 5: Recipes and loot

**Files:** content runtime and Lua/API bindings; schemas under `sdk/schemas/`.

**Tests:** shaped, shapeless, processing, tag ingredients, deterministic loot, malformed definitions.

### Task 6: Persistent player framework

**Files:** `src/player.c`, `src/save.c`, `src/entity.c`, `src/script.c`, API header.

**Tests:** identity, inventory/equipment, health/death/respawn, spawn points, interaction state, player data persistence.

### Task 7: Commands

**Files:** `src/console.c`, `src/script.c`, `include/dfe_api.h`.

**Tests:** typed arguments, permissions, completion, execution errors, Lua registration, duplicate command rejection.

### Task 8: Hot reload

**Files:** `src/reload.c`, `src/mods.c`, content/script integration.

**Tests:** Lua/data reload, invalid candidate rollback, recipes/tags/loot/structures, active-world safety.

### Task 9: SDK and CLI

**Files:** `tools/dfe_mod.py`, `sdk/`, `src/mods.c`, `CMakeLists.txt`.

**Tests:** validate missing refs/schema/dependencies/API, isolated mod test, deterministic package contents.

### Task 10: Versioning and compatibility

**Files:** `include/dfe_api.h`, `src/mods.c`, `src/save.c`, schemas and docs.

**Tests:** accepted ranges, clear incompatible failures, save migration gates, API struct-size compatibility.

### Task 11: Stress benchmarks

**Files:** `src/bench.c`, `tools/perf_matrix.py`, self-tests/benchmark fixtures.

**Tests:** registry, entity/chunk, Lua, state, dependency, save/load, structure workloads with memory/latency/hitch output.

### Task 12: Measurement-driven optimization and final verification

**Files:** only measured hotspots; documentation for benchmark baselines.

**Tests:** full self-test, sanitizer build, benchmark comparison, determinism checks, mod examples using APIs without engine edits.
