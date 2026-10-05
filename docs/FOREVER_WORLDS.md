# Forever Worlds

Forever Worlds is the first entry in the Experimental Features menu on the world create screen. When a world turns it
on, every column the generator makes is stamped with the **generation epoch** that made it. If a later engine or mod
version registers a newer epoch, new terrain is blended into the old terrain instead of ending at a wall at the chunk
border.

It is off by default. With it off nothing below happens: no metadata is written, no blend is computed, and the
generator runs the same code path as before (guarded by an identity flag, so the result is bit for bit the same).

## Turning it on

Create World → **Experimental Features** → toggle **Forever Worlds**. The registry of features is data:
`engine_assets/data/dfe/experimental_features.json` (schema `sdk/schemas/experimental_features.schema.json`). A world
records its choice in `world.json`:

```json
"experimental_features": { "forever_worlds": { "enabled": true, "enabled_version": "1.0.0", "enabled_at": "2026-10-04T12:00:00Z" } }
```

Turning a feature off after it was on asks for confirmation first (the warning text comes from `experimental_set`).
Turning it on for a world that already has chunks asks too: every existing chunk then counts as epoch 0.

## Generation Epoch Registry (GER)

One folder per epoch, `data/dfe/epochs/<id>/` (in the engine assets this is `engine_assets/data/dfe/epochs/<id>/`):

| file | contents |
|---|---|
| `noise.json` | `kernel`, `sea_level` (informational), `height_scale`, `height_offset`, `continentalness_bias`, `erosion_bias`, `weirdness_bias` |
| `biomes.json` | `kernel` (integer, 0 or 1, default 0), `temperature_bias`, `humidity_bias` |
| `caves.json` | `tunnel_width_scale`, `cheese_threshold_delta` |
| `surface.json` | `snowline_offset`, `treeline_offset` |
| `features.json` | `tree_chance_scale`, `plant_chance_scale`, `ore_chance_scale` |
| `structures.json` | `chance_scale` |
| `hash.txt` | lowercase hex SHA-256 of the six files |

The hash covers each file as `name NUL length(LE64) bytes`, in byte-wise alphabetical order of file name: `biomes.json`, `caves.json`, `features.json`, `noise.json`, `structures.json`, `surface.json` (the same order `src/epoch.c` and `tools/epoch_hash.py` use, not the table order). Regenerate it with
`tools/epoch_hash.py engine_assets/data/dfe/epochs/<id>`. Schema: `sdk/schemas/epoch.schema.json`.

Epoch 0 is the generator as it was before epochs existed: every value is the identity. A new epoch is a new folder
with the next id. **Epoch folders are frozen**; changing a file changes the hash, and a world that recorded the old
hash refuses to load.

Loading is strict. These are hard errors that stop the world from opening, each naming the file and key: a missing file,
an unexpected file, a hash mismatch, ids that are not 0..N−1, an unknown key, a value outside its range, a kernel other
than `terrain_v1`.

### Biome kernel and epoch 1

`biomes.json` `kernel` picks the biome selection: 0 is the original if-chain biomes; 1 reads the biome files under
`data/<ns>/biomes` (see [MODDING.md](MODDING.md#biome-files)). It is separate from the `kernel` string in `noise.json`,
which names the terrain kernel. The biome table keeps the eight original biomes at slots 0..7, so a biome index means
the same thing in both kernels and epoch blends can vote across them. Epoch 1 is epoch 0 plus `"kernel": 1`; the
hash covers the six files' bytes, so no hash code changed. Kernel 1 applies only while Forever Worlds is active and the
epoch is the current one. Existing worlds keep epoch 0 columns unchanged.

### What an epoch can change

An epoch is **data on top of a fixed kernel** (`terrain_v1`), not a copy of the generator code. Code cannot be stored in
a JSON file, so an epoch can only move the parameters listed above. A new algorithm needs a new kernel in the engine; the
`kernel` key names it, and an unknown kernel is an error. The hash covers the data files, not the code, so the engine
must keep every kernel it has shipped bit-stable.

## Chunk Generation Metadata (CGM)

One record per generated **column** (generation is per column, not per chunk), kept in `<save>/cgm.dat`: an append-only
log of `CGR1` records (cx, cz, length, payload, CRC-32); the newest record for a column wins. Schema:
`sdk/schemas/chunk_generation_metadata.schema.json`. A record holds:

- `epoch_id` and the 32-byte `epoch_hash` of that epoch;
- `seed_snapshot` (the derived generator seed);
- `terrain_params`, 64 bytes: the sixteen floats of the effective epoch parameters;
- `biome_weights`, 16 bytes: how much of the column each biome covers, 0..255;
- `structure_claims`, at most 8: id, bounding box (inclusive, world coordinates) and seed of each structure placed.

Bounds are enforced. A blob or claim list that does not fit is rejected with an error, never truncated; a column that
places more than 8 distinct structure ids reports it as a data error and records the first 8.

Opening a world fails if a record has a bad checksum, names an unknown epoch, or carries a hash that differs from the
registry's. Columns that exist on disk without a record (saved before the feature was on) count as epoch 0.

## The blend field

Radius **96** blocks by default, clamped to **[32, 256]**. A *blend plan* is built per column from the CGM: the older-epoch
columns within `radius` plus the structure claims that could touch the column. Interior columns of a same-epoch region
are dropped, so only the border costs anything. Plans are cached (64-slot LRU keyed by the registry generation,
reference counted).

At a block, with `d` the distance to the nearest older column and `R` the radius:

- the current epoch's weight is `smoothstep(d / R)`;
- older epochs share the rest, each in proportion to `1 / (d_i² + 0.25)`;
- at most 4 epochs take part (the current one and the 3 nearest older ones);
- a block inside an older column is exactly that epoch (weight 1).

Weights are a pure function of the block position and the set of boundary columns, so **generation order, thread count
and load order cannot change the terrain**.

Blended quantities:

| quantity | how |
|---|---|
| continentalness, erosion, weirdness, height | `ground + Σ aᵢ (rawᵢ − raw_current)`; exact when the epochs agree |
| biome | weighted vote of each epoch's own biome choice; ties go to the lower epoch |
| 3D cave density | `Σ aᵢ caveᵢ` at the cave lattice points |
| treeline, snowline | blended offsets |
| tree/plant/ore/structure chances | **not** blended as numbers (a half-way chance salts the seam); each cell takes one epoch's rule, and the seam carve decides where |

Structure bounding boxes of older epochs are **exclusion zones**: a new structure whose box overlaps one is skipped.
Trees keep 4 blocks of clearance beside an older column.

### Seam carving

For decoration, each cell of the blend band is assigned to the old or the new epoch by a **minimum cut** (push-relabel
on the 4-connected grid). Cells are forced to the old side where the current weight is ≤ 0.15 or an old claim covers
them, and to the new side where it is ≥ 0.85. Cutting an edge costs more where it would split a tree's root or a wet
cell, so the seam falls on bare ground. Afterwards the orphan pass removes plants with air or water below and lonely
leaves, and **re-runs plant placement** so valid cells get their plants back.

## Structure policy

A structure definition may carry:

```json
"forever_worlds_policy": { "on_water": "raise", "on_solid": "carve", "foundation_material": "base:stone" }
```

| `on_water` | when the footprint is wet |
|---|---|
| `ignore` (default) | place as before |
| `raise` | lift the structure to the waterline on a pillar of foundation |
| `carve_foundation` | replace water in the structure's cells and lay a foundation under its floor |
| `flood` | place as before, then fill open cells of the box below the waterline with water |

| `on_solid` | when the structure meets rock or earth |
|---|---|
| `ignore` (default) | existing blocks stay (`put_if_air`) |
| `carve` | the structure overwrites what is there |
| `reject_structure` | skip the structure if any of its cells is solid |

The policy applies only where a blend plan exists (the column borders an older epoch); elsewhere structures place
exactly as before, so the defaults change nothing.

## Testing and tools

```
./build/dfe --selftest                                         # everything, including the forever-worlds group
./build/dfe --headless --test-forever-worlds --seed 12345      # only the Forever Worlds tests (598 checks)
./build/dfe --headless --forever-bench --output after.json     # ms per column: off / epoch 0 only / 2 epochs / 3 epochs
tools/forever_perf.py --baseline before.json --current after.json
```

The tests (`src/selftest_forever.c`) cover the 33 cases of the spec. In real play only epoch 0 exists, so the blend is
exercised with **synthetic epoch folders** that the test writes to a temporary directory; they never ship.

## Known limits

- **CGM biome blob holds 16 entries.** With kernel 1 there can be up to 32 biomes; indices 16 and above are not recorded
  in a column's CGM biome weights (the existing recorder skips them), instead of the 16 highest-weight biomes.

- **Hydrology is not epoch-aware.** Rivers and lakes come from one drainage solve for the current epoch. An older
  epoch's height is `ground_y(current hydrology) + raw_old − raw_current`, so water bodies move when the epoch changes.
- `sea_level` in `noise.json` is informational; the engine still uses its configured sea level.
- Plant chance scales above 1 have no effect (plants can be thinned, not added, by an epoch).
- Structure claims with the same id in one column merge into one union box.
- A column generated but never saved leaves a stale record if the registry later grows.
- The hash covers data files, not engine code.
- There is no "mode" field at world creation; the feature is a toggle only. Existing worlds can turn it on through the
  library calls, but the Edit screen does not expose it yet.
- The per-world CGM is a sidecar (`cgm.dat`), not part of the region files; `world.json` carries `cgm_present`.
