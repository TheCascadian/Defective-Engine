#include "dfe.h"

Inventory g_inv;
bool g_creative = true;

static const char *stack_id(const ItemStack *s) { return s->item_id[0] ? s->item_id : (s->count ? item_name(s->state) : ""); }
static bool item_block(const BlockDef *b) { return b->shape != SHAPE_NONE && !(b->flags & BF_NO_ITEM) && strcmp(b->name, "dfe:missing"); }
int item_count(void) { int n=0; for(int i=0;i<g_block_count;i++) if(item_block(g_blocks[i])) n++; return n; }
u16 item_state_at(int index) { for(int i=0;i<g_block_count;i++) if(item_block(g_blocks[i])&&index--==0) return g_blocks[i]->default_state; return STATE_AIR; }
const char *item_name(u16 state) { const BlockDef *b=block_of_state(state); return b?b->name:""; }
u16 item_for_block(const BlockDef *b) { if(!b||!b->drop[0])return STATE_AIR; const BlockDef *d=strcmp(b->drop,b->name)?block_find(b->drop):b; return d&&item_block(d)?d->default_state:STATE_AIR; }
void inventory_clear(Inventory *inv) { memset(inv,0,sizeof *inv); }
int inventory_add(Inventory *inv,u16 state,int count) { if(state==STATE_AIR)return MAX(count,0); return inventory_add_item(inv,item_name(state),count); }
int inventory_count(const Inventory *inv,u16 state) { int n=0;for(int i=0;i<INV_SLOTS;i++)if(inv->slot[i].count&&inv->slot[i].state==state)n+=inv->slot[i].count;return n; }
bool inventory_take_one(Inventory *inv,int slot) { if(slot<0||slot>=INV_SLOTS)return false;ItemStack*s=&inv->slot[slot];if(!s->count)return false;if(--s->count==0)memset(s,0,sizeof *s);return true; }
static void normalise(ItemStack *s) { if(!s->count)memset(s,0,sizeof *s); }
void inventory_click(Inventory *inv,int slot,int button) {
    if(slot<0||slot>=INV_SLOTS)return;
    ItemStack*s=&inv->slot[slot],*c=&inv->cursor;
    bool same=s->count&&c->count&&!strcmp(stack_id(s),stack_id(c))&&!strcmp(s->metadata,c->metadata)&&s->durability==c->durability;
    int lim=item_find(stack_id(s))?item_find(stack_id(s))->max_stack:INV_MAX_STACK;
    if(button==0) {
        if(same){int put=MIN((int)c->count,MAX(0,lim-s->count));s->count+=put;c->count-=put;}
        else {ItemStack t=*s;*s=*c;*c=t;}
    } else if(!c->count&&s->count) {int take=(s->count+1)/2;*c=*s;c->count=(u8)take;s->count-=take;}
    else if(c->count&&(!s->count||same)) {lim=item_find(stack_id(c))?item_find(stack_id(c))->max_stack:INV_MAX_STACK;if(s->count<lim){if(!s->count){*s=*c;s->count=0;}s->count++;c->count--;}}
    normalise(s);normalise(c);
}
_Static_assert(SAVE_INV_SLOTS==INV_SLOTS,"save record must match the inventory size");
void inventory_store(const Inventory *inv,bool creative,SaveMeta *m) {
    m->has_inventory=true;m->creative=creative;m->selected=inv->selected;
    for(int i=0;i<INV_SLOTS;i++){const ItemStack*s=&inv->slot[i];m->inv_count[i]=s->count;snprintf(m->inv_name[i],SAVE_BLOCK_NAME_LEN,"%s",s->count?item_name(s->state):"");snprintf(m->inv_item[i],SAVE_BLOCK_NAME_LEN,"%s",s->count?stack_id(s):"");snprintf(m->inv_meta[i],SAVE_ITEM_META_LEN,"%s",s->metadata);m->inv_durability[i]=s->durability;}
    for(int i=0;i<4;i++){const ItemStack*s=&inv->equipment[i];snprintf(m->equip_item[i],SAVE_BLOCK_NAME_LEN,"%s",s->count?stack_id(s):"");snprintf(m->equip_meta[i],SAVE_ITEM_META_LEN,"%s",s->metadata);m->equip_durability[i]=s->durability;}
}
void inventory_restore(Inventory *inv,bool *creative,const SaveMeta *m) {
    inventory_clear(inv);*creative=m->creative;inv->selected=CLAMP(m->selected,0,INV_HOTBAR-1);
    for(int i=0;i<INV_SLOTS;i++){
        const char*id=m->inv_item[i][0]?m->inv_item[i]:m->inv_name[i];if(!id[0]||!m->inv_count[i])continue;
        const ItemDef*d=item_find(id);const BlockDef*b=block_find(id);if(!d&&!b)continue;
        ItemStack*s=&inv->slot[i];s->state=d&&d->place[0]?block_parse_state(d->place):b?b->default_state:STATE_AIR;s->count=m->inv_count[i];s->durability=m->inv_durability[i];snprintf(s->item_id,sizeof s->item_id,"%s",id);snprintf(s->metadata,sizeof s->metadata,"%s",m->inv_meta[i]);
    }
    for(int i=0;i<4;i++){
        const char*id=m->equip_item[i];if(!id[0])continue;const ItemDef*d=item_find(id);if(!d||d->equip_slot!=i)continue;
        ItemStack*s=&inv->equipment[i];s->state=d->place[0]?block_parse_state(d->place):STATE_AIR;s->count=1;s->durability=m->equip_durability[i];snprintf(s->item_id,sizeof s->item_id,"%s",id);snprintf(s->metadata,sizeof s->metadata,"%s",m->equip_meta[i]);
    }
}
void inventory_starter(Inventory *inv) {
    static const char *const names[]={"base:stone","base:dirt","base:planks","base:glass","base:cobblestone","base:log","base:lantern","base:sand","base:water"};int slot=0;
    for(size_t i=0;i<sizeof names/sizeof *names&&slot<INV_HOTBAR;i++){const BlockDef*b=block_find(names[i]);if(!b||!item_block(b))continue;inv->slot[slot].state=b->default_state;inv->slot[slot].count=INV_MAX_STACK;snprintf(inv->slot[slot].item_id,sizeof inv->slot[slot].item_id,"%s",b->name);slot++;}
}
bool inventory_equip(Inventory *inv,int slot) { if(slot<0||slot>=INV_SLOTS)return false;ItemStack*s=&inv->slot[slot];if(!s->count)return false;const ItemDef*d=item_find(stack_id(s));if(!d||d->equip_slot<0||d->equip_slot>=4)return false;ItemStack t=inv->equipment[d->equip_slot];inv->equipment[d->equip_slot]=*s;if(t.count)*s=t;else memset(s,0,sizeof *s);return true; }
float inventory_protection(const Inventory *inv) { float n=0;for(int i=0;i<4;i++){const ItemDef*d=item_find(stack_id(&inv->equipment[i]));if(d)n+=MAX(0,d->protection);}return n; }
void inventory_damage_slot(Inventory *inv,int slot,int amount) { if(slot<0||slot>=INV_SLOTS||amount<=0)return;ItemStack*s=&inv->slot[slot];const ItemDef*d=item_find(stack_id(s));if(!s->count||!d||d->durability<=0)return;if(amount>=d->durability-(int)s->durability){s->count--;s->durability=0;if(!s->count)memset(s,0,sizeof *s);}else s->durability=(u16)(s->durability+amount); }
void inventory_damage_equipment(Inventory *inv,int slot,int amount) { if(slot<0||slot>=4||amount<=0)return;ItemStack*s=&inv->equipment[slot];const ItemDef*d=item_find(stack_id(s));if(!s->count||!d||d->durability<=0)return;if(amount>=d->durability-(int)s->durability)memset(s,0,sizeof *s);else s->durability=(u16)(s->durability+amount); }
