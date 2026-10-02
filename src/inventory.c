/* Inventory: 36 slots of block items. An item is identified by the default state of the block it places, so
 * the registry is the only item table and a mod that adds a block adds an item with no extra file.
 *
 * Decision: stacks hold a state, not a block id, because states are what the world stores. Saves write the
 * block name instead (see save.c), so ids never leak into save data. */
#include "dfe.h"

Inventory g_inv;
bool g_creative = true;

static bool item_block(const BlockDef *b) {
    return b->shape != SHAPE_NONE && !(b->flags & BF_NO_ITEM) && strcmp(b->name, "dfe:missing") != 0;
}

int item_count(void) {
    int n = 0;
    for (int i = 0; i < g_block_count; i++) if (item_block(g_blocks[i])) n++;
    return n;
}

u16 item_state_at(int index) {
    for (int i = 0; i < g_block_count; i++)
        if (item_block(g_blocks[i]) && index-- == 0) return g_blocks[i]->default_state;
    return STATE_AIR;
}

const char *item_name(u16 state) {
    const BlockDef *b = block_of_state(state);
    return b ? b->name : "";
}

u16 item_for_block(const BlockDef *b) {
    if (!b->drop[0]) return STATE_AIR;
    const BlockDef *d = strcmp(b->drop, b->name) ? block_find(b->drop) : b;
    return d && item_block(d) ? d->default_state : STATE_AIR;
}

void inventory_clear(Inventory *inv) { memset(inv, 0, sizeof *inv); }

int inventory_add(Inventory *inv, u16 state, int count) {
    if (state == STATE_AIR) return 0;
    for (int i = 0; i < INV_SLOTS && count > 0; i++) {
        ItemStack *s = &inv->slot[i];
        if (s->state != state || s->count >= INV_MAX_STACK) continue;
        int put = MIN(count, INV_MAX_STACK - s->count);
        s->count = (u8)(s->count + put);
        count -= put;
    }
    for (int i = 0; i < INV_SLOTS && count > 0; i++) {
        ItemStack *s = &inv->slot[i];
        if (s->count) continue;
        int put = MIN(count, INV_MAX_STACK);
        s->state = state;
        s->count = (u8)put;
        count -= put;
    }
    return count;
}

int inventory_count(const Inventory *inv, u16 state) {
    int n = 0;
    for (int i = 0; i < INV_SLOTS; i++) if (inv->slot[i].count && inv->slot[i].state == state) n += inv->slot[i].count;
    return n;
}

bool inventory_take_one(Inventory *inv, int slot) {
    ItemStack *s = &inv->slot[slot];
    if (!s->count) return false;
    if (--s->count == 0) s->state = STATE_AIR;
    return true;
}

static void normalise(ItemStack *s) { if (!s->count) s->state = STATE_AIR; }

void inventory_click(Inventory *inv, int slot, int button) {
    if (slot < 0 || slot >= INV_SLOTS) return;
    ItemStack *s = &inv->slot[slot], *c = &inv->cursor;
    if (button == 0) {
        if (c->count && s->count && c->state == s->state) { /* merge into the stack */
            int put = MIN((int)c->count, INV_MAX_STACK - s->count);
            s->count = (u8)(s->count + put);
            c->count = (u8)(c->count - put);
        } else { /* swap, which also picks up or drops a whole stack */
            ItemStack t = *s;
            *s = *c;
            *c = t;
        }
    } else if (!c->count && s->count) { /* right click on a stack: pick up half, rounded up */
        int take = (s->count + 1) / 2;
        c->state = s->state;
        c->count = (u8)take;
        s->count = (u8)(s->count - take);
    } else if (c->count && (!s->count || s->state == c->state) && s->count < INV_MAX_STACK) { /* drop one */
        s->state = c->state;
        s->count++;
        c->count--;
    }
    normalise(s);
    normalise(c);
}

_Static_assert(SAVE_INV_SLOTS == INV_SLOTS, "save record must match the inventory size");

void inventory_store(const Inventory *inv, bool creative, SaveMeta *m) {
    m->has_inventory = true;
    m->creative = creative;
    m->selected = inv->selected;
    for (int i = 0; i < INV_SLOTS; i++) {
        const ItemStack *s = &inv->slot[i];
        m->inv_count[i] = s->count;
        snprintf(m->inv_name[i], SAVE_BLOCK_NAME_LEN, "%s", s->count ? item_name(s->state) : "");
    }
}

void inventory_restore(Inventory *inv, bool *creative, const SaveMeta *m) {
    inventory_clear(inv);
    *creative = m->creative;
    inv->selected = CLAMP(m->selected, 0, INV_HOTBAR - 1);
    for (int i = 0; i < INV_SLOTS; i++) {
        const BlockDef *b = m->inv_name[i][0] ? block_find(m->inv_name[i]) : NULL;
        if (!b || !m->inv_count[i]) continue;
        inv->slot[i].state = b->default_state;
        inv->slot[i].count = (u8)MIN((int)m->inv_count[i], INV_MAX_STACK);
    }
}

void inventory_starter(Inventory *inv) {
    static const char *const names[] = {"base:stone", "base:dirt", "base:planks", "base:glass", "base:cobblestone", "base:log", "base:lantern", "base:sand", "base:water"};
    int slot = 0;
    for (size_t i = 0; i < sizeof names / sizeof *names && slot < INV_HOTBAR; i++) {
        const BlockDef *b = block_find(names[i]);
        if (!b || !item_block(b)) continue;
        inv->slot[slot].state = b->default_state;
        inv->slot[slot].count = INV_MAX_STACK;
        slot++;
    }
}
