/* Private interface between gen.c (terrain, biomes, decoration) and hydro.c (region-scale drainage).
 * Nothing outside those two files includes this header. */
#ifndef DFE_GEN_INTERNAL_H
#define DFE_GEN_INTERNAL_H

#include "dfe.h"

/* Tunable drainage parameters, read from the "hydrology" object of data/<ns>/worldgen/default.json and clamped by
 * hydro_params_sanitize. Lengths are blocks unless the name says cells; a cell is HY_CELL blocks wide. */
typedef struct HydroParams {
    float river_min_area;   /* cells of upstream catchment before a stream exists (a cell is 16 square blocks) */
    float width_base;       /* full width of the narrowest stream, blocks */
    float width_scale;      /* extra full width per catchment_cells^width_exp */
    float width_exp;
    float depth_base, depth_scale; /* channel depth: base + scale * sqrt(half_width), at most 9 */
    float bank_grad_min, bank_grad_max; /* rise per block of the valley wall, flat ground to steep ground */
    float valley_reach;     /* blocks beyond a bank over which terrain is shaped toward the channel */
    float lake_min_depth;   /* blocks of fill above the natural ground before a depression becomes a lake */
    float lake_max_depth;   /* deeper depressions are left dry instead of becoming a bottomless pit of water */
    int lake_max_cells;     /* larger basins are left dry; an inland sea must come from the ocean field */
    float fall_drop;        /* water level drop across one cell (blocks) that makes a waterfall */
    float rapids_drop;      /* drop across one cell that makes rapids */
} HydroParams;

#define HY_CELL 4
#define HY_FAR 64.0f

void hydro_params_default(HydroParams *p);
/* Clamps every field into its safe range. Returns the number of values that had to change, and writes the name of
 * the first one into `first` so the caller can report it. */
int hydro_params_sanitize(HydroParams *p, char *first, size_t first_cap);

/* Terrain height (domain warp included) before any river or lake shaping, and the open-ocean test. gen.c owns the
 * noise; hydro.c only samples it. */
float gen_terrain_height_raw(float x, float z);
bool gen_ocean_cell(float x, float z);

enum { HYF_FALL = 1, HYF_RAPIDS = 2, HYF_ESTUARY = 4 };

typedef struct HydroRaw {
    float edge;        /* signed blocks from the nearest river bank: negative inside the channel, HY_FAR when none */
    float level;       /* river water surface */
    float half_width;
    float grade;       /* local ground slope that decides how steep the valley walls are, 0..1 */
    float flow;        /* upstream catchment in cells */
    float lake_level;  /* lake surface, valid where lakeness > 0 */
    float lakeness;    /* 0..1, share of the neighbourhood that belongs to a lake basin */
    float lake_dist;   /* blocks to the nearest lake basin, capped at HY_FAR */
    float dir_x, dir_z;
    u8 flags;
    bool river;        /* the river fields are valid */
} HydroRaw;

void hydro_init(u64 seed, int sea_level, const HydroParams *p);
void hydro_shutdown(void);
/* Pure in (seed, params, x, z): the answer never depends on which region was computed first or on any other call. */
void hydro_query(float x, float z, HydroRaw *out);

/* Diagnostics for the dump tools: flow-network cells of one region, and how many regions have been computed. */
/* flags: HYF_FALL / HYF_RAPIDS from the drop to the next cell, 0x80 for a lake cell. level is the water surface. */
typedef struct HydroCellInfo { int cx, cz; float level, flow, half_width; int down_cx, down_cz; u8 flags; bool lake_mouth; } HydroCellInfo;
int hydro_region_cells(int rx, int rz, HydroCellInfo *out, int cap);
int hydro_regions_built(void);

#endif
