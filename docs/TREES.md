# Trees

Trees are data. Every species is one JSON file, `data/<namespace>/trees/<name>.json`, validated against
[sdk/schemas/tree.schema.json](../sdk/schemas/tree.schema.json). The base game ships 11 in
`mods/base/data/base/trees/`: oak, birch, pine, spruce, palm, willow, acacia, mangrove, shrub, dead_snag, fallen_log.
A later species with the same id replaces an earlier one. With no species loaded the generator falls back to the original
single-shape trees.

## Species file

```json
{
  "id": "base:oak", "log": "base:log", "leaves": "base:leaves",
  "biomes": ["base:forest", "base:plains"],
  "climate": {"temperature": [0.35, 0.85], "moisture": [0.35, 1]},
  "substrate": ["base:grass_block"],
  "elevation": {"min_y": 60, "max_y": 150, "max_slope": 0.8},
  "water": {"min_distance": 0, "max_distance": 40, "prefer_near": false, "allow_shallow": false},
  "spacing": {"min_radius": 4, "cluster_size": 3, "clustering": 0.5},
  "density": 1.4, "succession": "climax", "layer": "canopy",
  "shape": {"type": "deciduous_broadleaf", "height": [8, 13]}
}
```

Required: `id`, `log`, `leaves`, `shape.type`. Everything else is optional. Ranges are a number or `[min, max]`.
Blocks must exist; use block ids only. Unknown fields, inverted ranges, out-of-bounds values and unknown shape types are
rejected with a data error naming the file and the field, and that species is skipped.

| Field | Meaning |
|---|---|
| `biomes` | Biomes it grows in. Omitted: all biomes (but with a custom biome set, only species that list biomes grow). |
| `climate` | Temperature and moisture ranges, 0..1. |
| `substrate` | Blocks the top layer may be made of. |
| `elevation` | `min_y`, `max_y` of the ground; `max_slope` in blocks per block. |
| `water` | Distance to the nearest water (rivers, lakes, sea). `prefer_near` raises the odds near water; `allow_shallow` allows wading depth up to 2. |
| `spacing` | `min_radius` is the least distance to a neighbour. `cluster_size` and `clustering` make groves. |
| `density` | Region-scale abundance, 0..4. |
| `succession` | `pioneer` favours young or open ground, `climax` mature stands. |
| `light` | Allowed range of the open-sky proxy (1 = open). |
| `layer` | `canopy` or `understory`. Understory and deadwood use their own site grid. |

### Shape fields

`type`, optional `canopy_shape`, then the ranges `height`, `trunk_radius`, `trunk_lean`, `trunk_taper`, `trunk_curve`,
`branch_count`, `branch_angle`, `branch_length_ratio`, `canopy_radius`, `canopy_density`, `canopy_flatness`,
`root_flare`, `buttress`, and the numbers `leaf_clumping`, `fork_chance`. See the schema for limits.

## Shape types

| Type | Look |
|---|---|
| `deciduous_broadleaf` | Thick tapering trunk, forks, irregular rounded crown, root flare. |
| `deciduous_slender` | Thin, tall, light crown. |
| `conifer_pine` | Tall bare trunk, layered crown high up. |
| `conifer_fir` | Conical crown of whorls down to low on the trunk. |
| `palm` | Curved unbranched trunk, frond crown. |
| `willow` | Leaning trunk, umbrella crown with drooping curtains. |
| `acacia` | Short forked trunk, flat umbrella crown. |
| `mangrove` | Stilt roots, bank growth, clustered crown. |
| `bush` | Low leaf mass, no trunk to speak of. |
| `dead_snag` | Bare broken trunk and a few stubs. |
| `fallen_log` | Horizontal trunk lying on the ground. |
| `custom` | Generic trunk and branches with the `canopy_shape` you choose. |

Canopy shapes: `irregular_sphere`, `conical`, `umbrella`, `drooping`, `layered`, `clustered`, `frond`, `none`.
Leaf density thins toward the crown edge. Every tree is pruned so each log connects to the root and each leaf to a log
through leaves; no floating leaves. Trees never exceed 15 blocks sideways, 40 trunk blocks, or displace water.

## Species added with kernel 1

Eleven species bind to the kernel 1 biomes: `autumn_oak_orange`, `autumn_oak_red`, `autumn_oak_yellow`, `cherry`,
`dark_oak`, `jungle_giant`, `mangrove_red`, `savanna_acacia`, `silver_birch`, `taiga_spruce` and `weeping_willow`. They
use the existing shape types only. A species file may carry `"kernel": 1` (only placed when the biome kernel is 1) and an
integer `salt` mixed into the placement hash, so two species sharing a biome do not stack on the same sites. The original
species name only the original biome ids, so they do not spread into the new biomes and epoch 0 output is unchanged.

## Placement

Placement is a pure function of (world seed, coordinates). Each layer has a jittered grid of candidate sites
(canopy cell 6, understory cell 8). For each site the sampler reads ground height, biome, climate, water, and, only if a
species survives the cheap checks, slope and surface block. Species are then weighted by density, region noise,
succession noise, light, water preference and grove clustering, and one is picked. Neighbouring sites are resolved by
priority (larger `min_radius` wins, then a hash), so no tree is closer than the rules allow. Hydrology comes from
`gen_hydrology_at`; trees never stand in rivers, lakes or the sea, and logs and roots never replace water.

## Determinism and chunk borders

The seed of a tree is `tree_seed(world_seed, site_x, site_z, layer)`; it never involves the chunk. A chunk asks for every
site whose tree can reach it (reach per species, at most 15) and draws only its own voxels, so two chunks always build
the same tree. Results do not depend on chunk order, thread count, or fresh start versus reload. Caches (a per-thread
site cache and shape cache) are keyed on everything the sampler reads and are dropped when species change. Shape code uses
an integer-degree sine table and `-ffp-contract=off` for identical results across platforms. Structures generated later
may still overwrite trees; orphan leaves are cleared.

## Performance

Measured with `./build/dfe --tree-bench SEED [RADIUS]`, which times chunk generation with the original trees and the
data-driven ones alternately. Typical ratio is about 1.03–1.06 (a few percent of total chunk time); the self-test only gates
at 1.35 because timing is noisy. The cost is mostly hydrology sampling at sites around the chunk.

## Diagnostics

```
./build/dfe --dump-trees SEED X0 Z0 X1 Z1        # TSV of trees plus per-species counts
./build/dfe --dump-tree-shape base:oak SEED X Z  # ASCII layers: L leaf, # log, R root, F fallen log
./build/dfe --tree-bench SEED [RADIUS]
./build/dfe --selftest                            # group "trees"
```

## Authoring a species

1. Create `data/<namespace>/trees/<name>.json` in your mod with `id`, `log`, `leaves` and a `shape.type`.
2. Add biomes, climate, substrate and elevation so it grows only where it should.
3. Check with `--dump-tree-shape` that it looks right, and `--dump-trees` that it appears where intended.
4. Read the data errors printed at load; a rejected species is skipped, the rest still load.
