/* Entry point: command line, subsystem start-up and the run modes. */
#include "dfe.h"
#include "screen.h"

#include <GLFW/glfw3.h>

Options g_opt;

static void print_usage(void) {
    puts("usage: dfe [options]\n"
         "  --benchmark          fly the fixed camera path and print frame statistics\n"
         "  --selftest           run the engine self-tests and exit\n"
         "  --dump-hydrology SEED X0 Z0 X1 Z1  print water samples along a line\n"
         "  --dump-rivers SEED   list river paths, widths and outlets near the origin\n"
         "  --dump-spawns SEED X Z RADIUS      count trees and plants by habitat; fails if any stand in water\n"
         "  --compare-worldgen SEED A B        generate twice in chunk order A and B (forward, reverse, spiral, shuffle) and diff\n"
         "  --repair-world NAME  scan and preserve damaged region files\n"
         "  --bench-seconds N    benchmark duration (default 20)\n"
         "  --bench-json FILE    write the benchmark summary as JSON\n"
         "  --bench-csv FILE     write one row per benchmark frame\n"
         "  --bench-matrix SPEC  run cases in one launch: preset:WxH,preset:WxH (seconds apply per case)\n"
         "  --bench-runs N       repeat the matrix N times, run after run\n"
         "  --bench-label TEXT   free text stored in the JSON, to tell runs apart\n"
         "  --no-render          benchmark simulation and streaming without a GL context\n"
         "  --headless           run without a window, GL context, audio, or input (used by mod test)\n"
         "  --mods DIR           mods directory (default: ./mods or next to the executable)\n"
         "  --world NAME         world to create or load\n"
         "  --seed N             world seed\n"
         "  --render-distance N  chunks (default from preset)\n"
         "  --preset NAME        quality preset: low, medium, high or one added by a mod\n"
         "  --render-scale F     draw the world at this share of the window size (0.4 to 1) and stretch it\n"
         "  --dev                reload shaders, the atmosphere file and presets whenever they change (also F5)\n"
         "  --dynamic-res        let the engine pick the render scale to hold the preset's target frame rate\n"
         "  --width W --height H window size\n"
         "  --workers N          worker thread count (default: cores minus one, at most 6)\n"
         "  --wireframe          draw chunk geometry as lines (also F4)\n"
         "  --allow-native       load native plugins declared by mods (they run unsandboxed)\n"
         "  --camera X,Y,Z,YAW,PITCH  pin the start pose in degrees and freeze the benchmark path\n"
         "  --time PHASE         start at a day phase: midnight dawn morning noon afternoon dusk night, or 0..1\n"
         "  --weather NAME       start with clear, overcast or rain\n"
         "  --no-vsync           disable vertical sync\n"
         "  --hidden             create the window hidden\n"
         "  --screenshot FILE    save the frame given by --screenshot-frame as PPM and exit\n"
         "  --screenshot-frame N frame index to capture (default 0)");
}

static int g_diag_argc;
static char **g_diag_argv;

static bool parse_args(int argc, char **argv) {
    g_opt.width = 1280;
    g_opt.height = 720;
    g_opt.bench_seconds = 20;
    g_opt.bench_runs = 1;
    snprintf(g_opt.world_name, sizeof g_opt.world_name, "world");
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has_val = i + 1 < argc;
        if (!strncmp(a, "--dump-", 7) || !strcmp(a, "--compare-worldgen")) { g_diag_argc = argc - i; g_diag_argv = argv + i; return true; }
        if (!strcmp(a, "--benchmark")) g_opt.benchmark = true;
        else if (!strcmp(a, "--selftest")) g_opt.selftest = true;
        else if (!strcmp(a, "--repair-world") && has_val) { snprintf(g_opt.world_name, sizeof g_opt.world_name, "%s", argv[++i]); g_opt.repair_world = true; }
        else if (!strcmp(a, "--no-vsync")) g_opt.no_vsync = true;
        else if (!strcmp(a, "--hidden")) g_opt.hidden_window = true;
        else if (!strcmp(a, "--no-render")) g_opt.no_render = true;
        else if (!strcmp(a, "--headless")) { g_opt.headless = true; g_opt.no_render = true; }
        else if (!strcmp(a, "--bench-seconds") && has_val) g_opt.bench_seconds = atoi(argv[++i]);
        else if (!strcmp(a, "--bench-entities") && has_val) { g_opt.bench_entities = atoi(argv[++i]); g_opt.bench_entities = CLAMP(g_opt.bench_entities, 0, MAX_ENTITIES); }
        else if (!strcmp(a, "--entity-legacy")) g_opt.entity_legacy = true;
        else if (!strcmp(a, "--bench-json") && has_val) snprintf(g_opt.bench_json, sizeof g_opt.bench_json, "%s", argv[++i]);
        else if (!strcmp(a, "--bench-csv") && has_val) snprintf(g_opt.bench_csv, sizeof g_opt.bench_csv, "%s", argv[++i]);
        else if (!strcmp(a, "--bench-matrix") && has_val) { snprintf(g_opt.bench_matrix, sizeof g_opt.bench_matrix, "%s", argv[++i]); g_opt.benchmark = true; }
        else if (!strcmp(a, "--bench-runs") && has_val) { g_opt.bench_runs = atoi(argv[++i]); if (g_opt.bench_runs < 1) g_opt.bench_runs = 1; }
        else if (!strcmp(a, "--bench-label") && has_val) snprintf(g_opt.bench_label, sizeof g_opt.bench_label, "%s", argv[++i]);
        else if (!strcmp(a, "--mods") && has_val) snprintf(g_opt.mods_dir, sizeof g_opt.mods_dir, "%s", argv[++i]);
        else if (!strcmp(a, "--world") && has_val) { snprintf(g_opt.world_name, sizeof g_opt.world_name, "%s", argv[++i]); g_opt.world_set = true; }
        else if (!strcmp(a, "--seed") && has_val) { g_opt.seed = strtoull(argv[++i], NULL, 10); g_opt.seed_set = true; }
        else if (!strcmp(a, "--render-distance") && has_val) g_opt.render_distance = atoi(argv[++i]);
        else if (!strcmp(a, "--render-scale") && has_val) { g_opt.render_scale = (float)atof(argv[++i]); g_opt.render_scale_set = true; }
        else if (!strcmp(a, "--dev")) g_opt.dev = true;
        else if (!strcmp(a, "--dynamic-res")) g_opt.dynamic_res = true;
        else if (!strcmp(a, "--preset") && has_val) snprintf(g_opt.preset, sizeof g_opt.preset, "%s", argv[++i]);
        else if (!strcmp(a, "--width") && has_val) g_opt.width = atoi(argv[++i]);
        else if (!strcmp(a, "--height") && has_val) g_opt.height = atoi(argv[++i]);
        else if (!strcmp(a, "--screenshot") && has_val) snprintf(g_opt.screenshot_path, sizeof g_opt.screenshot_path, "%s", argv[++i]);
        else if (!strcmp(a, "--screenshot-frame") && has_val) g_opt.screenshot_frame = atoi(argv[++i]);
        else if (!strcmp(a, "--wireframe")) g_opt.wireframe = true;
        else if (!strcmp(a, "--allow-native")) g_opt.allow_native = true;
        else if (!strcmp(a, "--workers") && has_val) g_opt.workers = atoi(argv[++i]);
        else if (!strcmp(a, "--camera") && has_val) {
            float *c = g_opt.camera;
            g_opt.camera_set = sscanf(argv[++i], "%f,%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3], &c[4]) == 5;
            if (!g_opt.camera_set) { fprintf(stderr, "--camera expects x,y,z,yaw,pitch in degrees, for example 0,90,0,0,-10\n"); return false; }
        }
        else if (!strcmp(a, "--time") && has_val) {
            g_opt.start_phase_set = atmosphere_parse_phase(argv[++i], &g_opt.start_phase);
            if (!g_opt.start_phase_set) { fprintf(stderr, "--time expects midnight, dawn, morning, noon, afternoon, dusk, night or a number from 0 to 1\n"); return false; }
        }
        else if (!strcmp(a, "--weather") && has_val) {
            Weather w;
            g_opt.start_weather_set = atmosphere_parse_weather(argv[++i], &w);
            g_opt.start_weather = (int)w;
            if (!g_opt.start_weather_set) { fprintf(stderr, "--weather expects clear, overcast or rain\n"); return false; }
        }
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
    /* Problems found here are kept in the data error list and shown once a window exists. */
    data_error_reset();
    mods_reset();
    mods_discover(mods);
    mods_resolve();
    mods_mount();
    return true;
}

static int worker_count_for_machine(void) { return g_opt.workers > 0 ? CLAMP(g_opt.workers, 1, 16) : CLAMP(cpu_count() - 1, 1, 6); }

#define DEFAULT_SEED 20240607ull
#define SPAWN_CLEARANCE 14.0f
#define BENCH_SPEED 16.0f
#define UPLOAD_BUDGET_S 0.003
#define MAX_TICKS_PER_FRAME 5
#define AUTOSAVE_INTERVAL_S 30.0
/* The first frames pay for shader and driver warm-up, which is not what a player sees in steady state. */
#define BENCH_WARMUP_FRAMES 2
/* Matrix cases follow one another in a warm process, so each discards its first second while the new size and
 * preset settle (target reallocation, driver shader variants) before anything is recorded. */
#define MATRIX_SETTLE_S 1.0
/* The first case also pays for shader variants and driver start-up, which show as a long frame if recorded. */
#define MATRIX_FIRST_SETTLE_S 3.0
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

/* Fills a grid of entities in front of the fixed entity-benchmark camera, standing a little above the terrain. */
static void bench_spawn_entities(int count) {
    const char *type = entity_type_count() ? entity_type_at(0)->id : "base:hopper";
    for (int i = 0; i < entity_type_count(); i++) if (!strcmp(entity_type_at(i)->id, "base:hopper")) type = entity_type_at(i)->id;
    int cols = 16;
    for (int i = 0; i < count; i++) {
        float x = 6.0f + (float)(i % cols) * 3.5f, z = -((float)(count / cols) * 3.5f) * 0.5f + (float)(i / cols) * 3.5f;
        entity_spawn(type, v3(x, MAX(gen_height_at(x, z), (float)gen_sea_level()) + 2.0f, z));
    }
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

#define SELECTION_PAD 0.003f /* the outline floats just outside the block so it does not z-fight */

static void draw_selection_box(void) {
    const RayHit *h = &g_interact.target;
    if (!h->hit) return;
    float x0 = (float)h->x - SELECTION_PAD, y0 = (float)h->y - SELECTION_PAD, z0 = (float)h->z - SELECTION_PAD;
    float x1 = (float)h->x + 1 + SELECTION_PAD, y1 = (float)h->y + 1 + SELECTION_PAD, z1 = (float)h->z + 1 + SELECTION_PAD;
    u32 c = rgba(0, 0, 0, 255);
    V3 v[8] = {v3(x0, y0, z0), v3(x1, y0, z0), v3(x1, y0, z1), v3(x0, y0, z1), v3(x0, y1, z0), v3(x1, y1, z0), v3(x1, y1, z1), v3(x0, y1, z1)};
    for (int i = 0; i < 4; i++) {
        debug_line(v[i], v[(i + 1) % 4], c);
        debug_line(v[4 + i], v[4 + (i + 1) % 4], c);
        debug_line(v[i], v[4 + i], c);
    }
}

/* Reads the keyboard and mouse into the player and mirrors the result into the camera. While a menu has the
 * input, the player still simulates (gravity, water) but receives no commands. */
static void drive_player(Camera *cam, double dt) {
    bool menu = console_open() || hud_inventory_open() || menu_is_open() || screen_is_open(NULL);
    PlayerInput in = {0};
    if (!menu && g_player.dead) {
        if (key_pressed(GLFW_KEY_SPACE)) player_respawn(&g_player, player_find_spawn());
    } else if (!menu) {
        in.forward = (key_down(GLFW_KEY_W) ? 1.0f : 0.0f) - (key_down(GLFW_KEY_S) ? 1.0f : 0.0f);
        in.strafe = (key_down(GLFW_KEY_D) ? 1.0f : 0.0f) - (key_down(GLFW_KEY_A) ? 1.0f : 0.0f);
        in.jump = key_down(GLFW_KEY_SPACE);
        in.jump_pressed = key_pressed(GLFW_KEY_SPACE);
        in.crouch = key_down(GLFW_KEY_LEFT_ALT);
        in.descend = key_down(GLFW_KEY_LEFT_SHIFT);
        in.sprint = key_down(GLFW_KEY_LEFT_CONTROL);
        in.toggle_fly = g_creative && key_pressed(GLFW_KEY_F);
        if (g_in.cursor_captured) {
            g_player.yaw -= (float)g_in.mouse_dx * 0.0022f;
            g_player.pitch = CLAMP(g_player.pitch - (float)g_in.mouse_dy * 0.0022f, -1.55f, 1.55f);
        }
    }
    player_step(&g_player, &in, (float)dt);
    cam->pos = player_eye_render(&g_player);
    cam->yaw = g_player.yaw;
    cam->pitch = g_player.pitch;
    juice_update(cam, &g_player, (float)dt, true);
    interact_update(&g_player, (float)dt, !menu && !g_player.dead && g_in.cursor_captured);
    server_set_focus(g_player.pos);
}

static bool boot_content(bool with_gl) {
    registry_reset();
    int known_errors = data_error_count();
    registry_load_blocks();
    content_load_all();
    registry_load_worldgen_config();
    registry_load_atmosphere();
    registry_load_presets();
    registry_load_entities();
    if (data_error_count() > known_errors) {
        LOGE("%d content error(s) found; the first is: %s", data_error_count() - known_errors, data_error_text(known_errors));
        errors_screen("Game content has errors", false);
        return false;
    }
    if (with_gl && !textures_build()) return false;
    return true;
}

/* Far plane covers the real chunks plus the distant terrain; it follows the settings so a change takes effect at once. */
static float view_far_plane(void) {
    return (float)(g_gfx.render_distance + 2 + g_gfx.far_chunks) * CHUNK_SIZE;
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

static void overlay_hydrology_page(float x, float y) {
    GenHydrologySample h;
    gen_hydrology_at(g_player.pos.x, g_player.pos.z, &h);
    overlay_text_line(&x, &y, "habitat %s  type %d  flags 0x%x", gen_habitat_at(&h), h.type, h.flags);
    overlay_text_line(&x, &y, "ground %.2f  water %.2f  bed %.2f  channel %.2f", h.ground_y, h.water_y, h.bed_y, h.channel);
    overlay_text_line(&x, &y, "flow %.0f cells  water distance %.1f  downstream %.1f %.1f", h.flow, h.water_dist, h.downstream_x, h.downstream_z);
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

/* A benchmark ignores the saved settings so two machines or two builds measure the same configuration. */
static void load_settings_for_run(void) {
    if (g_opt.benchmark) {
        settings_defaults();
        g_settings.dynamic_resolution = g_opt.dynamic_res ? 1 : 0;
    } else {
        settings_load();
        if (g_opt.dynamic_res) g_settings.dynamic_resolution = 1;
    }
    if (g_opt.preset[0]) snprintf(g_settings.preset, sizeof g_settings.preset, "%s", g_opt.preset);
    if (g_opt.render_distance > 0) g_settings.render_distance = g_opt.render_distance;
    if (g_opt.render_scale_set) {
        g_settings.render_scale = CLAMP(g_opt.render_scale, 0.4f, 1.0f);
        g_settings.dynamic_resolution = 0;
    }
}

static double matrix_settle_s(int case_index) { return case_index == 0 ? MATRIX_FIRST_SETTLE_S : MATRIX_SETTLE_S; }

/* Moves to the next matrix case: new settings and size, the camera back at the start of the path, atmosphere back at
 * its starting state, and the area around the start fully built so the case begins from the same view every time. */
static void matrix_begin_case(Camera *cam, const BenchCase *c) {
    bench_case_apply(c);
    game_time_set(0);
    atmosphere_init_state();
    gfx_apply();
    g_atmo.auto_weather = false;
    if (g_opt.start_phase_set) atmosphere_set_phase(g_opt.start_phase);
    benchmark_camera(cam, 0);
    camera_update(cam, (float)g_win.fb_width / (float)MAX(g_win.fb_height, 1));
    load_world_around(cam, g_gfx.render_distance);
}

static int run_viewer(void) {
    bool gl = !g_opt.no_render;
    load_settings_for_run();
    if (gl) {
        if (!window_create("Unbound", g_opt.width, g_opt.height, g_settings.vsync && !g_opt.no_vsync && !g_opt.benchmark, !g_opt.hidden_window)) return 1;
        if (!ui_init()) return 1;
        debug_lines_init();
        perf_init();
        if (!post_init()) return 1;
        if (!entity_gl_init()) return 1;
    }
    /* Mods that failed to resolve are excluded already; the player may continue without them. */
    if (data_error_count() > 0 && !errors_screen("Some mods could not be loaded", mods_find("base") && !mods_find("base")->failed)) return 1;
    jobs_init(worker_count_for_machine());
    if (!boot_content(gl)) return 1;
    console_init();
    mods_load_plugins(g_opt.allow_native);
    script_init();
    int before_scripts = data_error_count();
    script_load_mods();
    if (data_error_count() > before_scripts && !errors_screen("Some mod scripts failed", true)) return 1;
    if (gl && !scene_init()) return 1;
    u64 seed = g_opt.seed_set ? g_opt.seed : DEFAULT_SEED;
    /* Benchmarks use a fixed seed and must never read or write a player's world. */
    bool persist = !g_opt.benchmark;
    /* The title screen is for interactive play only; scripted runs name their world or have no world at all. */
    bool title = gl && persist && !g_opt.world_set && !g_opt.camera_set && !g_opt.screenshot_path[0] && !g_opt.hidden_window;
    if (title && !menu_title(g_opt.world_name, sizeof g_opt.world_name, &seed, &g_opt.seed_set)) return 0;
    if (persist) {
        char dir[600];
        snprintf(dir, sizeof dir, "saves/%s", g_opt.world_name);
        if (!save_open(dir, seed)) return 1;
        seed = save_seed();
    }
    world_init(seed);
    entity_world_init(seed);
    if (persist) entity_load();
    if (g_opt.bench_entities > 0) bench_spawn_entities(g_opt.bench_entities);
    if (persist) game_time_set(save_meta()->day_time);
    atmosphere_init_state();
    gfx_apply();
    if (g_opt.start_phase_set) atmosphere_set_phase(g_opt.start_phase);
    if (g_opt.start_weather_set) atmosphere_set_weather((Weather)g_opt.start_weather, true);
    /* A benchmark must not change weather mid-run, or two runs would not be comparable. */
    if (g_opt.benchmark) g_atmo.auto_weather = false;
    { dfe_event_t ev = {.name = "world_load"}; event_fire(&ev); }
    /* The benchmark and --camera runs keep the scripted or free camera so their results stay comparable. */
    bool play = gl && !g_opt.benchmark && !g_opt.camera_set;
    server_init();
    hud_init();
    if (persist && save_meta()->has_inventory) inventory_restore(&g_inv, &g_creative, save_meta());
    else { inventory_clear(&g_inv); inventory_starter(&g_inv); }
    if (gl) hud_build_icons();
    if (gl) {
        overlay_add_page("world", overlay_world_page);
        overlay_add_page("jobs", overlay_jobs_page);
        overlay_add_page("hydrology", overlay_hydrology_page);
    }
    g_scene_cfg.wireframe = g_opt.wireframe;
    int rd = g_gfx.render_distance;

    Camera cam = {.pos = v3(0, 80, 0), .yaw = -1.5707963f, .pitch = -0.2f, .fov_y = g_gfx.fov_deg * DEG2RAD, .znear = 0.1f};
    cam.zfar = view_far_plane();
    if (g_opt.camera_set) {
        cam.pos = v3(g_opt.camera[0], g_opt.camera[1], g_opt.camera[2]);
        cam.yaw = g_opt.camera[3] * (float)M_PI / 180.0f;
        cam.pitch = g_opt.camera[4] * (float)M_PI / 180.0f;
    } else if (g_opt.benchmark) benchmark_camera(&cam, 0);
    else if (persist && save_meta()->has_player) {
        const SaveMeta *m = save_meta();
        cam.pos = v3((float)m->x, (float)m->y, (float)m->z);
        cam.yaw = m->yaw;
        cam.pitch = m->pitch;
    } else cam.pos.y = MAX(gen_height_at(0, 0), (float)gen_sea_level()) + 4.0f;
    if (play) {
        const SaveMeta *m = save_meta();
        if (persist && m->has_player) {
            player_init(&g_player, v3((float)m->x, (float)m->y, (float)m->z));
            g_player.yaw = m->yaw;
            g_player.pitch = m->pitch;
            g_player.flying = m->flying && g_creative;
            g_player.health = CLAMP(m->health, 0.0f, PLAYER_MAX_HEALTH);
            g_player.dead = m->dead || g_player.health <= 0.0f;
            snprintf(g_player.id, sizeof g_player.id, "%s", m->player_id[0] ? m->player_id : "player");
            snprintf(g_player.name, sizeof g_player.name, "%s", m->player_name[0] ? m->player_name : "Player");
            snprintf(g_player.mod_state, sizeof g_player.mod_state, "%s", m->player_state);
        } else {
            player_init(&g_player, player_find_spawn());
            g_player.yaw = cam.yaw;
        }
        cam.pos = player_eye(&g_player);
        cam.yaw = g_player.yaw;
        cam.pitch = g_player.pitch;
        { dfe_event_t ev = {.name = "player_join", .text = "player"}; event_fire(&ev); }
    }
    camera_update(&cam, 16.0f / 9.0f);
    double cold = load_world_around(&cam, rd);
    jobs_stats_reset();

    if (gl && !g_opt.benchmark) window_set_cursor_captured(true);
    for (int i = 0; i < g_opt.overlay_page; i++) overlay_cycle();
    double last = time_now_s(), bench_start = last;
    int frame = 0;
    int case_index = 0;
    double record_start = 0;
    bool matrix = gl && g_opt.bench_matrix[0];
    if (g_opt.bench_matrix[0] && !gl) { fprintf(stderr, "--bench-matrix measures rendering and cannot be combined with --no-render\n"); return 2; }
    if (matrix) {
        if (!bench_matrix_parse(g_opt.bench_matrix, g_opt.bench_runs)) return 2;
        perf_matrix_begin();
        matrix_begin_case(&cam, bench_case_at(0));
        last = bench_start = time_now_s();
    }
    double last_autosave = last, tick_accumulator = 0;
    while (gl ? !g_win.should_close : (time_now_s() - bench_start < g_opt.bench_seconds)) {
        double frame_start = time_now_s();
        double dt = frame_start - last;
        last = frame_start;
        if (gl) {
            window_poll();
            console_update();
            if (play && !console_open() && !menu_is_open() && !screen_is_open(NULL) && key_pressed(GLFW_KEY_E)) hud_set_inventory_open(!hud_inventory_open());
            if (!console_open() && key_pressed(GLFW_KEY_ESCAPE)) {
                if (screen_is_open(NULL)) { /* screen_update already handled Escape for the top screen */ }
                else if (menu_is_open()) menu_back();
                else if (hud_inventory_open()) hud_set_inventory_open(false);
                else if (play) menu_set_open(true);
                else if (g_in.cursor_captured) window_set_cursor_captured(false);
                else g_win.should_close = true;
            }
            if (!console_open() && !hud_inventory_open() && !menu_is_open() && !screen_is_open(NULL) && g_in.mouse_pressed[GLFW_MOUSE_BUTTON_LEFT] && !g_in.cursor_captured) window_set_cursor_captured(true);
            if (key_pressed(GLFW_KEY_F3)) overlay_cycle();
            if (key_pressed(GLFW_KEY_F4)) g_scene_cfg.wireframe = !g_scene_cfg.wireframe;
            if (key_pressed(GLFW_KEY_F5)) hot_reload_now();
            hot_reload_poll(frame_start, g_opt.dev);
        }
        if (g_opt.benchmark && !g_opt.camera_set) {
            benchmark_camera(&cam, g_opt.bench_entities > 0 ? 0 : frame); /* the entity benchmark holds the view still */
            if (matrix && frame_start - bench_start >= matrix_settle_s(case_index) + g_opt.bench_seconds) {
                perf_report_case(bench_case_at(case_index), case_index, bench_case_total(), time_now_s() - record_start, cold);
                perf_reset_samples();
                record_start = 0;
                if (++case_index >= bench_case_total()) g_win.should_close = true;
                else {
                    matrix_begin_case(&cam, bench_case_at(case_index));
                    frame = 0;
                    last = bench_start = frame_start = time_now_s();
                    dt = 0;
                }
            } else if (!matrix && gl && frame_start - bench_start >= g_opt.bench_seconds) g_win.should_close = true;
        } else if (play) {
            drive_player(&cam, dt);
            server_pump();
            hud_update();
        } else if (gl && !console_open()) {
            fly_camera(&cam, dt);
        }
        if (!menu_is_open()) entity_update((float)dt);
        tick_accumulator = MIN(tick_accumulator + dt, GAME_TICK_DT * MAX_TICKS_PER_FRAME);
        while (tick_accumulator >= GAME_TICK_DT) { if (!menu_is_open()) game_tick(); tick_accumulator -= GAME_TICK_DT; }
        cam.fov_y = g_gfx.fov_deg * DEG2RAD * (play ? juice_fov_scale() : 1.0f);
        cam.zfar = view_far_plane();
        camera_update(&cam, gl ? (float)g_win.fb_width / (float)MAX(g_win.fb_height, 1) : 16.0f / 9.0f);

        g_scene_stats.uploads_this_frame = 0;
        double stream_start = time_now_s();
        world_stream(cam.pos, cam.forward, g_gfx.render_distance, false);
        jobs_pump(UPLOAD_BUDGET_S);
        double stream_ms = (time_now_s() - stream_start) * 1000.0;
        if (persist && frame_start - last_autosave > AUTOSAVE_INTERVAL_S) {
            world_save_dirty();
            entity_save();
            last_autosave = frame_start;
        }
        double render_start = time_now_s();
        if (gl) {
            perf_gpu_frame_begin();
            g_stats.draw_calls_last = 0;
            atmosphere_update(menu_is_open() ? 0.0 : dt, cam.pos);
            post_begin_scene(&cam);
            glClearColor(g_atmo.fog_color.x, g_atmo.fog_color.y, g_atmo.fog_color.z, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            scene_render(&cam, time_now_s() - bench_start);
            g_stats.draw_calls_last = g_scene_stats.draw_calls;
            if (play) { draw_selection_box(); debug_lines_flush(&cam); }
            perf_gpu_begin(GPU_POST);
            post_end_scene(&cam);
            perf_gpu_end();
            perf_gpu_begin(GPU_UI);
            ui_begin(g_win.width, g_win.height);
            if (play) hud_draw(g_win.width, g_win.height);
            overlay_draw();
            menu_draw(g_win.width, g_win.height);
            console_draw(g_win.width, g_win.height);
            ui_end();
            perf_gpu_end();
        }
        double cpu_end = time_now_s();
        double cpu_ms = (cpu_end - frame_start) * 1000.0;
        if (gl && g_opt.screenshot_path[0] && frame == g_opt.screenshot_frame) {
            screenshot_save_ppm(g_opt.screenshot_path);
            g_win.should_close = true;
        }
        if (gl) window_swap();
        double frame_ms = (time_now_s() - frame_start) * 1000.0;
        if (gl) overlay_frame(frame_ms / 1000.0, cpu_ms);
        if (gl) post_update_controller(frame_ms, frame_ms - cpu_ms);
        bool settled = !matrix || frame_start - bench_start >= matrix_settle_s(case_index);
        if (settled && !record_start) record_start = frame_start;
        if (g_opt.benchmark && frame > BENCH_WARMUP_FRAMES && settled) {
            FrameSample fs = {.frame_ms = (float)frame_ms, .cpu_ms = (float)cpu_ms, .stream_ms = (float)stream_ms,
                              .render_ms = (float)((cpu_end - render_start) * 1000.0), .swap_ms = (float)(frame_ms - cpu_ms),
                              .scale = post_scale(), .draw_calls = g_scene_stats.draw_calls, .uploads = g_scene_stats.uploads_this_frame,
                              .vertices = (u32)g_scene_stats.vertices_drawn,
                              .entity_draw_calls = g_entity_stats.draw_calls, .entity_instances = g_entity_stats.instances, .entity_culled = g_entity_stats.culled,
                              .entity_lod = {g_entity_stats.lod[0], g_entity_stats.lod[1], g_entity_stats.lod[2]}, .entity_upload_bytes = g_entity_stats.upload_bytes};
            perf_record_frame(&fs);
        }
        if (!gl) sleep_ms(1);
        frame++;
    }
    if (matrix) perf_matrix_end();
    if (g_opt.benchmark) {
        if (!matrix) perf_report(time_now_s() - bench_start, cold);
        print_stream_report(cold);
    }
    if (persist) {
        SaveMeta *m = save_meta();
        m->has_player = true;
        if (play) {
            m->x = g_player.pos.x; m->y = g_player.pos.y; m->z = g_player.pos.z;
            m->yaw = g_player.yaw; m->pitch = g_player.pitch;
            m->flying = g_player.flying;
            m->health = g_player.health;
            m->dead = g_player.dead;
            snprintf(m->player_id, sizeof m->player_id, "%s", g_player.id);
            snprintf(m->player_name, sizeof m->player_name, "%s", g_player.name);
            snprintf(m->player_state, sizeof m->player_state, "%s", g_player.mod_state);
            hud_set_inventory_open(false);
            inventory_store(&g_inv, g_creative, m);
        } else {
            m->x = cam.pos.x; m->y = cam.pos.y; m->z = cam.pos.z;
            m->yaw = cam.yaw; m->pitch = cam.pitch;
        }
        m->day_time = game_time_get();
    }
    if (play) { dfe_event_t ev = {.name = "player_leave", .text = "player"}; event_fire(&ev); }
    { dfe_event_t ev = {.name = "world_unload"}; event_fire(&ev); }
    if (persist) entity_save();
    server_shutdown();
    world_shutdown();
    save_close();
    mods_unload_plugins();
    script_shutdown();
    events_clear_all();
    jobs_shutdown();
    if (gl) {
        scene_shutdown();
        entity_gl_shutdown();
        post_shutdown();
        perf_shutdown();
        hud_shutdown();
        textures_destroy();
        menu_gl_shutdown();
        ui_shutdown();
        window_destroy();
    }
    return menu_return_to_title_requested() ? 2 : 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0); /* keep log lines ordered with stderr and visible if the process is killed */
    if (argc >= 2 && (!strcmp(argv[1], "mod") || (argc >= 3 && !strcmp(argv[1], "--headless") && !strcmp(argv[2], "mod")))) {
        if (argc >= 3 && !strcmp(argv[1], "--headless")) g_opt.headless = true;
        return modtool_run(argc, argv);
    }
    if (!parse_args(argc, argv)) return 2;
    if (!setup_vfs()) return 1;
    if (g_diag_argv) return hydro_diag_run(g_diag_argc, g_diag_argv);
    if (g_opt.selftest) return selftest_run() == 0 ? 0 : 1;
    if (g_opt.repair_world) {
        char dir[600]; snprintf(dir, sizeof dir, "saves/%s", g_opt.world_name);
        return save_repair_world(dir) ? 0 : 1;
    }
    int result;
    do {
        result = run_viewer();
    } while (result == 2);
    return result;
}
