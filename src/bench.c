/* Benchmark matrix: a list of preset and window size cases that run inside one launch.
 *
 * Relaunching the engine for every case spends most of the time on start-up and world loading. Running the cases in
 * one process removes that, at a price that is stated rather than hidden: the world and driver caches stay warm, so
 * the matrix measures steady-state rendering. Streaming cost and cold start are measured by the single-case
 * benchmark, which starts from nothing. */
#include "dfe.h"

#define MAX_BENCH_CASES 256

static BenchCase g_cases[MAX_BENCH_CASES];
static int g_total;

static bool parse_one(const char *text, BenchCase *out) {
    char preset[BENCH_PRESET_MAX];
    int w, h;
    if (sscanf(text, "%31[^:]:%dx%d", preset, &w, &h) != 3 || w < 160 || h < 120 || w > 16384 || h > 16384) {
        fprintf(stderr, "--bench-matrix case '%s' is not preset:WIDTHxHEIGHT, for example high:1920x1080\n", text);
        return false;
    }
    if (!preset_find(preset)) {
        fprintf(stderr, "--bench-matrix names preset '%s', which does not exist. Installed presets:", preset);
        for (int i = 0; i < preset_count(); i++) fprintf(stderr, " %s", preset_at(i)->id);
        fprintf(stderr, "\n");
        return false;
    }
    snprintf(out->preset, sizeof out->preset, "%s", preset);
    out->width = w;
    out->height = h;
    return true;
}

bool bench_matrix_parse(const char *spec, int runs) {
    BenchCase list[MAX_BENCH_CASES];
    int n = 0;
    char copy[512];
    snprintf(copy, sizeof copy, "%s", spec);
    for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ",")) {
        if (n >= MAX_BENCH_CASES || !parse_one(tok, &list[n])) { if (n >= MAX_BENCH_CASES) fprintf(stderr, "--bench-matrix has too many cases\n"); return false; }
        n++;
    }
    if (!n || runs < 1 || n * runs > MAX_BENCH_CASES) {
        fprintf(stderr, "--bench-matrix needs at least one case, and cases times --bench-runs must not exceed %d\n", MAX_BENCH_CASES);
        return false;
    }
    /* Run after run rather than case after case, so slow drift such as heat spreads over all cases evenly. */
    g_total = 0;
    for (int r = 0; r < runs; r++) for (int i = 0; i < n; i++) g_cases[g_total++] = list[i];
    return true;
}

int bench_case_total(void) { return g_total; }
const BenchCase *bench_case_at(int index) { return index >= 0 && index < g_total ? &g_cases[index] : NULL; }

void bench_case_apply(const BenchCase *c) {
    snprintf(g_settings.preset, sizeof g_settings.preset, "%s", c->preset);
    window_resize(c->width, c->height);
    if (g_win.fb_width != c->width || g_win.fb_height != c->height)
        fprintf(stderr, "warning: case %s:%dx%d was asked for but the window is %dx%d. The window manager overrode the size, so this case does not measure what it names. Start with --hidden, which a window manager does not resize.\n",
                c->preset, c->width, c->height, g_win.fb_width, g_win.fb_height);
    gfx_apply();
}
