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
#define SHAFT_STRENGTH 0.30f
#define SHAFT_FACING_MIN 0.05f     /* shafts fade in as the sun comes within this of the screen edge direction */

static struct {
    GLuint fbo, color, depth, vao;
    int w, h;
    Shader blit, shafts;
    float scale;
    double ema_ms;
    int frames_since_check;
    bool ready, drawing_offscreen;
    float shaft_strength;
} P;

static bool load_shaders(Shader *blit, Shader *shafts) {
    return shader_load(blit, "post", "assets/dfe/shaders/post.vert", "assets/dfe/shaders/post.frag", "") &&
           shader_load(shafts, "post_shafts", "assets/dfe/shaders/post.vert", "assets/dfe/shaders/post.frag", "#define SHAFTS 1\n");
}

bool post_init(void) {
    memset(&P, 0, sizeof P);
    if (!load_shaders(&P.blit, &P.shafts)) return false;
    glGenVertexArrays(1, &P.vao);
    P.scale = 1.0f;
    P.ready = true;
    return true;
}

bool post_reload_shaders(void) {
    Shader blit = {0}, shafts = {0};
    if (!load_shaders(&blit, &shafts)) {
        if (blit.program) shader_destroy(&blit);
        if (shafts.program) shader_destroy(&shafts);
        return false;
    }
    shader_destroy(&P.blit);
    shader_destroy(&P.shafts);
    P.blit = blit;
    P.shafts = shafts;
    return true;
}

static void release_target(void) {
    if (P.fbo) glDeleteFramebuffers(1, &P.fbo);
    if (P.color) glDeleteTextures(1, &P.color);
    if (P.depth) glDeleteTextures(1, &P.depth);
    P.fbo = P.color = P.depth = 0;
    P.w = P.h = 0;
}

void post_shutdown(void) {
    if (!P.ready) return;
    release_target();
    shader_destroy(&P.blit);
    shader_destroy(&P.shafts);
    glDeleteVertexArrays(1, &P.vao);
    P.ready = false;
}

static GLuint make_texture(GLint internal, GLenum format, GLenum type, int w, int h) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* Returns false when the driver refuses the target; the caller then draws straight to the window. */
static bool ensure_target(void) {
    if (P.fbo && P.w == g_win.fb_width && P.h == g_win.fb_height) return true;
    release_target();
    P.w = g_win.fb_width;
    P.h = g_win.fb_height;
    P.color = make_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, P.w, P.h);
    P.depth = make_texture(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, P.w, P.h);
    glGenFramebuffers(1, &P.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, P.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, P.color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, P.depth, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
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
    if (!g_gfx.light_shafts || g_atmo.sun_vis <= 0.01f) return 0.0f;
    V3 d = g_atmo.sun_dir;
    float vz = v3_dot(d, cam->forward);
    if (vz <= SHAFT_FACING_MIN) return 0.0f;
    float t = tanf(cam->fov_y * 0.5f);
    float nx = v3_dot(d, cam->right) / (vz * t * aspect), ny = v3_dot(d, cam->up) / (vz * t);
    *uv_out = v3(nx * 0.5f + 0.5f, ny * 0.5f + 0.5f, 0);
    float fade = smoothstepf(SHAFT_FACING_MIN, 0.5f, vz);
    return SHAFT_STRENGTH * g_atmo.sun_vis * fade * (1.0f - g_atmo.rain_amt) * (1.0f - g_atmo.underwater);
}

void post_begin_scene(const Camera *cam) {
    P.drawing_offscreen = false;
    if (!P.ready) { glViewport(0, 0, g_win.fb_width, g_win.fb_height); return; }
    float aspect = (float)g_win.fb_width / (float)MAX(g_win.fb_height, 1);
    V3 sun_uv;
    P.shaft_strength = compute_shafts(cam, &sun_uv, aspect);
    if (!g_gfx.dynamic_resolution) P.scale = g_gfx.fixed_scale;
    bool reduced = P.scale < SCALE_FULL;
    if ((reduced || P.shaft_strength > 0.0f) && ensure_target()) {
        glBindFramebuffer(GL_FRAMEBUFFER, P.fbo);
        glViewport(0, 0, MAX(1, (int)((float)P.w * P.scale)), MAX(1, (int)((float)P.h * P.scale)));
        P.drawing_offscreen = true;
    } else {
        glViewport(0, 0, g_win.fb_width, g_win.fb_height);
    }
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
    Shader *sh = shafts ? &P.shafts : &P.blit;
    shader_use(sh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, P.color);
    glUniform1i(shader_uniform(sh, "u_color"), 0);
    glUniform2f(shader_uniform(sh, "u_scale"), (float)MAX(1, (int)((float)P.w * P.scale)) / (float)P.w, (float)MAX(1, (int)((float)P.h * P.scale)) / (float)P.h);
    glUniform2f(shader_uniform(sh, "u_texel"), 1.0f / (float)P.w, 1.0f / (float)P.h);
    if (shafts) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, P.depth);
        glUniform1i(shader_uniform(sh, "u_depth"), 1);
        glUniform2f(shader_uniform(sh, "u_sun_uv"), sun_uv.x, sun_uv.y);
        V3 c = v3_scale(g_atmo.sun_color, P.shaft_strength);
        glUniform3f(shader_uniform(sh, "u_shaft_color"), c.x, c.y, c.z);
    }
    glBindVertexArray(P.vao);
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
