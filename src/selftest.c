/* Self-test driver. Each module contributes a group of checks through a function
 * declared here; failures are counted and reported with file and line. */
#include "dfe.h"

static int g_checks, g_failures;

void selftest_check(bool ok, const char *expr, const char *file, int line) {
    g_checks++;
    if (ok) return;
    g_failures++;
    fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, expr);
}

static int g_job_order[64];
static int g_job_order_n;
static Mutex *g_job_mutex;

static void job_record(void *data, int worker) {
    mutex_lock(g_job_mutex);
    g_job_order[g_job_order_n++] = (int)(intptr_t)data;
    mutex_unlock(g_job_mutex);
}

static int g_complete_sum;
static void job_complete_add(void *data) { g_complete_sum += (int)(intptr_t)data; }

static volatile int g_gate;
static void gate_job(void *d, int w) { while (!g_gate) sleep_ms(1); }

static void test_jobs(void) {
    g_job_mutex = mutex_create();
    jobs_init(1);
    /* With a single worker blocked behind a gate job, queued jobs must run in priority order. */
    jobs_submit(JOB_KIND_OTHER, 0.0f, gate_job, NULL, NULL);
    sleep_ms(30);
    jobs_submit(JOB_KIND_OTHER, 5.0f, job_record, NULL, (void *)(intptr_t)5);
    jobs_submit(JOB_KIND_OTHER, 1.0f, job_record, NULL, (void *)(intptr_t)1);
    jobs_submit(JOB_KIND_OTHER, 3.0f, job_record, NULL, (void *)(intptr_t)3);
    g_gate = 1;
    jobs_wait_idle();
    CHECK(g_job_order_n == 3);
    CHECK(g_job_order[0] == 1 && g_job_order[1] == 3 && g_job_order[2] == 5);
    for (int i = 1; i <= 10; i++) jobs_submit(JOB_KIND_OTHER, (float)i, job_record, job_complete_add, (void *)(intptr_t)i);
    jobs_wait_idle();
    while (jobs_in_flight() > 0) jobs_pump(1.0);
    CHECK(g_complete_sum == 55);
    jobs_shutdown();
    mutex_destroy(g_job_mutex);
}

static void test_base(void) {
    StrMap m;
    strmap_init(&m);
    char key[32];
    for (int i = 0; i < 1000; i++) { snprintf(key, sizeof key, "mod:block_%d", i); strmap_set(&m, key, (u32)i * 3); }
    u32 v = 0;
    CHECK(strmap_get(&m, "mod:block_777", &v) && v == 2331);
    CHECK(!strmap_get(&m, "mod:missing", &v));
    strmap_set(&m, "mod:block_5", 99);
    CHECK(strmap_get(&m, "mod:block_5", &v) && v == 99 && m.count == 1000);
    strmap_free(&m);

    M4 p = m4_perspective(70.0f * DEG2RAD, 16.0f / 9.0f, 0.1f, 500.0f);
    M4 inv = m4_inverse(p);
    M4 id = m4_mul(p, inv);
    bool ident = true;
    for (int i = 0; i < 16; i++) ident &= fabsf(id.m[i] - (i % 5 == 0 ? 1.0f : 0.0f)) < 1e-3f;
    CHECK(ident);

    Camera cam = {.pos = v3(0, 0, 0), .yaw = 0, .pitch = 0, .fov_y = 70.0f * DEG2RAD, .znear = 0.1f, .zfar = 100.0f};
    camera_update(&cam, 1.0f);
    CHECK(frustum_box_visible(&cam.frustum, v3(-1, -1, -11), v3(1, 1, -9)));
    CHECK(!frustum_box_visible(&cam.frustum, v3(-1, -1, 9), v3(1, 1, 11)));
    CHECK(!frustum_box_visible(&cam.frustum, v3(500, -1, -11), v3(501, 1, -9)));

    CHECK(floor_div(-1, 32) == -1 && floor_mod(-1, 32) == 31 && floor_div(32, 32) == 1);
    CHECK(hash3(7, 1, 2, 3) == hash3(7, 1, 2, 3) && hash3(7, 1, 2, 3) != hash3(7, 3, 2, 1));
    double vals[5] = {5, 1, 4, 2, 3};
    CHECK(fabs(percentile_of(vals, 5, 50) - 3.0) < 1e-9 && fabs(percentile_of(vals, 5, 100) - 5.0) < 1e-9);
}

static void test_vfs(void) {
    /* Two roots: the later one must win for a shared path while unique files stay visible. */
    char a[] = "selftest_vfs_a", b[] = "selftest_vfs_b";
    dir_make_all("selftest_vfs_a/sub");
    dir_make_all("selftest_vfs_b/sub");
    file_write_atomic("selftest_vfs_a/sub/x.txt", "low", 3);
    file_write_atomic("selftest_vfs_a/only_a.txt", "a", 1);
    file_write_atomic("selftest_vfs_b/sub/x.txt", "high", 4);
    vfs_reset();
    vfs_add_root(a, "first");
    vfs_add_root(b, "second");
    size_t n = 0;
    const char *owner = NULL;
    u8 *data = vfs_read("sub/x.txt", &n, &owner);
    CHECK(data && n == 4 && memcmp(data, "high", 4) == 0 && strcmp(owner, "second") == 0);
    free(data);
    CHECK(vfs_exists("only_a.txt") && !vfs_exists("nope.txt"));
    StrList l = {0};
    vfs_list("sub", &l);
    CHECK(l.n == 1);
    strlist_free(&l);
    remove("selftest_vfs_a/sub/x.txt"); remove("selftest_vfs_a/only_a.txt"); remove("selftest_vfs_b/sub/x.txt");
    remove("selftest_vfs_a/sub"); remove("selftest_vfs_b/sub"); remove("selftest_vfs_a"); remove("selftest_vfs_b");
    vfs_reset();
}

int selftest_run(void) {
    struct { const char *name; void (*fn)(void); } groups[] = {
        {"base", test_base},
        {"jobs", test_jobs},
        {"vfs", test_vfs},
    };
    for (int i = 0; i < ARRAY_LEN(groups); i++) {
        int before = g_failures;
        groups[i].fn();
        printf("selftest %-14s %s\n", groups[i].name, g_failures == before ? "ok" : "FAILED");
    }
    printf("selftest total: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
