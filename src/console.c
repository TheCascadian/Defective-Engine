/* In-game console and the load-error screen. The console shows the log history, so engine messages, mod
 * output and command replies all appear in one place, and it runs the same command table that Lua and native
 * plugins register into. Input is plain text; a line that matches no command falls through to the "command"
 * event, and the built-in lua command evaluates code in a console-only sandbox.
 *
 * Rejected: a separate console buffer (two histories to keep in sync for no gain). */
#include "dfe.h"
#include <GLFW/glfw3.h>

#define LINE_CAP 240
#define HISTORY_CAP 32
#define VISIBLE_FRACTION 0.55f
#define FONT_SIZE 15.0f
#define REPEAT_DELAY 0.4
#define REPEAT_RATE 0.04

static bool g_open;
static bool g_restore_capture;
static char g_line[LINE_CAP];
static int g_len;
static char g_hist[HISTORY_CAP][LINE_CAP];
static int g_hist_count, g_hist_pos = -1;
static int g_scroll;

void console_print(const char *fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    LOGI("%s", msg);
}

/* ---------------------------------------------------------------- built-in commands */

static void cmd_help(const char *args, void *user) {
    (void)args; (void)user;
    for (int i = 0; i < command_count(); i++) console_print("  %-12s %s", command_name(i), command_help(i));
}

static void cmd_mods(const char *args, void *user) {
    (void)args; (void)user;
    for (int i = 0; i < mods_total(); i++) {
        const ModInfo *m = mods_at(i);
        const char *state = m->failed ? "failed" : m->disabled ? "disabled" : "loaded";
        console_print("  %-14s %-9s %s (load order %d)", m->id, m->version, state, m->order);
    }
}

static void cmd_seed(const char *args, void *user) { (void)args; (void)user; console_print("seed %llu", (unsigned long long)world_seed()); }
static void cmd_time(const char *args, void *user) { (void)args; (void)user; console_print("game time %.1f s", game_time_get()); }
static void cmd_lua(const char *args, void *user) { (void)user; if (*args) script_eval(args); else console_print("usage: lua <expression or statement>"); }

static void cmd_getblock(const char *args, void *user) {
    (void)user;
    int x, y, z;
    if (sscanf(args, "%d %d %d", &x, &y, &z) != 3) { console_print("usage: getblock <x> <y> <z>"); return; }
    u16 s = world_get_state(x, y, z);
    if (s == STATE_UNLOADED) { console_print("that position is not loaded"); return; }
    char text[128];
    console_print("%s (state %u)", block_format_state(s, text, sizeof text) ? text : "unknown", s);
}

static void cmd_setblock(const char *args, void *user) {
    (void)user;
    int x, y, z, n = 0;
    char name[80];
    if (sscanf(args, "%d %d %d %79s%n", &x, &y, &z, name, &n) != 4) { console_print("usage: setblock <x> <y> <z> <namespace:block[prop=value]>"); return; }
    u16 state = block_parse_state(name);
    if (state == STATE_UNLOADED) { console_print("unknown block or state \"%s\". Use a registered name such as base:stone, or one with properties such as mymod:lamp[lit=on]", name); return; }
    if (world_get_state(x, y, z) == STATE_UNLOADED) { console_print("that position is not loaded"); return; }
    if (!game_edit_block(x, y, z, state)) console_print("the edit was cancelled by a mod");
}

void console_init(void) {
    const dfe_api_t *api = api_get();
    api->register_command("help", "list every command", cmd_help, NULL, "engine");
    api->register_command("mods", "list installed mods and their state", cmd_mods, NULL, "engine");
    api->register_command("seed", "show the world seed", cmd_seed, NULL, "engine");
    api->register_command("time", "show simulated time", cmd_time, NULL, "engine");
    api->register_command("lua", "run Lua in the console sandbox", cmd_lua, NULL, "engine");
    api->register_command("getblock", "getblock x y z", cmd_getblock, NULL, "engine");
    api->register_command("setblock", "setblock x y z name", cmd_setblock, NULL, "engine");
}

/* ---------------------------------------------------------------- input */

bool console_open(void) { return g_open; }

static void set_open(bool open) {
    if (open == g_open) return;
    g_open = open;
    if (open) {
        g_restore_capture = g_in.cursor_captured;
        if (g_restore_capture) window_set_cursor_captured(false);
    } else if (g_restore_capture) {
        window_set_cursor_captured(true);
    }
}

static void submit(void) {
    if (g_len == 0) return;
    g_line[g_len] = '\0';
    LOGI("> %s", g_line);
    if (g_hist_count == 0 || strcmp(g_hist[(g_hist_count - 1) % HISTORY_CAP], g_line)) {
        snprintf(g_hist[g_hist_count % HISTORY_CAP], LINE_CAP, "%s", g_line);
        g_hist_count++;
    }
    char run[LINE_CAP];
    memcpy(run, g_line, (size_t)g_len + 1);
    g_len = 0;
    g_line[0] = '\0';
    g_hist_pos = -1;
    g_scroll = 0;
    command_run(run);
}

static void recall(int step) {
    int avail = MIN(g_hist_count, HISTORY_CAP);
    if (!avail) return;
    g_hist_pos = CLAMP(g_hist_pos < 0 ? (step < 0 ? 0 : -1) : g_hist_pos + (step < 0 ? 1 : -1), -1, avail - 1);
    if (g_hist_pos < 0) { g_len = 0; g_line[0] = '\0'; return; }
    snprintf(g_line, LINE_CAP, "%s", g_hist[(g_hist_count - 1 - g_hist_pos) % HISTORY_CAP]);
    g_len = (int)strlen(g_line);
}

static void handle_backspace(void) {
    static double next_repeat;
    double now = time_now_s();
    if (key_pressed(GLFW_KEY_BACKSPACE)) next_repeat = now + REPEAT_DELAY;
    else if (!(key_down(GLFW_KEY_BACKSPACE) && now >= next_repeat)) return;
    else next_repeat = now + REPEAT_RATE;
    if (g_len > 0) g_line[--g_len] = '\0';
}

void console_update(void) {
    if (key_pressed(GLFW_KEY_GRAVE_ACCENT)) { set_open(!g_open); return; }
    if (!g_open) return;
    if (key_pressed(GLFW_KEY_ESCAPE)) { set_open(false); return; }
    for (int i = 0; i < g_in.text_len; i++) {
        char c = g_in.text[i];
        if (c == '`' || c == '~' || c < 32 || g_len >= LINE_CAP - 1) continue;
        g_line[g_len++] = c;
        g_line[g_len] = '\0';
    }
    handle_backspace();
    if (key_pressed(GLFW_KEY_ENTER) || key_pressed(GLFW_KEY_KP_ENTER)) submit();
    if (key_pressed(GLFW_KEY_UP)) recall(-1);
    if (key_pressed(GLFW_KEY_DOWN)) recall(1);
    if (key_pressed(GLFW_KEY_PAGE_UP)) g_scroll += 8;
    if (key_pressed(GLFW_KEY_PAGE_DOWN)) g_scroll -= 8;
    g_scroll = CLAMP(g_scroll + (int)g_in.scroll, 0, MAX(0, log_history_count() - 1));
}

void console_draw(int width, int height) {
    if (!g_open) return;
    float h = (float)height * VISIBLE_FRACTION, line_h = FONT_SIZE + 3.0f;
    ui_rect(0, 0, (float)width, h, rgba(10, 12, 18, 215));
    ui_rect(0, h, (float)width, 2, rgba(90, 140, 220, 255));
    int rows = (int)((h - line_h - 12.0f) / line_h);
    int total = log_history_count();
    int last = total - 1 - g_scroll;
    for (int r = 0; r < rows && last - r >= 0; r++) {
        LogLevel level;
        const char *text = log_history_line(last - r, &level);
        u32 color = level == LOG_ERROR ? rgba(255, 110, 100, 255) : level == LOG_WARN ? rgba(255, 210, 100, 255) : level == LOG_DEBUG ? rgba(140, 150, 165, 255) : rgba(225, 230, 240, 255);
        ui_text(8, h - line_h * 2 - (float)r * line_h, FONT_SIZE, color, text);
    }
    char prompt[LINE_CAP + 4];
    bool blink = ((int)(time_now_s() * 2.0)) & 1;
    snprintf(prompt, sizeof prompt, "> %s%s", g_line, blink ? "_" : "");
    ui_text(8, h - line_h, FONT_SIZE, rgba(160, 220, 255, 255), prompt);
}

/* ---------------------------------------------------------------- load-error screen */

/* Breaks text into lines that fit max_w pixels, at spaces where possible. Returns the number of lines drawn. */
static int draw_wrapped(float x, float y, float size, float max_w, u32 color, const char *text) {
    char line[512];
    int lines = 0;
    const char *p = text;
    while (*p) {
        int n = 0, last_space = -1;
        while (p[n] && n < (int)sizeof line - 1) {
            line[n] = p[n];
            line[n + 1] = '\0';
            if (p[n] == ' ') last_space = n;
            if (ui_text_width(size, line) > max_w) {
                if (last_space > 0) n = last_space + 1;
                break;
            }
            n++;
        }
        if (n == 0) n = 1;
        memcpy(line, p, (size_t)n);
        line[n] = '\0';
        ui_text(x, y + (float)lines * (size + 4.0f), size, color, line);
        lines++;
        p += n;
    }
    return lines;
}

bool errors_screen(const char *title, bool can_continue) {
    bool gl = g_win.handle != NULL;
    if (!gl) {
        fprintf(stderr, "%s\n", title);
        for (int i = 0; i < data_error_count(); i++) fprintf(stderr, "  %s\n", data_error_text(i));
        return can_continue;
    }
    for (;;) {
        window_poll();
        if (g_win.should_close || key_pressed(GLFW_KEY_ESCAPE)) return false;
        if (can_continue && (key_pressed(GLFW_KEY_ENTER) || key_pressed(GLFW_KEY_KP_ENTER))) return true;
        glViewport(0, 0, g_win.fb_width, g_win.fb_height);
        glClearColor(0.07f, 0.08f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ui_begin(g_win.width, g_win.height);
        float x = 32, y = 28, w = (float)g_win.width - 64;
        ui_text(x, y, 26, rgba(255, 150, 130, 255), title);
        y += 48;
        int shown = MIN(data_error_count(), 64);
        for (int i = 0; i < shown; i++) {
            int lines = draw_wrapped(x, y, 16, w, rgba(230, 232, 238, 255), data_error_text(i));
            y += (float)lines * 20.0f + 8.0f;
            if (y > (float)g_win.height - 70) { ui_text(x, y, 16, rgba(170, 175, 190, 255), "More errors are listed in the console output."); break; }
        }
        ui_text(x, (float)g_win.height - 40, 16, rgba(160, 220, 255, 255), can_continue ? "Enter: continue without the affected mods     Esc: quit" : "Esc: quit. Fix the problems above and start again.");
        ui_end();
        window_swap();
        sleep_ms(16);
    }
}
