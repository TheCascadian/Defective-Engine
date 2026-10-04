#pragma once
#include "dfe.h"

/* Data-driven UI icons: names map to cells of one atlas texture (assets/dfe/ui/icons.json + icons.png).
 * Mods add or replace icons with assets/<modid>/ui/icons.json + icons.png; packs are stitched into the same atlas. */
bool icons_load(void);   /* the previous icon set stays on any error */
bool icons_reload(void);
void icons_shutdown(void);
bool icons_find(const char *name, float uv[4]); /* unknown names get the "missing" icon; false only when that is absent too */
bool icons_has(const char *name);               /* true only for icons really defined */
void icons_draw(const char *name, float x, float y, float size, u32 color); /* no-op without a GL context */
int icons_count(void);
#define ICON_PART_STEPS 16.0f
void icons_draw_part(const char *name, float x, float y, float size, float frac, u32 color); /* left part, cut on a whole icon pixel */
