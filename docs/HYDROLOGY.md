# Hydrology

Rivers, lakes, estuaries and the sea are produced by one deterministic, region-scale drainage solve in `src/hydro.c`. `gen.c` turns the result into terrain, biome moisture, plants, trees, ores, structures and entity spawn rules.

## Drainage model

* The world is split into regions of 512 x 512 blocks. A region is solved with a 3 x 3 window of regions (1536 blocks across) so the core never sees a truncated catchment at its own edge.
* Elevation is sampled on a grid of 4-block cells from the raw terrain height (before any river shaping).
* A priority flood (Barnes) starts from ocean cells and window-border cells. Ties are broken by insertion order, so the result never depends on thread timing. The flood fills every closed depression up to its spill height, so every cell has a downhill path to the sea or the window edge.
* Flow accumulation runs on the flood order. A cell is a river once its catchment reaches `river_min_area` cells. Each cell drains to exactly one neighbour, so rivers merge at confluences and never split.
* Lakes are 4-connected groups of cells that the flood raised by at least `lake_min_depth`. A lake holds one flat surface at its spill level. A basin that is too large (`lake_max_cells`), too deep (`lake_max_depth`) or that touches the window border is left dry.

## Shape rules

* The water level follows the flood level, is smoothed along each path, never rises downstream, and never sits above the ground of its own cell.
* A drop across one cell of `fall_drop` or more is a waterfall (`GEN_HYD_FALL`), a drop of `rapids_drop` or more is rapids (`GEN_HYD_RAPIDS`). Smoothing never crosses a waterfall.
* Half-width grows with catchment and never shrinks downstream. It flares near the sea (`GEN_HYD_ESTUARY`). Depth is `depth_base + depth_scale * sqrt(half_width)`, at most 9 blocks.
* Cell paths are turned into Catmull-Rom centrelines and rasterised per region at 2 blocks per pixel, so there are no right-angle bends. Banks follow local slope between `bank_grad_min` and `bank_grad_max`, and the valley wall fades into the natural terrain over `valley_reach` blocks.
* Where two regions meet, results are blended with a smoothstep over 64 blocks, so there are no seams.

## Query API and contract

```c
void gen_hydrology_at(float x, float z, GenHydrologySample *out);
```

The function is pure in (seed, parameters, x, z). It does not depend on the call order, the thread, or previously generated chunks. Region solves are cached (12 slots, about 31 MB) behind a mutex; the cache only saves time. `GenSample` fields are listed in `src/dfe.h`: `channel`, `bed_y`, `water_y`, `downstream_x/z`, `ground_y`, `flow`, `water_dist`, `type` (1 stream, 2 river, 3 main river, 4 ocean, 5 lake), `flags` and `wet`. `gen_column` samples each column once and keeps the sample in `GenScratch`.

## Who uses it

* Biome moisture adds up to 0.55 for water within 24 blocks. Swamps need high moisture at low height or a low shore beside water.
* Plants never grow on wet or ocean columns. A band of tall grass (reeds) grows on bare ground within 6 blocks of water.
* Trees are rejected on wet columns, in channels and within 1.5 blocks of water. The check works across chunk borders.
* Features, structures and ores take `"habitat"`: `dry` (default for features and structures), `any` (default for ores), `river`, `lake`, `shore` or `wetland`, and an optional `"water_distance": {"min": 0, "max": 12}`.
* Entity types take `"habitat": "any" | "land" | "water"`. `entity_spawn` refuses a position that does not match.

## Parameters

The `"hydrology"` object in `worldgen/default.json` overrides any of `river_min_area`, `width_base`, `width_scale`, `width_exp`, `depth_base`, `depth_scale`, `bank_grad_min`, `bank_grad_max`, `valley_reach`, `lake_min_depth`, `lake_max_depth`, `lake_max_cells`, `fall_drop` and `rapids_drop`. Each value is clamped to a safe range (see `hydro_params_sanitize`); a changed value logs a data error naming the key.

## Diagnostics

```
dfe --dump-hydrology SEED X0 Z0 X1 Z1       TSV of samples along a line
dfe --dump-hydro-map SEED X Z SIZE STEP     ASCII map (~ ocean, = river, # main river, o lake, ^ fall, ; bank)
dfe --dump-rivers SEED                      river paths and outlets
dfe --dump-spawns SEED X Z RADIUS           habitat histogram of columns, trees and plants; exit 1 if any stands in water
dfe --compare-worldgen SEED A B             generate with order A and B (forward, reverse, spiral, shuffle) and diff
dfe --overlay N                             page "hydrology" shows habitat, levels, flow at the player
```

`dfe --selftest` runs the `hydrology` group (determinism, continuity, flow and shape, basins, spawns, visuals, cost).

## Caveats

* Catchments are truncated at the 3 x 3 window edge, so a very large river entering from far away has a smaller flow than it would in reality.
* Oversized or very deep basins are left dry rather than flooded.
* Deltas are a widening estuary, not branching distributaries.
* Results are stable for a given build and compiler. `-ffp-contract=off` is set for `gen.c` and `hydro.c` to avoid fused-multiply differences, but bit-identical results across different CPUs or libm versions are not promised.
* The region cache costs about 31 MB.
