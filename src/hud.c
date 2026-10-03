/* Heads-up display: crosshair, hotbar, break progress and the inventory screen.
 *
 * Item icons are small isometric cubes drawn once on the CPU from the block textures into one atlas, so a mod's
 * block gets an icon with no extra art and the HUD costs one texture bind.
 *
 * Slot drawing and grid placement come from ui_widgets.c: widget_slot paints frame/icon/count and reports hover,
 * layout_grid_cell turns (col, row) into a rect - the same seam mods use for custom slot art via UiIconFn. */
#include "dfe.h"
#include "ui_widgets.h"

#include <GLFW/glfw3.h>

#define ICON_PX 32
#define SLOT_PX 44.0f
#define SLOT_GAP 4.0f
#define HOTBAR_MARGIN 14.0f
#define CROSS_ARM 9.0f
#define CROSS_THICK 2.0f
#define NAME_TOAST_S 1.6
#define PALETTE_COLS 9
#define PALETTE_ROWS 5
#define PANEL_PAD 12.0f
#define TEXT_SIZE 16.0f

static struct {
    GLuint atlas;
    int cols;
    bool ready;
    bool inventory_open;
    int palette_scroll;
    int last_selected;
    double toast_until;
} H;

bool hud_init(void) { return true; }

void hud_shutdown(void) {
    if (H.atlas) glDeleteTextures(1, &H.atlas);
    memset(&H, 0, sizeof H);
}

bool hud_inventory_open(void) { return H.inventory_open; }

/* ------------------------------------------------------------------ icons */

static void sample_layer(int layer, float u, float v, float out[4]) {
    int t = g_tex.tile_size;
    int x = CLAMP((int)(u * (float)t), 0, t - 1), y = CLAMP((int)(v * (float)t), 0, t - 1);
    const u8 *p = g_tex.pixels + ((size_t)layer * t * t + (size_t)y * t + x) * 4;
    for (int i = 0; i < 4; i++) out[i] = (float)p[i] / 255.0f;
}

static void put_pixel(u8 *icon, int x, int y, const float c[4], float shade, const BlockDef *b, int d) {
    float tint[3] = {1, 1, 1};
    if (b->tint == TINT_GRASS && ((b->tint_mask >> d) & 1)) { tint[0] = 0.55f; tint[1] = 0.78f; tint[2] = 0.35f; }
    if (b->tint == TINT_FOLIAGE) { tint[0] = 0.42f; tint[1] = 0.66f; tint[2] = 0.30f; }
    u8 *o = icon + ((size_t)y * ICON_PX + x) * 4;
    for (int i = 0; i < 3; i++) o[i] = (u8)CLAMP((int)(c[i] * tint[i] * shade * 255.0f), 0, 255);
    o[3] = (u8)(c[3] * 255.0f);
}

/* Solves p = origin + u * a + v * b for (u, v). Returns false when the point is outside the unit square. */
static bool parallelogram_uv(float px, float py, const float o[2], const float a[2], const float b[2], float *u, float *v) {
    float det = a[0] * b[1] - a[1] * b[0];
    float dx = px - o[0], dy = py - o[1];
    *u = (dx * b[1] - dy * b[0]) / det;
    *v = (a[0] * dy - a[1] * dx) / det;
    return *u >= 0 && *u < 1 && *v >= 0 && *v < 1;
}

static void draw_cube_icon(u8 *icon, const BlockDef *b) {
    /* Top rhombus, then the left (south) and right (east) faces of an isometric cube filling the cell. */
    static const float TOP_O[2] = {16, 2}, TOP_A[2] = {14, 7}, TOP_B[2] = {-14, 7};
    static const float L_O[2] = {2, 9}, L_A[2] = {14, 7}, L_B[2] = {0, 15};
    static const float R_O[2] = {16, 16}, R_A[2] = {14, -7}, R_B[2] = {0, 15};
    for (int y = 0; y < ICON_PX; y++)
        for (int x = 0; x < ICON_PX; x++) {
            float u, v, c[4], px = (float)x + 0.5f, py = (float)y + 0.5f;
            if (parallelogram_uv(px, py, TOP_O, TOP_A, TOP_B, &u, &v)) { sample_layer(b->tex[DIR_PY], u, v, c); put_pixel(icon, x, y, c, 1.0f, b, DIR_PY); }
            else if (parallelogram_uv(px, py, L_O, L_A, L_B, &u, &v)) { sample_layer(b->tex[DIR_PZ], u, v, c); put_pixel(icon, x, y, c, 0.78f, b, DIR_PZ); }
            else if (parallelogram_uv(px, py, R_O, R_A, R_B, &u, &v)) { sample_layer(b->tex[DIR_PX], u, v, c); put_pixel(icon, x, y, c, 0.58f, b, DIR_PX); }
        }
}

static void draw_flat_icon(u8 *icon, const BlockDef *b) {
    const int margin = 4, size = ICON_PX - 2 * margin;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float c[4];
            sample_layer(b->tex[0], ((float)x + 0.5f) / (float)size, ((float)y + 0.5f) / (float)size, c);
            put_pixel(icon, x + margin, y + margin, c, 1.0f, b, DIR_PY);
        }
}

void hud_build_icons(void) {
    if (H.atlas) { glDeleteTextures(1, &H.atlas); H.atlas = 0; }
    int n = MAX(g_block_count, 1);
    H.cols = 1;
    while (H.cols * H.cols < n) H.cols++;
    int dim = H.cols * ICON_PX;
    u8 *atlas = xcalloc((size_t)dim * dim, 4);
    u8 *icon = xmalloc((size_t)ICON_PX * ICON_PX * 4);
    for (int i = 0; i < g_block_count; i++) {
        const BlockDef *b = g_blocks[i];
        if (b->shape == SHAPE_NONE) continue;
        memset(icon, 0, (size_t)ICON_PX * ICON_PX * 4);
        if (b->shape == SHAPE_CUBE || b->shape == SHAPE_FLUID) draw_cube_icon(icon, b); else draw_flat_icon(icon, b);
        int ox = (i % H.cols) * ICON_PX, oy = (i / H.cols) * ICON_PX;
        for (int y = 0; y < ICON_PX; y++) memcpy(atlas + ((size_t)(oy + y) * dim + ox) * 4, icon + (size_t)y * ICON_PX * 4, ICON_PX * 4);
    }
    glGenTextures(1, &H.atlas);
    glBindTexture(GL_TEXTURE_2D, H.atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dim, dim, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(atlas);
    free(icon);
    H.ready = true;
}

static void draw_icon(u16 state, float x, float y, float size) {
    const BlockDef *b = block_of_state(state);
    if (!b || !H.ready) return;
    float cell = 1.0f / (float)H.cols;
    float u0 = (float)(b->id % H.cols) * cell, v0 = (float)(b->id / H.cols) * cell;
    ui_image(H.atlas, x, y, size, size, u0, v0, u0 + cell, v0 + cell, rgba(255, 255, 255, 255));
}

/* ------------------------------------------------------------------ slots */

/* One grid cell of a `cols`-wide slot grid whose natural top-left is (left, top). The widget library only knows
 * boxes, so the HUD builds an anchor box here and layout_grid_cell does the arithmetic - no hand-added offsets. */
static UiBox slot_cell(float left, float top, int cols, int col, int row) {
    UiBox anchor = {left, top, (float)cols * SLOT_PX + (float)(cols - 1) * SLOT_GAP, SLOT_PX, UI_STACK_V, SLOT_GAP, true};
    return layout_grid_cell(&anchor, cols, 0, SLOT_GAP, row * cols + col);
}

/* Draws one slot through the generic widget; the local draw_icon above is the icon callback, which is exactly the
 * seam a mod would replace to paint custom slot art without the widget knowing anything about blocks. */
static bool show_slot(const ItemStack *s, float left, float top, int cols, int col, int row, bool selected) {
    UiBox cell = slot_cell(left, top, cols, col, row);
    return widget_slot(&cell, s->state, s->count, selected, draw_icon);
}

static float row_width(int cols) { return (float)cols * SLOT_PX + (float)(cols - 1) * SLOT_GAP; }

/* --------------------------------------------------------- inventory view */

typedef struct Layout { float left, palette_top, main_top, hotbar_top, panel_x, panel_y, panel_w, panel_h; } Layout;

static Layout inventory_layout(int width, int height) {
    Layout l;
    float grid_w = row_width(INV_HOTBAR);
    int palette_rows = g_creative ? PALETTE_ROWS : 0;
    float rows_h = (float)(palette_rows + 3 + 1) * (SLOT_PX + SLOT_GAP) + (g_creative ? PANEL_PAD : 0) + PANEL_PAD;
    l.panel_w = grid_w + 2 * PANEL_PAD;
    l.panel_h = rows_h + PANEL_PAD + TEXT_SIZE;
    l.panel_x = ((float)width - l.panel_w) * 0.5f;
    l.panel_y = ((float)height - l.panel_h) * 0.5f;
    l.left = l.panel_x + PANEL_PAD;
    l.palette_top = l.panel_y + PANEL_PAD + TEXT_SIZE;
    l.main_top = l.palette_top + (float)palette_rows * (SLOT_PX + SLOT_GAP) + (g_creative ? PANEL_PAD : 0);
    l.hotbar_top = l.main_top + 3.0f * (SLOT_PX + SLOT_GAP) + PANEL_PAD * 0.5f;
    return l;
}

static void draw_inventory(int width, int height) {
    ui_rect(0, 0, (float)width, (float)height, rgba(0, 0, 0, 120));
    Layout l = inventory_layout(width, height);
    ui_rect(l.panel_x, l.panel_y, l.panel_w, l.panel_h, rgba(34, 38, 48, 235));
    ui_text(l.left, l.panel_y + PANEL_PAD * 0.5f, TEXT_SIZE, rgba(220, 225, 235, 255), g_creative ? "Creative inventory" : "Inventory");
    const char *hover = NULL;
    if (g_creative) {
        int total = item_count();
        for (int r = 0; r < PALETTE_ROWS; r++)
            for (int c = 0; c < PALETTE_COLS; c++) {
                int idx = (r + H.palette_scroll) * PALETTE_COLS + c;
                ItemStack st = {idx < total ? item_state_at(idx) : STATE_AIR, idx < total ? 1 : 0};
                if (show_slot(&st, l.left, l.palette_top, PALETTE_COLS, c, r, false) && st.count) hover = item_name(st.state);
            }
    }
    for (int i = INV_HOTBAR; i < INV_SLOTS; i++) {
        if (show_slot(&g_inv.slot[i], l.left, l.main_top, INV_HOTBAR, (i - INV_HOTBAR) % INV_HOTBAR, (i - INV_HOTBAR) / INV_HOTBAR, false)
            && g_inv.slot[i].count) hover = item_name(g_inv.slot[i].state);
    }
    for (int i = 0; i < INV_HOTBAR; i++) {
        if (show_slot(&g_inv.slot[i], l.left, l.hotbar_top, INV_HOTBAR, i, 0, i == g_inv.selected)
            && g_inv.slot[i].count) hover = item_name(g_inv.slot[i].state);
    }
    if (g_inv.cursor.count) {
        draw_icon(g_inv.cursor.state, (float)g_in.mouse_x - 16, (float)g_in.mouse_y - 16, 32);
        char n[8];
        snprintf(n, sizeof n, "%d", g_inv.cursor.count);
        ui_text((float)g_in.mouse_x + 6, (float)g_in.mouse_y + 4, TEXT_SIZE * 0.85f, rgba(255, 255, 255, 255), n);
    } else if (hover) {
        float w = ui_text_width(TEXT_SIZE, hover);
        ui_rect((float)g_in.mouse_x + 10, (float)g_in.mouse_y - 4, w + 10, TEXT_SIZE + 8, rgba(10, 10, 16, 230));
        ui_text((float)g_in.mouse_x + 15, (float)g_in.mouse_y, TEXT_SIZE, rgba(240, 240, 250, 255), hover);
    }
}

static void click_palette(const Layout *l) {
    int total = item_count();
    for (int r = 0; r < PALETTE_ROWS; r++)
        for (int c = 0; c < PALETTE_COLS; c++) {
            int idx = (r + H.palette_scroll) * PALETTE_COLS + c;
            UiBox cell = slot_cell(l->left, l->palette_top, PALETTE_COLS, c, r);
            if (idx >= total || !ui_hover(cell.x, cell.y, cell.w, cell.h)) continue;
            for (int b = 0; b < 2; b++) {
                if (!g_in.mouse_pressed[b]) continue;
                g_inv.cursor.state = item_state_at(idx);
                g_inv.cursor.count = (u8)(b == 0 ? INV_MAX_STACK : 1);
            }
        }
}

static void click_slots(const Layout *l) {
    for (int i = 0; i < INV_SLOTS; i++) {
        UiBox cell = i < INV_HOTBAR ? slot_cell(l->left, l->hotbar_top, INV_HOTBAR, i, 0)
                                    : slot_cell(l->left, l->main_top, INV_HOTBAR, (i - INV_HOTBAR) % INV_HOTBAR, (i - INV_HOTBAR) / INV_HOTBAR);
        if (!ui_hover(cell.x, cell.y, cell.w, cell.h)) continue;
        for (int b = 0; b < 2; b++) if (g_in.mouse_pressed[b]) inventory_click(&g_inv, i, b);
    }
}

void hud_set_inventory_open(bool open) {
    if (open == H.inventory_open) return;
    H.inventory_open = open;
    if (!open && g_inv.cursor.count) { /* what the mouse still holds goes back to the bag, or is discarded if full */
        inventory_add(&g_inv, g_inv.cursor.state, g_inv.cursor.count);
        g_inv.cursor = (ItemStack){0};
    }
    window_set_cursor_captured(!open);
}

void hud_update(void) {
    if (!H.inventory_open) return;
    Layout l = inventory_layout(g_win.width, g_win.height);
    int rows = (item_count() + PALETTE_COLS - 1) / PALETTE_COLS;
    if (g_creative) {
        H.palette_scroll = CLAMP(H.palette_scroll - (int)g_in.scroll, 0, MAX(rows - PALETTE_ROWS, 0));
        click_palette(&l);
    }
    click_slots(&l);
}

/* ------------------------------------------------------------------- HUD */

static void draw_crosshair(int width, int height) {
    float cx = (float)width * 0.5f, cy = (float)height * 0.5f;
    u32 c = rgba(255, 255, 255, 210);
    ui_rect(cx - CROSS_ARM, cy - CROSS_THICK * 0.5f, CROSS_ARM * 2, CROSS_THICK, c);
    ui_rect(cx - CROSS_THICK * 0.5f, cy - CROSS_ARM, CROSS_THICK, CROSS_ARM * 2, c);
    if (g_interact.breaking && g_interact.break_progress > 0) {
        float w = 44.0f;
        ui_rect(cx - w * 0.5f, cy + 18, w, 4, rgba(0, 0, 0, 150));
        ui_rect(cx - w * 0.5f, cy + 18, w * CLAMP(g_interact.break_progress, 0.0f, 1.0f), 4, rgba(255, 255, 255, 230));
    }
}

static void draw_hurt_cracks(int width, int height) {
    float amount = CLAMP(g_player.hurt_timer / 0.8f, 0.0f, 1.0f);
    if (amount <= 0.0f || g_player.dead) return;
    static const float paths[][8] = {
        {0.00f, 0.12f, 0.16f, 0.24f, 0.20f, 0.38f, 0.32f, 0.44f},
        {0.00f, 0.12f, 0.11f, 0.08f, 0.20f, 0.00f, 0.27f, 0.09f},
        {1.00f, 0.18f, 0.84f, 0.25f, 0.79f, 0.40f, 0.68f, 0.47f},
        {1.00f, 0.18f, 0.88f, 0.08f, 0.78f, 0.00f, 0.72f, 0.10f},
        {0.00f, 0.85f, 0.16f, 0.76f, 0.22f, 0.61f, 0.33f, 0.55f},
        {1.00f, 0.88f, 0.85f, 0.78f, 0.79f, 0.64f, 0.67f, 0.57f},
    };
    u32 color = rgba(235, 242, 245, (int)(190.0f * amount));
    for (int i = 0; i < ARRAY_LEN(paths); i++)
        for (int j = 0; j < 3; j++)
            ui_line(paths[i][j * 2] * width, paths[i][j * 2 + 1] * height,
                    paths[i][j * 2 + 2] * width, paths[i][j * 2 + 3] * height, 1.5f, color);
}

static void draw_health(int width) {
    float ratio = CLAMP(g_player.health / PLAYER_MAX_HEALTH, 0.0f, 1.0f);
    float x = ((float)width - 160.0f) * 0.5f;
    ui_rect(x, 14.0f, 160.0f, 8.0f, rgba(10, 10, 12, 180));
    ui_rect(x, 14.0f, 160.0f * ratio, 8.0f, rgba(196, 54, 47, 230));
}

static void draw_death(int width, int height) {
    ui_rect(0, 0, (float)width, (float)height, rgba(48, 8, 10, 190));
    const char *title = "You died";
    const char *hint = "Press Space to respawn";
    ui_text(((float)width - ui_text_width(36.0f, title)) * 0.5f, (float)height * 0.43f, 36.0f, rgba(255, 235, 230, 255), title);
    ui_text(((float)width - ui_text_width(TEXT_SIZE, hint)) * 0.5f, (float)height * 0.52f, TEXT_SIZE, rgba(245, 220, 215, 255), hint);
}

static void draw_hotbar(int width, int height) {
    float left = ((float)width - row_width(INV_HOTBAR)) * 0.5f, top = (float)height - SLOT_PX - HOTBAR_MARGIN;
    for (int i = 0; i < INV_HOTBAR; i++) show_slot(&g_inv.slot[i], left, top, INV_HOTBAR, i, 0, i == g_inv.selected);
    if (H.last_selected != g_inv.selected) {
        H.last_selected = g_inv.selected;
        H.toast_until = time_now_s() + NAME_TOAST_S;
    }
    const ItemStack *held = &g_inv.slot[g_inv.selected];
    if (held->count && time_now_s() < H.toast_until) {
        const char *n = item_name(held->state);
        float w = ui_text_width(TEXT_SIZE, n);
        ui_text(((float)width - w) * 0.5f, top - TEXT_SIZE - 10, TEXT_SIZE, rgba(255, 255, 255, 235), n);
    }
}

void hud_draw(int width, int height) {
    if (!H.ready) return;
    if (g_player.dead) { draw_death(width, height); return; }
    draw_hurt_cracks(width, height);
    draw_health(width);
    draw_crosshair(width, height);
    draw_hotbar(width, height);
    if (H.inventory_open) draw_inventory(width, height);
}
