/* Chunk mesher: runs on workers over a padded snapshot, so it touches no shared state.
 *
 * Faces are merged greedily only when merging is exact. Each face carries four corner values (ambient
 * occlusion and light). Two neighbouring faces merge along an axis only when they are identical and the
 * values do not vary along that axis, otherwise the interpolation across the merged quad would differ
 * from what the separate quads would show. In flat, uniformly lit terrain this still yields very large quads.
 *
 * Vertex layout (two u32):
 *   a: px 0..9, py 10..19, pz 20..29 in 1/16 block units, extra 30..31 (AO; water depth bucket for fluids;
 *      sway weight for cross plants)
 *   b: layer 0..9, face 10..12, sky 13..16, r 17..20, g 21..24, b 25..28, tint 29..30, wind 31
 */
#include "dfe.h"

#define PAD MESH_PAD
#define UNIT 16
#define FACE_CROSS_A 6
#define FACE_CROSS_B 7
#define PLANT_JITTER 3

static const int U_AXIS[6] = {1, 2, 2, 0, 0, 1};
static const int V_AXIS[6] = {2, 1, 0, 2, 1, 0};

int conn_pair_index(int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    return a * 5 - a * (a - 1) / 2 + (b - a - 1);
}

typedef struct VBuf { MeshVertex *v; u32 n, cap; } VBuf;

static MeshVertex *vbuf_grow(VBuf *b, u32 add) {
    if (b->n + add > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        while (b->cap < b->n + add) b->cap *= 2;
        b->v = xrealloc(b->v, (size_t)b->cap * sizeof(MeshVertex));
    }
    MeshVertex *p = b->v + b->n;
    b->n += add;
    return p;
}

typedef struct FaceDesc {
    u16 light[4];
    u16 tex;
    u8 valid, layer, tint, wind, ao[4], extra[4];
    u8 uni_u, uni_v;
} FaceDesc;

/* Everything that must match for two faces to share a quad. */
static bool desc_equal(const FaceDesc *a, const FaceDesc *b) {
    return a->tex == b->tex && a->layer == b->layer && a->tint == b->tint && a->wind == b->wind &&
           !memcmp(a->light, b->light, sizeof a->light) &&
           !memcmp(a->ao, b->ao, sizeof a->ao) && !memcmp(a->extra, b->extra, sizeof a->extra);
}

static inline int pidx(int x, int y, int z) { return ((y + 1) * PAD + (z + 1)) * PAD + (x + 1); }

typedef struct Ctx {
    const MeshInput *in;
    VBuf out[LAYER_COUNT];
    FaceDesc mask[CHUNK_AREA];
} Ctx;

static inline const BlockDef *block_at_state(u16 s) { return g_blocks[g_state_block[s]]; }

/* Whether the face of `a` towards `b` is drawn. */
static bool face_visible(u16 a, u16 b) {
    if (g_state_flags[b] & BF_OPAQUE) return false;
    if (g_state_block[a] == g_state_block[b]) return false;
    return true;
}

static inline u16 light_of(const MeshInput *in, int x, int y, int z) { return in->light[pidx(x, y, z)]; }

static int fluid_depth_bucket(const MeshInput *in, int x, int y, int z) {
    u16 block = g_state_block[in->states[pidx(x, y, z)]];
    int depth = 0;
    for (int yy = y - 1; yy >= -1; yy--) {
        if (g_state_block[in->states[pidx(x, yy, z)]] != block) break;
        depth++;
    }
    /* Depth is measured in blocks, not voxels, so a coarse LOD ocean is as deep as the real one at the seam;
     * counting voxels made far water read as shallow and let the sea bed show through it. */
    depth <<= in->scale_shift;
    return depth >= 6 ? 3 : depth >= 3 ? 2 : depth >= 1 ? 1 : 0;
}

/* Fills the four corner light and AO values for the face of cell `p` in direction d. */
static void corner_values(const MeshInput *in, int d, const int p[3], FaceDesc *f, bool want_ao) {
    int ua = U_AXIS[d], va = V_AXIS[d];
    int b[3] = {p[0] + DIR_VEC[d][0], p[1] + DIR_VEC[d][1], p[2] + DIR_VEC[d][2]};
    u16 center = in->light[pidx(b[0], b[1], b[2])];
    for (int c = 0; c < 4; c++) {
        int du = (c == 1 || c == 2) ? 1 : -1, dv = c >= 2 ? 1 : -1;
        int s1[3] = {b[0], b[1], b[2]}, s2[3] = {b[0], b[1], b[2]}, sc[3] = {b[0], b[1], b[2]};
        s1[ua] += du; s2[va] += dv; sc[ua] += du; sc[va] += dv;
        const int *cells[4] = {b, s1, s2, sc};
        int sum_s = 0, sum_r = 0, sum_g = 0, sum_b = 0;
        bool opaque[4];
        for (int k = 0; k < 4; k++) {
            int i = pidx(cells[k][0], cells[k][1], cells[k][2]);
            opaque[k] = k > 0 && state_opaque(in->states[i]);
            u16 l = opaque[k] ? center : in->light[i];
            sum_s += LIGHT_SKY(l); sum_r += LIGHT_R(l); sum_g += LIGHT_G(l); sum_b += LIGHT_B(l);
        }
        f->light[c] = LIGHT_PACK((sum_s + 2) >> 2, (sum_r + 2) >> 2, (sum_g + 2) >> 2, (sum_b + 2) >> 2);
        if (want_ao) f->ao[c] = (opaque[1] && opaque[2]) ? 0 : (u8)(3 - (opaque[1] + opaque[2] + opaque[3]));
        else f->ao[c] = 3;
    }
}

static void finish_uniformity(FaceDesc *f) {
    const u16 *l = f->light;
    f->uni_u = l[0] == l[1] && l[3] == l[2] && f->ao[0] == f->ao[1] && f->ao[3] == f->ao[2];
    f->uni_v = l[0] == l[3] && l[1] == l[2] && f->ao[0] == f->ao[3] && f->ao[1] == f->ao[2];
}

static bool build_face(Ctx *cx, int d, const int p[3], FaceDesc *f) {
    const MeshInput *in = cx->in;
    u16 a = in->states[pidx(p[0], p[1], p[2])];
    const BlockDef *def = block_at_state(a);
    if (def->shape != SHAPE_CUBE && def->shape != SHAPE_FLUID) return false;
    u16 b = in->states[pidx(p[0] + DIR_VEC[d][0], p[1] + DIR_VEC[d][1], p[2] + DIR_VEC[d][2])];
    if (!face_visible(a, b)) return false;
    memset(f, 0, sizeof *f);
    f->valid = 1;
    f->tex = def->tex[d];
    f->layer = def->layer;
    f->tint = (def->tint_mask >> d) & 1 ? def->tint : 0;
    f->wind = (def->flags & BF_WIND) ? 1 : 0;
    bool fluid = def->shape == SHAPE_FLUID;
    corner_values(in, d, p, f, !fluid);
    if (fluid) {
        int bucket = fluid_depth_bucket(in, p[0], p[1], p[2]);
        for (int c = 0; c < 4; c++) f->extra[c] = (u8)bucket;
    } else {
        for (int c = 0; c < 4; c++) f->extra[c] = f->ao[c];
        if (f->wind) for (int c = 0; c < 4; c++) f->extra[c] = 3;
    }
    finish_uniformity(f);
    return true;
}

static inline u32 pack_a(int x, int y, int z, int extra) {
    return (u32)x | ((u32)y << 10) | ((u32)z << 20) | ((u32)extra << 30);
}

static inline u32 pack_b(const FaceDesc *f, int face, u16 light) {
    return (u32)f->tex | ((u32)face << 10) | ((u32)LIGHT_SKY(light) << 13) | ((u32)LIGHT_R(light) << 17) |
           ((u32)LIGHT_G(light) << 21) | ((u32)LIGHT_B(light) << 25) | ((u32)f->tint << 29) | ((u32)f->wind << 31);
}

static void emit_face_quad(Ctx *cx, int d, int slice, int u0, int v0, int w, int h) {
    const FaceDesc *cells[4] = {
        &cx->mask[v0 * 32 + u0], &cx->mask[v0 * 32 + u0 + w - 1],
        &cx->mask[(v0 + h - 1) * 32 + u0 + w - 1], &cx->mask[(v0 + h - 1) * 32 + u0]};
    const FaceDesc *f = cells[0];
    int n = d >> 1, ua = U_AXIS[d], va = V_AXIS[d];
    int plane = (slice + ((d & 1) ? 0 : 1)) * UNIT;
    int ur[2] = {u0 * UNIT, (u0 + w) * UNIT}, vr[2] = {v0 * UNIT, (v0 + h) * UNIT};
    static const int CU[4] = {0, 1, 1, 0}, CV[4] = {0, 0, 1, 1};
    int pos[4][3], ao[4], ex[4];
    u16 lt[4];
    for (int c = 0; c < 4; c++) {
        pos[c][n] = plane;
        pos[c][ua] = ur[CU[c]];
        pos[c][va] = vr[CV[c]];
        ao[c] = cells[c]->ao[c];
        ex[c] = cells[c]->extra[c];
        lt[c] = cells[c]->light[c];
    }
    int order[4] = {0, 1, 2, 3};
    if (ao[0] + ao[2] < ao[1] + ao[3]) { order[0] = 1; order[1] = 2; order[2] = 3; order[3] = 0; }
    MeshVertex *out = vbuf_grow(&cx->out[f->layer], 4);
    for (int k = 0; k < 4; k++) {
        int c = order[k];
        out[k].a = pack_a(pos[c][0], pos[c][1], pos[c][2], ex[c]);
        out[k].b = pack_b(f, d, lt[c]);
    }
}

/* Greedy merge over one slice mask. */
static void merge_slice(Ctx *cx, int d, int slice) {
    FaceDesc *m = cx->mask;
    for (int v = 0; v < 32; v++)
        for (int u = 0; u < 32;) {
            FaceDesc *f = &m[v * 32 + u];
            if (!f->valid) { u++; continue; }
            int w = 1;
            if (f->uni_u)
                while (u + w < 32 && m[v * 32 + u + w].valid && desc_equal(f, &m[v * 32 + u + w])) w++;
            int h = 1;
            if (f->uni_v) {
                bool ok = true;
                while (ok && v + h < 32) {
                    for (int k = 0; k < w; k++) {
                        const FaceDesc *g = &m[(v + h) * 32 + u + k];
                        if (!g->valid || !desc_equal(f, g)) { ok = false; break; }
                    }
                    if (ok) h++;
                }
            }
            emit_face_quad(cx, d, slice, u, v, w, h);
            for (int dv = 0; dv < h; dv++)
                for (int du = 0; du < w; du++) m[(v + dv) * 32 + u + du].valid = 0;
            u += w;
        }
}

static void mesh_direction(Ctx *cx, int d) {
    int n = d >> 1, ua = U_AXIS[d], va = V_AXIS[d];
    for (int s = 0; s < 32; s++) {
        bool any = false;
        for (int v = 0; v < 32; v++)
            for (int u = 0; u < 32; u++) {
                int p[3];
                p[n] = s; p[ua] = u; p[va] = v;
                FaceDesc *f = &cx->mask[v * 32 + u];
                u16 st = cx->in->states[pidx(p[0], p[1], p[2])];
                if (st == STATE_AIR) { f->valid = 0; continue; }
                if (build_face(cx, d, p, f)) any = true;
                else f->valid = 0;
            }
        if (any) merge_slice(cx, d, s);
    }
}

/* Cross-shaped plants: two crossed quads, each emitted for both sides so back-face culling stays on. */
static void emit_cross(Ctx *cx, const int p[3], u16 state) {
    const BlockDef *def = block_at_state(state);
    const MeshInput *in = cx->in;
    FaceDesc f;
    memset(&f, 0, sizeof f);
    f.tex = def->tex[0];
    f.layer = def->layer;
    f.tint = def->tint;
    f.wind = (def->flags & BF_WIND) ? 1 : 0;
    u16 light = light_of(in, p[0], p[1], p[2]);
    u64 h = hash3(0x51ab, in->cx * 32 + p[0], in->cy * 32 + p[1], in->cz * 32 + p[2]);
    int jx = (int)(h & 7) - PLANT_JITTER - 1, jz = (int)((h >> 3) & 7) - PLANT_JITTER - 1;
    int x0 = p[0] * UNIT + jx, z0 = p[2] * UNIT + jz, x1 = x0 + UNIT, z1 = z0 + UNIT;
    int y0 = p[1] * UNIT, y1 = y0 + UNIT;
    x0 = MAX(x0, 0); z0 = MAX(z0, 0);
    int diag[2][4][2] = {
        {{x0, z0}, {x1, z1}, {x1, z1}, {x0, z0}},
        {{x1, z0}, {x0, z1}, {x0, z1}, {x1, z0}}};
    for (int q = 0; q < 2; q++)
        for (int side = 0; side < 2; side++) {
            MeshVertex *out = vbuf_grow(&cx->out[f.layer], 4);
            static const int YS[4] = {0, 0, 1, 1};
            int idx[4] = {0, 1, 2, 3};
            if (side) { idx[0] = 1; idx[1] = 0; idx[2] = 3; idx[3] = 2; }
            for (int k = 0; k < 4; k++) {
                int c = idx[k];
                out[k].a = pack_a(diag[q][c][0], YS[c] ? y1 : y0, diag[q][c][1], YS[c] ? 3 : 0);
                out[k].b = pack_b(&f, q ? FACE_CROSS_B : FACE_CROSS_A, light);
            }
        }
}

static void mesh_cross_plants(Ctx *cx) {
    for (int y = 0; y < 32; y++)
        for (int z = 0; z < 32; z++)
            for (int x = 0; x < 32; x++) {
                u16 s = cx->in->states[pidx(x, y, z)];
                if (s == STATE_AIR || block_at_state(s)->shape != SHAPE_CROSS) continue;
                int p[3] = {x, y, z};
                emit_cross(cx, p, s);
            }
}

/* ------------------------------------------------------------ connectivity */

static u16 compute_connectivity(const MeshInput *in) {
    u8 *seen = xcalloc(CHUNK_VOL / 8, 1);
    u16 *stack = xmalloc(CHUNK_VOL * sizeof(u16));
    u16 conn = 0;
    for (int start = 0; start < CHUNK_VOL; start++) {
        if (seen[start >> 3] & (1 << (start & 7))) continue;
        int sx = start & 31, sz = (start >> 5) & 31, sy = start >> 10;
        if (state_opaque(in->states[pidx(sx, sy, sz)])) continue;
        int top = 0;
        u8 faces = 0;
        stack[top++] = (u16)start;
        seen[start >> 3] |= 1 << (start & 7);
        while (top) {
            int i = stack[--top];
            int x = i & 31, z = (i >> 5) & 31, y = i >> 10;
            if (x == 0) faces |= 1 << DIR_NX;
            if (x == 31) faces |= 1 << DIR_PX;
            if (y == 0) faces |= 1 << DIR_NY;
            if (y == 31) faces |= 1 << DIR_PY;
            if (z == 0) faces |= 1 << DIR_NZ;
            if (z == 31) faces |= 1 << DIR_PZ;
            for (int d = 0; d < 6; d++) {
                int nx = x + DIR_VEC[d][0], ny = y + DIR_VEC[d][1], nz = z + DIR_VEC[d][2];
                if ((unsigned)nx > 31 || (unsigned)ny > 31 || (unsigned)nz > 31) continue;
                int ni = (ny << 10) | (nz << 5) | nx;
                if (seen[ni >> 3] & (1 << (ni & 7))) continue;
                if (state_opaque(in->states[pidx(nx, ny, nz)])) continue;
                seen[ni >> 3] |= 1 << (ni & 7);
                stack[top++] = (u16)ni;
            }
        }
        for (int a = 0; a < 6; a++)
            if (faces & (1 << a))
                for (int b = a + 1; b < 6; b++)
                    if (faces & (1 << b)) conn |= (u16)CONN_BIT(a, b);
        if (conn == 0x7FFF) break;
    }
    free(stack);
    free(seen);
    return conn;
}

static bool chunk_is_uniform_air(const MeshInput *in) {
    for (int y = 0; y < 32; y++)
        for (int z = 0; z < 32; z++) {
            const u16 *row = in->states + pidx(0, y, z);
            for (int x = 0; x < 32; x++) if (row[x] != STATE_AIR) return false;
        }
    return true;
}

void mesh_build(const MeshInput *in, MeshOutput *out) {
    double t0 = (time_now_s() * 1000.0);
    memset(out, 0, sizeof *out);
    out->cx = in->cx; out->cy = in->cy; out->cz = in->cz;
    out->version = in->version;
    if (chunk_is_uniform_air(in)) {
        out->conn = 0x7FFF;
        out->build_ms = (time_now_s() * 1000.0) - t0;
        return;
    }
    Ctx *cx = xcalloc(1, sizeof *cx);
    cx->in = in;
    for (int d = 0; d < 6; d++) mesh_direction(cx, d);
    mesh_cross_plants(cx);
    for (int l = 0; l < LAYER_COUNT; l++) {
        out->verts[l] = cx->out[l].v;
        out->count[l] = cx->out[l].n;
    }
    out->conn = compute_connectivity(in);
    free(cx);
    out->build_ms = (time_now_s() * 1000.0) - t0;
}

void mesh_output_free(MeshOutput *out) {
    for (int l = 0; l < LAYER_COUNT; l++) { free(out->verts[l]); out->verts[l] = NULL; out->count[l] = 0; }
}
