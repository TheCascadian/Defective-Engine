# Drainage-Aware Hydrology Design

## Goal

Replace the noise-overlay river system with deterministic drainage-aware hydrology. Rivers must be continuous routes to the sea, water must not spread as an unbounded inland flood plane, and tributaries must vary in placement, order, width, and confluence shape.

## Scope and constraints

- World generation remains deterministic from the world seed and independent of chunk-generation order.
- Near chunks and voxel LOD use one hydrology query, so they cannot disagree about channels or water surfaces.
- The solution stays compatible with streaming generation: it may derive information from a bounded, deterministic region but cannot retain mutable global generation state.
- Hydrology changes apply to the built-in generator only; custom biome data remains supported.
- Existing sea generation continues to use `sea_level`.

## Architecture

### Hydrology regions

Partition world coordinates into overlapping, seed-stable hydrology regions. A region samples the uncarved terrain base on a coarse grid with a margin. The margin makes routes that cross a region edge agree with the adjacent region. Each sample records elevation, whether it is ocean-connected, a drainage direction, accumulated upstream flow, and a channel classification.

The region builder resolves depressions through a deterministic priority-flood/spill pass. A depression either receives a lowest spill outlet or is classified as a bounded lake only when its spill height and area meet configured limits. Every non-lake channel cell therefore has a downstream successor and ultimately reaches the ocean.

### Flow network and channel types

Flow accumulation is computed from the resolved drainage directions. Thresholds derived from local relief, moisture, and seeded regional variation classify cells as ephemeral stream, tributary, main river, or lake outlet. A seeded field controls source density, threshold variation, bank-side branching, meander amplitude, and widths, while accumulated flow remains the primary ordering signal. This produces asymmetric, irregular basins and genuine merging confluences rather than nested parallel noise lines.

Fine-resolution queries interpolate the coarse route and apply bounded local meanders that preserve the coarse downstream path. The channel profile uses its accumulated flow/type to determine width and bed depth. It cannot widen outside a routed corridor.

### Terrain and water query

`gen_height_at` first evaluates uncarved terrain and then subtracts the routed channel profile. A hydrology query returns channel strength, channel type, bed elevation, and water-surface elevation.

The water surface is monotonically non-increasing along a route. For a river it is below its uncarved bank elevation and above the carved bed. For a lake it is the resolved spill elevation and water is emitted only inside the bounded flooded basin. Ocean water remains governed solely by sea level.

Chunk filling emits water only when the shared hydrology result marks the queried column as wet. The LOD generator takes terrain and water values from the same footprint-aware hydrology query; it must not combine minimum sampled terrain with a centre-sampled water level.

### Materials and decoration

Existing river material selection continues to consume normalized channel strength, with channel type adding modest material variation. Tree and plant suppression query the routed corridor rather than the former noise mask.

## Failure handling

- If a region route ends at an unresolved edge, the region margin is expanded/rebuilt deterministically; the generator never emits a river from an unverified route.
- Invalid or unavailable optional material roles retain their existing fallback block behavior.
- A lake that exceeds bounded area/depth limits is drained through its computed spillway rather than becoming an inland sea.

## Tests

- Determinism: generate the same region/chunks in different orders and compare results.
- Connectivity: every emitted river sample reaches a sea outlet or a bounded lake with a verified spill path; water surfaces do not rise downstream.
- Containment: no non-ocean wet area may exceed the lake limits without a route to sea.
- Continuity: channels and water levels agree across chunk and hydrology-region boundaries, including LOD sampling.
- Variety: fixed-seed samples contain multiple channel types, nonuniform tributary spacing/widths, and nonparallel confluences.
- Regression: preserve the current smooth-height and general world-generation tests, updating only expectations superseded by routed hydrology.

## Non-goals

- Continuous runtime fluid simulation, sediment transport, and dynamic erosion are out of scope.
- Exact real-world watershed modelling is not required; visual plausibility, drainage guarantees, and reproducibility are the acceptance criteria.
