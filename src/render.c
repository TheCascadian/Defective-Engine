/* Window, input, GL helpers, camera, 2D batch with text, and the debug overlay.
 * World rendering (chunks, sky, post) is added to this unit in later milestones. */
#include "dfe.h"

#include <GLFW/glfw3.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

Window g_win;
Input g_in;
FrameStats g_stats;

/* ---------------------------------------------------------------- window */

static GLFWwindow *g_glfw;
static char g_gl_info[512];
static int g_windowed_x, g_windowed_y, g_windowed_w, g_windowed_h;

static void on_glfw_error(int code, const char *desc) { LOGE("GLFW error %d: %s", code, desc); }

static void on_key(GLFWwindow *w, int key, int scancode, int action, int mods) {
    if (key < 0 || key >= (int)ARRAY_LEN(g_in.keys)) return;
    if (action == GLFW_PRESS) { g_in.keys[key] = true; g_in.keys_pressed[key] = true; }
    else if (action == GLFW_RELEASE) g_in.keys[key] = false;
    else if (action == GLFW_REPEAT) g_in.keys_pressed[key] = true; /* repeat counts for console editing */
}

static void on_mouse_button(GLFWwindow *w, int button, int action, int mods) {
    if (button < 0 || button >= (int)ARRAY_LEN(g_in.mouse_buttons)) return;
    if (action == GLFW_PRESS) { g_in.mouse_buttons[button] = true; g_in.mouse_pressed[button] = true; }
    else { g_in.mouse_buttons[button] = false; g_in.mouse_released[button] = true; }
}

static void on_cursor(GLFWwindow *w, double x, double y) {
    g_in.mouse_dx += x - g_in.mouse_x;
    g_in.mouse_dy += y - g_in.mouse_y;
    g_in.mouse_x = x;
    g_in.mouse_y = y;
}

static void on_scroll(GLFWwindow *w, double dx, double dy) { g_in.scroll += dy; }

static void on_char(GLFWwindow *w, unsigned int cp) {
    if (cp < 32 || cp > 126 || g_in.text_len >= (int)sizeof(g_in.text) - 1) return;
    g_in.text[g_in.text_len++] = (char)cp;
    g_in.text[g_in.text_len] = 0;
}

static void on_resize(GLFWwindow *w, int width, int height) {
    g_win.fb_width = width;
    g_win.fb_height = height;
    glfwGetWindowSize(w, &g_win.width, &g_win.height);
}

static void on_focus(GLFWwindow *w, int focused) { g_win.focused = focused != 0; }

bool window_create(const char *title, int width, int height, bool vsync, bool visible) {
    glfwSetErrorCallback(on_glfw_error);
    if (!glfwInit()) {
        LOGE("Could not initialise GLFW. On Linux an X11 display is required; set DISPLAY or run under xvfb-run.");
        return false;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 0); /* the scene renders into its own FBO */
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    g_glfw = glfwCreateWindow(width, height, title, NULL, NULL);
    if (!g_glfw) {
        LOGE("Could not create an OpenGL 3.3 core context. Update the graphics driver; 3.3 is the minimum.");
        return false;
    }
    glfwMakeContextCurrent(g_glfw);
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
        LOGE("Could not load OpenGL entry points.");
        return false;
    }
    g_win.handle = g_glfw;
    g_win.vsync = vsync;
    g_win.focused = true;
    glfwSwapInterval(vsync ? 1 : 0);
    glfwGetWindowSize(g_glfw, &g_win.width, &g_win.height);
    glfwGetFramebufferSize(g_glfw, &g_win.fb_width, &g_win.fb_height);
    glfwSetKeyCallback(g_glfw, on_key);
    glfwSetMouseButtonCallback(g_glfw, on_mouse_button);
    glfwSetCursorPosCallback(g_glfw, on_cursor);
    glfwSetScrollCallback(g_glfw, on_scroll);
    glfwSetCharCallback(g_glfw, on_char);
    glfwSetFramebufferSizeCallback(g_glfw, on_resize);
    glfwSetWindowFocusCallback(g_glfw, on_focus);
    glfwGetCursorPos(g_glfw, &g_in.mouse_x, &g_in.mouse_y);
    snprintf(g_gl_info, sizeof g_gl_info, "%s | %s | GL %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    LOGI("GL: %s", g_gl_info);
    return true;
}

void window_destroy(void) {
    if (g_glfw) glfwDestroyWindow(g_glfw);
    g_glfw = NULL;
    glfwTerminate();
}

void window_poll(void) {
    memset(g_in.keys_pressed, 0, sizeof g_in.keys_pressed);
    memset(g_in.mouse_pressed, 0, sizeof g_in.mouse_pressed);
    memset(g_in.mouse_released, 0, sizeof g_in.mouse_released);
    g_in.mouse_dx = g_in.mouse_dy = 0;
    g_in.scroll = 0;
    g_in.text_len = 0;
    g_in.text[0] = 0;
    glfwPollEvents();
    g_win.should_close = glfwWindowShouldClose(g_glfw) != 0;
}

void window_swap(void) { glfwSwapBuffers(g_glfw); }

void window_set_cursor_captured(bool captured) {
    g_in.cursor_captured = captured;
    glfwSetInputMode(g_glfw, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(g_glfw, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
    glfwGetCursorPos(g_glfw, &g_in.mouse_x, &g_in.mouse_y); /* avoids a view jump on the next delta */
}

void window_set_vsync(bool vsync) {
    g_win.vsync = vsync;
    glfwSwapInterval(vsync ? 1 : 0);
}

void window_set_fullscreen(bool fullscreen) {
    bool is_full = glfwGetWindowMonitor(g_glfw) != NULL;
    if (fullscreen == is_full) return;
    if (fullscreen) {
        glfwGetWindowPos(g_glfw, &g_windowed_x, &g_windowed_y);
        glfwGetWindowSize(g_glfw, &g_windowed_w, &g_windowed_h);
        GLFWmonitor *mon = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = glfwGetVideoMode(mon);
        glfwSetWindowMonitor(g_glfw, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(g_glfw, NULL, g_windowed_x, g_windowed_y, g_windowed_w ? g_windowed_w : 1280, g_windowed_h ? g_windowed_h : 720, 0);
    }
    glfwSwapInterval(g_win.vsync ? 1 : 0);
}

void window_set_title(const char *title) { glfwSetWindowTitle(g_glfw, title); }

bool key_down(int key) { return key >= 0 && key < (int)ARRAY_LEN(g_in.keys) && g_in.keys[key]; }
bool key_pressed(int key) { return key >= 0 && key < (int)ARRAY_LEN(g_in.keys_pressed) && g_in.keys_pressed[key]; }
const char *gl_info_string(void) { return g_gl_info; }

/* --------------------------------------------------------------- shaders */

#define SHADER_INCLUDE_DEPTH 4
#define SHADER_MAX_FILES 16

typedef struct ShaderSource {
    char *text;
    size_t len, cap;
    char *files[SHADER_MAX_FILES];
    int file_count;
} ShaderSource;

static void ss_append(ShaderSource *ss, const char *s, size_t n) {
    if (ss->len + n + 1 > ss->cap) {
        ss->cap = MAX(ss->cap * 2, ss->len + n + 1024);
        ss->text = xrealloc(ss->text, ss->cap);
    }
    memcpy(ss->text + ss->len, s, n);
    ss->len += n;
    ss->text[ss->len] = 0;
}

/* Expands #include "path" through the VFS so shader packs can swap shared snippets. */
static bool ss_add_file(ShaderSource *ss, const char *vpath, int depth) {
    size_t size;
    const char *owner = "?";
    char *src = (char *)vfs_read(vpath, &size, &owner);
    if (!src) {
        LOGE("Shader file '%s' was not found in any mod. Create it under a mod's assets/<namespace>/shaders folder or remove the #include that references it.", vpath);
        return false;
    }
    if (ss->file_count >= SHADER_MAX_FILES) { free(src); LOGE("Shader '%s' includes too many files (limit %d).", vpath, SHADER_MAX_FILES); return false; }
    int file_id = ss->file_count++;
    char label[160];
    snprintf(label, sizeof label, "%s [mod %s]", vpath, owner);
    ss->files[file_id] = xstrdup(label);
    char buf[64];
    snprintf(buf, sizeof buf, "#line 1 %d\n", file_id);
    ss_append(ss, buf, strlen(buf));
    int line = 1;
    for (char *p = src; *p;) {
        char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        if (strncmp(p, "#version", 8) == 0) {
            /* The engine controls the version line; dropping it keeps packs from selecting an unsupported GLSL. */
        } else if (strncmp(p, "#include", 8) == 0) {
            char *q1 = memchr(p, '"', n);
            char *q2 = q1 ? memchr(q1 + 1, '"', n - (size_t)(q1 - p) - 1) : NULL;
            if (!q1 || !q2 || depth >= SHADER_INCLUDE_DEPTH) {
                LOGE("%s:%d: malformed or too deeply nested #include. Write it as #include \"assets/dfe/shaders/name.glsl\" and keep nesting under %d levels.", label, line, SHADER_INCLUDE_DEPTH);
                free(src);
                return false;
            }
            char inc[160];
            snprintf(inc, sizeof inc, "%.*s", (int)(q2 - q1 - 1), q1 + 1);
            if (!ss_add_file(ss, inc, depth + 1)) { free(src); return false; }
            snprintf(buf, sizeof buf, "#line %d %d\n", line + 1, file_id);
            ss_append(ss, buf, strlen(buf));
        } else {
            ss_append(ss, p, n);
            ss_append(ss, "\n", 1);
        }
        line++;
        p = eol ? eol + 1 : p + n;
    }
    free(src);
    return true;
}

static GLuint compile_stage(GLenum type, const char *vpath, const char *defines, char **log_out) {
    ShaderSource ss = {0};
    char head[1024];
    snprintf(head, sizeof head, "#version 330 core\n%s\n", defines ? defines : "");
    ss_append(&ss, head, strlen(head));
    bool ok = ss_add_file(&ss, vpath, 0);
    GLuint sh = 0;
    if (ok) {
        sh = glCreateShader(type);
        const char *p = ss.text;
        glShaderSource(sh, 1, &p, NULL);
        glCompileShader(sh);
        GLint status = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
        if (!status) {
            char log[2048];
            glGetShaderInfoLog(sh, sizeof log, NULL, log);
            *log_out = xstrfmt("Shader compile error in %s (file index 0 is the first line, others are #include files: ", vpath);
            for (int i = 0; i < ss.file_count; i++) {
                char *tmp = xstrfmt("%s%d=%s%s", *log_out, i, ss.files[i], i + 1 < ss.file_count ? ", " : "): ");
                free(*log_out);
                *log_out = tmp;
            }
            char *tmp = xstrfmt("%s\n%s Fix the reported line in the file, or remove the pack that overrides it.", *log_out, log);
            free(*log_out);
            *log_out = tmp;
            glDeleteShader(sh);
            sh = 0;
        }
    }
    for (int i = 0; i < ss.file_count; i++) free(ss.files[i]);
    free(ss.text);
    return sh;
}

bool shader_load(Shader *s, const char *name, const char *vs_path, const char *fs_path, const char *defines) {
    char *log = NULL;
    GLuint vs = compile_stage(GL_VERTEX_SHADER, vs_path, defines, &log);
    GLuint fs = vs ? compile_stage(GL_FRAGMENT_SHADER, fs_path, defines, &log) : 0;
    if (!vs || !fs) {
        LOGE("%s", log ? log : "shader source missing");
        free(log);
        if (vs) glDeleteShader(vs);
        return false;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint status = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        char plog[2048];
        glGetProgramInfoLog(prog, sizeof plog, NULL, plog);
        LOGE("Shader link error for '%s' (%s + %s): %s Check that vertex outputs match fragment inputs.", name, vs_path, fs_path, plog);
        glDeleteProgram(prog);
        return false;
    }
    /* Only swap in the new program once it linked, so a bad hot reload keeps the old one running. */
    if (s->program) glDeleteProgram(s->program);
    s->program = prog;
    s->loc_count = 0;
    snprintf(s->name, sizeof s->name, "%s", name);
    return true;
}

void shader_destroy(Shader *s) {
    if (s->program) glDeleteProgram(s->program);
    memset(s, 0, sizeof *s);
}

GLint shader_uniform(Shader *s, const char *name) {
    for (int i = 0; i < s->loc_count; i++) if (s->loc_names[i] == name) return s->loc_cache[i];
    GLint loc = glGetUniformLocation(s->program, name);
    /* Pointer comparison is safe: callers pass string literals, so the same call site hits the cache. */
    if (s->loc_count < (int)ARRAY_LEN(s->loc_cache)) {
        s->loc_names[s->loc_count] = name;
        s->loc_cache[s->loc_count++] = loc;
    }
    return loc;
}

void shader_use(Shader *s) { glUseProgram(s->program); }

/* ---------------------------------------------------------------- camera */

void camera_update(Camera *c, float aspect) {
    float cp = cosf(c->pitch);
    c->forward = v3(-sinf(c->yaw) * cp, sinf(c->pitch), -cosf(c->yaw) * cp);
    c->right = v3_norm(v3_cross(c->forward, v3(0, 1, 0)));
    c->up = v3_cross(c->right, c->forward);
    c->view = m4_look_dir(c->pos, c->forward, v3(0, 1, 0));
    c->proj = m4_perspective(c->fov_y, aspect, c->znear, c->zfar);
    c->viewproj = m4_mul(c->proj, c->view);
    frustum_from_matrix(&c->frustum, c->viewproj);
}

/* -------------------------------------------------------------- 2D batch */

#define UI_MAX_VERTS 24576
#define FONT_BAKE_PX 32.0f
#define FONT_ATLAS_SIZE 512
#define FONT_FIRST_CHAR 32
#define FONT_CHAR_COUNT 95

typedef struct UiVertex { float x, y, u, v; u32 color; } UiVertex;

static struct {
    Shader shader;
    GLuint vao, vbo, font_tex;
    UiVertex verts[UI_MAX_VERTS];
    int vert_count;
    GLuint bound_tex;
    int mode; /* 0 font atlas (alpha), 1 plain RGBA texture */
    int width, height;
    stbtt_bakedchar glyphs[FONT_CHAR_COUNT];
    float white_u, white_v;
    bool font_ready;
} g_ui;

bool ui_init(void) {
    if (!shader_load(&g_ui.shader, "ui", "assets/dfe/shaders/ui.vert", "assets/dfe/shaders/ui.frag", NULL)) return false;
    glGenVertexArrays(1, &g_ui.vao);
    glGenBuffers(1, &g_ui.vbo);
    glBindVertexArray(g_ui.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_ui.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof g_ui.verts, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex), (void *)offsetof(UiVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex), (void *)offsetof(UiVertex, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(UiVertex), (void *)offsetof(UiVertex, color));
    return ui_load_font();
}

void ui_shutdown(void) {
    shader_destroy(&g_ui.shader);
    glDeleteBuffers(1, &g_ui.vbo);
    glDeleteVertexArrays(1, &g_ui.vao);
    if (g_ui.font_tex) glDeleteTextures(1, &g_ui.font_tex);
    memset(&g_ui, 0, sizeof g_ui);
}

bool ui_load_font(void) {
    size_t size;
    const char *owner;
    u8 *ttf = vfs_read("assets/dfe/fonts/ui.ttf", &size, &owner);
    if (!ttf) {
        LOGE("UI font assets/dfe/fonts/ui.ttf not found in any mod. Add a TrueType file at that path in a resource mod.");
        return false;
    }
    u8 *bitmap = xcalloc(FONT_ATLAS_SIZE * FONT_ATLAS_SIZE, 1);
    int rc = stbtt_BakeFontBitmap(ttf, 0, FONT_BAKE_PX, bitmap, FONT_ATLAS_SIZE, FONT_ATLAS_SIZE, FONT_FIRST_CHAR, FONT_CHAR_COUNT, g_ui.glyphs);
    free(ttf);
    if (rc <= 0) {
        free(bitmap);
        LOGE("Font from mod '%s' does not fit a %dx%d atlas at %.0f px. Use a font with narrower glyphs.", owner, FONT_ATLAS_SIZE, FONT_ATLAS_SIZE, FONT_BAKE_PX);
        return false;
    }
    /* A solid texel in the corner lets rectangles share the glyph texture and batch together. */
    for (int y = FONT_ATLAS_SIZE - 4; y < FONT_ATLAS_SIZE; y++)
        for (int x = FONT_ATLAS_SIZE - 4; x < FONT_ATLAS_SIZE; x++) bitmap[y * FONT_ATLAS_SIZE + x] = 255;
    g_ui.white_u = (FONT_ATLAS_SIZE - 2) / (float)FONT_ATLAS_SIZE;
    g_ui.white_v = (FONT_ATLAS_SIZE - 2) / (float)FONT_ATLAS_SIZE;
    if (!g_ui.font_tex) glGenTextures(1, &g_ui.font_tex);
    glBindTexture(GL_TEXTURE_2D, g_ui.font_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, FONT_ATLAS_SIZE, FONT_ATLAS_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, bitmap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(bitmap);
    g_ui.font_ready = true;
    return true;
}

void ui_begin(int width, int height) {
    g_ui.width = width;
    g_ui.height = height;
    g_ui.vert_count = 0;
    g_ui.bound_tex = g_ui.font_tex;
    g_ui.mode = 0;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void ui_flush(void) {
    if (g_ui.vert_count == 0) return;
    shader_use(&g_ui.shader);
    glUniform2f(shader_uniform(&g_ui.shader, "u_screen"), (float)g_ui.width, (float)g_ui.height);
    glUniform1i(shader_uniform(&g_ui.shader, "u_tex"), 0);
    glUniform1i(shader_uniform(&g_ui.shader, "u_mode"), g_ui.mode);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_ui.bound_tex);
    glBindVertexArray(g_ui.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_ui.vbo);
    /* Orphan the buffer so the driver never stalls on the previous frame's use. */
    glBufferData(GL_ARRAY_BUFFER, sizeof g_ui.verts, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)g_ui.vert_count * sizeof(UiVertex)), g_ui.verts);
    glDrawArrays(GL_TRIANGLES, 0, g_ui.vert_count);
    g_stats.draw_calls_last++;
    g_ui.vert_count = 0;
}

void ui_end(void) {
    ui_flush();
    glDisable(GL_SCISSOR_TEST);
}

void ui_clip(int x, int y, int w, int h) {
    ui_flush();
    if (w <= 0) { glDisable(GL_SCISSOR_TEST); return; }
    float sx = (float)g_win.fb_width / (float)g_ui.width, sy = (float)g_win.fb_height / (float)g_ui.height;
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)(x * sx), (int)((g_ui.height - y - h) * sy), (int)(w * sx), (int)(h * sy));
}

static void ui_set_texture(GLuint tex, int mode) {
    if (g_ui.bound_tex != tex || g_ui.mode != mode) {
        ui_flush();
        g_ui.bound_tex = tex;
        g_ui.mode = mode;
    }
}

static void ui_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 c0, u32 c1) {
    if (g_ui.vert_count + 6 > UI_MAX_VERTS) ui_flush();
    UiVertex *v = g_ui.verts + g_ui.vert_count;
    v[0] = (UiVertex){x0, y0, u0, v0, c0};
    v[1] = (UiVertex){x1, y0, u1, v0, c0};
    v[2] = (UiVertex){x1, y1, u1, v1, c1};
    v[3] = (UiVertex){x0, y0, u0, v0, c0};
    v[4] = (UiVertex){x1, y1, u1, v1, c1};
    v[5] = (UiVertex){x0, y1, u0, v1, c1};
    g_ui.vert_count += 6;
}

void ui_rect(float x, float y, float w, float h, u32 color) {
    ui_set_texture(g_ui.font_tex, 0);
    ui_quad(x, y, x + w, y + h, g_ui.white_u, g_ui.white_v, g_ui.white_u, g_ui.white_v, color, color);
}

void ui_rect_gradient(float x, float y, float w, float h, u32 top, u32 bottom) {
    ui_set_texture(g_ui.font_tex, 0);
    ui_quad(x, y, x + w, y + h, g_ui.white_u, g_ui.white_v, g_ui.white_u, g_ui.white_v, top, bottom);
}

void ui_line(float x0, float y0, float x1, float y1, float thickness, u32 color) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float nx = -dy / len * thickness * 0.5f, ny = dx / len * thickness * 0.5f;
    ui_set_texture(g_ui.font_tex, 0);
    if (g_ui.vert_count + 6 > UI_MAX_VERTS) ui_flush();
    UiVertex *v = g_ui.verts + g_ui.vert_count;
    float u = g_ui.white_u, w = g_ui.white_v;
    v[0] = (UiVertex){x0 + nx, y0 + ny, u, w, color};
    v[1] = (UiVertex){x1 + nx, y1 + ny, u, w, color};
    v[2] = (UiVertex){x1 - nx, y1 - ny, u, w, color};
    v[3] = v[0];
    v[4] = v[2];
    v[5] = (UiVertex){x0 - nx, y0 - ny, u, w, color};
    g_ui.vert_count += 6;
}

void ui_image(GLuint tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 color) {
    ui_set_texture(tex, 1);
    ui_quad(x, y, x + w, y + h, u0, v0, u1, v1, color, color);
}

float ui_text_width(float size, const char *text) {
    float scale = size / FONT_BAKE_PX, w = 0;
    for (; *text; text++) {
        int c = (u8)*text;
        if (c < FONT_FIRST_CHAR || c >= FONT_FIRST_CHAR + FONT_CHAR_COUNT) continue;
        w += g_ui.glyphs[c - FONT_FIRST_CHAR].xadvance * scale;
    }
    return w;
}

static void ui_text_pass(float x, float y, float size, u32 color, const char *text) {
    float scale = size / FONT_BAKE_PX;
    float cx = 0, cy = 0;
    for (; *text; text++) {
        int c = (u8)*text;
        if (c < FONT_FIRST_CHAR || c >= FONT_FIRST_CHAR + FONT_CHAR_COUNT) continue;
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(g_ui.glyphs, FONT_ATLAS_SIZE, FONT_ATLAS_SIZE, c - FONT_FIRST_CHAR, &cx, &cy, &q, 1);
        ui_quad(x + q.x0 * scale, y + (q.y0 + FONT_BAKE_PX * 0.78f) * scale, x + q.x1 * scale, y + (q.y1 + FONT_BAKE_PX * 0.78f) * scale, q.s0, q.t0, q.s1, q.t1, color, color);
    }
}

void ui_text(float x, float y, float size, u32 color, const char *text) {
    if (!g_ui.font_ready) return;
    ui_set_texture(g_ui.font_tex, 0);
    u32 shadow = (color & 0x00FFFFFFu) | ((((color >> 24) * 160u) / 255u) << 24);
    ui_text_pass(x + size * 0.07f, y + size * 0.07f, size, shadow & 0xFF000000u, text);
    ui_text_pass(x, y, size, color, text);
}

/* --------------------------------------------------------- debug overlay */

#define OVERLAY_MAX_PAGES 12
#define OVERLAY_GRAPH_SAMPLES 240
#define OVERLAY_FONT_PX 17.0f

static struct {
    const char *names[OVERLAY_MAX_PAGES];
    OverlayPageFn fns[OVERLAY_MAX_PAGES];
    int page_count;
    int page; /* 0 hidden, 1 stats, 2 stats with frame graph, 3.. registered pages */
    float graph[OVERLAY_GRAPH_SAMPLES];
    int graph_head;
    double fps_accum, fps_value, cpu_accum;
    int fps_frames;
    double frame_ms_last, cpu_ms_last;
} g_ov;

void overlay_add_page(const char *name, OverlayPageFn fn) {
    if (g_ov.page_count >= OVERLAY_MAX_PAGES) return;
    g_ov.names[g_ov.page_count] = name;
    g_ov.fns[g_ov.page_count++] = fn;
}

void overlay_cycle(void) { g_ov.page = (g_ov.page + 1) % (3 + g_ov.page_count); }
int overlay_page(void) { return g_ov.page; }
bool overlay_visible(void) { return g_ov.page != 0; }

void overlay_frame(double dt, double cpu_ms) {
    g_ov.graph[g_ov.graph_head] = (float)(dt * 1000.0);
    g_ov.graph_head = (g_ov.graph_head + 1) % OVERLAY_GRAPH_SAMPLES;
    g_ov.frame_ms_last = dt * 1000.0;
    g_ov.cpu_ms_last = cpu_ms;
    g_ov.fps_accum += dt;
    g_ov.cpu_accum += cpu_ms;
    g_ov.fps_frames++;
    if (g_ov.fps_accum >= 0.5) {
        g_ov.fps_value = g_ov.fps_frames / g_ov.fps_accum;
        g_ov.fps_accum = 0;
        g_ov.fps_frames = 0;
        g_ov.cpu_accum = 0;
    }
}

void overlay_text_line(float *x, float *y, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_text(*x, *y, OVERLAY_FONT_PX, rgba(235, 235, 235, 255), buf);
    *y += OVERLAY_FONT_PX * 1.15f;
}

static void overlay_draw_graph(float x, float y, float w, float h) {
    const float max_ms = 50.0f;
    ui_rect(x, y, w, h, rgba(0, 0, 0, 150));
    float bar = w / OVERLAY_GRAPH_SAMPLES;
    for (int i = 0; i < OVERLAY_GRAPH_SAMPLES; i++) {
        float ms = g_ov.graph[(g_ov.graph_head + i) % OVERLAY_GRAPH_SAMPLES];
        float bh = CLAMP(ms / max_ms, 0.0f, 1.0f) * h;
        u32 col = ms <= 16.8f ? rgba(90, 200, 110, 230) : (ms <= 33.4f ? rgba(235, 200, 70, 230) : rgba(230, 80, 70, 230));
        ui_rect(x + i * bar, y + h - bh, MAX(bar - 0.5f, 1.0f), bh, col);
    }
    ui_line(x, y + h - 16.7f / max_ms * h, x + w, y + h - 16.7f / max_ms * h, 1, rgba(255, 255, 255, 90));
    ui_line(x, y + h - 33.3f / max_ms * h, x + w, y + h - 33.3f / max_ms * h, 1, rgba(255, 255, 255, 60));
    ui_text(x + 4, y + h - 16.7f / max_ms * h - 15, 12, rgba(255, 255, 255, 150), "60 fps");
    ui_text(x + 4, y + h - 33.3f / max_ms * h - 15, 12, rgba(255, 255, 255, 150), "30 fps");
}

void overlay_draw(void) {
    if (!g_ov.page) return;
    float x = 8, y = 6;
    ui_rect(0, 0, 440, g_ov.page >= 3 ? 8 + OVERLAY_FONT_PX * 1.15f * 40 : 190, rgba(0, 0, 0, g_ov.page >= 3 ? 150 : 90));
    overlay_text_line(&x, &y, "%.0f fps  frame %.2f ms  cpu %.2f ms", g_ov.fps_value, g_ov.frame_ms_last, g_ov.cpu_ms_last);
    overlay_text_line(&x, &y, "mem %.0f MB (peak %.0f MB)  draws %d", mem_current_rss_bytes() / 1048576.0, mem_peak_rss_bytes() / 1048576.0, g_stats.draw_calls_last);
    overlay_text_line(&x, &y, "jobs queued %d in flight %d workers %d", jobs_queued(), jobs_in_flight(), jobs_worker_count());
    overlay_text_line(&x, &y, "F3 cycles pages  [page %d/%d]", g_ov.page, 2 + g_ov.page_count);
    if (g_ov.page >= 2) {
        if (g_ov.page == 2) overlay_draw_graph(8, y + 4, 424, 90);
        else if (g_ov.page - 3 < g_ov.page_count) {
            overlay_text_line(&x, &y, "-- %s --", g_ov.names[g_ov.page - 3]);
            g_ov.fns[g_ov.page - 3](x, y);
        }
    }
}

/* ----------------------------------------------------------- debug lines */

#define DEBUG_LINE_MAX 16384

typedef struct DebugVertex { float x, y, z; u32 color; } DebugVertex;
static struct {
    Shader shader;
    GLuint vao, vbo;
    DebugVertex verts[DEBUG_LINE_MAX];
    int count;
    bool ready;
} g_dl;

void debug_lines_init(void) {
    if (!shader_load(&g_dl.shader, "debug_line", "assets/dfe/shaders/debug_line.vert", "assets/dfe/shaders/debug_line.frag", NULL)) return;
    glGenVertexArrays(1, &g_dl.vao);
    glGenBuffers(1, &g_dl.vbo);
    glBindVertexArray(g_dl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_dl.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof g_dl.verts, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(DebugVertex), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(DebugVertex), (void *)offsetof(DebugVertex, color));
    g_dl.ready = true;
}

void debug_line(V3 a, V3 b, u32 color) {
    if (g_dl.count + 2 > DEBUG_LINE_MAX) return;
    g_dl.verts[g_dl.count++] = (DebugVertex){a.x, a.y, a.z, color};
    g_dl.verts[g_dl.count++] = (DebugVertex){b.x, b.y, b.z, color};
}

void debug_lines_flush(const Camera *cam) {
    if (!g_dl.ready || g_dl.count == 0) { g_dl.count = 0; return; }
    shader_use(&g_dl.shader);
    glUniformMatrix4fv(shader_uniform(&g_dl.shader, "u_viewproj"), 1, GL_FALSE, cam->viewproj.m);
    glBindVertexArray(g_dl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_dl.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)g_dl.count * sizeof(DebugVertex)), g_dl.verts);
    glDrawArrays(GL_LINES, 0, g_dl.count);
    g_stats.draw_calls_last++;
    g_dl.count = 0;
}

/* ----------------------------------------------------------------- stats */

void stats_record_frame(double dt_ms, double cpu_ms) {
    if (g_stats.count >= (int)ARRAY_LEN(g_stats.frame_ms)) return;
    g_stats.frame_ms[g_stats.count] = dt_ms;
    g_stats.cpu_ms[g_stats.count] = cpu_ms;
    g_stats.count++;
}

/* Developer aid used by automated visual checks: dumps the back buffer as a binary PPM. */
bool screenshot_save_ppm(const char *path) {
    int w = g_win.fb_width, h = g_win.fb_height;
    u8 *px = xmalloc((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return false; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
    fclose(f);
    free(px);
    return true;
}
