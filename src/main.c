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
         "  --wireframe          draw chunk geometry as lines (also F4)\n"
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
        else if (!strcmp(a, "--wireframe")) g_opt.wireframe = true;
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

#define DEFAULT_SEED 20240607ull
#define SPAWN_CLEARANCE 14.0f
#define BENCH_SPEED 16.0f
#define UPLOAD_BUDGET_S 0.003
#define PRESET_RD_LOW 8
#define PRESET_RD_MEDIUM 12
#define PRESET_RD_HIGH 16

static int render_distance_for_preset(void) {
    if (g_opt.render_distance > 0) return g_opt.render_distance;
    if (!strcmp(g_opt.preset, "high")) return PRESET_RD_HIGH;
    if (!strcmp(g_opt.preset, "medium")) return PRESET_RD_MEDIUM;
    return PRESET_RD_LOW;
}

/* Deterministic path so two benchmark runs sample the same view: time advances by a fixed step per frame.
 * The camera follows the terrain at constant clearance while travelling, which exercises streaming, meshing
 * and unloading the same way on every run. */
static void benchmark_camera(Camera *cam, int frame) {
    static float smooth_y = 0;
    float t = (float)frame / 60.0f;
    float x = BENCH_SPEED * t, z = 40.0f * sinf(t * 0.12f);
    float ground = gen_height_at(x, z);
    float target = MAX(ground, (float)gen_sea_level()) + SPAWN_CLEARANCE;
    smooth_y = frame == 0 ? target : smooth_y + (target - smooth_y) * 0.04f;
    cam->pos = v3(x, smooth_y, z);
    cam->yaw = -1.5707963f + 0.5f * sinf(t * 0.35f);
    cam->pitch = -0.22f + 0.08f * sinf(t * 0.5f);
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

static bool boot_content(bool with_gl) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    if (data_error_count() > 0) {
        LOGE("%d content error(s) found; the first is: %s", data_error_count(), data_error_text(0));
        return false;
    }
    if (with_gl && !textures_build()) return false;
    return true;
}

static void print_stream_report(double cold_start_s) {
    WorldStats ws;
    world_stats(&ws);
    JobKindStats gen = jobs_stats(JOB_KIND_GEN), mesh = jobs_stats(JOB_KIND_MESH);
    printf("cold start to playable: %.2f s\n", cold_start_s);
    printf("worker threads: %d\n", jobs_worker_count());
    printf("column gen: %llu jobs, avg %.2f ms, max %.2f ms\n", (unsigned long long)gen.count, gen.count ? gen.total_s * 1000.0 / (double)gen.count : 0.0, gen.max_s * 1000.0);
    printf("chunk mesh: %llu jobs, avg %.2f ms, max %.2f ms\n", (unsigned long long)mesh.count, mesh.count ? mesh.total_s * 1000.0 / (double)mesh.count : 0.0, mesh.max_s * 1000.0);
    printf("world: %d columns, %d chunks, %d meshed, light queue %d\n", ws.columns_loaded, ws.chunks_loaded, ws.chunks_meshed, ws.light_queue);
    printf("arena: %d page(s), %.1f MB resident, %llu vertices drawn last frame\n", g_scene_stats.arena_pages, g_scene_stats.arena_used_mb, (unsigned long long)g_scene_stats.vertices_drawn);
    printf("visibility: %d visible, %d culled by frustum, draw calls %d\n", g_scene_stats.chunks_visible, g_scene_stats.chunks_culled_frustum, g_scene_stats.draw_calls);
}

/* Blocks until the area around the camera is fully built, so the first frame is not a half-loaded world. */
static double load_world_around(const Camera *cam, int rd) {
    double t0 = time_now_s();
    int quiet = 0;
    while (quiet < 3) {
        world_stream(cam->pos, cam->forward, rd, true);
        jobs_pump(0.02);
        if (world_ready()) quiet++; else quiet = 0;
        static double last_log;
        if (time_now_s() - last_log > 1.0) {
            WorldStats ws; world_stats(&ws);
            LOGI("loading: missing %d pending %d unmeshed %d mesh_pending %d light %d", ws.columns_missing, ws.columns_pending, ws.chunks_unmeshed, ws.mesh_pending, ws.light_queue);
            last_log = time_now_s();
        }
        sleep_ms(1);
    }
    return time_now_s() - t0;
}

static void overlay_world_page(float x, float y) {
    WorldStats w;
    world_stats(&w);
    overlay_text_line(&x, &y, "columns %d (pending %d, missing %d)  chunks %d", w.columns_loaded, w.columns_pending, w.columns_missing, w.chunks_loaded);
    overlay_text_line(&x, &y, "meshed %d  unmeshed %d  mesh pending %d  light queue %d", w.chunks_meshed, w.chunks_unmeshed, w.mesh_pending, w.light_queue);
    overlay_text_line(&x, &y, "visible %d  frustum culled %d  drawn O/C/T %d/%d/%d", g_scene_stats.chunks_visible, g_scene_stats.chunks_culled_frustum,
                      g_scene_stats.chunks_drawn[0], g_scene_stats.chunks_drawn[1], g_scene_stats.chunks_drawn[2]);
    overlay_text_line(&x, &y, "arena %d page(s) %.1f MB  verts drawn %llu", g_scene_stats.arena_pages, g_scene_stats.arena_used_mb,
                      (unsigned long long)g_scene_stats.vertices_drawn);
    overlay_text_line(&x, &y, "uploads this frame %d", g_scene_stats.uploads_this_frame);
}

static void overlay_jobs_page(float x, float y) {
    static const char *names[JOB_KIND_COUNT] = {"gen", "light", "mesh", "save", "far", "other"};
    overlay_text_line(&x, &y, "workers %d  queued %d  in flight %d", jobs_worker_count(), jobs_queued(), jobs_in_flight());
    for (int k = 0; k < JOB_KIND_COUNT; k++) {
        JobKindStats st = jobs_stats(k);
        if (!st.count) continue;
        overlay_text_line(&x, &y, "%-6s n %llu  avg %.2f ms  max %.2f ms", names[k], (unsigned long long)st.count, st.total_s / (double)st.count * 1000.0, st.max_s * 1000.0);
    }
}

static int run_viewer(void) {
    bool gl = !g_opt.no_render;
    if (gl) {
        if (!window_create("Defective Engine", g_opt.width, g_opt.height, !g_opt.no_vsync && !g_opt.benchmark, !g_opt.hidden_window)) return 1;
        if (!ui_init()) return 1;
        debug_lines_init();
    }
    jobs_init(worker_count_for_machine());
    if (!boot_content(gl)) return 1;
    if (gl && !scene_init()) return 1;
    u64 seed = g_opt.seed_set ? g_opt.seed : DEFAULT_SEED;
    world_init(seed);
    if (gl) {
        overlay_add_page("world", overlay_world_page);
        overlay_add_page("jobs", overlay_jobs_page);
    }
    int rd = render_distance_for_preset();
    g_scene_cfg.render_distance = rd;
    g_scene_cfg.wireframe = g_opt.wireframe;

    Camera cam = {.pos = v3(0, 80, 0), .yaw = -1.5707963f, .pitch = -0.2f, .fov_y = 75.0f * DEG2RAD, .znear = 0.1f, .zfar = (float)(rd + 2) * 32.0f};
    if (g_opt.benchmark) benchmark_camera(&cam, 0);
    else cam.pos.y = MAX(gen_height_at(0, 0), (float)gen_sea_level()) + 4.0f;
    camera_update(&cam, 16.0f / 9.0f);
    double cold = load_world_around(&cam, rd);
    jobs_stats_reset();

    if (gl && !g_opt.benchmark) window_set_cursor_captured(true);
    for (int i = 0; i < g_opt.overlay_page; i++) overlay_cycle();
    double last = time_now_s(), bench_start = last;
    int frame = 0;
    while (gl ? !g_win.should_close : (time_now_s() - bench_start < g_opt.bench_seconds)) {
        double frame_start = time_now_s();
        double dt = frame_start - last;
        last = frame_start;
        if (gl) {
            window_poll();
            if (key_pressed(GLFW_KEY_ESCAPE)) {
                if (g_in.cursor_captured) window_set_cursor_captured(false);
                else g_win.should_close = true;
            }
            if (g_in.mouse_pressed[GLFW_MOUSE_BUTTON_LEFT] && !g_in.cursor_captured) window_set_cursor_captured(true);
            if (key_pressed(GLFW_KEY_F3)) overlay_cycle();
            if (key_pressed(GLFW_KEY_F4)) g_scene_cfg.wireframe = !g_scene_cfg.wireframe;
        }
        if (g_opt.benchmark) {
            benchmark_camera(&cam, frame);
            if (gl && frame_start - bench_start >= g_opt.bench_seconds) g_win.should_close = true;
        } else if (gl) {
            fly_camera(&cam, dt);
        }
        camera_update(&cam, gl ? (float)g_win.fb_width / (float)MAX(g_win.fb_height, 1) : 16.0f / 9.0f);

        g_scene_stats.uploads_this_frame = 0;
        world_stream(cam.pos, cam.forward, rd, false);
        jobs_pump(UPLOAD_BUDGET_S);
        if (gl) {
            g_stats.draw_calls_last = 0;
            glViewport(0, 0, g_win.fb_width, g_win.fb_height);
            glClearColor(0.62f, 0.76f, 0.95f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            scene_render(&cam, time_now_s() - bench_start);
            g_stats.draw_calls_last = g_scene_stats.draw_calls;
            ui_begin(g_win.width, g_win.height);
            overlay_draw();
            ui_end();
        }
        double cpu_ms = (time_now_s() - frame_start) * 1000.0;
        if (gl && g_opt.screenshot_path[0] && frame == g_opt.screenshot_frame) {
            screenshot_save_ppm(g_opt.screenshot_path);
            g_win.should_close = true;
        }
        if (gl) window_swap();
        double frame_ms = (time_now_s() - frame_start) * 1000.0;
        if (gl) overlay_frame(frame_ms / 1000.0, cpu_ms);
        if (g_opt.benchmark && frame > 2) stats_record_frame(frame_ms, cpu_ms);
        if (!gl) sleep_ms(1);
        frame++;
    }
    if (g_opt.benchmark) {
        if (g_stats.count) print_benchmark_report(time_now_s() - bench_start);
        print_stream_report(cold);
    }
    world_shutdown();
    jobs_shutdown();
    if (gl) {
        scene_shutdown();
        textures_destroy();
        ui_shutdown();
        window_destroy();
    }
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0); /* keep log lines ordered with stderr and visible if the process is killed */
    if (!parse_args(argc, argv)) return 2;
    if (!setup_vfs()) return 1;
    if (g_opt.selftest) return selftest_run() == 0 ? 0 : 1;
    return run_viewer();
}
