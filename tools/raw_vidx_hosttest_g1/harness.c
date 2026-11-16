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

/* G1: rvx_sync.c */
struct rvx_sync_cfg
{
    uint32_t evf_obj_pp, evf_type, evf_name, evf_dispatch, evf_matrix, evf_inputs, evf_states;
    uint32_t obj_lo, obj_hi, mem1_slot, mem1_stock, pool_head, pool_base, pool_end;
};
extern struct rvx_sync_cfg rvx_sync_cfg;
extern int rvx_sync_src, rvx_pool_watch, rvx_pool_stop_below, rvx_pool_period, rvx_pool_walk_max;
extern volatile int rvx_sync_active;
extern volatile uint32_t rvx_sync_ticks;
extern int rvx_sync_core_ignored, rvx_sync_missed, rvx_sync_refused, rvx_sync_restore_fail;
extern int rvx_sync_silent, rvx_sync_last_err;
extern int rvx_pool_armed, rvx_pool_free_start, rvx_pool_free_last, rvx_pool_free_min;
extern int rvx_pool_checks, rvx_pool_tripped;
uint32_t rvx_pool_free(uint32_t limit);

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
/* G1: 1 while a fake Canon dispatch / callback is on the stack.  The module may only count
 * and give a semaphore there; anything else it calls is a violation. */
static int in_canon = 0, canon_violations = 0, canon_gives = 0;
static int sync_tasks_created = 0, in_pump = 0, cli_depth = 0, cli_calls = 0, writes_outside_cli = 0;
#define CANON_FORBIDDEN() do { if (in_canon) canon_violations++; } while (0)
uint32_t cli(void) { cli_depth++; cli_calls++; return 0; }
void sei(uint32_t old) { (void)old; cli_depth--; }
static char logbuf[1 << 20];
static size_t loglen = 0;

void m50ii_uart_str(const char *s)
{
    CANON_FORBIDDEN();
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
    CANON_FORBIDDEN();
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
    CANON_FORBIDDEN();
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
static ucontext_t main_ctx, worker_ctx, pump_ctx, pump_caller_ctx;
static void (*pump_entry)(void) = NULL;
static int pump_started = 0;
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
    CANON_FORBIDDEN();
    if (!take_semaphore_now(s)) return 0;
    if (in_pump && !in_worker)
    {
        /* the "rvx_sync" task blocks: back to the test.  Resumed with the semaphore given =
         * a frame signal; resumed without = the wait timed out. */
        swapcontext(&pump_ctx, &pump_caller_ctx);
        return take_semaphore_now(s) ? 1 : 0;
    }
    /* we would block: lower-priority tasks get to run */
    while (run_pending())
        if (!take_semaphore_now(s)) return 0;
    return 1; /* timeout */
}
int give_semaphore(struct semaphore *s) { if (in_canon) canon_gives++; s->count = 1; return 0; }
/* G1: let the "rvx_sync" task run until it blocks on its semaphore again */
static void run_pump(void)
{
    if (!pump_entry) return;
    if (!pump_started)
    {
        getcontext(&pump_ctx);
        pump_ctx.uc_stack.ss_size = 1 << 20;
        pump_ctx.uc_stack.ss_sp = malloc(pump_ctx.uc_stack.ss_size);
        pump_ctx.uc_link = &pump_caller_ctx;
        makecontext(&pump_ctx, pump_entry, 0);
        pump_started = 1;
    }
    in_pump = 1;
    swapcontext(&pump_caller_ctx, &pump_ctx);
    in_pump = 0;
}

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
    CANON_FORBIDDEN();
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
    CANON_FORBIDDEN();
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
    CANON_FORBIDDEN();
    if (!strcmp(name, "raw_v_worker")) { worker_entry = (void (*)(void))entry; return (void *)1; }
    if (!strcmp(name, "rvx_sync")) { pump_entry = (void (*)(void))entry; sync_tasks_created++; return (void *)2; }
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

/* ---- G1: fake Canon objects ---- */
static uint32_t fake_obj[8];          /* state object: type, name, auto, dispatch, matrix, inputs, states, state */
static uint32_t fake_obj_p;           /* stands for the word at 0x8E8C */
static int dispatch_calls = 0, dispatch_input5 = 0;
static int fake_dispatch(void *self, void *x, uint32_t input, void *z, void *t)
{
    (void)x; (void)z; (void)t;
    if (self != fake_obj) { fprintf(stderr, "harness: dispatcher called with a wrong object\n"); exit(96); }
    dispatch_calls++;
    if (input == 5) dispatch_input5++;
    return 0x600D;
}
static int foreign_calls = 0;
static int foreign_dispatch(void *self, void *x, uint32_t input, void *z, void *t)
{
    foreign_calls++;
    return fake_dispatch(self, x, input, z, t);
}
static int last_dispatch_ret = 0;
static void canon_evf_event(uint32_t input) /* Canon's EvfCap task dispatches one input */
{
    in_canon++;
    last_dispatch_ret = ((int (*)(void *, void *, uint32_t, void *, void *))(uintptr_t)fake_obj[3])(fake_obj, NULL, input, NULL, NULL);
    in_canon--;
}
static uint32_t fake_slot[2];         /* stands for 0x1029C (callback) and 0x102A0 (argument) */
static int mem1_calls = 0, mem1_bad_arg = 0;
static void fake_mem1_cbr(void *arg) { mem1_calls++; if ((uintptr_t)arg != 0xA11C0DE) mem1_bad_arg++; }
static void other_mem1_cbr(void *arg) { (void)arg; }
static void canon_mem1_complete(void) /* Canon's 0xE0753D9C */
{
    if (!fake_slot[0]) return;
    in_canon++;
    ((void (*)(void *))(uintptr_t)fake_slot[0])((void *)(uintptr_t)fake_slot[1]);
    in_canon--;
}
#define POOL_N 1536
static uint32_t fake_pool[POOL_N * 4];
static uint32_t fake_pool_head;
static void pool_set_free(int n) /* the first n nodes are free, linked as Canon's init loop does */
{
    for (int i = 0; i < POOL_N; i++) fake_pool[i * 4] = 0xE0000000u + (uint32_t)i; /* in use: event data */
    for (int i = 0; i < n; i++) fake_pool[i * 4] = (i + 1 < n) ? (uint32_t)(uintptr_t)&fake_pool[(i + 1) * 4] : 0;
    fake_pool_head = n ? (uint32_t)(uintptr_t)&fake_pool[0] : 0;
}
static void fake_canon_reset(void)
{
    fake_obj[0] = 0x57A7E0B1; fake_obj[1] = 0x4E414D45; fake_obj[2] = 0;
    fake_obj[3] = (uint32_t)(uintptr_t)fake_dispatch; fake_obj[4] = 0x4D415452;
    fake_obj[5] = 0x11; fake_obj[6] = 8; fake_obj[7] = 5;
    fake_obj_p = (uint32_t)(uintptr_t)fake_obj;
    fake_slot[0] = (uint32_t)(uintptr_t)fake_mem1_cbr; fake_slot[1] = 0xA11C0DE;
}
static void fake_canon_setup(void)
{
    fake_canon_reset();
    pool_set_free(POOL_N);
    rvx_sync_cfg.evf_obj_pp = (uint32_t)(uintptr_t)&fake_obj_p;
    rvx_sync_cfg.evf_type = fake_obj[0]; rvx_sync_cfg.evf_name = fake_obj[1];
    rvx_sync_cfg.evf_dispatch = fake_obj[3]; rvx_sync_cfg.evf_matrix = fake_obj[4];
    rvx_sync_cfg.obj_lo = 0x1000; rvx_sync_cfg.obj_hi = 0xFFFFFFFF;
    rvx_sync_cfg.mem1_slot = (uint32_t)(uintptr_t)&fake_slot[0];
    rvx_sync_cfg.mem1_stock = fake_slot[0];
    rvx_sync_cfg.pool_head = (uint32_t)(uintptr_t)&fake_pool_head;
    rvx_sync_cfg.pool_base = (uint32_t)(uintptr_t)&fake_pool[0];
    rvx_sync_cfg.pool_end = (uint32_t)(uintptr_t)&fake_pool[POOL_N * 4];
}

static void setup(void)
{
    fake_canon_setup(); /* every test: the module must never read the real camera addresses here */
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

/* ---- G1 tests ---- */
static int evf_hooked(void) { return fake_obj[3] != (uint32_t)(uintptr_t)fake_dispatch; }
static int mem1_hooked(void) { return fake_slot[0] != (uint32_t)(uintptr_t)fake_mem1_cbr; }
/* one frame period on a module-installed source: Canon's signal, then the tasks by priority */
static void frame_src(int src)
{
    if (src == 1) { canon_evf_event(3); canon_evf_event(4); canon_evf_event(5); }
    else canon_mem1_complete();
    vsync();      /* the core's vsync callback keeps firing; it must be ignored */
    run_pump();
    run_worker();
}
static void canon_checks(void)
{
    CHECK(canon_violations == 0, "nothing but count + give_semaphore ran in Canon's context (%d violations)", canon_violations);
    CHECK(cli_depth == 0, "interrupt mask balanced (%d)", cli_depth);
}
static void t_g1_defaults(void)
{
    press_rec(); run_worker();
    CHECK(rvx_sync_active == 0 && !evf_hooked() && !mem1_hooked(), "default source 0: no Canon pointer was written");
    CHECK(sync_tasks_created == 0 && cli_calls == 0, "default: no rvx_sync task, no interrupt masking");
    CHECK(rvx_pool_armed == 1 && rvx_pool_free_start == 1536, "default: pool watchdog armed on a full pool (start level %d)", rvx_pool_free_start);
    vsync();
    for (int i = 0; i < 20; i++) { vsync(); run_worker(); }
    press_rec(); run_worker(); vsync();
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(p.valid && p.vidf == 20 && p.header_count == 20, "20 frames recorded on the core vsync hook (%u)", p.vidf);
    CHECK(rvx_pool_checks == 2 && rvx_pool_tripped == 0 && rvx_pool_free_min == 1536, "pool checked every 8 frames (%d checks), never tripped", rvx_pool_checks);
    CHECK(rvx_sync_core_ignored == 0 && rvx_sync_ticks == 0, "no core vsync ignored, no module signal counted");
    common_checks(); canon_checks();
}
static void t_g1_install(int src)
{
    int n = 9;
    rvx_sync_src = src;
    press_rec(); run_worker();
    CHECK(rvx_sync_active == src, "source %d active after start (%d)", src, rvx_sync_active);
    CHECK(src == 1 ? (evf_hooked() && !mem1_hooked()) : (mem1_hooked() && !evf_hooked()), "only the selected Canon pointer was replaced");
    CHECK(fake_slot[1] == 0xA11C0DE, "callback argument word untouched");
    CHECK(sync_tasks_created == 1, "rvx_sync task created once (%d)", sync_tasks_created);
    if (src == 1)
    {   /* inputs other than 5, and input 5 outside states 3..6, are not frames */
        canon_evf_event(4); canon_evf_event(6); fake_obj[7] = 2; canon_evf_event(5); fake_obj[7] = 5;
        run_pump(); run_worker();
        CHECK(rvx_sync_ticks == 0, "inputs 4, 6 and input 5 in state 2 are not counted as frames (%u)", rvx_sync_ticks);
        CHECK(last_dispatch_ret == 0x600D, "Canon's dispatcher return value is passed through");
    }
    for (int i = 0; i < n + 1; i++) frame_src(src); /* first signal of a session is setup only */
    CHECK(rvx_sync_ticks == (uint32_t)(n + 1), "one signal per frame counted in Canon's context (%u)", rvx_sync_ticks);
    CHECK(canon_gives == n + 1, "one give_semaphore per frame in Canon's context (%d)", canon_gives);
    CHECK(rvx_sync_core_ignored == n + 1, "core vsync callbacks were ignored while the source was active (%d)", rvx_sync_core_ignored);
    if (src == 1) CHECK(dispatch_calls == 3 + 3 * (n + 1), "every dispatch reached Canon's dispatcher (%d)", dispatch_calls);
    else CHECK(mem1_calls == n + 1 && mem1_bad_arg == 0, "Canon's callback ran for every completion with its argument (%d)", mem1_calls);
    press_rec(); run_worker();
    CHECK(!evf_hooked() && !mem1_hooked(), "stock pointer restored on stop");
    CHECK(rvx_sync_active == 0, "source back to 0 after stop");
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(p.valid && p.vidf == (uint32_t)n && p.header_count == (uint32_t)n, "%d frames in a well-formed file (%u)", n, p.vidf);
    int mixed, good = frames_matching("tst00.mlv", 1792, 896, 200, 100, 0, 0, &mixed);
    CHECK(good == n && mixed == 0, "frames byte-exact (good=%d bad=%d)", good, mixed);
    CHECK(get_recording_state() == 1 && raw_lv_requests == 0 && fio_live == 0, "state INACTIVE, raw LV and buffers released");
    /* after the stop Canon's events must not reach the module, and source 0 must work again */
    uint32_t ticks = rvx_sync_ticks; int gives = canon_gives;
    canon_evf_event(5); canon_mem1_complete(); run_pump();
    CHECK(rvx_sync_ticks == ticks && canon_gives == gives, "no signal after the stop");
    rvx_sync_src = 0;
    record(3, 1, 0);
    p = parse_file("tst01.mlv", 1);
    CHECK(p.valid && p.vidf == 3, "a later recording on source 0 works (%u frames)", p.vidf);
    /* and the source can be installed a second time */
    rvx_sync_src = src;
    press_rec(); run_worker();
    for (int i = 0; i < 4; i++) frame_src(src);
    press_rec(); run_worker();
    p = parse_file("tst02.mlv", 1);
    CHECK(p.valid && p.vidf == 3 && !evf_hooked() && !mem1_hooked(), "second install / restore cycle works (%u frames)", p.vidf);
    CHECK(sync_tasks_created == 1, "still one rvx_sync task (%d)", sync_tasks_created);
    common_checks(); canon_checks();
}
/* a start with a broken identity: nothing written, refusal logged, recording works on source 0 */
static void refuse_case(const char *what, int src, int *sessions)
{
    char name[24]; snprintf(name, sizeof(name), "tst%02d.mlv", *sessions); (*sessions)++;
    uint32_t obj3 = fake_obj[3], slot0 = fake_slot[0];
    int refused = rvx_sync_refused; size_t mark = loglen;
    rvx_sync_src = src;
    press_rec(); run_worker();
    int untouched = fake_obj[3] == obj3 && fake_slot[0] == slot0;
    int said = strstr(logbuf + mark, "REFUSED") != NULL;
    vsync(); vsync(); run_worker(); vsync(); run_worker();
    press_rec(); run_worker(); vsync();
    struct parsed p = parse_file(name, 1);
    CHECK(untouched && said && rvx_sync_refused == refused + 1 && rvx_sync_active == 0 && p.valid && p.vidf == 2,
          "%s: refused on UART, nothing written, 2 frames on the core hook (untouched=%d said=%d err=%d frames=%u)",
          what, untouched, said, rvx_sync_last_err, p.vidf);
    fake_canon_reset();
}
static void t_g1_evf_refuse(void)
{
    int s = 0;
    fake_obj_p = 0;                refuse_case("NULL object pointer", 1, &s);
    fake_obj_p |= 2;               refuse_case("unaligned object pointer", 1, &s);
    fake_obj[0] ^= 1;              refuse_case("wrong type word", 1, &s);
    fake_obj[1] ^= 0x100;          refuse_case("wrong name pointer", 1, &s);
    fake_obj[3] = (uint32_t)(uintptr_t)foreign_dispatch; refuse_case("foreign dispatcher", 1, &s);
    fake_obj[4] += 8;              refuse_case("wrong matrix", 1, &s);
    fake_obj[5] = 0x10;            refuse_case("wrong input count", 1, &s);
    fake_obj[6] = 9;               refuse_case("wrong state count", 1, &s);
    fake_obj[7] = 8;               refuse_case("state out of range", 1, &s);
    CHECK(foreign_calls == 0 && dispatch_calls == 0, "no dispatcher was ever called by the module itself");
    rvx_sync_src = 1;
    press_rec(); run_worker();
    CHECK(evf_hooked() && rvx_sync_active == 1, "a healthy object is accepted afterwards");
    press_rec(); run_worker();
    CHECK(!evf_hooked(), "and restored");
    common_checks(); canon_checks();
}
static void t_g1_mem1_refuse(void)
{
    int s = 0;
    fake_slot[0] = 0; refuse_case("empty slot", 2, &s);
    fake_slot[0] = (uint32_t)(uintptr_t)other_mem1_cbr; refuse_case("slot holds another callback", 2, &s);
    CHECK(rvx_sync_last_err == 11, "last error names the slot value (%d)", rvx_sync_last_err);
    common_checks(); canon_checks();
}
static void t_g1_bad_src(void)
{
    int s = 0;
    refuse_case("rvx_sync_src = 3", 3, &s);
    refuse_case("rvx_sync_src = -1", -1, &s);
    CHECK(sync_tasks_created == 0, "no task created for an invalid source");
    common_checks(); canon_checks();
}
static void t_g1_foreign_chain(void)
{
    rvx_sync_src = 1;
    press_rec(); run_worker();
    for (int i = 0; i < 3; i++) frame_src(1);
    uint32_t ours = fake_obj[3];
    fake_obj[3] = (uint32_t)(uintptr_t)foreign_dispatch; /* e.g. the serial `statespy` hooked on top */
    press_rec(); run_worker();
    CHECK(fake_obj[3] == (uint32_t)(uintptr_t)foreign_dispatch, "a pointer that is not ours is not overwritten at stop");
    CHECK(rvx_sync_restore_fail == 1 && log_has("NOT restored"), "the failed restore is counted and reported");
    CHECK(get_recording_state() == 1 && rvx_sync_active == 0, "recording stopped normally");
    /* the foreign hook goes away and puts OUR function back: it must be a pure pass-through */
    fake_obj[3] = ours;
    int gives = canon_gives, calls = dispatch_calls;
    canon_evf_event(5);
    CHECK(dispatch_calls == calls + 1 && canon_gives == gives, "left-over hook passes through to Canon and signals nothing");
    rvx_sync_src = 1;
    press_rec(); run_worker();
    CHECK(rvx_sync_active == 1 && fake_obj[3] == ours, "next recording re-uses the left-over hook");
    for (int i = 0; i < 3; i++) frame_src(1);
    press_rec(); run_worker();
    CHECK(!evf_hooked(), "and this time the stock dispatcher is restored");
    struct parsed p = parse_file("tst01.mlv", 1);
    CHECK(p.valid && p.vidf == 2, "that recording holds 2 frames (%u)", p.vidf);
    common_checks(); canon_checks();
}
static void t_g1_start_failure(void)
{
    rvx_sync_src = 1; fail_create = 1;
    press_rec(); run_worker();
    CHECK(get_recording_state() == 1 && !evf_hooked() && rvx_sync_active == 0, "failed start: no hook installed");
    rvx_sync_src = 2; fail_create = 1;
    press_rec(); run_worker();
    CHECK(get_recording_state() == 1 && !mem1_hooked() && rvx_sync_active == 0, "failed start: no chain installed");
    CHECK(raw_lv_requests == 0 && fio_live == 0, "raw mode and buffers released");
    common_checks(); canon_checks();
}
/* Canon takes the slot back during the recording: clears it (0xE07541AC) or rewrites it (0xE07541A4) */
static void t_g1_mem1_lost(int rewrite)
{
    rvx_sync_src = 2;
    press_rec(); run_worker();
    for (int i = 0; i < 4; i++) frame_src(2);
    fake_slot[0] = rewrite ? (uint32_t)(uintptr_t)fake_mem1_cbr : 0;
    for (int i = 0; i < 3; i++) { canon_mem1_complete(); run_pump(); run_worker(); } /* 3 waits time out */
    CHECK(get_recording_state() == 2, "still recording after 1.5 s without a signal");
    canon_mem1_complete(); run_pump(); run_worker(); /* 4th timeout */
    CHECK(get_recording_state() == 1, "recording ended by itself after 2 s without a signal");
    CHECK(rvx_sync_silent == 1 && log_has("no frame signal"), "silence counted and reported");
    CHECK(fake_slot[0] == (rewrite ? (uint32_t)(uintptr_t)fake_mem1_cbr : 0), "the slot is left as Canon set it (no stale callback written back)");
    CHECK(log_has("rewritten by Canon"), "the lost chain is reported");
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(p.valid && p.vidf == 3 && p.header_count == 3, "the 3 frames captured before are in a closed file (%u)", p.vidf);
    CHECK(raw_lv_requests == 0 && fio_live == 0 && rvx_sync_active == 0, "raw LV, buffers and source released");
    for (int i = 0; i < 6; i++) run_pump();
    CHECK(rvx_sync_silent == 1, "idle rvx_sync task raises no further alarm");
    common_checks(); canon_checks();
}
static void t_g1_pool_stop(int src)
{
    rvx_sync_src = src;
    press_rec(); run_worker();
    if (src) frame_src(src); else vsync();
    for (int i = 0; i < 10; i++) { if (src) frame_src(src); else { vsync(); run_worker(); } }
    CHECK(get_recording_state() == 2 && rvx_pool_checks == 1 && rvx_pool_free_last == 1536, "healthy pool: recording continues (checks=%d)", rvx_pool_checks);
    pool_set_free(300);
    for (int i = 0; i < 8; i++) { if (src) frame_src(src); else { vsync(); run_worker(); } }
    CHECK(get_recording_state() == 2 && rvx_pool_free_min == 300, "300 free is above the threshold of 256: still recording (min %d)", rvx_pool_free_min);
    pool_set_free(100);
    for (int i = 0; i < 12; i++) { if (src) frame_src(src); else { vsync(); run_worker(); } }
    CHECK(get_recording_state() == 1, "100 free: the recording ended by itself");
    CHECK(rvx_pool_tripped == 101 && rvx_pool_free_min == 100, "level at the stop is in telemetry (%d)", rvx_pool_tripped - 1);
    CHECK(log_has("below the stop threshold"), "the stop is reported");
    struct parsed p = parse_file("tst00.mlv", 1);
    /* 10 + 8 frames, then frames up to the check at frame 24 (the 24th is not copied) */
    CHECK(p.valid && p.vidf == 23 && p.header_count == 23, "closed, well-formed file with the 23 frames copied before the stop (%u)", p.vidf);
    CHECK(raw_lv_requests == 0 && fio_live == 0, "raw LV and buffers released");
    CHECK(!evf_hooked() && !mem1_hooked() && rvx_sync_active == 0, "Canon pointers stock after the watchdog stop");
    CHECK(!log_has("not ACTIVE"), "frames queued behind the stop were discarded quietly");
    pool_set_free(1536);
    rvx_sync_src = 0;
    record(3, 1, 0);
    p = parse_file("tst01.mlv", 1);
    CHECK(p.valid && p.vidf == 3 && rvx_pool_tripped == 0, "a later recording works and starts with clean telemetry (%u frames)", p.vidf);
    common_checks(); canon_checks();
}
static void t_g1_pool_unarmed(void)
{
    fake_pool_head = 0x1234; /* as if the head address were wrong on the camera */
    record(20, 1, 0);
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(rvx_pool_armed == 0 && log_has("NOT armed"), "implausible baseline: watchdog not armed, and says so");
    CHECK(p.valid && p.vidf == 20 && rvx_pool_tripped == 0, "the recording is not stopped by a watchdog that cannot see the pool (%u frames)", p.vidf);
    pool_set_free(400); /* real but low baseline: below 2 x threshold */
    record(20, 1, 0);
    p = parse_file("tst01.mlv", 1);
    CHECK(rvx_pool_armed == 0 && rvx_pool_free_start == 400 && p.vidf == 20, "baseline 400 < 512: measured, not armed (%u frames)", p.vidf);
    common_checks(); canon_checks();
}
static void t_g1_pool_measure_only(void)
{
    rvx_pool_watch = 2; rvx_pool_period = 1;
    press_rec(); run_worker(); vsync();
    for (int i = 0; i < 5; i++) { vsync(); run_worker(); }
    pool_set_free(10);
    for (int i = 0; i < 5; i++) { vsync(); run_worker(); }
    CHECK(get_recording_state() == 2 && rvx_pool_free_min == 10 && rvx_pool_checks == 10, "measure-only: level 10 recorded, recording continues (checks=%d)", rvx_pool_checks);
    press_rec(); run_worker(); vsync();
    CHECK(log_has("pool: free minimum=0x0000000A"), "minimum level printed in the stop summary");
    common_checks(); canon_checks();
}
static void t_g1_pool_off(void)
{
    rvx_pool_watch = 0; pool_set_free(0);
    fake_pool_head = 1; /* would fault if dereferenced */
    record(20, 1, 0);
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(rvx_pool_checks == 0 && rvx_pool_free_start == 0 && p.vidf == 20, "watch off: pool never read, 20 frames (%u)", p.vidf);
    CHECK(!log_has("pool:"), "watch off: no pool line on the UART");
    common_checks();
}
static void t_g1_pool_walk(void)
{
    pool_set_free(1536);
    CHECK(rvx_pool_free(1536) == 1536 && rvx_pool_free(5000) == 1536, "full pool counts 1536, also with a larger limit");
    CHECK(rvx_pool_free(256) == 256 && rvx_pool_free(0) == 0, "the limit bounds the walk");
    pool_set_free(7);
    CHECK(rvx_pool_free(1536) == 7, "7 free nodes counted");
    pool_set_free(0);
    CHECK(rvx_pool_free(1536) == 0, "empty list (head 0) counts 0");
    pool_set_free(50);
    fake_pool[49 * 4] = (uint32_t)(uintptr_t)&fake_pool[10 * 4]; /* corrupt: a cycle */
    CHECK(rvx_pool_free(1536) == 1536, "a cycle ends at the limit instead of looping for ever");
    pool_set_free(50);
    fake_pool[20 * 4] = (uint32_t)(uintptr_t)&fake_pool[21 * 4] + 4; /* misaligned link */
    CHECK(rvx_pool_free(1536) == 21, "a misaligned link is not followed (%u)", rvx_pool_free(1536));
    pool_set_free(50);
    fake_pool[20 * 4] = (uint32_t)(uintptr_t)&fake_pool[POOL_N * 4]; /* one past the end */
    CHECK(rvx_pool_free(1536) == 21, "a link outside the pool is not followed (%u)", rvx_pool_free(1536));
    fake_pool[20 * 4] = (uint32_t)(uintptr_t)fake_obj; /* popped node: word 0 is event data */
    CHECK(rvx_pool_free(1536) == 21, "event data in a node ends the walk (%u)", rvx_pool_free(1536));
    uint32_t sum = 0; for (int i = 0; i < POOL_N * 4; i++) sum += fake_pool[i];
    rvx_pool_free(1536);
    uint32_t sum2 = 0; for (int i = 0; i < POOL_N * 4; i++) sum2 += fake_pool[i];
    CHECK(sum == sum2 && cli_calls == 0, "the walk writes nothing and masks no interrupt");
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
    else if (!strcmp(t, "g1_defaults")) t_g1_defaults();
    else if (!strcmp(t, "g1_evf_install")) t_g1_install(1);
    else if (!strcmp(t, "g1_mem1_install")) t_g1_install(2);
    else if (!strcmp(t, "g1_evf_refuse")) t_g1_evf_refuse();
    else if (!strcmp(t, "g1_mem1_refuse")) t_g1_mem1_refuse();
    else if (!strcmp(t, "g1_bad_src")) t_g1_bad_src();
    else if (!strcmp(t, "g1_evf_foreign_chain")) t_g1_foreign_chain();
    else if (!strcmp(t, "g1_evf_start_failure")) t_g1_start_failure();
    else if (!strcmp(t, "g1_mem1_canon_clears")) t_g1_mem1_lost(0);
    else if (!strcmp(t, "g1_mem1_canon_rewrites")) t_g1_mem1_lost(1);
    else if (!strcmp(t, "g1_pool_stop")) t_g1_pool_stop(0);
    else if (!strcmp(t, "g1_pool_stop_hooked")) t_g1_pool_stop(2);
    else if (!strcmp(t, "g1_pool_unarmed")) t_g1_pool_unarmed();
    else if (!strcmp(t, "g1_pool_measure_only")) t_g1_pool_measure_only();
    else if (!strcmp(t, "g1_pool_off")) t_g1_pool_off();
    else if (!strcmp(t, "g1_pool_walk")) t_g1_pool_walk();
    else { fprintf(stderr, "unknown test '%s'\n", t); return 98; }
    return failures ? 1 : 0;
}
