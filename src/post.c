/* Render target and resolve pass: dynamic resolution and light shafts.
 *
 * The world is drawn into an offscreen target, possibly at a reduced size, then stretched over the window by one
 * fullscreen pass. The target is allocated once at window size and the scene uses a corner of it, so changing the
 * scale never reallocates anything. When the scale is 1 and no shafts are wanted the scene draws straight to the
 * window and this module costs nothing.
 *
 * The controller reacts to GPU time, not frame time: with vsync on every frame lasts one refresh, which hides
 * headroom, and a CPU-bound frame gains nothing from fewer pixels. Rejected: temporal upscaling (needs motion
 * vectors and history buffers, too heavy for the integrated-GPU target) and a fixed resolution list (jumps are
 * visible; small steps with hysteresis are not). */
#include "dfe.h"

#define SCALE_STEP 0.05f
#define SCALE_FULL 0.999f
#define CONTROL_INTERVAL_FRAMES 20
#define CONTROL_EMA_ALPHA 0.15
#define DOWN_THRESHOLD 0.95        /* shrink when GPU time exceeds this share of the frame budget */
#define UP_THRESHOLD 0.72          /* grow back only with this much headroom, so the scale does not oscillate */
#define MAX_DROP_PER_STEP 0.15f
#define SWAP_SHARE_GPU_BOUND 0.3   /* without timer queries, a long swap wait means the GPU is the limit */
#define SHAFT_DEFAULT_DIVISOR 2    /* mask size while shafts are off */
#define SHAFT_FACING_MIN 0.05f     /* shafts fade in as the sun comes within this of the screen edge direction */

static struct {
    GLuint fbo, color, depth, vao;
    GLuint mask_fbo, mask_tex;    /* quarter-pixel-count shaft brightness */
    int mask_w, mask_h, mask_div;
    int w, h;
    Shader blit, shafts, mask;
    float scale;
    double ema_ms;
    int frames_since_check;
    bool ready, drawing_offscreen;
    bool pack_active;        /* a mod replaces post.frag, so the pass must run even at full scale */
    bool shafts_supported;   /* the engine's post.frag, or a replacement that handles the shaft passes */
    float shaft_strength;
} P;

static bool load_shaders(Shader *blit, Shader *shafts, Shader *mask) {
    return shader_load(blit, "post", "assets/dfe/shaders/post.vert", "assets/dfe/shaders/post.frag", "") &&
           shader_load(shafts, "post_shafts", "assets/dfe/shaders/post.vert", "assets/dfe/shaders/post.frag", "#define SHAFTS 1\n") &&
           shader_load(mask, "post_shaft_mask", "assets/dfe/shaders/post.vert", "assets/dfe/shaders/post.frag", "#define SHAFT_MASK 1\n");
}

/* A shader pack that grades the final image only works if the pass runs. The engine skips the pass at full scale to
 * save a full-screen copy, so a replaced post.frag turns it back on; the cost is paid only when a mod asks for it. */
static bool contains_text(const u8 *text, size_t size, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; text && i + n <= size; i++) if (!memcmp(text + i, needle, n)) return true;
    return false;
}

/* Sets pack_active and shafts_supported. A replacement that never mentions SHAFT_MASK would run its own grade in
 * the mask pass and ignore the result, so shafts are turned off for it rather than paying for a wasted pass. */
static void inspect_post_shader(void) {
    size_t size = 0;
    const char *owner = NULL;
    u8 *text = vfs_read("assets/dfe/shaders/post.frag", &size, &owner);
    P.pack_active = owner && strcmp(owner, "dfe") != 0;
    P.shafts_supported = !P.pack_active || contains_text(text, size, "SHAFT_MASK");
    free(text);
}

bool post_init(void) {
    memset(&P, 0, sizeof P);
    if (!load_shaders(&P.blit, &P.shafts, &P.mask)) return false;
    glGenVertexArrays(1, &P.vao);
    P.scale = 1.0f;
    inspect_post_shader();
    P.ready = true;
    return true;
}

bool post_reload_shaders(void) {
    Shader blit = {0}, shafts = {0}, mask = {0};
    if (!load_shaders(&blit, &shafts, &mask)) {
        if (blit.program) shader_destroy(&blit);
        if (shafts.program) shader_destroy(&shafts);
        if (mask.program) shader_destroy(&mask);
        return false;
    }
    shader_destroy(&P.blit);
    shader_destroy(&P.shafts);
    shader_destroy(&P.mask);
    P.blit = blit;
    P.shafts = shafts;
    P.mask = mask;
    inspect_post_shader();
    return true;
}

static void release_target(void) {
    if (P.fbo) glDeleteFramebuffers(1, &P.fbo);
    if (P.color) glDeleteTextures(1, &P.color);
    if (P.depth) glDeleteTextures(1, &P.depth);
    if (P.mask_fbo) glDeleteFramebuffers(1, &P.mask_fbo);
    if (P.mask_tex) glDeleteTextures(1, &P.mask_tex);
    P.fbo = P.color = P.depth = P.mask_fbo = P.mask_tex = 0;
    P.w = P.h = 0;
}

void post_shutdown(void) {
    if (!P.ready) return;
    release_target();
    shader_destroy(&P.blit);
    shader_destroy(&P.shafts);
    shader_destroy(&P.mask);
    glDeleteVertexArrays(1, &P.vao);
    P.ready = false;
}

static GLuint make_texture(GLint internal, GLenum format, GLenum type, int w, int h, GLint filter) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* Returns false when the driver refuses the target; the caller then draws straight to the window. */
static bool ensure_target(void) {
    int div = g_gfx.light_shafts ? g_gfx.godray.divisor : SHAFT_DEFAULT_DIVISOR;
    if (P.fbo && P.w == g_win.fb_width && P.h == g_win.fb_height && P.mask_div == div) return true;
    release_target();
    P.mask_div = div;
    P.w = g_win.fb_width;
    P.h = g_win.fb_height;
    P.color = make_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, P.w, P.h, GL_LINEAR);
    /* Depth is only ever compared against the far plane, so nearest filtering is both cheaper and exact. */
    P.depth = make_texture(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, P.w, P.h, GL_NEAREST);
    P.mask_w = MAX(1, P.w / div);
    P.mask_h = MAX(1, P.h / div);
    P.mask_tex = make_texture(GL_R8, GL_RED, GL_UNSIGNED_BYTE, P.mask_w, P.mask_h, GL_LINEAR);
    glGenFramebuffers(1, &P.mask_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, P.mask_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, P.mask_tex, 0);
    bool mask_ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glGenFramebuffers(1, &P.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, P.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, P.color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, P.depth, 0);
    bool ok = mask_ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
        LOGW("the offscreen render target is not supported by this driver; dynamic resolution and light shafts are off");
        release_target();
    }
    return ok;
}

float post_scale(void) { return P.drawing_offscreen ? P.scale : 1.0f; }

/* Sun position over the window in 0..1, and how strongly shafts should show. Shafts need the sun in front of the
 * camera and visible, and fade in rain and underwater where the sky is not a bright source. */
static float compute_shafts(const Camera *cam, V3 *uv_out, float aspect) {
    *uv_out = v3(0.5f, 0.5f, 0);
    if (!g_gfx.light_shafts || !P.shafts_supported || g_atmo.sun_vis <= 0.01f) return 0.0f;
    V3 d = g_atmo.sun_dir;
    float vz = v3_dot(d, cam->forward);
    if (vz <= SHAFT_FACING_MIN) return 0.0f;
    float t = tanf(cam->fov_y * 0.5f);
    float nx = v3_dot(d, cam->right) / (vz * t * aspect), ny = v3_dot(d, cam->up) / (vz * t);
    *uv_out = v3(nx * 0.5f + 0.5f, ny * 0.5f + 0.5f, 0);
    float fade = smoothstepf(SHAFT_FACING_MIN, 0.5f, vz);
    return g_gfx.godray.strength * g_atmo.sun_vis * fade * (1.0f - g_atmo.rain_amt) * (1.0f - g_atmo.underwater);
}

void post_begin_scene(const Camera *cam) {
    P.drawing_offscreen = false;
    if (!P.ready) { glViewport(0, 0, g_win.fb_width, g_win.fb_height); return; }
    float aspect = (float)g_win.fb_width / (float)MAX(g_win.fb_height, 1);
    V3 sun_uv;
    P.shaft_strength = compute_shafts(cam, &sun_uv, aspect);
    if (!g_gfx.dynamic_resolution) P.scale = g_gfx.fixed_scale;
    bool reduced = P.scale < SCALE_FULL;
    if ((reduced || P.shaft_strength > 0.0f || P.pack_active) && ensure_target()) {
        glBindFramebuffer(GL_FRAMEBUFFER, P.fbo);
        glViewport(0, 0, MAX(1, (int)((float)P.w * P.scale)), MAX(1, (int)((float)P.h * P.scale)));
        P.drawing_offscreen = true;
    } else {
        glViewport(0, 0, g_win.fb_width, g_win.fb_height);
    }
}

/* Marches toward the sun into the small mask target. Leaves the default framebuffer unbound; the caller rebinds it. */
static void draw_shaft_mask(V3 sun_uv) {
    glBindFramebuffer(GL_FRAMEBUFFER, P.mask_fbo);
    glViewport(0, 0, P.mask_w, P.mask_h);
    shader_use(&P.mask);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, P.depth);
    glUniform1i(shader_uniform(&P.mask, "u_depth"), 0);
    glUniform2f(shader_uniform(&P.mask, "u_scale"), (float)MAX(1, (int)((float)P.w * P.scale)) / (float)P.w, (float)MAX(1, (int)((float)P.h * P.scale)) / (float)P.h);
    glUniform2f(shader_uniform(&P.mask, "u_texel"), 1.0f / (float)P.w, 1.0f / (float)P.h);
    glUniform2f(shader_uniform(&P.mask, "u_sun_uv"), sun_uv.x, sun_uv.y);
    glUniform1i(shader_uniform(&P.mask, "u_shaft_taps"), g_gfx.godray.taps);
    glUniform1f(shader_uniform(&P.mask, "u_shaft_density"), g_gfx.godray.density);
    glUniform1f(shader_uniform(&P.mask, "u_shaft_decay"), g_gfx.godray.decay);
    glUniform1f(shader_uniform(&P.mask, "u_shaft_jitter"), g_gfx.godray.jitter);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_win.fb_width, g_win.fb_height);
    glBindTexture(GL_TEXTURE_2D, P.color);
}

void post_end_scene(const Camera *cam) {
    if (!P.drawing_offscreen) return;
    float aspect = (float)g_win.fb_width / (float)MAX(g_win.fb_height, 1);
    V3 sun_uv;
    compute_shafts(cam, &sun_uv, aspect);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_win.fb_width, g_win.fb_height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    bool shafts = P.shaft_strength > 0.0f;
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(P.vao);
    if (shafts) draw_shaft_mask(sun_uv);
    Shader *sh = shafts ? &P.shafts : &P.blit;
    shader_use(sh);
    glBindTexture(GL_TEXTURE_2D, P.color);
    glUniform1i(shader_uniform(sh, "u_color"), 0);
    glUniform2f(shader_uniform(sh, "u_scale"), (float)MAX(1, (int)((float)P.w * P.scale)) / (float)P.w, (float)MAX(1, (int)((float)P.h * P.scale)) / (float)P.h);
    glUniform2f(shader_uniform(sh, "u_texel"), 1.0f / (float)P.w, 1.0f / (float)P.h);
    if (shafts) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, P.mask_tex);
        glUniform1i(shader_uniform(sh, "u_shaft_mask"), 1);
        V3 c = v3_scale(g_atmo.sun_color, P.shaft_strength);
        glUniform3f(shader_uniform(sh, "u_shaft_color"), c.x, c.y, c.z);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
    g_stats.draw_calls_last++;
}

/* Called once per frame with the frame's timings, in milliseconds. */
void post_update_controller(double frame_ms, double swap_ms) {
    if (!P.ready || !g_gfx.dynamic_resolution) { P.ema_ms = 0; P.frames_since_check = 0; return; }
    double gpu = 0;
    if (perf_gpu_available()) {
        for (int k = 0; k < GPU_SECTION_COUNT; k++) gpu += perf_gpu_latest_ms((GpuSection)k);
    } else if (swap_ms > frame_ms * SWAP_SHARE_GPU_BOUND) {
        gpu = frame_ms; /* the GPU is the limit; the whole frame time is the best estimate */
    } else {
        gpu = frame_ms * (1.0 - SWAP_SHARE_GPU_BOUND);
    }
    P.ema_ms = P.ema_ms > 0 ? P.ema_ms + (gpu - P.ema_ms) * CONTROL_EMA_ALPHA : gpu;
    if (++P.frames_since_check < CONTROL_INTERVAL_FRAMES) return;
    P.frames_since_check = 0;
    double budget = g_gfx.target_ms;
    float scale = P.scale;
    if (P.ema_ms > budget * DOWN_THRESHOLD) {
        /* Pixel cost goes with the square of the scale, so the root of the ratio is the scale that fits. */
        float want = scale * sqrtf((float)(budget * DOWN_THRESHOLD / P.ema_ms));
        scale = MAX(want, scale - MAX_DROP_PER_STEP);
    } else if (P.ema_ms < budget * UP_THRESHOLD) {
        scale += SCALE_STEP;
    }
    scale = roundf(scale / SCALE_STEP) * SCALE_STEP;
    P.scale = CLAMP(scale, g_gfx.min_scale, 1.0f);
}
