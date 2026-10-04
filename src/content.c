#include "dfe.h"

#define CONTENT_MAX 8192
#define TAG_MAX 1024
#define TAG_ENTRIES 128
#define ITEM_MAX 4096
#define RECIPE_MAX 2048
#define LOOT_MAX 2048
#define ITEM_ID_LEN 64
#define ITEM_META_LEN 192

typedef struct ContentId { char id[ITEM_ID_LEN]; char kind[20]; } ContentId;
typedef struct Tag { char id[ITEM_ID_LEN]; char registry[20]; char mod[32]; char entries[TAG_ENTRIES][ITEM_ID_LEN]; int n; bool replace; } Tag;
typedef struct Ingredient { char id[ITEM_ID_LEN]; int count; } Ingredient;
typedef struct Recipe { char id[ITEM_ID_LEN], kind[12], pattern[3][4]; Ingredient ing[9]; int n, width, height, cell[9]; float process_time; ItemStack output; char output_id[ITEM_ID_LEN]; } Recipe;
typedef struct LootEntry { char item[ITEM_ID_LEN]; int min, max; float chance; } LootEntry;
typedef struct LootTable { char id[ITEM_ID_LEN]; LootEntry entries[32]; int n; } LootTable;
static ContentId ids[CONTENT_MAX]; static int ids_n;
static Tag tags[TAG_MAX]; static int tags_n;
static ItemDef items[ITEM_MAX]; static int items_n;
static Recipe recipes[RECIPE_MAX]; static int recipes_n;
static LootTable loots[LOOT_MAX]; static int loots_n;

static void full_id(char *out, size_t cap, const char *ns, const char *stem) { size_t a=strlen(ns),b=strlen(stem); if(!cap||a+1+b>=cap){if(cap)out[0]=0;return;} memcpy(out,ns,a);out[a]=':';memcpy(out+a+1,stem,b+1); }
static int json_int_default(const Json *j, const char *key, int d) { return json_int(j, key, d); }
static bool valid_id(const char *s) { const char *c = strchr(s, ':'); return c && c != s && c[1] && !strchr(c + 1, ':') && strlen(s) < ITEM_ID_LEN && strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789_:") == strlen(s); }
static bool json_file(const char *rel, const char *owner, Json **out) {
    size_t n = 0; u8 *p = vfs_read(rel, &n, NULL); if (!p) return false;
    char err[192]; int line = 0; *out = json_parse((char *)p, n, err, sizeof err, &line); free(p);
    if (!*out) data_error(owner, rel, line, "%s", err);
    return *out != NULL;
}
void content_reset(void) { ids_n = tags_n = items_n = recipes_n = loots_n = 0; memset(ids, 0, sizeof ids); memset(tags, 0, sizeof tags); memset(items, 0, sizeof items); memset(recipes, 0, sizeof recipes); memset(loots, 0, sizeof loots); }
int content_register(const char *kind, const char *id) {
    if (!valid_id(id)) return -1;
    for (int i=0;i<ids_n;i++) if (!strcmp(ids[i].id,id) && !strcmp(ids[i].kind,kind)) return i;
    if (ids_n >= CONTENT_MAX) return -1;
    snprintf(ids[ids_n].id,sizeof ids[ids_n].id,"%s",id); snprintf(ids[ids_n].kind,sizeof ids[ids_n].kind,"%s",kind); return ids_n++;
}
int content_count(const char *kind) { int n=0; for(int i=0;i<ids_n;i++) n += !kind || !strcmp(kind,ids[i].kind); return n; }
const char *content_id_at(const char *kind,int index) { for(int i=0;i<ids_n;i++) if(!kind || !strcmp(kind,ids[i].kind)) { if(index--==0)return ids[i].id; } return ""; }
const char *content_kind(const char *id) { for(int i=0;i<ids_n;i++) if(!strcmp(id,ids[i].id)) return ids[i].kind; return NULL; }
const ItemDef *item_find(const char *id) { for(int i=0;i<items_n;i++) if(!strcmp(id,items[i].id)) return &items[i]; return NULL; }
const ItemDef *item_for_state(u16 state) { const char *id=item_name(state); const ItemDef *d=item_find(id); return d; }
int item_definition_count(void) { return items_n; }
const ItemDef *item_definition_at(int i) { return i>=0&&i<items_n?&items[i]:NULL; }

static bool tag_member(const char *registry,const char *tag_id,const char *id,int depth) {
    if(depth>16)return false;
    for(int i=0;i<items_n;i++)if(!strcmp(items[i].id,id))for(int k=0;k<items[i].tag_n;k++)if(!strcmp(items[i].tags[k],tag_id))return true;
    for(int i=0;i<tags_n;i++)if(!strcmp(tags[i].registry,registry)&&!strcmp(tags[i].id,tag_id))for(int k=0;k<tags[i].n;k++){
        const char*e=tags[i].entries[k];if(e[0]=='#'){if(tag_member(registry,e+1,id,depth+1))return true;}else if(!strcmp(e,id))return true;
    }
    return false;
}
bool content_tag_contains(const char *registry,const char *tag_id,const char *id) {
    for(int i=0;i<tags_n;i++)if(!strcmp(tags[i].registry,registry)&&!strcmp(tags[i].id,tag_id))return tag_member(registry,tag_id,id,0);
    for(int i=0;i<items_n;i++)if(!strcmp(items[i].id,id))for(int k=0;k<items[i].tag_n;k++)if(!strcmp(items[i].tags[k],tag_id))return true;
    return false;
}
int content_tag_count(void) { return tags_n; }

static void load_tag_files(void) {
    StrList ns={0}; vfs_list("data",&ns);
    for(int a=0;a<ns.n;a++) { char root[180]; snprintf(root,sizeof root,"data/%s/tags",ns.d[a]); StrList regs={0}; vfs_list(root,&regs);
        for(int b=0;b<regs.n;b++) { char dir[220]; snprintf(dir,sizeof dir,"%s/%s",root,regs.d[b]); StrList files={0};vfs_list(dir,&files);
            for(int c=0;c<files.n;c++) { size_t ln=strlen(files.d[c]);if(ln<6||strcmp(files.d[c]+ln-5,".json"))continue;
                char rel[280],stem[ITEM_ID_LEN];snprintf(rel,sizeof rel,"%s/%s",dir,files.d[c]);snprintf(stem,sizeof stem,"%.*s",(int)MIN(ln-5,sizeof stem-1),files.d[c]);
                Json*j=NULL;if(!json_file(rel,ns.d[a],&j))continue;char tid[ITEM_ID_LEN];full_id(tid,sizeof tid,ns.d[a],stem);const char*target=json_str(j,"id","");if(target[0]){if(!valid_id(target)){data_error(ns.d[a],rel,j->line,"tag id must be a namespaced identifier");json_free(j);continue;}snprintf(tid,sizeof tid,"%s",target);}int ti=-1;for(int q=0;q<tags_n;q++)if(!strcmp(tags[q].id,tid)&&!strcmp(tags[q].registry,regs.d[b]))ti=q;if(ti<0&&tags_n<TAG_MAX)ti=tags_n++;if(ti<0){data_error(ns.d[a],rel,j->line,"tag registry capacity (%d) exceeded",TAG_MAX);json_free(j);continue;}Tag*t=&tags[ti];if(json_bool(j,"replace",false))t->n=0;snprintf(t->id,sizeof t->id,"%s",tid);snprintf(t->registry,sizeof t->registry,"%s",regs.d[b]);snprintf(t->mod,sizeof t->mod,"%s",ns.d[a]);t->replace=json_bool(j,"replace",false);const Json*arr=json_get(j,"values");if(!arr)arr=json_get(j,"entries");
                if(!arr||arr->type!=JSON_ARRAY)data_error(ns.d[a],rel,1,"tag file needs a values array of namespaced ids or #tag references");
                else { for(int k=0;k<arr->count&&t->n<TAG_ENTRIES;k++){const char*v=json_as_str(arr->items[k],"");if((v[0]=='#'&&valid_id(v+1))||valid_id(v))snprintf(t->entries[t->n++],ITEM_ID_LEN,"%s",v);else data_error(ns.d[a],rel,arr->items[k]->line,"invalid tag value '%s'",v);} content_register("tag",t->id); }
                json_free(j);
            }strlist_free(&files);
        }strlist_free(&regs);
    }strlist_free(&ns);
}

static bool parse_item(const char *ns,const char *rel,const char *stem) {
    Json*j=NULL;if(!json_file(rel,ns,&j))return false;if(j->type!=JSON_OBJECT){data_error(ns,rel,1,"item definition must be a JSON object");json_free(j);return false;}
    if(items_n>=ITEM_MAX){data_error(ns,rel,1,"item registry capacity (%d) exceeded",ITEM_MAX);json_free(j);return false;}
    ItemDef d={0};full_id(d.id,sizeof d.id,ns,stem);snprintf(d.kind,sizeof d.kind,"%s",json_str(j,"type","material"));snprintf(d.place,sizeof d.place,"%s",json_str(j,"place",""));
    d.max_stack=CLAMP(json_int_default(j,"max_stack",64),1,64);d.durability=CLAMP(json_int_default(j,"durability",0),0,65535);d.damage=(float)json_num(j,"damage",1.0);d.protection=(float)json_num(j,"protection",0);const char*slot=json_str(j,"slot","");d.equip_slot=!strcmp(slot,"head")?0:!strcmp(slot,"chest")?1:!strcmp(slot,"legs")?2:!strcmp(slot,"feet")?3:-1;
    if(d.equip_slot>=0)d.max_stack=1;
    const Json*a=json_get(j,"tags");for(int i=0;a&&a->type==JSON_ARRAY&&i<a->count&&d.tag_n<8;i++){const char*tag=json_as_str(a->items[i],"");if(!valid_id(tag))data_error(ns,rel,a->items[i]->line,"item tags must be namespaced identifiers");else snprintf(d.tags[d.tag_n++],ITEM_ID_LEN,"%s",tag);}
    if(d.place[0]&&!block_find(d.place))data_error(ns,rel,j->line,"place references unknown block '%s'",d.place);
    int at=-1;for(int i=0;i<items_n;i++)if(!strcmp(items[i].id,d.id))at=i;if(at<0)at=items_n++;items[at]=d;content_register("item",d.id);json_free(j);return true;
}
static bool parse_ingredient(Ingredient *o,const char *s,int count) { if((s[0]=='#'&&valid_id(s+1))||valid_id(s)){snprintf(o->id,sizeof o->id,"%s",s);o->count=CLAMP(count,1,64);return true;}return false; }
static bool parse_recipe(const char *ns,const char *rel,const char *stem) {
    Json *j=NULL;
    if(!json_file(rel,ns,&j)) return false;
    if(j->type!=JSON_OBJECT || recipes_n>=RECIPE_MAX) { json_free(j); return false; }
    Recipe r={0};for(int z=0;z<9;z++)r.cell[z]=-1;full_id(r.id,sizeof r.id,ns,stem);
    snprintf(r.kind,sizeof r.kind,"%s",json_str(j,"type","shapeless"));
    const Json *out=json_get(j,"result");
    snprintf(r.output_id,sizeof r.output_id,"%s",json_str(out,"item",""));
    r.output.count=(u8)CLAMP(json_int(out,"count",1),1,64);
    const ItemDef *od=item_find(r.output_id); const BlockDef *ob=block_find(r.output_id);
    r.output.state=od&&od->place[0]?block_parse_state(od->place):ob?ob->default_state:STATE_AIR;
    if(!valid_id(r.output_id)||(!od&&!ob)) data_error(ns,rel,out?out->line:1,"recipe result item is unknown");
    if(!strcmp(r.kind,"shaped")) {
        const Json *pat=json_get(j,"pattern"),*key=json_get(j,"key");
        if(!pat||pat->type!=JSON_ARRAY||pat->count>3) data_error(ns,rel,j->line,"shaped recipe pattern must contain up to three rows");
        else for(int y=0;y<pat->count;y++) {
            const char *row=json_as_str(pat->items[y],"");
            if(strlen(row)>3) data_error(ns,rel,pat->items[y]->line,"recipe row exceeds 3 columns");
            snprintf(r.pattern[y],sizeof r.pattern[y],"%.3s",row);r.height=MAX(r.height,y+1);r.width=MAX(r.width,(int)MIN(strlen(row),3));
            for(int x=0;row[x]&&x<3;x++) if(row[x]!=' '&&r.n<9) {
                char ch[2]={row[x],0}; const Json *ko=json_get(key,ch);
                const char *ref=ko&&ko->type==JSON_OBJECT?json_str(ko,"item",""):json_str(key,ch,"");
                int count=ko&&ko->type==JSON_OBJECT?json_int(ko,"count",1):1;
                r.cell[y*3+x]=r.n;
                if(!parse_ingredient(&r.ing[r.n++],ref,count)) data_error(ns,rel,ko?ko->line:j->line,"recipe key '%c' has invalid item",row[x]);
            }
        }
    } else if(!strcmp(r.kind,"shapeless")) {
        const Json *arr=json_get(j,"ingredients");
        if(!arr||arr->type!=JSON_ARRAY||arr->count>9) data_error(ns,rel,j->line,"shapeless recipe ingredients must be an array (maximum 9)");
        else for(int i=0;i<arr->count;i++) {
            const Json *it=arr->items[i]; const char *ref=json_as_str(it,""); int count=1;
            if(it->type==JSON_OBJECT){ref=json_str(it,"item","");count=json_int(it,"count",1);}
            if(!parse_ingredient(&r.ing[r.n++],ref,count)) data_error(ns,rel,it->line,"invalid recipe ingredient");
        }
    } else if(!strcmp(r.kind,"processing")) {
        const Json *arr = json_get(j,"ingredients");
        if (!arr) arr = json_get(j,"inputs");
        if (!arr || arr->type != JSON_ARRAY || arr->count < 1 || arr->count > 9) data_error(ns,rel,j->line,"processing recipe inputs must contain one to nine ingredients");
        else for (int i=0; i<arr->count; i++) { const Json *it=arr->items[i]; const char *ref=json_as_str(it,""); int count=1; if(it->type==JSON_OBJECT){ref=json_str(it,"item","");count=json_int(it,"count",1);} if(!parse_ingredient(&r.ing[r.n++],ref,count)) data_error(ns,rel,it->line,"invalid processing ingredient"); }
        r.process_time=(float)MAX(0.0,json_num(j,"time",json_num(j,"duration",1.0)));
    } else data_error(ns,rel,j->line,"recipe type must be shaped, shapeless, or processing");
    recipes[recipes_n++]=r; content_register("recipe",r.id); json_free(j); return true;
}
static bool parse_loot(const char*ns,const char*rel,const char*stem) {
    Json*j=NULL;if(!json_file(rel,ns,&j))return false;if(j->type!=JSON_OBJECT){json_free(j);return false;}if(loots_n>=LOOT_MAX){data_error(ns,rel,j->line,"loot registry capacity (%d) exceeded",LOOT_MAX);json_free(j);return false;}LootTable t={0};full_id(t.id,sizeof t.id,ns,stem);const Json*a=json_get(j,"pools");if(!a)a=json_get(j,"entries");
    for(int i=0;a&&a->type==JSON_ARRAY&&i<a->count&&t.n<32;i++){const Json*e=a->items[i];LootEntry*x=&t.entries[t.n++];snprintf(x->item,sizeof x->item,"%s",json_str(e,"item",""));x->min=CLAMP(json_int(e,"min",1),0,4096);x->max=CLAMP(json_int(e,"max",x->min),x->min,4096);x->chance=(float)CLAMP(json_num(e,"chance",1.0),0.0,1.0);if(!item_find(x->item)&&!block_find(x->item))data_error(ns,rel,e->line,"loot entry references unknown item '%s'",x->item);}
    loots[loots_n++]=t;content_register("loot",t.id);json_free(j);return true;
}
static void scan_domain(const char*domain,const char*kind) {
    StrList ns={0};vfs_list("data",&ns);for(int a=0;a<ns.n;a++){char dir[180];snprintf(dir,sizeof dir,"data/%s/%s",ns.d[a],domain);StrList fs={0};vfs_list(dir,&fs);for(int b=0;b<fs.n;b++){size_t ln=strlen(fs.d[b]);if(ln<6||strcmp(fs.d[b]+ln-5,".json"))continue;char rel[280],stem[ITEM_ID_LEN],id[ITEM_ID_LEN];snprintf(rel,sizeof rel,"%s/%s",dir,fs.d[b]);snprintf(stem,sizeof stem,"%.*s",(int)MIN(ln-5,sizeof stem-1),fs.d[b]);full_id(id,sizeof id,ns.d[a],stem);content_register(kind,id);if(!strcmp(domain,"items"))parse_item(ns.d[a],rel,stem);else if(!strcmp(domain,"recipes"))parse_recipe(ns.d[a],rel,stem);else if(!strcmp(domain,"loot_tables"))parse_loot(ns.d[a],rel,stem);}strlist_free(&fs);}strlist_free(&ns);
}
static void register_runtime_domains(void) {
    for (int i = 0; i < g_block_count; i++) content_register("block", g_blocks[i]->name);
    for (int i = 0; i < entity_type_count(); i++) {
        const EntityType *t = entity_type_at(i);
        if (t) content_register("entity", t->id);
    }
}
static void register_data_domain(const char *domain, const char *kind) {
    StrList ns = {0}; vfs_list("data", &ns);
    for (int a = 0; a < ns.n; a++) {
        char dir[180]; snprintf(dir, sizeof dir, "data/%s/%s", ns.d[a], domain);
        StrList files = {0}; vfs_list(dir, &files);
        for (int b = 0; b < files.n; b++) {
            size_t n = strlen(files.d[b]);
            if (n < 6 || strcmp(files.d[b] + n - 5, ".json")) continue;
            char id[ITEM_ID_LEN], stem[ITEM_ID_LEN];
            snprintf(stem, sizeof stem, "%.*s", (int)MIN(n - 5, sizeof stem - 1), files.d[b]);
            full_id(id, sizeof id, ns.d[a], stem);
            content_register(kind, id);
        }
        strlist_free(&files);
    }
    strlist_free(&ns);
}
int content_load_all(void) {
    tags_n=items_n=recipes_n=loots_n=0;
    register_runtime_domains();
    scan_domain("items","item"); scan_domain("recipes","recipe"); scan_domain("loot_tables","loot");
    register_data_domain("biomes","biome"); register_data_domain("features","feature");
    register_data_domain("structures","structure"); register_data_domain("sounds","sound");
    register_data_domain("effects","effect"); load_tag_files();
    return data_error_count();
}
int recipe_count(void){return recipes_n;}
const char *recipe_id_at(int i){return i>=0&&i<recipes_n?recipes[i].id:"";}
static bool ingredient_match(const Ingredient*g,const char*id){return g->id[0]=='#'?content_tag_contains("items",g->id+1,id):!strcmp(g->id,id);}
static const char *stack_id(const ItemStack*s){if(s->item_id[0])return s->item_id;return s->count?item_name(s->state):"";}
static bool consume_ingredient(Inventory*inv,const Ingredient*g){int need=g->count;for(int i=0;i<INV_SLOTS&&need;i++){ItemStack*s=&inv->slot[i];if(s->count&&ingredient_match(g,stack_id(s))){int n=MIN(need,s->count);s->count-=n;need-=n;if(!s->count)memset(s,0,sizeof *s);}}return need==0;}
static bool recipe_match(const Recipe*r,const Inventory*inv){
    if(!strcmp(r->kind,"shapeless") || !strcmp(r->kind,"processing")){
        int available[INV_SLOTS];for(int i=0;i<INV_SLOTS;i++)available[i]=inv->slot[i].count;
        for(int g=0;g<r->n;g++){int need=r->ing[g].count;for(int i=0;i<INV_SLOTS&&need;i++)if(available[i]&&ingredient_match(&r->ing[g],stack_id(&inv->slot[i]))){int n=MIN(need,available[i]);available[i]-=n;need-=n;}if(need)return false;}
        return true;
    }
    for(int oy=0;oy<=3-r->height;oy++)for(int ox=0;ox<=3-r->width;ox++){
        bool ok=true;for(int y=0;y<3&&ok;y++)for(int x=0;x<3;x++){
            int cell=-1;if(y>=oy&&y<oy+r->height&&x>=ox&&x<ox+r->width)cell=r->cell[(y-oy)*3+(x-ox)];
            const ItemStack*s=&inv->slot[y*3+x];if(cell<0){if(s->count){ok=false;break;}}else if(!s->count||!ingredient_match(&r->ing[cell],stack_id(s))||s->count<r->ing[cell].count){ok=false;break;}
        }if(ok)return true;
    }return false;
}
bool recipe_craft(Inventory*inv,const char*id){for(int i=0;i<recipes_n;i++){Recipe*r=&recipes[i];if(id&&strcmp(id,r->id))continue;if(!recipe_match(r,inv))continue;Inventory copy=*inv;for(int g=0;g<r->n;g++)if(!consume_ingredient(&copy,&r->ing[g]))return false;int left=inventory_add_item(&copy,r->output_id,r->output.count);if(left)continue;*inv=copy;return true;}return false;}
bool recipe_process(Inventory*inv,const char*id,float *duration){for(int i=0;i<recipes_n;i++){Recipe*r=&recipes[i];if(strcmp(r->kind,"processing")|| (id&&strcmp(id,r->id)))continue;if(!recipe_match(r,inv))continue;Inventory copy=*inv;for(int g=0;g<r->n;g++)if(!consume_ingredient(&copy,&r->ing[g]))return false;if(inventory_add_item(&copy,r->output_id,r->output.count)){continue;}*inv=copy;if(duration)*duration=r->process_time;return true;}return false;}
int loot_roll(const char*id,Rng*rng,Inventory*inv){for(int i=0;i<loots_n;i++)if(!strcmp(loots[i].id,id)){int n=0;for(int k=0;k<loots[i].n;k++){LootEntry*e=&loots[i].entries[k];if(rng_float(rng)>e->chance)continue;int count=rng_range(rng,e->min,e->max);n+=count-inventory_add_item(inv,e->item,count);}return n;}return 0;}
void container_init(Container*c,int slots){memset(c,0,sizeof *c);c->slots=CLAMP(slots,1,CONTAINER_MAX_SLOTS);}
bool container_transfer(Container*c,Inventory*inv,int from,int to){if(from<0||from>=c->slots||to<0||to>=INV_SLOTS)return false;ItemStack*s=&c->slot[from],*d=&inv->slot[to];if(!s->count)return false;if(!d->count){*d=*s;memset(s,0,sizeof *s);return true;}if(strcmp(stack_id(s),stack_id(d))||strcmp(s->metadata,d->metadata)||s->durability!=d->durability)return false;int limit=item_find(stack_id(s))?item_find(stack_id(s))->max_stack:INV_MAX_STACK;int moved=MIN((int)s->count,MAX(0,limit-d->count));if(!moved)return false;d->count+=moved;s->count-=moved;if(!s->count)memset(s,0,sizeof *s);return true;}
int container_add_item(Container*c,const char*id,int count){if(!c||count<=0||!valid_id(id))return count;const ItemDef*d=item_find(id);const BlockDef*b=block_find(id);if(!d&&!b)return count;int max=d?d->max_stack:64;if(d&&d->durability>0)max=1;u16 state=d&&d->place[0]?block_parse_state(d->place):b?b->default_state:STATE_AIR;for(int i=0;i<c->slots&&count;i++){ItemStack*s=&c->slot[i];if(s->count&&!strcmp(stack_id(s),id)&&s->count<max){int put=MIN(count,max-s->count);s->count+=put;count-=put;}}for(int i=0;i<c->slots&&count;i++){ItemStack*s=&c->slot[i];if(s->count)continue;int put=MIN(count,max);memset(s,0,sizeof *s);s->state=state;s->count=put;snprintf(s->item_id,sizeof s->item_id,"%s",id);count-=put;}return count;}
int inventory_add_item(Inventory*inv,const char*id,int count){if(count<=0||!valid_id(id))return count;const ItemDef*d=item_find(id);const BlockDef*b=block_find(id);if(!d&&!b)return count;dfe_event_t ev={.name="inventory_change",.text=id};if(event_fire(&ev))return count;u16 state=d&&d->place[0]?block_parse_state(d->place):b?b->default_state:STATE_AIR;int max=d?d->max_stack:64;if(d&&d->durability>0)max=1;
    for(int i=0;i<INV_SLOTS&&count;i++){ItemStack*s=&inv->slot[i];if(s->count&&strcmp(stack_id(s),id)==0&&s->count<max){int put=MIN(count,max-s->count);s->count+=put;count-=put;}}
    for(int i=0;i<INV_SLOTS&&count;i++){ItemStack*s=&inv->slot[i];if(s->count)continue;int put=MIN(count,max);memset(s,0,sizeof *s);s->state=state;s->count=put;snprintf(s->item_id,sizeof s->item_id,"%s",id);count-=put;}return count;
}

int content_reload(void) {
    ContentId *oi=malloc(sizeof ids); Tag *ot=malloc(sizeof tags); ItemDef *om=malloc(sizeof items); Recipe *or=malloc(sizeof recipes); LootTable *ol=malloc(sizeof loots);
    if(!oi||!ot||!om||!or||!ol){free(oi);free(ot);free(om);free(or);free(ol);return 1;}
    memcpy(oi,ids,sizeof ids);memcpy(ot,tags,sizeof tags);memcpy(om,items,sizeof items);memcpy(or,recipes,sizeof recipes);memcpy(ol,loots,sizeof loots);
    int ic=ids_n,tc=tags_n,mc=items_n,rc=recipes_n,lc=loots_n,before=data_error_count();
    content_reset();
    for(int i=0;i<g_block_count;i++)content_register("block",g_blocks[i]->name);
    for(int i=0;i<entity_type_count();i++){const EntityType*t=entity_type_at(i);if(t)content_register("entity",t->id);}
    int errors=content_load_all();(void)errors;
    if(data_error_count()!=before){memcpy(ids,oi,sizeof ids);memcpy(tags,ot,sizeof tags);memcpy(items,om,sizeof items);memcpy(recipes,or,sizeof recipes);memcpy(loots,ol,sizeof loots);ids_n=ic;tags_n=tc;items_n=mc;recipes_n=rc;loots_n=lc;errors=data_error_count()-before;}
    else errors=0;
    free(oi);free(ot);free(om);free(or);free(ol);return errors;
}


bool container_save(const char *mod_id,const char *key,const Container *c) {
    if(!mod_id||!key||!c||c->slots<1||c->slots>CONTAINER_MAX_SLOTS)return false;
    JsonWriter w={0};jw_begin_obj(&w);jw_key(&w,"slots");jw_num(&w,c->slots);jw_key(&w,"items");jw_begin_arr(&w);
    for(int i=0;i<c->slots;i++){const ItemStack*s=&c->slot[i];jw_begin_obj(&w);jw_key(&w,"item");jw_str(&w,s->count?stack_id(s):"");jw_key(&w,"block");char state[128]="";if(s->count)block_format_state(s->state,state,sizeof state);jw_str(&w,state);jw_key(&w,"count");jw_num(&w,s->count);jw_key(&w,"durability");jw_num(&w,s->durability);jw_key(&w,"meta");jw_str(&w,s->metadata);jw_end_obj(&w);}
    jw_end_arr(&w);jw_end_obj(&w);char err[128];int line=0;Json*j=json_parse(w.buf,w.len,err,sizeof err,&line);bool ok=j&&mod_storage_set(mod_id,key,j);json_free(j);jw_free(&w);return ok;
}
bool container_load(const char *mod_id,const char *key,Container *c) {
    const Json*j=mod_storage_get(mod_id,key);if(!j||j->type!=JSON_OBJECT||!c)return false;int slots=CLAMP(json_int(j,"slots",0),1,CONTAINER_MAX_SLOTS);const Json*a=json_get(j,"items");Container next;container_init(&next,slots);
    for(int i=0;a&&i<json_len(a)&&i<slots;i++){const Json*e=json_at(a,i);const char*id=json_str(e,"item","");int count=CLAMP(json_int(e,"count",0),0,64);if(!id[0]||!count)continue;const ItemDef*d=item_find(id);const BlockDef*b=block_find(id);if(!d&&!b)continue;ItemStack*s=&next.slot[i];s->state=d&&d->place[0]?block_parse_state(d->place):b?b->default_state:STATE_AIR;const char*st=json_str(e,"block","");if(st[0]){u16 parsed=block_parse_state(st);if(parsed!=STATE_MISSING&&parsed!=STATE_AIR)s->state=parsed;}s->count=(u8)count;s->durability=(u16)CLAMP(json_int(e,"durability",0),0,65535);snprintf(s->item_id,sizeof s->item_id,"%s",id);snprintf(s->metadata,sizeof s->metadata,"%s",json_str(e,"meta",""));}
    *c=next;return true;
}

bool container_open(const char *mod_id, const char *key, Container *c, int slots) {
    if (!mod_id || !key || !c) return false;
    char ref[256]; snprintf(ref, sizeof ref, "%s:%s", mod_id, key);
    dfe_event_t ev = {.name="container_open", .text=ref};
    if (event_fire(&ev)) return false;
    if (!container_load(mod_id, key, c)) container_init(c, slots > 0 ? slots : 27);
    return true;
}

bool container_close(const char *mod_id, const char *key, const Container *c) {
    if (!mod_id || !key || !c) return false;
    char ref[256]; snprintf(ref, sizeof ref, "%s:%s", mod_id, key);
    dfe_event_t ev = {.name="container_close", .text=ref};
    if (event_fire(&ev)) return false;
    return container_save(mod_id, key, c);
}
