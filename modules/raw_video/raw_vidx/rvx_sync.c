// [M50 II, firmware 1.0.1] Module-only frame-sync sources (rvx_sync_src) and the event-container
// pool watchdog (rvx_pool_watch).  Toggles and constants: rvx_sync.h.  Background: docs/offline
// F1.md, F4.md, G1.md.  Nothing in this file has run on the camera.
//
// Rules this file keeps:
//  - Canon's context (the two hook functions below) only counts and gives a semaphore.
//  - A pointer is written only after every identity check passed, and only if the word still
//    holds the expected stock value at the moment of the write (checked with interrupts off).
//  - A pointer is restored only if the word still holds OUR function; otherwise it is left alone
//    and the hook function stays a pure pass-through (the module is never unloaded).
//  - The pool is only read.

#include "module.h"
#include "dryos.h"

#include "raw_vid.h"
#include "raw_vid_worker.h"
#include "rvx_sync.h"

struct rvx_sync_cfg rvx_sync_cfg = {
    .evf_obj_pp   = 0x00008E8C,
    .evf_type     = 0xE13731D8,
    .evf_name     = 0xE137558C,
    .evf_dispatch = 0xE05BB2B3,
    .evf_matrix   = 0xE0B5050C,
    .evf_inputs   = 0x11,
    .evf_states   = 8,
    .obj_lo       = 0x00001000,
    .obj_hi       = 0x40000000,
    .mem1_slot    = 0x0001029C,
    .mem1_stock   = 0xE06AD51B,
    .pool_head    = 0x00039B84,
    .pool_base    = 0x000BEE18,
    .pool_end     = 0x000C4E18,
};

int rvx_sync_src = 0;
int rvx_pool_watch = 1;
int rvx_pool_stop_below = 256;
int rvx_pool_period = 8;
int rvx_pool_walk_max = 1536;

volatile int rvx_sync_active = 0;
volatile uint32_t rvx_sync_ticks = 0;
int rvx_sync_missed = 0;
int rvx_sync_refused = 0;
int rvx_sync_restore_fail = 0;
int rvx_sync_silent = 0;
int rvx_sync_last_err = RVX_SYNC_OK;
uint32_t rvx_sync_last_val = 0;
int rvx_pool_armed = 0;
int rvx_pool_free_start = 0, rvx_pool_free_last = 0, rvx_pool_free_min = 0;
int rvx_pool_checks = 0;
int rvx_pool_tripped = 0;

extern int rvx_sync_core_ignored; // event_pusher.c

typedef int (*rvx_dispatch_t)(void *self, void *x, uint32_t input, void *z, void *t);
typedef void (*rvx_mem1_cbr_t)(void *arg);

static struct semaphore *sync_sem = NULL;
static int sync_task_made = 0;
// 1 while a hook of ours is in place AND its signals are wanted.  Cleared before a restore.
static volatile int sync_armed = 0;
static volatile uint32_t pump_seen = 0;
static int silent_reported = 0;

// Saved stock pointers.  Written before the hook goes in and never cleared, so a hook function
// that is still reachable after a failed restore always has a valid target.
static rvx_dispatch_t evf_orig = NULL;
static volatile uint32_t *evf_obj = NULL; // the object whose +0x0C we wrote
static rvx_mem1_cbr_t mem1_orig = NULL;

/* ---- Canon's context.  Count, give a semaphore, nothing else. ---- */

// Replaces the dispatch pointer of EvfState.  Runs in the EvfCap task (F1), possibly nested:
// the input-5 action dispatches further inputs on the same object before it returns.
static int rvx_evf_spy(void *self, void *x, uint32_t input, void *z, void *t)
{
    uint32_t old_state = ((volatile uint32_t *)self)[7]; // +0x1C, before the transition
    int ret = evf_orig(self, x, input, z, t);
    if (input == 5 && old_state >= 3 && old_state <= 6)
    {
        rvx_sync_ticks++;
        if (sync_armed)
            give_semaphore(sync_sem);
    }
    return ret;
}

// Chained in front of Canon's EDMAC channel-3 write-complete callback.  Context unknown,
// probably interrupt (F1); Canon's own callback gives a semaphore from here (A1).
static void rvx_mem1_tramp(void *arg)
{
    rvx_sync_ticks++;
    if (sync_armed)
        give_semaphore(sync_sem);
    mem1_orig(arg);
}

/* ---- the "rvx_sync" task: turns signals into the module's per-frame work ---- */

static void rvx_sync_task(void *unused)
{
    int silent = 0;
    TASK_LOOP
    {
        int err = take_semaphore(sync_sem, RVX_SYNC_WAIT_MS);
        if (!sync_armed)
        {
            silent = 0;
            continue;
        }
        if (err)
        {
            // Hook in place but no signal.  For source 2 this is what a slot rewritten or
            // cleared by Canon (0xE07541A4 / 0xE07541AC) looks like.  End the recording; the
            // stop restores (or leaves alone) the pointer.
            if (++silent >= RVX_SYNC_SILENT_N && !silent_reported)
            {
                silent_reported = 1;
                rvx_sync_silent++;
                rvx_sync_last_err = RVX_SYNC_E_SILENT;
                SEND_LOG_DATA_STR("sync: ERROR selected source gives no frame signal, stopping the recording\n");
                rvx_request_stop();
            }
            continue;
        }
        silent = 0;
        uint32_t now = rvx_sync_ticks;
        if (now - pump_seen > 1)
            rvx_sync_missed += (int)(now - pump_seen - 1);
        pump_seen = now;
        if (get_recording_state() == ACTIVE)
            rvx_frame_tick();
    }
}

static int sync_task_ready(void)
{
    if (sync_sem == NULL)
        sync_sem = create_named_semaphore("rvx_sync", SEM_CREATE_LOCKED);
    if (sync_sem == NULL)
        return 0;
    if (!sync_task_made)
    {
        uint32_t ret = (uint32_t)task_create("rvx_sync", RVX_SYNC_PRIO, RVX_SYNC_STACK, rvx_sync_task, NULL);
        if (ret & 1) // DryOS: odd = error
            return 0;
        sync_task_made = 1;
    }
    return 1;
}

static int refuse(int err, uint32_t val, const char *msg)
{
    sync_armed = 0;
    rvx_sync_active = 0;
    rvx_sync_refused++;
    rvx_sync_last_err = err;
    rvx_sync_last_val = val;
    SEND_LOG_DATA_STR("sync: REFUSED, nothing written, using the core vsync hook: ");
    m50ii_uart_str(msg);
    m50ii_uart_str("\n");
    RVX_HEX("sync: refused, value read", val);
    return 0;
}

static int install_evf(void)
{
    const struct rvx_sync_cfg *c = &rvx_sync_cfg;
    uint32_t obj = *(volatile uint32_t *)c->evf_obj_pp;
    if (obj == 0 || (obj & 3) || obj < c->obj_lo || obj >= c->obj_hi)
        return refuse(RVX_SYNC_E_OBJ_PTR, obj, "EvfState object pointer is not usable");
    volatile uint32_t *o = (volatile uint32_t *)obj;
    if (o[0] != c->evf_type)
        return refuse(RVX_SYNC_E_OBJ_TYPE, o[0], "object +0x00 is not the state-object type word");
    if (o[1] != c->evf_name)
        return refuse(RVX_SYNC_E_OBJ_NAME, o[1], "object +0x04 is not the EvfState name pointer");
    if (o[4] != c->evf_matrix)
        return refuse(RVX_SYNC_E_OBJ_MATRIX, o[4], "object +0x10 is not the EvfState matrix");
    if (o[5] != c->evf_inputs || o[6] != c->evf_states)
        return refuse(RVX_SYNC_E_OBJ_DIMS, (o[5] << 16) | (o[6] & 0xFFFF), "matrix dimensions are not 17 x 8");
    if (o[7] >= c->evf_states)
        return refuse(RVX_SYNC_E_OBJ_STATE, o[7], "current state is out of range");
    if (o[3] != c->evf_dispatch && !(o[3] == (uint32_t)rvx_evf_spy && evf_orig != NULL))
        return refuse(RVX_SYNC_E_OBJ_DISPATCH, o[3], "object +0x0C is not the stock dispatcher");

    // The hook function must be complete before it becomes reachable.
    pump_seen = rvx_sync_ticks;
    rvx_sync_active = 1; // from here the core's vsync callback is ignored
    sync_armed = 1;
    uint32_t seen;
    uint32_t irq = cli();
    seen = o[3];
    if (seen == c->evf_dispatch)
    {
        evf_orig = (rvx_dispatch_t)c->evf_dispatch;
        evf_obj = o;
        o[3] = (uint32_t)rvx_evf_spy;
    }
    else if (seen == (uint32_t)rvx_evf_spy && evf_orig != NULL)
    {
        evf_obj = o; // our pass-through from an earlier recording is still there: use it
    }
    sei(irq);
    if (seen != c->evf_dispatch && seen != (uint32_t)rvx_evf_spy)
        return refuse(RVX_SYNC_E_OBJ_DISPATCH, seen, "dispatch pointer changed during the install");
    SEND_LOG_DATA_STR("sync: source 1 installed (EvfState input 5)\n");
    return 1;
}

static int install_mem1(void)
{
    const struct rvx_sync_cfg *c = &rvx_sync_cfg;
    volatile uint32_t *slot = (volatile uint32_t *)c->mem1_slot;
    uint32_t v = *slot;
    if (v == 0)
        return refuse(RVX_SYNC_E_SLOT_EMPTY, v, "Mem1Path callback slot is empty (raw path not set up)");
    if (v != c->mem1_stock)
        return refuse(RVX_SYNC_E_SLOT_VALUE, v, "Mem1Path callback slot does not hold the stock callback");

    mem1_orig = (rvx_mem1_cbr_t)c->mem1_stock;
    pump_seen = rvx_sync_ticks;
    rvx_sync_active = 2;
    sync_armed = 1;
    uint32_t irq = cli();
    v = *slot;
    if (v == c->mem1_stock)
        *slot = (uint32_t)rvx_mem1_tramp;
    sei(irq);
    if (v != c->mem1_stock)
        return refuse(v ? RVX_SYNC_E_SLOT_VALUE : RVX_SYNC_E_SLOT_EMPTY, v, "Mem1Path slot changed during the install");
    SEND_LOG_DATA_STR("sync: source 2 installed (Mem1Path write-complete chain)\n");
    return 2;
}

int rvx_sync_install(void)
{
    int src = rvx_sync_src;
    sync_armed = 0;
    rvx_sync_active = 0;
    rvx_sync_ticks = 0;
    rvx_sync_missed = 0;
    rvx_sync_core_ignored = 0;
    rvx_sync_last_err = RVX_SYNC_OK;
    rvx_sync_last_val = 0;
    silent_reported = 0;
    if (src == 0)
        return 0;
    if (src != 1 && src != 2)
        return refuse(RVX_SYNC_E_SRC, (uint32_t)src, "rvx_sync_src must be 0, 1 or 2");
    if (!sync_task_ready())
        return refuse(RVX_SYNC_E_TASK, 0, "could not create the rvx_sync semaphore / task");
    return src == 1 ? install_evf() : install_mem1();
}

void rvx_sync_uninstall(void)
{
    const struct rvx_sync_cfg *c = &rvx_sync_cfg;
    int src = rvx_sync_active;
    if (!sync_armed)
        return; // nothing of ours is wanted (source 0, or the install was refused)
    sync_armed = 0; // the hook functions are pass-through from here on

    if (src == 1)
    {
        uint32_t seen = 0;
        int same_obj = (*(volatile uint32_t *)c->evf_obj_pp == (uint32_t)evf_obj);
        if (same_obj)
        {
            uint32_t irq = cli();
            seen = evf_obj[3];
            if (seen == (uint32_t)rvx_evf_spy)
                evf_obj[3] = (uint32_t)evf_orig;
            sei(irq);
        }
        if (same_obj && seen == (uint32_t)rvx_evf_spy)
        {
            SEND_LOG_DATA_STR("sync: source 1 removed, stock dispatcher restored\n");
        }
        else
        {
            // Somebody chained on top of us, or Canon made a new object.  Writing our saved
            // value now could cut their chain or hit freed memory: leave everything alone.
            rvx_sync_restore_fail++;
            rvx_sync_last_err = RVX_SYNC_E_RESTORE;
            rvx_sync_last_val = same_obj ? seen : *(volatile uint32_t *)c->evf_obj_pp;
            SEND_LOG_DATA_STR("sync: WARN EvfState dispatch pointer is not ours any more, NOT restored (hook stays as pass-through)\n");
            RVX_HEX("sync: value found", rvx_sync_last_val);
        }
    }
    else if (src == 2)
    {
        volatile uint32_t *slot = (volatile uint32_t *)c->mem1_slot;
        uint32_t irq = cli();
        uint32_t seen = *slot;
        if (seen == (uint32_t)rvx_mem1_tramp)
            *slot = (uint32_t)mem1_orig;
        sei(irq);
        if (seen == (uint32_t)rvx_mem1_tramp)
        {
            SEND_LOG_DATA_STR("sync: source 2 removed, stock Mem1Path callback restored\n");
        }
        else
        {
            // Canon rewrote (0xE07541A4) or cleared (0xE07541AC) the slot itself.  A stale
            // callback must never be written back.
            rvx_sync_last_err = RVX_SYNC_E_CHAIN_LOST;
            rvx_sync_last_val = seen;
            SEND_LOG_DATA_STR("sync: Mem1Path slot was rewritten by Canon during the recording, left as found\n");
            RVX_HEX("sync: value found", seen);
        }
    }
}

void rvx_sync_session_end(void)
{
    rvx_sync_uninstall(); // normally a no-op: the stop already did it
    rvx_sync_active = 0;
}

void rvx_sync_report(void)
{
    if (rvx_sync_active != 0 || rvx_sync_last_err != RVX_SYNC_OK)
    {
        RVX_HEX("sync: source used", (uint32_t)rvx_sync_active);
        RVX_HEX("sync: signals", rvx_sync_ticks);
        RVX_HEX("sync: core vsyncs ignored", (uint32_t)rvx_sync_core_ignored);
        RVX_HEX("sync: signals not handled in time", (uint32_t)rvx_sync_missed);
        RVX_HEX("sync: last error code", (uint32_t)rvx_sync_last_err);
    }
    if (rvx_pool_watch)
    {
        RVX_HEX("pool: free at start", (uint32_t)rvx_pool_free_start);
        RVX_HEX("pool: free minimum", (uint32_t)rvx_pool_free_min);
        RVX_HEX("pool: free last", (uint32_t)rvx_pool_free_last);
        RVX_HEX("pool: checks", (uint32_t)rvx_pool_checks);
        if (rvx_pool_tripped)
            RVX_HEX("pool: recording stopped at level", (uint32_t)(rvx_pool_tripped - 1));
    }
}

/* ---- event-container pool watchdog (F4) ----
 * Node layout, checked in the ROM: the pool is 0x600 nodes of 0x10 bytes from 0xBEE18; word 0 of
 * a free node is the link to the next free node, the last link is 0 (init loop
 * 0xE05B9EB8-0xE05B9EC8); pop reads `head = [0x39B84]; [0x39B84] = [head]` (0xE05B9EF6-0xE05B9EFC),
 * push does `[node] = head; head = node` (0xE05B9DBE-0xE05B9DC2).  Both run under Canon's lock
 * 0xE021B8F0 (interrupts off + spinlock word 0x39B80).  We do not take that lock: the walk only
 * reads, every pointer is range- and alignment-checked before it is followed, and the number of
 * steps is bounded, so a concurrent pop / push can make the count wrong but cannot make the walk
 * leave the pool or loop for ever.  A node popped under us carries event data in word 0, which
 * normally ends the walk early (count too low); a low reading is therefore confirmed twice. */

static uint32_t pool_nodes(void)
{
    return (rvx_sync_cfg.pool_end - rvx_sync_cfg.pool_base) / 0x10;
}

uint32_t rvx_pool_free(uint32_t limit)
{
    const uint32_t base = rvx_sync_cfg.pool_base, end = rvx_sync_cfg.pool_end;
    uint32_t n = 0;
    if (limit > pool_nodes())
        limit = pool_nodes();
    uint32_t p = *(volatile uint32_t *)rvx_sync_cfg.pool_head;
    while (n < limit && p >= base && p < end && ((p - base) & 0xF) == 0)
    {
        p = *(volatile uint32_t *)p;
        n++;
    }
    return n;
}

static uint32_t pool_walk_max(void)
{
    uint32_t m = rvx_pool_walk_max > 0 ? (uint32_t)rvx_pool_walk_max : 1;
    return m > pool_nodes() ? pool_nodes() : m;
}

static int pool_frames = 0;

void rvx_pool_session_start(void)
{
    rvx_pool_armed = 0;
    rvx_pool_checks = 0;
    rvx_pool_tripped = 0;
    rvx_pool_free_start = rvx_pool_free_last = rvx_pool_free_min = 0;
    pool_frames = 0;
    if (!rvx_pool_watch)
        return;
    int level = (int)rvx_pool_free(pool_walk_max());
    rvx_pool_free_start = rvx_pool_free_last = rvx_pool_free_min = level;
    RVX_HEX("pool: free event containers at start", (uint32_t)level);
    if (rvx_pool_watch != 1)
        return; // measure only
    // Arm only on a plausible, healthy baseline.  If the head address or the layout were wrong
    // on the camera the walk returns 0 or a small number at once; a watchdog that then stopped
    // every recording would be worse than none.
    if (rvx_pool_stop_below > 0 && level >= 2 * rvx_pool_stop_below)
        rvx_pool_armed = 1;
    else
        SEND_LOG_DATA_STR("pool: baseline below twice the stop threshold, watchdog NOT armed (measuring only)\n");
}

int rvx_pool_frame_check(void)
{
    if (!rvx_pool_watch)
        return 0;
    int period = rvx_pool_period > 0 ? rvx_pool_period : 1;
    if (++pool_frames < period)
        return 0;
    pool_frames = 0;

    int level = (int)rvx_pool_free(pool_walk_max());
    if (rvx_pool_armed && level < rvx_pool_stop_below)
    {
        // Confirm: a walk that raced a pop reads low.  Keep the highest of three readings.
        for (int i = 0; i < 2 && level < rvx_pool_stop_below; i++)
        {
            int again = (int)rvx_pool_free(pool_walk_max());
            if (again > level)
                level = again;
        }
    }
    rvx_pool_checks++;
    rvx_pool_free_last = level;
    if (level < rvx_pool_free_min)
        rvx_pool_free_min = level;
    if (rvx_pool_armed && level < rvx_pool_stop_below && !rvx_pool_tripped)
    {
        rvx_pool_tripped = level + 1;
        return 1;
    }
    return 0;
}
