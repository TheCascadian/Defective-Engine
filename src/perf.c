/* Performance measurement: GPU pass timers, per-frame samples and the benchmark report.
 *
 * GPU time comes from GL_TIME_ELAPSED queries, which are core in OpenGL 3.3. Results are read several frames after
 * they were issued so the read never stalls the pipeline. Queries cannot nest, so the sections are sequential. */
#include "dfe.h"

#define QUERY_RING 4            /* frames in flight before a result is read; deep enough for triple buffering */
#define HITCH_MS 25.0           /* the budget: no single frame above this */
#define HITCHES_LISTED 8
#define BUDGET_FPS_AVG 60.0
#define BUDGET_FPS_LOW1 40.0
#define BUDGET_MEMORY_MB 1536.0
#define BUDGET_COLD_START_S 10.0
#define NS_TO_MS 1e-6

static const char *const SECTION_NAME[GPU_SECTION_COUNT] = {"opaque", "cutout", "sky", "water", "rain", "ui", "post", "entity"};

static struct {
    GLuint queries[QUERY_RING][GPU_SECTION_COUNT];
    bool issued[QUERY_RING][GPU_SECTION_COUNT];
    int slot;                   /* ring slot being written this frame */
    int active;                 /* section with an open query, or -1 */
    bool ok;                    /* at least one result has come back */
    bool created;
    float latest[GPU_SECTION_COUNT];
    FrameSample *samples;
    int count, cap;
} P;

void perf_init(void) {
    for (int r = 0; r < QUERY_RING; r++) glGenQueries(GPU_SECTION_COUNT, P.queries[r]);
    P.created = true;
    P.active = -1;
}

void perf_shutdown(void) {
    if (P.created) for (int r = 0; r < QUERY_RING; r++) glDeleteQueries(GPU_SECTION_COUNT, P.queries[r]);
    P.created = false;
    free(P.samples);
    P.samples = NULL;
    P.count = P.cap = 0;
}

void perf_gpu_frame_begin(void) {
    if (!P.created) return;
    P.slot = (P.slot + 1) % QUERY_RING;
    /* The slot about to be reused holds the oldest frame, the one most likely to be finished. */
    for (int s = 0; s < GPU_SECTION_COUNT; s++) {
        if (!P.issued[P.slot][s]) continue;
        GLuint avail = 0;
        glGetQueryObjectuiv(P.queries[P.slot][s], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) continue;
        GLuint64 ns = 0;
        glGetQueryObjectui64v(P.queries[P.slot][s], GL_QUERY_RESULT, &ns);
        P.latest[s] = (float)((double)ns * NS_TO_MS);
        P.ok = true;
        P.issued[P.slot][s] = false;
    }
}

void perf_gpu_begin(GpuSection s) {
    if (!P.created || P.active >= 0) return;
    glBeginQuery(GL_TIME_ELAPSED, P.queries[P.slot][s]);
    P.active = (int)s;
}

void perf_gpu_end(void) {
    if (!P.created || P.active < 0) return;
    glEndQuery(GL_TIME_ELAPSED);
    P.issued[P.slot][P.active] = true;
    P.active = -1;
}

bool perf_gpu_available(void) { return P.ok; }
float perf_gpu_latest_ms(GpuSection s) { return P.latest[s]; }

void perf_record_frame(const FrameSample *in) {
    if (P.count == P.cap) {
        P.cap = P.cap ? P.cap * 2 : 4096;
        P.samples = xrealloc(P.samples, (size_t)P.cap * sizeof *P.samples);
    }
    FrameSample *s = &P.samples[P.count++];
    *s = *in;
    for (int k = 0; k < GPU_SECTION_COUNT; k++) {
        s->gpu_section_ms[k] = P.latest[k];
        s->gpu_ms += P.latest[k];
    }
}

/* ---------------------------------------------------------------- report */

typedef struct Summary {
    int n;
    double wall_s, fps_avg, fps_low1, avg_ms, p50, p95, p99, max_ms;
    int hitches;
    double cpu_avg, stream_avg, render_avg, swap_avg, gpu_avg;
    double section_avg[GPU_SECTION_COUNT];
    double peak_mb, cold_s, scale_avg, scale_min;
    double draw_calls_avg, ent_calls_avg, ent_instances_avg, ent_culled_avg, ent_lod_avg[3], ent_upload_avg;
} Summary;

typedef float (*FrameField)(const FrameSample *);
static float field_frame(const FrameSample *s) { return s->frame_ms; }

static double *collect(FrameField get) {
    double *v = xmalloc((size_t)MAX(P.count, 1) * sizeof(double));
    for (int i = 0; i < P.count; i++) v[i] = get(&P.samples[i]);
    return v;
}

static int desc_cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) - (x > y);
}

/* One percent low is the mean fps of the slowest one percent of frames, the common definition. */
static double low1_fps(const double *frame_ms, int n) {
    double *sorted = xmalloc((size_t)n * sizeof(double));
    memcpy(sorted, frame_ms, (size_t)n * sizeof(double));
    qsort(sorted, (size_t)n, sizeof(double), desc_cmp);
    int worst = MAX(1, n / 100);
    double sum = 0;
    for (int i = 0; i < worst; i++) sum += sorted[i];
    free(sorted);
    return 1000.0 / (sum / worst);
}

static void summarise(Summary *out, double wall_s, double cold_s) {
    memset(out, 0, sizeof *out);
    int n = P.count;
    out->n = n;
    out->wall_s = wall_s;
    out->cold_s = cold_s;
    out->peak_mb = (double)mem_peak_rss_bytes() / 1048576.0;
    if (!n) return;
    double *ms = collect(field_frame);
    for (int i = 0; i < n; i++) {
        const FrameSample *s = &P.samples[i];
        out->avg_ms += s->frame_ms;
        out->cpu_avg += s->cpu_ms;
        out->stream_avg += s->stream_ms;
        out->render_avg += s->render_ms;
        out->swap_avg += s->swap_ms;
        out->gpu_avg += s->gpu_ms;
        out->scale_avg += s->scale;
        out->scale_min = i ? MIN(out->scale_min, s->scale) : s->scale;
        for (int k = 0; k < GPU_SECTION_COUNT; k++) out->section_avg[k] += s->gpu_section_ms[k];
        out->draw_calls_avg += s->draw_calls;
        out->ent_calls_avg += s->entity_draw_calls;
        out->ent_instances_avg += s->entity_instances;
        out->ent_culled_avg += s->entity_culled;
        out->ent_upload_avg += s->entity_upload_bytes;
        for (int l = 0; l < 3; l++) out->ent_lod_avg[l] += s->entity_lod[l];
        if (s->frame_ms > HITCH_MS) out->hitches++;
    }
    out->draw_calls_avg /= n; out->ent_calls_avg /= n; out->ent_instances_avg /= n; out->ent_culled_avg /= n; out->ent_upload_avg /= n;
    for (int l = 0; l < 3; l++) out->ent_lod_avg[l] /= n;
    out->avg_ms /= n; out->cpu_avg /= n; out->stream_avg /= n; out->render_avg /= n; out->swap_avg /= n; out->gpu_avg /= n; out->scale_avg /= n;
    for (int k = 0; k < GPU_SECTION_COUNT; k++) out->section_avg[k] /= n;
    out->fps_avg = wall_s > 0 ? n / wall_s : 0;
    out->fps_low1 = low1_fps(ms, n);
    out->p50 = percentile_of(ms, n, 50);
    out->p95 = percentile_of(ms, n, 95);
    out->p99 = percentile_of(ms, n, 99);
    out->max_ms = percentile_of(ms, n, 100);
    free(ms);
}

static const char *verdict(bool pass) { return pass ? "pass" : "FAIL"; }

/* Lists the worst frames rather than the first ones, since the first are dominated by start-up streaming. */
static void print_hitches(void) {
    bool *listed = xcalloc((size_t)MAX(P.count, 1), sizeof(bool));
    for (int shown = 0; shown < HITCHES_LISTED; shown++) {
        int worst = -1;
        for (int i = 0; i < P.count; i++) if (!listed[i] && P.samples[i].frame_ms > HITCH_MS && (worst < 0 || P.samples[i].frame_ms > P.samples[worst].frame_ms)) worst = i;
        if (worst < 0) break;
        const FrameSample *s = &P.samples[worst];
        if (!shown) printf("worst frames above %.0f ms (index, total, stream / render / swap, gpu, uploads):\n", HITCH_MS);
        printf("  #%-5d %7.2f ms   %6.2f / %6.2f / %6.2f   gpu %6.2f   uploads %d\n", worst, s->frame_ms, s->stream_ms, s->render_ms, s->swap_ms, s->gpu_ms, s->uploads);
        listed[worst] = true;
    }
    free(listed);
}

static void print_text_report(const Summary *m) {
    printf("\n=== benchmark ===\n");
    if (g_opt.bench_label[0]) printf("label: %s\n", g_opt.bench_label);
    printf("gl: %s\n", gl_info_string());
    printf("setup: %dx%d  preset %s  render distance %d  far %d  workers %d  cores %d\n", g_win.fb_width, g_win.fb_height, g_settings.preset,
           g_scene_cfg.render_distance, g_scene_cfg.far_chunks, jobs_worker_count(), cpu_count());
    printf("frames: %d in %.2f s   render scale avg %.2f min %.2f\n", m->n, m->wall_s, m->scale_avg, m->scale_min);
    printf("fps avg: %.1f   1%% low: %.1f\n", m->fps_avg, m->fps_low1);
    printf("frame ms  avg %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f   hitches %d\n", m->avg_ms, m->p50, m->p95, m->p99, m->max_ms, m->hitches);
    printf("cpu ms    avg %.2f  (stream %.2f, render submit %.2f)   swap wait avg %.2f\n", m->cpu_avg, m->stream_avg, m->render_avg, m->swap_avg);
    if (P.ok) {
        printf("gpu ms    avg %.2f  (", m->gpu_avg);
        for (int k = 0; k < GPU_SECTION_COUNT; k++) printf("%s%s %.2f", k ? ", " : "", SECTION_NAME[k], m->section_avg[k]);
        printf(")\n");
    } else {
        printf("gpu ms    unavailable (the driver returned no timer query results)\n");
    }
    printf("draw calls (last frame): %d, average %.1f\n", g_stats.draw_calls_last, m->draw_calls_avg);
    printf("entities: %.1f draws, %.1f instances, %.1f culled, lod %.1f/%.1f/%.1f, %.0f B uploaded per frame%s\n", m->ent_calls_avg, m->ent_instances_avg, m->ent_culled_avg,
           m->ent_lod_avg[0], m->ent_lod_avg[1], m->ent_lod_avg[2], m->ent_upload_avg, g_opt.entity_legacy ? " (legacy path)" : "");
    printf("peak memory: %.1f MB\n", m->peak_mb);
    print_hitches();
    printf("budget (reference machine, Low, 720p, render distance 8):\n");
    printf("  fps avg >= %.0f: %s   1%% low >= %.0f: %s   no frame above %.0f ms: %s   memory < %.0f MB: %s   cold start < %.0f s: %s\n",
           BUDGET_FPS_AVG, verdict(m->fps_avg >= BUDGET_FPS_AVG), BUDGET_FPS_LOW1, verdict(m->fps_low1 >= BUDGET_FPS_LOW1), HITCH_MS,
           verdict(m->hitches == 0), BUDGET_MEMORY_MB, verdict(m->peak_mb < BUDGET_MEMORY_MB), BUDGET_COLD_START_S, verdict(m->cold_s < BUDGET_COLD_START_S));
}

static void write_json_object(FILE *f, const Summary *m) {
    fprintf(f, "{\n  \"label\": \"%s\",\n  \"gl\": \"%s\",\n", g_opt.bench_label, gl_info_string());
    fprintf(f, "  \"width\": %d, \"height\": %d, \"preset\": \"%s\", \"render_distance\": %d, \"far_chunks\": %d, \"workers\": %d, \"cores\": %d,\n",
            g_win.fb_width, g_win.fb_height, g_settings.preset, g_scene_cfg.render_distance, g_scene_cfg.far_chunks, jobs_worker_count(), cpu_count());
    fprintf(f, "  \"frames\": %d, \"seconds\": %.3f, \"fps_avg\": %.2f, \"fps_low1\": %.2f, \"scale_avg\": %.3f, \"scale_min\": %.3f,\n", m->n, m->wall_s, m->fps_avg, m->fps_low1, m->scale_avg, m->scale_min);
    fprintf(f, "  \"frame_ms\": {\"avg\": %.3f, \"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, \"max\": %.3f}, \"hitches\": %d,\n", m->avg_ms, m->p50, m->p95, m->p99, m->max_ms, m->hitches);
    fprintf(f, "  \"cpu_ms\": {\"total\": %.3f, \"stream\": %.3f, \"render\": %.3f, \"swap\": %.3f},\n", m->cpu_avg, m->stream_avg, m->render_avg, m->swap_avg);
    fprintf(f, "  \"gpu_available\": %s, \"gpu_ms\": {\"total\": %.3f", P.ok ? "true" : "false", m->gpu_avg);
    for (int k = 0; k < GPU_SECTION_COUNT; k++) fprintf(f, ", \"%s\": %.3f", SECTION_NAME[k], m->section_avg[k]);
    fprintf(f, "},\n  \"draw_calls\": %.1f,\n  \"entities\": {\"requested\": %d, \"legacy\": %s, \"draw_calls\": %.1f, \"instances\": %.1f, \"culled\": %.1f, \"lod\": [%.1f, %.1f, %.1f], \"upload_bytes\": %.0f},\n  \"peak_memory_mb\": %.1f, \"cold_start_s\": %.3f\n}\n",
            m->draw_calls_avg, g_opt.bench_entities, g_opt.entity_legacy ? "true" : "false", m->ent_calls_avg, m->ent_instances_avg, m->ent_culled_avg,
            m->ent_lod_avg[0], m->ent_lod_avg[1], m->ent_lod_avg[2], m->ent_upload_avg, m->peak_mb, m->cold_s);
}

static void write_json(const Summary *m, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { LOGW("cannot write %s; check that the folder exists and is writable", path); return; }
    write_json_object(f, m);
    fclose(f);
}

/* ------------------------------------------------------------ matrix mode */

static FILE *g_matrix_json;
static int g_matrix_written;

void perf_reset_samples(void) { P.count = 0; }

void perf_matrix_begin(void) {
    g_matrix_written = 0;
    if (!g_opt.bench_json[0]) return;
    g_matrix_json = fopen(g_opt.bench_json, "w");
    if (!g_matrix_json) { LOGW("cannot write %s; check that the folder exists and is writable", g_opt.bench_json); return; }
    fprintf(g_matrix_json, "[\n");
}

void perf_matrix_end(void) {
    if (!g_matrix_json) return;
    fprintf(g_matrix_json, "]\n");
    fclose(g_matrix_json);
    g_matrix_json = NULL;
}

static const char *largest_section(const Summary *m, double *ms) {
    int best = 0;
    for (int k = 1; k < GPU_SECTION_COUNT; k++) if (m->section_avg[k] > m->section_avg[best]) best = k;
    *ms = m->section_avg[best];
    return SECTION_NAME[best];
}

void perf_report_case(const BenchCase *c, int index, int total, double wall_s, double cold_start_s) {
    if (!P.count) return;
    Summary m;
    summarise(&m, wall_s, cold_start_s);
    double top_ms;
    const char *top = largest_section(&m, &top_ms);
    printf("[%d/%d] %-8s %dx%d rd %d  %8.1f fps  1%% low %7.1f  p99 %5.1f ms  max %5.1f ms  hitches %d  gpu %5.2f ms (%s %.2f)\n", index + 1, total, c->preset,
           g_win.fb_width, g_win.fb_height, g_scene_cfg.render_distance, m.fps_avg, m.fps_low1, m.p99, m.max_ms, m.hitches, m.gpu_avg, top, top_ms);
    fflush(stdout);
    if (g_matrix_json) {
        if (g_matrix_written++) fprintf(g_matrix_json, ",\n");
        write_json_object(g_matrix_json, &m);
    }
}

static void write_csv(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { LOGW("cannot write %s; check that the folder exists and is writable", path); return; }
    fprintf(f, "frame,frame_ms,cpu_ms,stream_ms,render_ms,swap_ms,gpu_ms");
    for (int k = 0; k < GPU_SECTION_COUNT; k++) fprintf(f, ",gpu_%s_ms", SECTION_NAME[k]);
    fprintf(f, ",scale,draw_calls,vertices,uploads\n");
    for (int i = 0; i < P.count; i++) {
        const FrameSample *s = &P.samples[i];
        fprintf(f, "%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", i, s->frame_ms, s->cpu_ms, s->stream_ms, s->render_ms, s->swap_ms, s->gpu_ms);
        for (int k = 0; k < GPU_SECTION_COUNT; k++) fprintf(f, ",%.3f", s->gpu_section_ms[k]);
        fprintf(f, ",%.3f,%d,%u,%d\n", s->scale, s->draw_calls, s->vertices, s->uploads);
    }
    fclose(f);
}

void perf_report(double wall_s, double cold_start_s) {
    if (!P.count) return;
    Summary m;
    summarise(&m, wall_s, cold_start_s);
    print_text_report(&m);
    if (g_opt.bench_json[0]) write_json(&m, g_opt.bench_json);
    if (g_opt.bench_csv[0]) write_csv(g_opt.bench_csv);
}
