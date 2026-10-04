# Drainage-Aware Hydrology Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace noise-painted rivers with deterministic routed drainage that creates contained lakes and varied tributary networks.

**Architecture:** `src/gen.c` gains a bounded, seed-stable coarse hydrology grid that resolves drainage, flow accumulation, and channel types from uncarved terrain. A single fine-column hydrology query supplies carved terrain and wet-state data to near chunks, decoration rules, and footprint-aware LOD sampling.

**Tech Stack:** C17, FastNoiseLite, CMake, existing C self-test driver.

**Spec:** `docs/superpowers/specs/2026-10-03-drainage-aware-hydrology-design.md`

## Global Constraints

- Generation must be deterministic from seed and independent of chunk-generation order.
- Hydrology may use bounded derived regions but no mutable global generation state.
- Built-in generator behavior changes; custom biome data remains supported.
- Ocean water remains governed by `sea_level`.
- Near chunks and voxel LOD use the same hydrology result.

## Review Focus

- Region-edge routes: an emitted stream must have the same downstream continuation when sampled from either side of the region edge (Task 2).
- Flat/depressed terrain: water must be bounded by a computed spill path and never form an unbounded inland ocean (Task 2).
- Downstream gradient: each routed river water sample must be no higher than its predecessor (Task 2).
- LOD footprints: coarse water must never use a centre-sampled level above its sampled terrain footprint (Task 4).
- Hydrology variety: fixed seed coverage must include distinct channel orders and nonuniform tributary intervals, rather than repeated parallel bands (Task 3).

---

### Task 1: Establish hydrology query and regression harness

**Files:**
- Modify: `src/dfe.h:696-724`
- Modify: `src/gen.c:1-120`
- Modify: `src/selftest.c:624-702`

**Interfaces:**
- Produces: `typedef struct GenHydrologySample { float channel, bed_y, water_y; u8 type; bool wet; } GenHydrologySample;`
- Produces: `void gen_hydrology_at(float x, float z, GenHydrologySample *out);`
- Produces: test helpers that initialize the built-in worldgen and sample a fixed coordinate grid.

- [ ] **Step 1: Write failing public-query tests in `test_gen_determinism`**

Assert that two `gen_hydrology_at` calls at the same coordinate after `gen_init(777)` produce identical fields, that dry samples set `wet == false`, and that every wet sample has `bed_y < water_y`.

- [ ] **Step 2: Run the self-test to verify the new assertions fail**

Run: `./build/dfe --selftest`
Expected: FAIL because `GenHydrologySample` and `gen_hydrology_at` do not exist.

- [ ] **Step 3: Add the declared `GenHydrologySample` API and a temporary query implementation**

Define the result type in `src/dfe.h`; implement `gen_hydrology_at` in `src/gen.c` using the current terrain coordinate transform and uncarved terrain only. Do not alter chunk water emission in this task.

- [ ] **Step 4: Rebuild and run the self-test**

Run: `cmake --build build -j2 && ./build/dfe --selftest`
Expected: PASS; existing terrain results remain unchanged.

- [ ] **Step 5: Commit the query harness**

```bash
git add src/dfe.h src/gen.c src/selftest.c
git commit -m "test: add hydrology query harness"
```

### Task 2: Build deterministic region drainage and containment

**Files:**
- Modify: `src/gen.c:1-630`
- Modify: `src/selftest.c:624-702`

**Interfaces:**
- Consumes: `gen_hydrology_at(float, float, GenHydrologySample *)`.
- Produces: an internal region builder that samples uncarved `terrain_base_height`, resolves a drain/spill direction for every region cell, and accumulates upstream flow.
- Produces: wet samples whose downstream water surface never rises and whose routes end at sea or a bounded spill basin.

- [ ] **Step 1: Write failing drainage invariants in `test_gen_determinism`**

For a fixed sampling window and seed 777, assert that at least one wet non-ocean sample exists; following each sample's coarse downstream route reaches sea or a declared bounded lake within the region step limit; and each adjacent downstream water value is less than or equal to the prior value plus `0.01f`.

- [ ] **Step 2: Run the self-test to verify the drainage assertions fail**

Run: `./build/dfe --selftest`
Expected: FAIL because the temporary query has no route metadata/contained wet network.

- [ ] **Step 3: Implement the bounded hydrology-region builder in `src/gen.c`**

Use a fixed cell size, overlap margin, and deterministic region origin. Sample uncarved heights, mark ocean-connected cells, apply a priority-flood/spill resolution, select one lowest valid downstream neighbour, and accumulate flow in descending resolved-height order. Do not cache mutable state: build the same bounded region for every query. Classify only verified routes as wet; drain basins over the configured area/depth limit through their spill cell.

- [ ] **Step 4: Replace the temporary query with routed bed and water values**

Map a fine coordinate to its region cell/corridor. Return dry outside a classified channel or bounded lake; return a carved bed and a water level constrained between that bed and local banks within a wet corridor. Preserve `water_y <= sea_level` only for ocean samples.

- [ ] **Step 5: Rebuild and run the self-test**

Run: `cmake --build build -j2 && ./build/dfe --selftest`
Expected: PASS, including the new route, containment, and downstream-gradient assertions.

- [ ] **Step 6: Commit the routed drainage core**

```bash
git add src/gen.c src/selftest.c
git commit -m "feat: route deterministic drainage basins"
```

### Task 3: Carve varied channel orders into near terrain

**Files:**
- Modify: `src/gen.c:542-1015`
- Modify: `src/selftest.c:624-702`

**Interfaces:**
- Consumes: `gen_hydrology_at(float, float, GenHydrologySample *)`.
- Produces: `gen_height_at` and `gen_column` that use the routed sample for channel carving and river-water emission.

- [ ] **Step 1: Write failing variety and near-water tests**

Across a fixed multi-region grid for seed 777, assert that the query yields at least tributary and main-river types, that widths/channels have more than one distinct normalized strength, and that wet river columns produced by `gen_column` contain water only above a carved river bed. Keep the existing determinism comparison.

- [ ] **Step 2: Run the self-test to verify the new assertions fail**

Run: `./build/dfe --selftest`
Expected: FAIL because near terrain and chunk emission still use the retired noise river functions.

- [ ] **Step 3: Remove the former `river_strength_at`, `river_surface_at`, and `river_channel_at` overlay path**

Replace their callers with `gen_hydrology_at`. Use channel type/accumulated flow to derive a bounded width, depth, bank material strength, and seed-varied meander offset that cannot leave its routed corridor. Carve through `gen_height_at`; emit water in `gen_column` only when `sample.wet` is true and `y <= sample.water_y`.

- [ ] **Step 4: Update river-adjacent surface, tree, and plant rules**

Continue using normalized `sample.channel` for gravel/clay/mud selection and decoration suppression. Preserve existing optional-material fallbacks and custom-biome behavior.

- [ ] **Step 5: Rebuild and run the self-test**

Run: `cmake --build build -j2 && ./build/dfe --selftest`
Expected: PASS; river tests show deterministic multi-order, irregular networks and no dry channel water.

- [ ] **Step 6: Commit the near-terrain hydrology integration**

```bash
git add src/gen.c src/selftest.c
git commit -m "feat: carve varied routed river channels"
```

### Task 4: Make LOD hydrology footprint-safe

**Files:**
- Modify: `src/gen.c:1045-1135`
- Modify: `src/selftest.c:624-702`

**Interfaces:**
- Consumes: `gen_hydrology_at(float, float, GenHydrologySample *)`.
- Produces: LOD `top` and `water_top` derived from the same footprint samples.

- [ ] **Step 1: Write failing LOD continuity assertions**

For fixed LOD tiles crossing both a routed channel and a region boundary, assert `water_top <= top` only for dry columns, wet columns have `water_top > top`, and equivalent footprint queries produce the same water result on either tile border. Assert no LOD river top exceeds the maximum water level returned from the footprint's wet samples.

- [ ] **Step 2: Run the self-test to verify the LOD assertions fail**

Run: `./build/dfe --selftest`
Expected: FAIL because LOD combines a minimum terrain sample with centre-sampled river water.

- [ ] **Step 3: Extend `lod_sample_column` to aggregate hydrology over its terrain footprint**

Sample `gen_hydrology_at` at every terrain-footprint point used for height. Return the minimum carved terrain, whether any point is wet, and the conservative water top valid for that wet footprint; do not independently sample a river level at the centre.

- [ ] **Step 4: Use the aggregated result in `gen_lod_grid` and preserve material consistency**

Set `water_top` only from the footprint result. Feed the same representative normalized channel value into `surface_block`; retain sea-water behaviour outside routed channels.

- [ ] **Step 5: Rebuild and run full verification**

Run: `cmake --build build -j2 && ./build/dfe --selftest && cmake --build build-sanitize -j2 && ./build-sanitize/dfe --selftest`
Expected: both builds succeed; both self-test runs report zero failures.

- [ ] **Step 6: Commit the LOD integration**

```bash
git add src/gen.c src/selftest.c
git commit -m "fix: align lod water with routed hydrology"
```
