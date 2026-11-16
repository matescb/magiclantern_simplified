/* Host-side behavioural test for the raw_vidx module's worker.c + event_pusher.c.
 *
 * The two module files are compiled UNMODIFIED (32-bit, they cast pointers to uint32_t)
 * against stub headers in stubs/. This file supplies a single-threaded model of DryOS:
 *   - the worker task is a coroutine; it yields to the test when its queue is empty
 *   - "stop_com" tasks run inline
 *   - "raw_write" tasks run inline, or (defer_writes=1) only once the worker blocks,
 *     which models the write task having lower priority than the worker
 *   - FIO is an in-memory file system that remembers, per file, the contents at the
 *     last FIO_CloseFile ("committed" = what would survive a wedge + power cycle)
 * mlv_3.c is replaced by fake_mlv.c. Nothing here says anything about the camera.
 *
 * Usage: hosttest <test-name>     (one test per process; see run.sh)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ucontext.h>

/* ---- module interface (no ML headers here: ML's FILE clashes with stdio) ---- */
struct msg_queue;
struct semaphore { int count; };
struct hosttest_cbr { int type; unsigned int (*fn)(unsigned int); };
extern struct hosttest_cbr hosttest_cbrs[];
void init_event_pusher(struct msg_queue *q);
void init_worker(struct msg_queue *q);
int get_recording_state(void); /* enum: 0 ERROR, 1 INACTIVE, 2 ACTIVE */
extern int rvx_wait_raw_job; /* must be 0 here: the poll reads Canon ROM addresses */
extern int rvx_checkpoint __attribute__((weak));
extern int rvx_dropped_queue __attribute__((weak));
extern int rvx_dropped_busy __attribute__((weak));
extern int fake_mlv_session_in_progress;
/* C12: toggles and telemetry of rvx_step1.c / event_pusher.c */
extern int rvx_write_prio, rvx_copy_prio, rvx_copy_chunk_kb, rvx_copy_sleep_ms;
extern int rvx_burst_frames, rvx_burst_release_first;
extern int rvx_integrity, rvx_sidecar, rvx_drop_torn, rvx_fast_copy;
extern int rvx_torn, rvx_dst_diff, rvx_copy_task_fail;
extern int rvx_crop_w, rvx_crop_h, rvx_crop_x, rvx_crop_y;
extern int rvx_src_from_raw_info;
extern void (*rvx_dcache_inval)(uint32_t addr, uint32_t size);

struct raw_info { void *buffer; int height, width, pitch; };
struct raw_info raw_info;

/* ---- test knobs / observations ---- */
static int verbose = 0;
static int defer_writes = 0;
static int fail_create = 0;      /* next N FIO_CreateFile calls fail */
static int raw_lv_requests = 0;  /* raw_lv_request minus raw_lv_release */
static int fio_live = 0;         /* outstanding fio_malloc blocks */
static int bad_handle_use = 0;   /* FIO call on a closed handle */
static int write_from_freed = 0; /* FIO_WriteFile source inside a freed fio block */
/* C12 observations */
static int data_writes = 0;          /* FIO_WriteFile calls larger than a header */
static int data_writes_while_active = 0; /* ... made while recording_state was ACTIVE */
static int lv_requests_at_first_data_write = -1;
static int fio_live_max = 0;
static int prio_min_bulk = 0xFF;     /* highest priority (lowest number) of raw_write / rvx_copy */
static int prio_last_write = -1, prio_last_copy = -1, copy_tasks = 0;
static int copy_sleeps = 0;          /* msleep calls made from inside a frame copy */
static int in_copy = 0;              /* a memcpy64 / throttle pause belongs to a copy */
static int sensor_live = 0;          /* 1: the "sensor" rewrites the raw buffer at every pause */
static uint32_t sensor_gen = 0;
static int inval_calls = 0, inval_bad = 0, copy64_calls = 0;
static char logbuf[1 << 20];
static size_t loglen = 0;

void m50ii_uart_str(const char *s)
{
    size_t n = strlen(s);
    if (loglen + n < sizeof(logbuf) - 1) { memcpy(logbuf + loglen, s, n); loglen += n; logbuf[loglen] = 0; }
    if (verbose) fputs(s, stderr);
}
static int log_has(const char *s) { return strstr(logbuf, s) != NULL; }

/* ---- memory ---- */
#define MAX_REG 64
static struct { char *p; size_t n; int live; } reg[MAX_REG];
void *fio_malloc(size_t n)
{
    char *p = malloc(n);
    if (!p) return NULL;
    for (int i = 0; i < MAX_REG; i++) /* forget freed records the new block overlaps */
        if (reg[i].p && !reg[i].live && p < reg[i].p + reg[i].n && reg[i].p < p + n) reg[i].p = NULL;
    for (int i = 0; i < MAX_REG; i++)
        if (!reg[i].p) { reg[i].p = p; reg[i].n = n; reg[i].live = 1; fio_live++; if (fio_live > fio_live_max) fio_live_max = fio_live; return p; }
    fprintf(stderr, "harness: region table full\n"); exit(99);
}
void fio_free(void *p)
{
    for (int i = 0; i < MAX_REG; i++)
        if (reg[i].p == p && reg[i].live) { reg[i].live = 0; fio_live--; free(p); return; }
    fprintf(stderr, "harness: fio_free of unknown block\n"); exit(99);
}
static int in_freed(const char *p)
{
    for (int i = 0; i < MAX_REG; i++)
        if (reg[i].p && !reg[i].live && p >= reg[i].p && p < reg[i].p + reg[i].n) return 1;
    return 0;
}

/* ---- files ---- */
#define MAX_FILES 128
struct mfile { char name[24]; char *data; size_t size, cap; char *committed; size_t csize; int exists; };
static struct mfile files[MAX_FILES];
struct ml_file { struct mfile *f; size_t pos; int open; };

static struct mfile *find_file(const char *name)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].exists && !strcmp(files[i].name, name)) return &files[i];
    return NULL;
}
struct ml_file *FIO_CreateFile(const char *name)
{
    if (fail_create > 0) { fail_create--; return NULL; }
    struct mfile *f = find_file(name);
    if (!f)
        for (int i = 0; i < MAX_FILES && !f; i++)
            if (!files[i].exists) { f = &files[i]; memset(f, 0, sizeof(*f)); snprintf(f->name, sizeof(f->name), "%s", name); f->exists = 1; }
    if (!f) return NULL;
    f->size = 0;
    struct ml_file *h = calloc(1, sizeof(*h));
    h->f = f; h->open = 1;
    return h;
}
struct ml_file *FIO_OpenFile(const char *name, unsigned mode)
{
    (void)mode;
    struct mfile *f = find_file(name);
    if (!f) return NULL;
    struct ml_file *h = calloc(1, sizeof(*h));
    h->f = f; h->open = 1;
    return h;
}
int FIO_WriteFile(struct ml_file *h, const void *p, size_t n)
{
    if (!h || !h->open) { bad_handle_use++; return -1; }
    if (in_freed(p)) write_from_freed++;
    if (n > 4096)
    {
        data_writes++;
        if (get_recording_state() == 2 && !strstr(logbuf, "Worker got stop request")) data_writes_while_active++;
        if (lv_requests_at_first_data_write < 0) lv_requests_at_first_data_write = raw_lv_requests;
    }
    struct mfile *f = h->f;
    if (h->pos + n > f->cap) { f->cap = (h->pos + n) * 2; f->data = realloc(f->data, f->cap); }
    memcpy(f->data + h->pos, p, n);
    h->pos += n;
    if (h->pos > f->size) f->size = h->pos;
    return (int)n;
}
int64_t FIO_SeekSkipFile(struct ml_file *h, int64_t pos, int whence)
{
    if (!h || !h->open) { bad_handle_use++; return -1; }
    if (whence == 0) h->pos = (size_t)pos;
    else if (whence == 1) h->pos += (size_t)pos;
    else h->pos = h->f->size + (size_t)pos;
    return (int64_t)h->pos;
}
void FIO_CloseFile(struct ml_file *h)
{
    if (!h || !h->open) { bad_handle_use++; return; }
    h->open = 0; /* handle struct is leaked on purpose so later misuse is detectable */
    struct mfile *f = h->f;
    free(f->committed);
    f->committed = malloc(f->size ? f->size : 1);
    memcpy(f->committed, f->data, f->size);
    f->csize = f->size;
}
int get_numbered_file_name(const char *pattern, int nmax, char *filename, int maxlen)
{
    for (int i = 0; i <= nmax; i++)
    {
        snprintf(filename, maxlen, pattern, i);
        if (!find_file(filename)) return i;
    }
    return -1;
}

/* ---- misc camera stubs ---- */
void raw_lv_request(void) { raw_lv_requests++; }
void raw_lv_release(void) { raw_lv_requests--; }
void raw_lv_redirect_edmac(void *p) { (void)p; }
void edmac_copy_rectangle_adv_cleanup(void) {}
int fps_get_current_x1000(void) { return 23976; }
int liveview_display_idle(void) { return 1; }
int is_movie_mode(void) { return 1; }
static uint32_t fake_ms = 0;
uint32_t get_ms_clock(void) { return fake_ms++; }
uint64_t get_us_clock(void) { return (uint64_t)fake_ms * 1000; }

/* ---- scheduler model ---- */
static ucontext_t main_ctx, worker_ctx;
static void (*worker_entry)(void) = NULL;
static int in_worker = 0, worker_started = 0;
static struct { void (*fn)(void *); void *arg; } pending[16];
static int npending = 0;

static int run_pending(void)
{
    int ran = 0;
    while (npending > 0)
    {
        void (*fn)(void *) = pending[0].fn; void *arg = pending[0].arg;
        memmove(&pending[0], &pending[1], sizeof(pending[0]) * (size_t)(--npending));
        fn(arg);
        ran = 1;
    }
    return ran;
}
static void run_worker(void) /* let the worker run until its queue is empty */
{
    if (!worker_started)
    {
        getcontext(&worker_ctx);
        worker_ctx.uc_stack.ss_size = 4 << 20;
        worker_ctx.uc_stack.ss_sp = malloc(worker_ctx.uc_stack.ss_size);
        worker_ctx.uc_link = &main_ctx;
        makecontext(&worker_ctx, worker_entry, 0);
        worker_started = 1;
    }
    in_worker = 1;
    swapcontext(&main_ctx, &worker_ctx);
    in_worker = 0;
}

struct semaphore *create_named_semaphore(const char *name, int initial)
{
    (void)name;
    struct semaphore *s = calloc(1, sizeof(*s));
    s->count = initial ? 1 : 0;
    return s;
}
int take_semaphore_now(struct semaphore *s) { if (s->count) { s->count = 0; return 0; } return 1; }
int take_semaphore(struct semaphore *s, int timeout_ms)
{
    (void)timeout_ms;
    if (!take_semaphore_now(s)) return 0;
    /* we would block: lower-priority tasks get to run */
    while (run_pending())
        if (!take_semaphore_now(s)) return 0;
    return 1; /* timeout */
}
int give_semaphore(struct semaphore *s) { s->count = 1; return 0; }

struct msg_queue { uint32_t *m; uint32_t len, n; };
struct msg_queue *msg_queue_create(const char *name, uint32_t len)
{
    (void)name;
    struct msg_queue *q = calloc(1, sizeof(*q));
    q->m = calloc(len, sizeof(uint32_t)); q->len = len;
    return q;
}
int msg_queue_post(struct msg_queue *q, uint32_t msg)
{
    if (q->n == q->len) return 1;
    q->m[q->n++] = msg;
    return 0;
}
int msg_queue_receive(struct msg_queue *q, void *msg_out, uint32_t timeout)
{
    (void)timeout;
    while (q->n == 0)
    {
        if (run_pending()) continue;      /* worker blocked: low-prio tasks run */
        swapcontext(&worker_ctx, &main_ctx); /* nothing to do: back to the test */
    }
    *(uint32_t *)msg_out = q->m[0];
    memmove(&q->m[0], &q->m[1], sizeof(uint32_t) * (--q->n));
    return 0;
}
static void fill_sensor(void)
{
    /* every byte depends on row, column and generation, so a copy that mixes two
     * generations, or takes a row from the wrong place, is detectable */
    uint8_t *b = raw_info.buffer;
    for (int row = 0; row < raw_info.height; row++)
        for (int col = 0; col < raw_info.pitch; col++)
            b[row * raw_info.pitch + col] = (uint8_t)(row * 31 + col * 7 + sensor_gen * 13 + (col >> 8));
}
static uint8_t sensor_byte(int row, int col, uint32_t gen)
{
    return (uint8_t)(row * 31 + col * 7 + gen * 13 + (col >> 8));
}
static void host_inval(uint32_t addr, uint32_t size)
{
    uintptr_t lo = (uintptr_t)raw_info.buffer, hi = lo + (size_t)raw_info.pitch * raw_info.height;
    inval_calls++;
    if ((addr & 31) || addr + 32 <= lo || addr + size > hi + 32 || size == 0) inval_bad++;
}
void *memcpy64(void *d, void *s_, size_t n)
{
    if (((uintptr_t)d & 3) || ((uintptr_t)s_ & 3) || (n & 7)) { fprintf(stderr, "harness: memcpy64 contract broken\n"); exit(97); }
    copy64_calls++;
    memcpy(d, s_, n);
    return (char *)d + n;
}
void msleep(int ms)
{
    if (in_copy)
    {
        copy_sleeps++;
        if (sensor_live) { sensor_gen++; fill_sensor(); }
    }
    (void)ms;
    if (!in_worker && worker_started) run_worker(); /* others make progress while we sleep */
    else if (in_worker) run_pending();
}
void *task_create(const char *name, uint32_t prio, uint32_t stack, void *entry, void *arg)
{
    (void)stack;
    if (!strcmp(name, "raw_v_worker")) { worker_entry = (void (*)(void))entry; return (void *)1; }
    if (!strcmp(name, "raw_write")) { prio_last_write = (int)prio; if ((int)prio < prio_min_bulk) prio_min_bulk = (int)prio; }
    if (!strcmp(name, "rvx_copy"))
    {
        prio_last_copy = (int)prio; copy_tasks++;
        if ((int)prio < prio_min_bulk) prio_min_bulk = (int)prio;
        in_copy = 1; ((void (*)(void *))entry)(arg); in_copy = 0;
        return (void *)2; /* even = success */
    }
    if (!strcmp(name, "raw_write") && defer_writes)
    {
        if (npending == 16) { fprintf(stderr, "harness: too many pending tasks\n"); exit(99); }
        pending[npending].fn = (void (*)(void *))entry; pending[npending].arg = arg; npending++;
        return (void *)2;
    }
    ((void (*)(void *))entry)(arg); /* stop_com, or raw_write in inline mode */
    return (void *)2;
}

/* ---- driving the module ---- */
static unsigned int cbr(int type, unsigned int arg)
{
    for (int i = 0; hosttest_cbrs[i].fn; i++)
        if (hosttest_cbrs[i].type == type) return hosttest_cbrs[i].fn(arg);
    fprintf(stderr, "harness: cbr not found\n"); exit(99);
}
static void press_rec(void) { cbr(2, 12 /* MODULE_KEY_REC */); }
static void vsync(void) { cbr(1, 0); }

static void setup(void)
{
    raw_info.width = 2096; raw_info.height = 1164; raw_info.pitch = 2096 * 14 / 8;
    raw_info.buffer = malloc((size_t)raw_info.pitch * (size_t)raw_info.height);
    fill_sensor();
    rvx_dcache_inval = host_inval; /* the module default is a Canon ROM address */
    rvx_wait_raw_job = 0;
    struct msg_queue *q = msg_queue_create("raw_vid_q", 20);
    init_event_pusher(q);
    init_worker(q);
}
/* start; `frames` vsyncs that should each yield a frame; stop (unless no_stop).
 * drain: let the worker run after each vsync. */
static void record(int frames, int drain, int no_stop)
{
    press_rec();
    run_worker();
    vsync();                 /* first vsync of a session is setup only, by design */
    for (int i = 0; i < frames; i++) { vsync(); if (drain) run_worker(); }
    if (no_stop) { run_worker(); return; }
    press_rec();             /* stop_com runs inline and posts COMMAND_STOP */
    run_worker();
    vsync();                 /* LiveView keeps running after the stop */
}

/* ---- reading back ---- */
struct parsed { int valid; uint32_t header_count; uint32_t vidf; };
static struct parsed parse(const char *d, size_t n)
{
    struct parsed r = {0, 0, 0};
    if (!d || n < 64 || memcmp(d, "MLVI", 4)) return r;
    memcpy(&r.header_count, d + 8, 4);
    size_t pos = 64;
    while (pos < n)
    {
        uint32_t sz;
        if (n - pos < 8 || memcmp(d + pos, "VIDF", 4)) return r;
        memcpy(&sz, d + pos + 4, 4);
        if (sz < 32 || sz > n - pos) return r; /* truncated block */
        r.vidf++; pos += sz;
    }
    r.valid = 1;
    return r;
}
static struct parsed parse_file(const char *name, int committed)
{
    struct mfile *f = find_file(name);
    struct parsed r = {0, 0, 0};
    if (!f) return r;
    return committed ? parse(f->committed, f->csize) : parse(f->data, f->size);
}

static int failures = 0;
#define CHECK(cond, ...) do { int ok_ = (cond); printf("  %s: ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) failures++; } while (0)
static void common_checks(void)
{
    CHECK(bad_handle_use == 0, "no FIO call on a closed handle (%d)", bad_handle_use);
    CHECK(write_from_freed == 0, "no card write from a freed buffer (%d)", write_from_freed);
}

/* ---- tests ---- */
static void t_clip(int frames, int defer)
{
    defer_writes = defer;
    record(frames, 1, 0);
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(p.valid, "closed file is a well-formed block stream");
    CHECK(p.vidf == (uint32_t)frames, "frames in file %u, expected %d", p.vidf, frames);
    CHECK(p.header_count == p.vidf, "header count %u equals frames in file %u", p.header_count, p.vidf);
    CHECK(get_recording_state() == 1, "state INACTIVE after stop");
    CHECK(raw_lv_requests == 0, "raw_lv request/release balanced (%d)", raw_lv_requests);
    CHECK(fio_live == 0, "no fio buffers left allocated (%d)", fio_live);
    common_checks();
}
static void t_many_sessions(void)
{
    int good = 0, n = 25;
    for (int i = 0; i < n; i++)
    {
        char name[24];
        snprintf(name, sizeof(name), "tst%02d.mlv", i);
        record(2, 1, 0);
        struct parsed p = parse_file(name, 1);
        if (p.valid && p.vidf == 2 && p.header_count == 2) good++;
        else if (verbose) fprintf(stderr, "session %d: valid=%d vidf=%u hdr=%u\n", i, p.valid, p.vidf, p.header_count);
    }
    CHECK(good == n, "%d of %d consecutive sessions produced a 2-frame file with header count 2", good, n);
    CHECK(!log_has("no empty event for START"), "event pool never ran dry");
    CHECK(fio_live == 0, "no fio buffers left allocated (%d)", fio_live);
    common_checks();
}
static void t_start_fail(void)
{
    fail_create = 1;
    press_rec(); run_worker();
    CHECK(get_recording_state() == 1, "state INACTIVE after failed start");
    CHECK(raw_lv_requests == 0, "raw mode released after failed start (%d)", raw_lv_requests);
    CHECK(fio_live == 0, "crop buffers freed after failed start (%d)", fio_live);
    CHECK(fake_mlv_session_in_progress == 0, "no MLV session left in progress");
    record(3, 1, 0);
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(p.valid && p.vidf == 3, "a later start works and records (valid=%d frames=%u)", p.valid, p.vidf);
    common_checks();
}
static void t_wedge(int checkpoint)
{
    if (&rvx_checkpoint) rvx_checkpoint = checkpoint;
    record(12, 1, 1); /* 12 frames, then the camera wedges: no stop, no close */
    struct parsed p = parse_file("tst00.mlv", 1);
    if (checkpoint)
    {
        CHECK(p.valid, "file as of last close is a well-formed block stream");
        CHECK(p.vidf == 10, "two full buffers (10 frames) survive, got %u", p.vidf);
        CHECK(p.header_count == p.vidf, "header count %u equals surviving frames %u", p.header_count, p.vidf);
    }
    else
        CHECK(p.vidf == 0, "without checkpoints nothing survives (frames=%u) - documents the old behaviour", p.vidf);
    common_checks();
}
static void t_queue_full(void)
{
    press_rec(); run_worker();
    for (int i = 0; i < 41; i++) vsync(); /* worker never scheduled: queue overflows */
    CHECK(get_recording_state() == 2, "still recording after queue overflow");
    CHECK(!log_has("Sending stop command"), "no automatic stop was requested");
    if (&rvx_dropped_queue) CHECK(rvx_dropped_queue > 0, "producer counted %d dropped frames", rvx_dropped_queue);
    run_worker();
    for (int i = 0; i < 3; i++) { vsync(); run_worker(); }
    press_rec(); run_worker();
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(p.valid && p.vidf > 0, "file is valid with %u frames", p.vidf);
    CHECK(p.header_count == p.vidf, "header count %u equals frames in file %u", p.header_count, p.vidf);
    CHECK(get_recording_state() == 1, "stop still works afterwards");
    common_checks();
}
static void t_slow_card(void)
{
    /* Write task only runs when the worker blocks; 19 frames arrive in one burst, so the
     * second buffer fills while the first is still waiting to be written. */
    defer_writes = 1;
    press_rec(); run_worker();
    vsync();
    for (int i = 0; i < 19; i++) vsync();
    run_worker();
    CHECK(get_recording_state() == 2, "still recording after both buffers were busy");
    for (int i = 0; i < 4; i++) { vsync(); run_worker(); } /* card caught up: must record again */
    press_rec(); run_worker(); vsync();
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(p.valid, "closed file is a well-formed block stream");
    CHECK(p.vidf == 14, "10 buffered + 4 later frames on disk, got %u", p.vidf);
    CHECK(p.header_count == p.vidf, "header count %u equals frames in file %u", p.header_count, p.vidf);
    if (&rvx_dropped_busy) CHECK(rvx_dropped_busy == 9, "worker counted %d dropped frames (expected 9)", rvx_dropped_busy);
    CHECK(fio_live == 0, "no fio buffers left allocated (%d)", fio_live);
    common_checks();
}

/* ---- C12 tests ---- */
#define VIDF_HDR 32
#define ALIGN 0x40
/* Walks the committed file and checks every stored frame against the sensor pattern.
 * Returns the number of frames whose first and last row both match generation `gen`
 * exactly (whole rows compared); *mixed counts frames that match no single generation. */
static int frames_matching(const char *name, int w, int h, int x, int y, uint32_t gen_lo, uint32_t gen_hi, int *mixed)
{
    struct mfile *f = find_file(name);
    int good = 0; *mixed = 0;
    if (!f || !f->committed) return -1;
    size_t line = (size_t)w * 14 / 8, img = line * (size_t)h, pos = 64;
    int col0 = (x + 7) / 8 * 14, row0 = y / 2 * 2;
    while (pos + 8 <= f->csize)
    {
        uint32_t sz; memcpy(&sz, f->committed + pos + 4, 4);
        const uint8_t *p = (const uint8_t *)f->committed + pos + sz - img;
        int ok = 0;
        for (uint32_t g = gen_lo; g <= gen_hi && !ok; g++)
        {
            ok = 1;
            for (size_t c = 0; c < line && ok; c++)
                if (p[c] != sensor_byte(row0, col0 + (int)c, g)
                    || p[(size_t)(h - 1) * line + c] != sensor_byte(row0 + h - 1, col0 + (int)c, g)
                    || p[(size_t)(h / 2) * line + c] != sensor_byte(row0 + h / 2, col0 + (int)c, g)) ok = 0;
        }
        if (ok) good++; else (*mixed)++;
        pos += sz;
    }
    return good;
}
static void dump_files(void)
{
    const char *dir = getenv("HOSTTEST_DUMP");
    if (!dir) return;
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].exists && files[i].committed && strstr(files[i].name, ".RVI"))
        {
            char path[8192]; snprintf(path, sizeof(path), "%s/%s", dir, files[i].name);
            FILE *o = fopen(path, "wb"); if (!o) continue;
            fwrite(files[i].committed, 1, files[i].csize, o); fclose(o);
        }
}
static uint32_t rvi_u32(const char *name, int field)
{
    struct mfile *f = find_file(name); uint32_t v = 0xDEADBEEF;
    if (f && f->committed && f->csize >= (size_t)(field + 1) * 4) memcpy(&v, f->committed + field * 4, 4);
    return v;
}
static void burst_checks(const char *name, int expect_frames)
{
    struct parsed p = parse_file(name, 1);
    CHECK(p.valid, "closed file is a well-formed block stream");
    CHECK(p.vidf == (uint32_t)expect_frames, "frames in file %u, expected %d", p.vidf, expect_frames);
    CHECK(p.header_count == p.vidf, "header count %u equals frames in file %u", p.header_count, p.vidf);
    CHECK(data_writes_while_active == 0, "no card data write while capturing (%d)", data_writes_while_active);
    CHECK(get_recording_state() == 1, "recording stopped by itself (state INACTIVE)");
    CHECK(raw_lv_requests == 0, "raw_lv request/release balanced (%d)", raw_lv_requests);
    CHECK(fio_live == 0, "no fio buffers left allocated (%d)", fio_live);
    CHECK(!log_has("not ACTIVE"), "no error logged for frames queued behind the stop");
    common_checks();
}
static void t_burst(int target, int expect, int drain, int release_first)
{
    rvx_burst_frames = target; rvx_burst_release_first = release_first;
    press_rec(); run_worker(); vsync();
    for (int i = 0; i < expect + 8; i++) { vsync(); if (drain) run_worker(); }
    run_worker(); vsync();
    burst_checks("tst00.mlv", expect);
    if (release_first)
        CHECK(lv_requests_at_first_data_write == 0, "raw LV was released before the first card data write (%d)", lv_requests_at_first_data_write);
    else
        CHECK(lv_requests_at_first_data_write == 1, "raw LV still requested during the card write (%d)", lv_requests_at_first_data_write);
    /* the next recording must work, and be continuous again when the toggle is cleared */
    rvx_burst_frames = 0;
    record(3, 1, 0);
    struct parsed p = parse_file("tst01.mlv", 1);
    CHECK(p.valid && p.vidf == 3 && p.header_count == 3, "a later continuous recording works (valid=%d frames=%u)", p.valid, p.vidf);
}
static void t_burst_720p(void)
{
    /* the plan's first clip: 1280x720 at x=192, 32 MiB of RAM = 20 frames */
    rvx_crop_w = 1280; rvx_crop_h = 720; rvx_crop_x = 192; rvx_crop_y = 100;
    rvx_burst_frames = 1000;
    press_rec(); run_worker(); vsync();
    for (int i = 0; i < 30; i++) { vsync(); run_worker(); }
    burst_checks("tst00.mlv", 20);
    int mixed, good = frames_matching("tst00.mlv", 1280, 720, 192, 100, 0, 0, &mixed);
    CHECK(good == 20 && mixed == 0, "all 20 frames hold the 1280x720 window at (192,100) byte-exact (good=%d bad=%d)", good, mixed);
    CHECK(log_has("RAM full before the frame target"), "burst ended because RAM was full");
}
static void t_prio(void)
{
    rvx_copy_prio = 0x15; rvx_write_prio = 0x16;
    record(7, 1, 0);
    CHECK(copy_tasks == 7 && prio_last_copy == 0x15, "7 frames copied in an rvx_copy task at 0x15 (tasks=%d prio=0x%X)", copy_tasks, prio_last_copy);
    CHECK(prio_last_write == 0x16, "raw_write task at 0x16 (0x%X)", prio_last_write);
    int mixed, good = frames_matching("tst00.mlv", 1792, 896, 200, 100, 0, 0, &mixed);
    CHECK(good == 7 && mixed == 0, "frames byte-exact (good=%d bad=%d)", good, mixed);
    rvx_copy_prio = 0x05; rvx_write_prio = 0x09; /* above Canon's LV tasks: must be refused */
    record(6, 1, 0);
    CHECK(prio_min_bulk >= 0x15, "no bulk task was created above 0x15 (highest seen 0x%X)", prio_min_bulk);
    rvx_copy_prio = 0; rvx_write_prio = 0x1A; copy_tasks = 0;
    record(2, 1, 0);
    CHECK(copy_tasks == 0 && prio_last_write == 0x1A, "defaults: copy in the worker, raw_write at 0x1A");
    CHECK(rvx_copy_task_fail == 0, "no copy task creation failure counted");
    common_checks();
}
static void t_throttle(void)
{
    rvx_copy_chunk_kb = 256; in_copy = 1; /* copy runs inline in the worker */
    record(3, 1, 0);
    in_copy = 0;
    /* 1792x896 frame = 2809856 bytes, 83 rows per 256 KiB chunk, 896 rows -> 10 pauses */
    CHECK(copy_sleeps == 30, "10 pauses per frame, 30 for 3 frames (%d)", copy_sleeps);
    int mixed, good = frames_matching("tst00.mlv", 1792, 896, 200, 100, 0, 0, &mixed);
    CHECK(good == 3 && mixed == 0, "frames byte-exact (good=%d bad=%d)", good, mixed);
    CHECK(rvx_torn == 0 && rvx_dst_diff == 0, "static source: no frame flagged torn (%d) or different (%d)", rvx_torn, rvx_dst_diff);
    common_checks();
}
static void t_torn(int drop)
{
    /* The sensor rewrites the buffer at every throttle pause, i.e. several times per copy. */
    rvx_copy_chunk_kb = 256; rvx_sidecar = 1; rvx_drop_torn = drop; sensor_live = 1; in_copy = 1;
    record(4, 1, 0);
    in_copy = 0;
    struct parsed p = parse_file("tst00.mlv", 1);
    int mixed, good = frames_matching("tst00.mlv", 1792, 896, 200, 100, 0, sensor_gen, &mixed);
    CHECK(rvx_torn == 4, "all 4 frames flagged torn (%d)", rvx_torn);
    if (drop)
        CHECK(p.valid && p.vidf == 0 && p.header_count == 0, "torn frames were not stored (frames=%u header=%u)", p.vidf, p.header_count);
    else
    {
        CHECK(p.valid && p.vidf == 4 && p.header_count == 4, "torn frames are stored by default (frames=%u)", p.vidf);
        CHECK(mixed == 4 && good == 0, "independent check: all 4 stored frames mix sensor generations (mixed=%d)", mixed);
    }
    CHECK(rvi_u32("tst00.RVI", 0) == 0x49585652, "sidecar tst00.RVI exists with magic RVXI");
    CHECK(rvi_u32("tst00.RVI", 4) == 4, "sidecar holds 4 records (%u)", rvi_u32("tst00.RVI", 4));
    CHECK(fio_live == 0, "no fio buffers left allocated (%d)", fio_live);
    dump_files();
    common_checks();
}
static void t_integrity_clean(void)
{
    rvx_sidecar = 1;
    press_rec(); run_worker(); vsync();
    for (int i = 0; i < 30; i++) vsync();   /* worker not scheduled: 20 queued, 10 dropped */
    run_worker();
    for (int i = 0; i < 3; i++) { vsync(); run_worker(); }
    press_rec(); run_worker(); vsync();
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(p.valid && p.header_count == p.vidf, "file valid, header count %u = frames %u", p.header_count, p.vidf);
    CHECK(rvx_torn == 0 && rvx_dst_diff == 0, "static source: nothing flagged (torn=%d diff=%d)", rvx_torn, rvx_dst_diff);
    CHECK(rvi_u32("tst00.RVI", 4) == p.vidf, "one sidecar record per stored frame (%u)", rvi_u32("tst00.RVI", 4));
    CHECK(rvi_u32("tst00.RVI", 10) == 34, "sidecar counts 34 vsyncs (1 setup + 30 + 3) in the session (%u)", rvi_u32("tst00.RVI", 10));
    CHECK(rvi_u32("tst00.RVI", 13) == (uint32_t)rvx_dropped_queue && rvx_dropped_queue > 0, "sidecar carries the queue-drop count (%u)", rvi_u32("tst00.RVI", 13));
    CHECK(data_writes >= 1, "sidecar and clip were written");
    dump_files();
    common_checks();
}
static void t_fast_copy(int x)
{
    rvx_fast_copy = 1; rvx_crop_x = x;
    record(3, 1, 0);
    int mixed, good = frames_matching("tst00.mlv", 1792, 896, x, 100, 0, 0, &mixed);
    CHECK(good == 3 && mixed == 0, "frames byte-exact (good=%d bad=%d)", good, mixed);
    CHECK(inval_calls == 3 * 14, "invalidate called for every 64-row group: 14 per frame (%d)", inval_calls);
    CHECK(inval_bad == 0, "every invalidate range is line-aligned and inside the raw buffer (%d bad)", inval_bad);
    if (x % 16 == 0) CHECK(copy64_calls == 3 * 896, "aligned window: every row went through memcpy64 (%d)", copy64_calls);
    else CHECK(copy64_calls == 0, "window 2 bytes off alignment: memcpy64 not used (%d)", copy64_calls);
    rvx_fast_copy = 0; inval_calls = 0;
    record(2, 1, 0);
    CHECK(inval_calls == 0, "toggle off: the Canon cache routine is never called");
    common_checks();
}
static void t_crop_reject(void)
{
    rvx_crop_w = 1790; /* not a multiple of 8 */
    press_rec(); run_worker();
    CHECK(get_recording_state() == 1 && log_has("not usable"), "odd width is refused at REC");
    rvx_crop_w = 1792; rvx_crop_x = 400; /* 400 + 1792 > 2096 */
    press_rec(); run_worker(); vsync(); run_worker(); vsync(); run_worker(); vsync(); run_worker();
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(get_recording_state() == 1, "window outside the raw frame: recording stopped by itself");
    CHECK(p.valid && p.vidf == 0, "no frame was copied from outside the buffer (frames=%u)", p.vidf);
    CHECK(raw_lv_requests == 0 && fio_live == 0, "raw mode and buffers released (%d, %d)", raw_lv_requests, fio_live);
    rvx_crop_x = 200;
    record(2, 1, 0);
    p = parse_file("tst01.mlv", 1);
    CHECK(p.valid && p.vidf == 2, "a later valid recording works (frames=%u)", p.vidf);
    common_checks();
}
static void t_defaults(void)
{
    record(12, 1, 0);
    CHECK(fio_live_max == 3, "defaults: 2 crop buffers + 1 record block allocated, no capture frames (%d)", fio_live_max);
    CHECK(copy_tasks == 0 && inval_calls == 0 && copy_sleeps == 0, "defaults: copy inline, no pause, no cache call");
    CHECK(prio_last_write == 0x1A, "defaults: raw_write at 0x1A (0x%X)", prio_last_write);
    CHECK(find_file("tst00.RVI") == NULL, "defaults: no sidecar file");
    CHECK(data_writes_while_active > 0, "defaults: continuous mode writes while capturing (%d)", data_writes_while_active);
    int mixed, good = frames_matching("tst00.mlv", 1792, 896, 200, 100, 0, 0, &mixed);
    CHECK(good == 12 && mixed == 0, "frames byte-exact (good=%d bad=%d)", good, mixed);
    rvx_src_from_raw_info = 0; fio_live_max = 0;
    record(2, 1, 0);
    CHECK(fio_live_max == 5, "double-buffer source selected: capture frames are allocated again (%d)", fio_live_max);
    CHECK(fio_live == 0, "and freed (%d)", fio_live);
    common_checks();
}

int main(int argc, char **argv)
{
    const char *t = argc > 1 ? argv[1] : "";
    verbose = getenv("HOSTTEST_VERBOSE") != NULL;
    setup();
    printf("%s\n", t);
    if (!strcmp(t, "short_clip")) t_clip(3, 0);
    else if (!strcmp(t, "long_clip")) t_clip(12, 0);
    else if (!strcmp(t, "long_clip_lowprio_writer")) t_clip(12, 1);
    else if (!strcmp(t, "stop_right_after_swap")) t_clip(6, 1);
    else if (!strcmp(t, "many_sessions")) t_many_sessions();
    else if (!strcmp(t, "start_failure")) t_start_fail();
    else if (!strcmp(t, "wedge_with_checkpoint")) t_wedge(1);
    else if (!strcmp(t, "wedge_without_checkpoint")) t_wedge(0);
    else if (!strcmp(t, "queue_full")) t_queue_full();
    else if (!strcmp(t, "slow_card")) t_slow_card();
    else if (!strcmp(t, "c12_defaults")) t_defaults();
    else if (!strcmp(t, "c12_burst_target")) t_burst(7, 7, 1, 0);
    else if (!strcmp(t, "c12_burst_ram_full")) t_burst(100, 10, 1, 0);
    else if (!strcmp(t, "c12_burst_backlog")) t_burst(3, 3, 0, 0);
    else if (!strcmp(t, "c12_burst_release_first")) t_burst(7, 7, 1, 1);
    else if (!strcmp(t, "c12_burst_720p")) t_burst_720p();
    else if (!strcmp(t, "c12_prio")) t_prio();
    else if (!strcmp(t, "c12_throttle")) t_throttle();
    else if (!strcmp(t, "c12_torn_flagged")) t_torn(0);
    else if (!strcmp(t, "c12_torn_dropped")) t_torn(1);
    else if (!strcmp(t, "c12_integrity_clean")) t_integrity_clean();
    else if (!strcmp(t, "c12_fast_copy_aligned")) t_fast_copy(192);
    else if (!strcmp(t, "c12_fast_copy_unaligned")) t_fast_copy(200);
    else if (!strcmp(t, "c12_crop_reject")) t_crop_reject();
    else { fprintf(stderr, "unknown test '%s'\n", t); return 98; }
    return failures ? 1 : 0;
}
