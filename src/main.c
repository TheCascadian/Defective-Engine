/* Entry point: command line, subsystem start-up and the run modes. */
#include "dfe.h"

#include <GLFW/glfw3.h>

Options g_opt;

static void print_usage(void) {
    puts("usage: dfe [options]\n"
         "  --benchmark          fly the fixed camera path and print frame statistics\n"
         "  --selftest           run the engine self-tests and exit\n"
         "  --bench-seconds N    benchmark duration (default 20)\n"
         "  --no-render          benchmark simulation and streaming without a GL context\n"
         "  --mods DIR           mods directory (default: ./mods or next to the executable)\n"
         "  --world NAME         world to create or load\n"
         "  --seed N             world seed\n"
         "  --render-distance N  chunks (default from preset)\n"
         "  --preset low|medium|high\n"
         "  --width W --height H window size\n"
         "  --no-vsync           disable vertical sync\n"
         "  --hidden             create the window hidden\n"
         "  --screenshot FILE    save the frame given by --screenshot-frame as PPM and exit\n"
         "  --screenshot-frame N frame index to capture (default 0)");
}

static bool parse_args(int argc, char **argv) {
    g_opt.width = 1280;
    g_opt.height = 720;
    g_opt.bench_seconds = 20;
    snprintf(g_opt.preset, sizeof g_opt.preset, "low");
    snprintf(g_opt.world_name, sizeof g_opt.world_name, "world");
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has_val = i + 1 < argc;
        if (!strcmp(a, "--benchmark")) g_opt.benchmark = true;
        else if (!strcmp(a, "--selftest")) g_opt.selftest = true;
        else if (!strcmp(a, "--no-vsync")) g_opt.no_vsync = true;
        else if (!strcmp(a, "--hidden")) g_opt.hidden_window = true;
        else if (!strcmp(a, "--no-render")) g_opt.no_render = true;
        else if (!strcmp(a, "--bench-seconds") && has_val) g_opt.bench_seconds = atoi(argv[++i]);
        else if (!strcmp(a, "--mods") && has_val) snprintf(g_opt.mods_dir, sizeof g_opt.mods_dir, "%s", argv[++i]);
        else if (!strcmp(a, "--world") && has_val) snprintf(g_opt.world_name, sizeof g_opt.world_name, "%s", argv[++i]);
        else if (!strcmp(a, "--seed") && has_val) { g_opt.seed = strtoull(argv[++i], NULL, 10); g_opt.seed_set = true; }
        else if (!strcmp(a, "--render-distance") && has_val) g_opt.render_distance = atoi(argv[++i]);
        else if (!strcmp(a, "--preset") && has_val) snprintf(g_opt.preset, sizeof g_opt.preset, "%s", argv[++i]);
        else if (!strcmp(a, "--width") && has_val) g_opt.width = atoi(argv[++i]);
        else if (!strcmp(a, "--height") && has_val) g_opt.height = atoi(argv[++i]);
        else if (!strcmp(a, "--screenshot") && has_val) snprintf(g_opt.screenshot_path, sizeof g_opt.screenshot_path, "%s", argv[++i]);
        else if (!strcmp(a, "--screenshot-frame") && has_val) g_opt.screenshot_frame = atoi(argv[++i]);
        else if (!strcmp(a, "--overlay") && has_val) g_opt.overlay_page = atoi(argv[++i]);
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { print_usage(); return false; }
        else {
            fprintf(stderr, "unknown or incomplete option '%s'\n", a);
            print_usage();
            return false;
        }
    }
    return true;
}

/* Looks for a data folder next to the executable, one level up (build tree), then the working directory. */
static bool find_dir(const char *name, const char *override, char *out, size_t cap) {
    if (override && override[0]) { snprintf(out, cap, "%s", override); return path_is_dir(out); }
    char exe[512];
    path_exe_dir(exe, sizeof exe);
    const char *fmts[] = {"%s/%s", "%s/../%s", "%s/../../%s"};
    for (int i = 0; i < 3; i++) {
        snprintf(out, cap, fmts[i], exe, name);
        if (path_is_dir(out)) return true;
    }
    snprintf(out, cap, "%s", name);
    return path_is_dir(out);
}

static bool setup_vfs(void) {
    char engine_assets[512], mods[512];
    if (!find_dir("engine_assets", NULL, engine_assets, sizeof engine_assets)) {
        LOGE("engine_assets folder not found next to the executable or in the working directory. Run from the repository root or install the folder beside the binary.");
        return false;
    }
    if (!find_dir("mods", g_opt.mods_dir, mods, sizeof mods)) {
        LOGE("mods folder not found. Pass --mods DIR or run from the repository root.");
        return false;
    }
    snprintf(g_opt.assets_dir, sizeof g_opt.assets_dir, "%s", engine_assets);
    snprintf(g_opt.mods_dir, sizeof g_opt.mods_dir, "%s", mods);
    vfs_reset();
    vfs_add_root(engine_assets, "dfe");
    /* Milestone 1 mounts only the base mod directly; the mod loader replaces this. */
    char base[600];
    snprintf(base, sizeof base, "%s/base", mods);
    vfs_add_root(base, "base");
    return true;
}

static int worker_count_for_machine(void) { return CLAMP(cpu_count() - 1, 1, 6); }

/* Deterministic path so two benchmark runs sample the same view: time advances by a fixed step per frame. */
static void benchmark_camera(Camera *cam, int frame) {
    float t = (float)frame / 60.0f;
    float radius = 24.0f + 10.0f * sinf(t * 0.4f);
    cam->pos = v3(cosf(t * 0.35f) * radius, 12.0f + 6.0f * sinf(t * 0.5f), sinf(t * 0.35f) * radius);
    V3 to_center = v3_norm(v3_sub(v3(0, 2, 0), cam->pos));
    cam->yaw = atan2f(-to_center.x, -to_center.z);
    cam->pitch = asinf(to_center.y);
}

static void draw_test_scene(void) {
    const int extent = 48;
    u32 grid = rgba(90, 120, 150, 255), major = rgba(150, 190, 220, 255);
    for (int i = -extent; i <= extent; i += 2) {
        u32 c = (i % 16 == 0) ? major : grid;
        debug_line(v3((float)i, 0, (float)-extent), v3((float)i, 0, (float)extent), c);
        debug_line(v3((float)-extent, 0, (float)i), v3((float)extent, 0, (float)i), c);
    }
    debug_line(v3(0, 0, 0), v3(8, 0, 0), rgba(255, 70, 70, 255));
    debug_line(v3(0, 0, 0), v3(0, 8, 0), rgba(70, 255, 70, 255));
    debug_line(v3(0, 0, 0), v3(0, 0, 8), rgba(70, 70, 255, 255));
}

static void fly_camera(Camera *cam, double dt) {
    float speed = (key_down(GLFW_KEY_LEFT_SHIFT) ? 40.0f : 10.0f) * (float)dt;
    V3 move = v3(0, 0, 0);
    V3 flat_fwd = v3_norm(v3(cam->forward.x, 0, cam->forward.z));
    if (key_down(GLFW_KEY_W)) move = v3_add(move, flat_fwd);
    if (key_down(GLFW_KEY_S)) move = v3_sub(move, flat_fwd);
    if (key_down(GLFW_KEY_D)) move = v3_add(move, cam->right);
    if (key_down(GLFW_KEY_A)) move = v3_sub(move, cam->right);
    if (key_down(GLFW_KEY_SPACE)) move.y += 1;
    if (key_down(GLFW_KEY_LEFT_CONTROL)) move.y -= 1;
    cam->pos = v3_add(cam->pos, v3_scale(v3_norm(move), speed));
    if (g_in.cursor_captured) {
        cam->yaw -= (float)g_in.mouse_dx * 0.0022f;
        cam->pitch = CLAMP(cam->pitch - (float)g_in.mouse_dy * 0.0022f, -1.55f, 1.55f);
    }
}

static void print_benchmark_report(double wall_s) {
    int n = g_stats.count;
    double *sorted = xmalloc((size_t)n * sizeof(double));
    memcpy(sorted, g_stats.frame_ms, (size_t)n * sizeof(double));
    double sum = 0;
    for (int i = 0; i < n; i++) sum += sorted[i];
    double avg_ms = n ? sum / n : 0;
    /* One percent low is the mean fps of the slowest one percent of frames, the common definition. */
    int worst_count = MAX(1, n / 100);
    double worst_sum = 0;
    double *tmp = xmalloc((size_t)n * sizeof(double));
    memcpy(tmp, sorted, (size_t)n * sizeof(double));
    for (int k = 0; k < worst_count; k++) {
        int idx = 0;
        for (int i = 1; i < n; i++) if (tmp[i] > tmp[idx]) idx = i;
        worst_sum += tmp[idx];
        tmp[idx] = -1;
    }
    double low1_fps = 1000.0 / (worst_sum / worst_count);
    printf("\n=== benchmark ===\n");
    printf("gl: %s\n", gl_info_string());
    printf("frames: %d in %.2f s\n", n, wall_s);
    printf("fps avg: %.1f   1%% low: %.1f\n", n / wall_s, low1_fps);
    printf("frame ms  avg %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f\n", avg_ms,
           percentile_of(g_stats.frame_ms, n, 50), percentile_of(g_stats.frame_ms, n, 95),
           percentile_of(g_stats.frame_ms, n, 99), percentile_of(g_stats.frame_ms, n, 100));
    printf("cpu ms    avg %.2f  p99 %.2f\n", n ? sum / n : 0, percentile_of(g_stats.cpu_ms, n, 99));
    printf("draw calls (last frame): %d\n", g_stats.draw_calls_last);
    printf("peak memory: %.1f MB\n", mem_peak_rss_bytes() / 1048576.0);
    free(sorted);
    free(tmp);
}

static int run_viewer(void) {
    if (!window_create("Defective Engine", g_opt.width, g_opt.height, !g_opt.no_vsync && !g_opt.benchmark, !g_opt.hidden_window)) return 1;
    if (!ui_init()) return 1;
    debug_lines_init();
    jobs_init(worker_count_for_machine());

    Camera cam = {.pos = v3(0, 6, 20), .yaw = 0, .pitch = -0.2f, .fov_y = 75.0f * DEG2RAD, .znear = 0.1f, .zfar = 1000.0f};
    if (!g_opt.benchmark) window_set_cursor_captured(true);
    for (int i = 0; i < g_opt.overlay_page; i++) overlay_cycle();
    double last = time_now_s(), bench_start = last;
    int frame = 0;
    while (!g_win.should_close) {
        double frame_start = time_now_s();
        double dt = frame_start - last;
        last = frame_start;
        window_poll();
        if (key_pressed(GLFW_KEY_ESCAPE)) {
            if (g_in.cursor_captured) window_set_cursor_captured(false);
            else g_win.should_close = true;
        }
        if (g_in.mouse_pressed[GLFW_MOUSE_BUTTON_LEFT] && !g_in.cursor_captured) window_set_cursor_captured(true);
        if (key_pressed(GLFW_KEY_F3)) overlay_cycle();

        if (g_opt.benchmark) {
            benchmark_camera(&cam, frame);
            if (frame_start - bench_start >= g_opt.bench_seconds) g_win.should_close = true;
        } else {
            fly_camera(&cam, dt);
        }
        camera_update(&cam, (float)g_win.fb_width / (float)MAX(g_win.fb_height, 1));

        g_stats.draw_calls_last = 0;
        glViewport(0, 0, g_win.fb_width, g_win.fb_height);
        glClearColor(0.08f, 0.10f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_test_scene();
        debug_lines_flush(&cam);
        ui_begin(g_win.width, g_win.height);
        overlay_draw();
        ui_end();

        double cpu_ms = (time_now_s() - frame_start) * 1000.0;
        if (g_opt.screenshot_path[0] && frame == g_opt.screenshot_frame) {
            screenshot_save_ppm(g_opt.screenshot_path);
            g_win.should_close = true;
        }
        window_swap();
        double frame_ms = (time_now_s() - frame_start) * 1000.0;
        overlay_frame(frame_ms / 1000.0, cpu_ms);
        if (g_opt.benchmark && frame > 2) stats_record_frame(frame_ms, cpu_ms);
        frame++;
    }
    if (g_opt.benchmark) print_benchmark_report(time_now_s() - bench_start);
    jobs_shutdown();
    ui_shutdown();
    window_destroy();
    return 0;
}

int main(int argc, char **argv) {
    if (!parse_args(argc, argv)) return 2;
    if (g_opt.selftest) return selftest_run() == 0 ? 0 : 1;
    if (!setup_vfs()) return 1;
    return run_viewer();
}
