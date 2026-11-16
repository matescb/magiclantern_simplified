/* C10 copy of tools/raw_vidx_hosttest (original by C4, left untouched), extended with tests for
 * the paths C4-review.md found by reading. Differences from the original: task_create() returns
 * an even value on success like DryOS (the original returns 1, which DryOS uses as an error
 * code), knobs for task-creation failure, short card writes and a broken seek-to-end, and access
 * to the module's two semaphores so a test can hold them.
 *
 * Host-side behavioural test for the raw_vidx module's worker.c + event_pusher.c.
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
extern volatile int rvx_wedged __attribute__((weak));        /* C10 */
extern volatile int rvx_write_failed __attribute__((weak));  /* C10 */
extern int fake_mlv_session_in_progress;

struct raw_info { void *buffer; int height, width, pitch; };
struct raw_info raw_info;

/* ---- test knobs / observations ---- */
static int verbose = 0;
static int defer_writes = 0;
static int fail_create = 0;      /* next N FIO_CreateFile calls fail */
static int fail_write_task = 0;  /* next N task_create("raw_write") calls fail (odd return, as DryOS) */
static int short_write_at = 0;   /* the Nth large FIO_WriteFile (1-based) writes only half and returns that */
static int large_writes = 0;
static int seek_end_broken = 0;  /* FIO_SeekSkipFile(SEEK_END) leaves the position at 0 */
static struct semaphore *sem_write = NULL, *sem_edmac = NULL; /* the module's semaphores, by name */
static int raw_lv_requests = 0;  /* raw_lv_request minus raw_lv_release */
static int fio_live = 0;         /* outstanding fio_malloc blocks */
static int bad_handle_use = 0;   /* FIO call on a closed handle */
static int write_from_freed = 0; /* FIO_WriteFile source inside a freed fio block */
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
        if (!reg[i].p) { reg[i].p = p; reg[i].n = n; reg[i].live = 1; fio_live++; return p; }
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
    struct mfile *f = h->f;
    if (n > (1 << 20) && ++large_writes == short_write_at) n /= 2; /* card full part-way */
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
    else h->pos = seek_end_broken ? 0 : h->f->size + (size_t)pos;
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
    struct semaphore *s = calloc(1, sizeof(*s));
    s->count = initial ? 1 : 0;
    if (name && !strcmp(name, "raw_vid_write")) sem_write = s;
    if (name && !strcmp(name, "edmac_raw_worker")) sem_edmac = s;
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
void msleep(int ms)
{
    (void)ms;
    if (!in_worker && worker_started) run_worker(); /* others make progress while we sleep */
    else if (in_worker) run_pending();
}
void *task_create(const char *name, uint32_t prio, uint32_t stack, void *entry, void *arg)
{
    (void)prio; (void)stack;
    /* Return value as DryOS: even (handle << 1) on success, small odd error code on failure. */
    if (!strcmp(name, "raw_v_worker")) { worker_entry = (void (*)(void))entry; return (void *)2; }
    if (!strcmp(name, "raw_write") && fail_write_task > 0) { fail_write_task--; return (void *)5; }
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
    memset(raw_info.buffer, 0x5A, (size_t)raw_info.pitch * (size_t)raw_info.height);
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

/* ---- C10 tests: paths the review of offline/C4 found by reading (C4-review.md) ---- */
static size_t file_size(const char *name) { struct mfile *f = find_file(name); return f ? f->size : 0; }

/* finding 1: the write semaphore cannot be taken when a buffer fills up (previous writer
 * still busy).  Once it is free again, the full buffer must be flushed and recording resume. */
static void t_flush_sem_timeout(void)
{
    press_rec(); run_worker(); vsync();
    for (int i = 0; i < 5; i++) { vsync(); run_worker(); }  /* buffer A full */
    sem_write->count = 0;                                    /* "previous write task" holds it */
    for (int i = 0; i < 8; i++) { vsync(); run_worker(); }  /* 5 fill buffer B, 3 have nowhere to go */
    CHECK(get_recording_state() == 2, "still recording while the write semaphore is held");
    sem_write->count = 1;                                    /* the writer finally lets go */
    for (int i = 0; i < 3; i++) { vsync(); run_worker(); }  /* must be recorded again */
    press_rec(); run_worker(); vsync();
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(p.valid, "closed file is a well-formed block stream");
    CHECK(p.vidf == 13, "5 + 5 buffered + 3 after the semaphore came back = 13 frames, got %u", p.vidf);
    CHECK(p.header_count == p.vidf, "header count %u equals frames in file %u", p.header_count, p.vidf);
    if (&rvx_dropped_busy) CHECK(rvx_dropped_busy == 3, "worker counted %d dropped frames (expected 3)", rvx_dropped_busy);
    CHECK(fio_live == 0, "no fio buffers left allocated (%d)", fio_live);
    common_checks();
}
/* finding 4: task_create fails while a buffer flush is being started. */
static void t_write_task_create_fails(int at_stop)
{
    if (!at_stop) fail_write_task = 2;
    press_rec(); run_worker(); vsync();
    for (int i = 0; i < 12; i++) { vsync(); run_worker(); }
    if (at_stop) fail_write_task = 100;
    press_rec(); run_worker(); vsync();
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(p.valid, "closed file is a well-formed block stream");
    CHECK(p.vidf == 12, "all 12 frames in file, got %u", p.vidf);
    CHECK(p.header_count == p.vidf, "header count %u equals frames in file %u", p.header_count, p.vidf);
    CHECK(get_recording_state() == 1, "state INACTIVE after stop");
    CHECK(fio_live == 0, "no fio buffers left allocated (%d)", fio_live);
    fail_write_task = 0;
    record(3, 1, 0);
    p = parse_file("tst01.mlv", 1);
    CHECK(p.valid && p.vidf == 3 && p.header_count == 3, "a later recording works (valid=%d frames=%u count=%u)", p.valid, p.vidf, p.header_count);
    common_checks();
}
/* finding 3: a card write never comes back, stop times out.  The file must not be closed
 * and the buffers must not be freed under it; later starts are refused. */
static void t_stop_timeout(void)
{
    press_rec(); run_worker(); vsync();
    for (int i = 0; i < 3; i++) { vsync(); run_worker(); }
    sem_write->count = 0;                    /* a write task is stuck inside FIO_WriteFile */
    int closes_before = 0; { struct mfile *f = find_file("tst00.mlv"); closes_before = f && f->committed; }
    press_rec(); run_worker(); vsync();
    struct mfile *f = find_file("tst00.mlv");
    CHECK(f && !f->committed && !closes_before, "file was not closed under the running write");
    CHECK(fio_live == 2, "both crop buffers still allocated (%d fio blocks live)", fio_live);
    CHECK(get_recording_state() == 1, "state INACTIVE after the timed-out stop");
    CHECK(raw_lv_requests == 0, "raw_lv request/release balanced (%d)", raw_lv_requests);
    CHECK(fake_mlv_session_in_progress == 0, "no MLV session left in progress");
    if (&rvx_wedged) CHECK(rvx_wedged == 1, "rvx_wedged is set");
    sem_write->count = 1;                    /* the stuck task returns much later */
    press_rec(); run_worker(); vsync(); vsync(); run_worker();
    CHECK(get_recording_state() == 1, "a later start is refused");
    CHECK(find_file("tst01.mlv") == NULL, "no second file was created");
    CHECK(raw_lv_requests == 0, "raw_lv still balanced after the refused start (%d)", raw_lv_requests);
    common_checks();
}
/* finding 5: a card write comes back short.  Nothing may be appended after the cut-off block. */
static void t_short_write(void)
{
    short_write_at = 2;                      /* second buffer flush: card full half-way */
    press_rec(); run_worker(); vsync();
    for (int i = 0; i < 17; i++) { vsync(); run_worker(); }
    size_t size_after_failure = file_size("tst00.mlv");
    press_rec(); run_worker(); vsync();
    struct parsed p = parse_file("tst00.mlv", 1);
    /* half of the second 5-frame buffer reached the file: 2 whole blocks and a cut-off one */
    CHECK(p.vidf == 7 && !p.valid, "5 frames + 2 from the half-written buffer, then a cut-off block (frames=%u, clean end=%d)", p.vidf, p.valid);
    CHECK(p.header_count == 5, "header count %u counts only the fully written buffer (5)", p.header_count);
    CHECK(file_size("tst00.mlv") == size_after_failure, "nothing appended after the failed write (%zu -> %zu bytes)", size_after_failure, file_size("tst00.mlv"));
    if (&rvx_write_failed) CHECK(rvx_write_failed == 1, "rvx_write_failed is set");
    CHECK(get_recording_state() == 1, "state INACTIVE after stop");
    CHECK(fio_live == 0, "no fio buffers left allocated (%d)", fio_live);
    short_write_at = 0;
    record(3, 1, 0);
    p = parse_file("tst01.mlv", 1);
    CHECK(p.valid && p.vidf == 3 && p.header_count == 3, "a later recording works (valid=%d frames=%u count=%u)", p.valid, p.vidf, p.header_count);
    common_checks();
}
/* finding 2: with checkpoints on, seek-to-end does not land at the end of the file. */
static void t_checkpoint_bad_seek(void)
{
    if (&rvx_checkpoint) rvx_checkpoint = 1;
    seek_end_broken = 1;
    press_rec(); run_worker(); vsync();
    for (int i = 0; i < 12; i++) { vsync(); run_worker(); }
    press_rec(); run_worker(); vsync();
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(p.valid, "file on card is a well-formed block stream (header not overwritten)");
    CHECK(p.vidf == 5, "the 5 frames written before the bad seek are intact, got %u", p.vidf);
    CHECK(p.header_count == p.vidf, "header count %u equals frames in file %u", p.header_count, p.vidf);
    CHECK(get_recording_state() == 1, "state INACTIVE after stop");
    CHECK(fio_live == 0, "no fio buffers left allocated (%d)", fio_live);
    common_checks();
}
/* finding 6 / C1 B4: the copy semaphore is busy for one frame.  That frame must be
 * dropped whole, not left in the file as a counted block without image data. */
static void t_copy_sem_busy(void)
{
    press_rec(); run_worker(); vsync();
    vsync(); run_worker();
    sem_edmac->count = 0; vsync(); run_worker(); sem_edmac->count = 1; /* one frame hits a busy copy */
    vsync(); run_worker(); vsync(); run_worker();
    press_rec(); run_worker(); vsync();
    struct parsed p = parse_file("tst00.mlv", 1);
    CHECK(p.valid, "closed file is a well-formed block stream");
    CHECK(p.vidf == 3, "3 copied frames in file (the 4th was dropped), got %u", p.vidf);
    CHECK(p.header_count == p.vidf, "header count %u equals frames in file %u", p.header_count, p.vidf);
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
    else if (!strcmp(t, "flush_sem_timeout")) t_flush_sem_timeout();
    else if (!strcmp(t, "write_task_create_fails")) t_write_task_create_fails(0);
    else if (!strcmp(t, "write_task_create_fails_at_stop")) t_write_task_create_fails(1);
    else if (!strcmp(t, "stop_timeout")) t_stop_timeout();
    else if (!strcmp(t, "short_write")) t_short_write();
    else if (!strcmp(t, "checkpoint_bad_seek")) t_checkpoint_bad_seek();
    else if (!strcmp(t, "copy_sem_busy")) t_copy_sem_busy();
    else { fprintf(stderr, "unknown test '%s'\n", t); return 98; }
    return failures ? 1 : 0;
}
