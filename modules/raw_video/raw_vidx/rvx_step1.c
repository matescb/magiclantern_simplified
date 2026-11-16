// [M50 II] Plan steps as runtime toggles: copy / write task priorities, throttled copy,
// frame-integrity records, optional cacheable-source copy.  See rvx_step1.h for the toggles.
// Nothing in this file has run on the camera.

#include "module.h"
#include "dryos.h"
#include "mem.h" // memcpy64

#include "raw_vid.h"
#include "raw_vid_worker.h"
#include "rvx_step1.h"

int rvx_write_prio = WORKER_WRITE_PRIO;
int rvx_copy_prio = 0;
int rvx_copy_chunk_kb = 0;
int rvx_copy_sleep_ms = 10;
int rvx_burst_frames = 0;
int rvx_burst_release_first = 0;
int rvx_integrity = 1;
int rvx_sidecar = 0;
int rvx_drop_torn = 0;
int rvx_fast_copy = 0;

/* Read-only telemetry of the current / most recent session. */
int rvx_torn = 0;           // frames whose source changed during the copy
int rvx_dst_diff = 0;       // frames whose copy differs from the source read afterwards
int rvx_copy_task_fail = 0; // "rvx_copy" task could not be created; frame copied inline instead

/* Canon's d-cache invalidate by range (addr, size), M50 II 1.0.1 ROM 0xE05D088C, Thumb (A13:
 * `bic r0,#0x1f`, loop DCIMVAC step 0x20, then a write of 0 to 0xC1100730).  Only called with
 * rvx_fast_copy = 1.  A pointer, not a direct call, so the host test can replace it. */
void (*rvx_dcache_inval)(uint32_t addr, uint32_t size) = (void (*)(uint32_t, uint32_t))0xE05D088D;

#define CACHE_LINE 32

static struct semaphore *copy_done_sem = NULL;
static struct rvx_copy_job task_job; // the one job the "rvx_copy" task works on
static int task_fast = 0;

// The record array and its header live in one fio_malloc block (uncached, so it can be handed
// to FIO_WriteFile as it is).  NULL = no records this session.
static struct rvx_sidecar_header *hdr = NULL;
static struct rvx_frame_rec *recs = NULL;
static struct rvx_frame_rec *last_rec = NULL;
static struct rvx_sidecar_header no_alloc_hdr; // used for the summary when hdr is NULL
static int log_copy_wait = 0;

int rvx_prio_clamp(int prio)
{
    if (prio < RVX_PRIO_MIN) return RVX_PRIO_MIN;
    if (prio > RVX_PRIO_MAX) return RVX_PRIO_MAX;
    return prio;
}

int rvx_step1_init(void)
{
    copy_done_sem = create_named_semaphore("rvx_copy_done", SEM_CREATE_LOCKED);
    return copy_done_sem == NULL;
}

static struct rvx_sidecar_header *cur_hdr(void)
{
    return hdr != NULL ? hdr : &no_alloc_hdr;
}

void rvx_session_start(uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint32_t fps_core)
{
    rvx_session_end(); // a leftover block of a session that never reported
    rvx_torn = 0;
    rvx_dst_diff = 0;
    rvx_copy_task_fail = 0;
    log_copy_wait = 0;
    memset(&no_alloc_hdr, 0, sizeof(no_alloc_hdr));

    if (rvx_integrity)
    {
        uint32_t size = sizeof(struct rvx_sidecar_header) + RVX_MAX_RECS * sizeof(struct rvx_frame_rec);
        hdr = fio_malloc(size);
        if (hdr == NULL)
            SEND_LOG_DATA_STR("WARN: no memory for integrity records, recording without them\n");
        else
        {
            memset(hdr, 0, sizeof(*hdr));
            recs = (struct rvx_frame_rec *)(hdr + 1);
        }
    }

    struct rvx_sidecar_header *s = cur_hdr();
    s->magic = RVX_SIDECAR_MAGIC;
    s->version = RVX_SIDECAR_VERSION;
    s->header_size = sizeof(struct rvx_sidecar_header);
    s->record_size = sizeof(struct rvx_frame_rec);
    s->w = w; s->h = h; s->x = x; s->y = y;
    s->copy_prio = rvx_copy_prio ? (uint32_t)rvx_prio_clamp(rvx_copy_prio) : 0;
    s->write_prio = (uint32_t)rvx_prio_clamp(rvx_write_prio);
    s->chunk_kb = (uint32_t)rvx_copy_chunk_kb;
    s->sleep_ms = (uint32_t)rvx_copy_sleep_ms;
    s->burst_frames = (uint32_t)rvx_burst_frames;
    s->release_first = (uint32_t)rvx_burst_release_first;
    s->fast_copy = (uint32_t)rvx_fast_copy;
    s->drop_torn = (uint32_t)rvx_drop_torn;
    s->fps_x1000_core = fps_core;
}

void rvx_session_end(void)
{
    if (hdr != NULL)
        fio_free(hdr);
    hdr = NULL;
    recs = NULL;
    last_rec = NULL;
}

// FNV-1a over RVX_PROBES probes of RVX_PROBE_BYTES bytes.  Probe k sits in row k*(rows-1)/7,
// so the first and the last row of the window are always sampled, at a column that moves from
// the left to the right edge.  base / pitch select source or copy.
static uint32_t probe_hash(uint32_t base, uint32_t pitch, uint32_t line_size, uint32_t rows)
{
    uint32_t hash = 0x811C9DC5;
    if (rows == 0 || line_size < RVX_PROBE_BYTES)
        return hash;
    for (uint32_t k = 0; k < RVX_PROBES; k++)
    {
        uint32_t row = k * (rows - 1) / (RVX_PROBES - 1);
        uint32_t col = (k * (line_size - RVX_PROBE_BYTES) / (RVX_PROBES - 1)) & ~3u;
        const volatile uint8_t *p = (const volatile uint8_t *)(base + row * pitch + col);
        for (uint32_t i = 0; i < RVX_PROBE_BYTES; i++)
            hash = (hash ^ p[i]) * 0x01000193;
    }
    return hash;
}

static int sleep_ms_clamped(void)
{
    int ms = rvx_copy_sleep_ms;
    if (ms < 1) return 1;
    if (ms > 100) return 100;
    return ms;
}

// The copy itself.  Runs in the worker or in the "rvx_copy" task.
static void do_copy(const struct rvx_copy_job *j, int fast)
{
    uint32_t chunk_rows = 0; // 0 = no pauses
    if (rvx_copy_chunk_kb > 0 && j->line_size > 0)
    {
        chunk_rows = (uint32_t)rvx_copy_chunk_kb * 1024 / j->line_size;
        if (chunk_rows == 0)
            chunk_rows = 1;
    }
    // Lines are invalidated just before the rows they cover are read, never a whole frame
    // ahead: a pause (or a slow copy) must not sit between the invalidate and the read.
    const uint32_t inval_rows = chunk_rows ? chunk_rows : 64;
    const uint32_t src = fast ? (uint32_t)CACHEABLE(j->src) : j->src;
    const uint32_t body = j->line_size & ~7u;
    const int use64 = fast && body > 0
                      && ((src | j->src_pitch | j->dst | j->line_size) & 3) == 0;

    for (uint32_t row = 0; row < j->rows; row++)
    {
        uint32_t s = src + row * j->src_pitch;
        uint32_t d = j->dst + row * j->line_size;
        if (fast && row % inval_rows == 0)
        {
            uint32_t n = j->rows - row < inval_rows ? j->rows - row : inval_rows;
            uint32_t first = s & ~(uint32_t)(CACHE_LINE - 1);
            uint32_t end = s + (n - 1) * j->src_pitch + j->line_size;
            rvx_dcache_inval(first, end - first);
        }
        if (use64)
        {
            memcpy64((void *)d, (void *)s, body);
            if (j->line_size > body)
                memcpy((void *)(d + body), (void *)(s + body), j->line_size - body);
        }
        else
        {
            memcpy((void *)d, (void *)s, j->line_size);
        }
        if (chunk_rows && (row + 1) % chunk_rows == 0 && row + 1 < j->rows)
            msleep(sleep_ms_clamped());
    }
}

static void copy_task(void *unused)
{
    do_copy(&task_job, task_fast);
    give_semaphore(copy_done_sem);
}

// DryOS task_create() returns an odd error code when it fails (see worker.c).
static int create_failed(uint32_t ret)
{
    return (ret & 1) != 0;
}

static void run_copy(const struct rvx_copy_job *job, int fast)
{
    if (rvx_copy_prio != 0 && copy_done_sem != NULL)
    {
        task_job = *job;
        task_fast = fast;
        uint32_t ret = (uint32_t)task_create("rvx_copy", rvx_prio_clamp(rvx_copy_prio),
                                             RVX_COPY_STACK, copy_task, NULL);
        if (!create_failed(ret))
        {
            // The task writes into the capture buffer until it is done, so there is no safe
            // way to give up on it: wait, however long it takes.
            while (take_semaphore(copy_done_sem, 1000))
            {
                if (log_copy_wait < 5)
                {
                    SEND_LOG_DATA_STR("WARN: frame copy task has not finished after 1 s, still waiting\n");
                    log_copy_wait++;
                }
            }
            return;
        }
        rvx_copy_task_fail++;
    }
    do_copy(job, fast);
}

uint32_t rvx_copy_frame(const struct rvx_copy_job *job, uint32_t vd_seq, uint32_t t_vsync_us,
                        uint32_t stored_index)
{
    const int fast = rvx_fast_copy && rvx_dcache_inval != NULL;
    last_rec = NULL;

    if (!rvx_integrity)
    {
        run_copy(job, fast);
        return fast ? RVX_REC_FAST_COPY : 0;
    }

    // The source is always hashed through the address the caller gave (uncached), so the
    // hashes do not depend on the cache state the fast copy is trying to get right.
    uint32_t t0 = (uint32_t)get_us_clock();
    uint32_t before = probe_hash(job->src, job->src_pitch, job->line_size, job->rows);
    run_copy(job, fast);
    uint32_t t1 = (uint32_t)get_us_clock();
    uint32_t in_copy = probe_hash(job->dst, job->line_size, job->line_size, job->rows);
    uint32_t after = probe_hash(job->src, job->src_pitch, job->line_size, job->rows);

    uint32_t flags = fast ? RVX_REC_FAST_COPY : 0;
    if (before != after) { flags |= RVX_REC_TORN; rvx_torn++; }
    if (in_copy != after) { flags |= RVX_REC_DST_DIFF; rvx_dst_diff++; }

    if (hdr != NULL)
    {
        if (hdr->n_records < RVX_MAX_RECS)
        {
            struct rvx_frame_rec *r = &recs[hdr->n_records++];
            r->stored_index = stored_index;
            r->vd_seq = vd_seq;
            r->t_vsync_us = t_vsync_us;
            r->t_copy_us = t0;
            r->copy_us = t1 - t0;
            r->hash_before = before;
            r->hash_after = after;
            r->hash_dst = in_copy;
            r->flags = flags;
            last_rec = r;
        }
        else
        {
            hdr->records_lost++;
        }
    }
    return flags;
}

void rvx_mark_last_dropped(void)
{
    if (last_rec != NULL)
    {
        last_rec->flags |= RVX_REC_DROPPED;
        last_rec->stored_index = 0xFFFFFFFF;
    }
}

// Replaces the extension of an 8.3 name; returns 0 on success.
static int sidecar_name(const char *mlv_name, char *out, uint32_t out_size)
{
    uint32_t len = strlen(mlv_name);
    if (len < 5 || len + 1 > out_size || mlv_name[len - 4] != '.')
        return 1;
    memcpy(out, mlv_name, len + 1);
    out[len - 3] = 'R'; out[len - 2] = 'V'; out[len - 1] = 'I';
    return 0;
}

void rvx_session_report(const char *mlv_name, const struct rvx_session_counts *c)
{
    struct rvx_sidecar_header *s = cur_hdr();
    s->vsyncs = rvx_vsyncs;
    s->frames_stored = c->frames_stored;
    s->frames_on_disk = c->frames_on_disk;
    s->dropped_queue = c->dropped_queue;
    s->dropped_busy = c->dropped_busy;
    s->dropped_nofile = c->dropped_nofile;
    s->dropped_copy = c->dropped_copy;
    s->torn = (uint32_t)rvx_torn;
    s->dst_diff = (uint32_t)rvx_dst_diff;
    s->copy_task_fail = (uint32_t)rvx_copy_task_fail;

    uint32_t copy_us_max = 0;
    if (hdr != NULL && hdr->n_records > 0)
    {
        const struct rvx_frame_rec *first = &recs[0];
        const struct rvx_frame_rec *last = &recs[hdr->n_records - 1];
        s->first_t_us = first->t_vsync_us;
        s->last_t_us = last->t_vsync_us;
        uint32_t dt = last->t_vsync_us - first->t_vsync_us; // wraps after 71 minutes
        uint32_t dn = last->vd_seq - first->vd_seq;
        uint32_t dt_ms = dt / 1000;
        // 32-bit arithmetic only (no 64-bit division helper in a module).
        if (dt_ms > 0 && dn > 0)
            s->fps_x1000_measured = dn < 4000 ? dn * 1000000 / dt_ms
                                              : dn * 1000 / dt_ms * 1000;
        for (uint32_t i = 0; i < hdr->n_records; i++)
            if (recs[i].copy_us > copy_us_max)
                copy_us_max = recs[i].copy_us;
    }

    RVX_HEX("vsyncs in session", s->vsyncs);
    RVX_HEX("frames torn (source changed during copy)", s->torn);
    RVX_HEX("frames whose copy differs from source", s->dst_diff);
    RVX_HEX("slowest frame copy, us", copy_us_max);
    RVX_HEX("fps x1000 measured", s->fps_x1000_measured);
    RVX_HEX("fps x1000 from core", s->fps_x1000_core);
    if (s->copy_task_fail)
        RVX_HEX("copy task create failures", s->copy_task_fail);

    if (!rvx_sidecar || hdr == NULL || mlv_name == NULL)
        return;

    char name[20];
    if (sidecar_name(mlv_name, name, sizeof(name)))
    {
        SEND_LOG_DATA_STR("ERROR: cannot derive sidecar name\n");
        return;
    }
    FILE *fp = FIO_CreateFile(name);
    if (fp == NULL)
    {
        SEND_LOG_DATA_STR("ERROR: cannot create integrity sidecar\n");
        return;
    }
    uint32_t size = sizeof(*hdr) + hdr->n_records * sizeof(struct rvx_frame_rec);
    if (FIO_WriteFile(fp, hdr, size) != (int)size)
        SEND_LOG_DATA_STR("ERROR: short write of integrity sidecar\n");
    FIO_CloseFile(fp);
    SEND_LOG_DATA_STR("integrity sidecar written\n");
}
