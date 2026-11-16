#ifndef _rvx_sync_h_
#define _rvx_sync_h_

/* [M50 II, firmware 1.0.1] Module-only frame-sync sources and the event-container pool watchdog.
 * See docs/offline/G1.md, F1.md (sync sources) and F4.md (pool).  NOTHING here has run on the
 * camera.  All toggles are plain ints, changed over serial with `wr &<name> <value>`.
 *
 * name                 default  meaning
 * rvx_sync_src         0        which per-frame signal drives the recording; read at REC start.
 *                               0 = the core's vsync hook (CBR_VSYNC; behaviour before this change)
 *                               1 = module hook on the dispatch pointer of the state object
 *                                   "EvfState" *(0x8E8C), input 5 (evfReadOutDoneInterrupt)
 *                               2 = module chain on the Mem1Path write-complete callback slot
 *                                   (RAM 0x1029C, stock value 0xE06AD51B)
 *                               With 1 or 2 the hook only counts and gives a semaphore; the
 *                               "rvx_sync" task (created on first use) does the per-frame work.
 *                               If validation fails nothing is written and the recording runs
 *                               on source 0 (rvx_sync_active tells which source is in use).
 * rvx_pool_watch       1        0 = off; 1 = measure Canon's event-container free list from the
 *                               worker and stop the recording when it falls below
 *                               rvx_pool_stop_below; 2 = measure only, never stop.  Read-only
 *                               on Canon's data: a bounded, range-checked walk, no lock taken.
 * rvx_pool_stop_below  256      stop threshold in free containers (F4 section 7, rule 5).
 * rvx_pool_period      8        check every N frames handled by the worker.
 * rvx_pool_walk_max    1536     upper bound of one walk (clamped to the pool size).
 */

/* Firmware constants.  A table, not #defines, so that the host test can point the code at a fake
 * state object, callback slot and pool.  Values are for M50 II 1.0.1 only. */
struct rvx_sync_cfg
{
    uint32_t evf_obj_pp;   // 0x8E8C: holds the pointer to the EvfState state object
    uint32_t evf_type;     // object +0x00: 0xE13731D8, the value Canon's dispatch wrapper
                           //   0xE05BB298 compares against (ROM word at 0xE0A6C4CC)
    uint32_t evf_name;     // object +0x04: 0xE137558C -> "EvfState"
    uint32_t evf_dispatch; // object +0x0C: 0xE05BB2B3, the stock dispatcher
    uint32_t evf_matrix;   // object +0x10: 0xE0B5050C
    uint32_t evf_inputs;   // object +0x14: 0x11
    uint32_t evf_states;   // object +0x18: 8
    uint32_t obj_lo, obj_hi; // range a state-object pointer must lie in
    uint32_t mem1_slot;    // 0x1029C: callback word (its argument at +4 is never touched)
    uint32_t mem1_stock;   // 0xE06AD51B: imagerMem1PathWrDmacCompleteCBR
    uint32_t pool_head;    // 0x39B84: free-list head of the event-container pool
    uint32_t pool_base;    // 0xBEE18
    uint32_t pool_end;     // 0xC4E18 = base + 0x600 * 0x10
};
extern struct rvx_sync_cfg rvx_sync_cfg;

/* rvx_sync_last_err values (why the last install / restore did not do what was asked) */
#define RVX_SYNC_OK              0
#define RVX_SYNC_E_SRC           1  // rvx_sync_src is not 0, 1 or 2
#define RVX_SYNC_E_TASK          2  // semaphore or "rvx_sync" task could not be created
#define RVX_SYNC_E_OBJ_PTR       3  // *(0x8E8C) is NULL, unaligned or outside RAM
#define RVX_SYNC_E_OBJ_TYPE      4
#define RVX_SYNC_E_OBJ_NAME      5
#define RVX_SYNC_E_OBJ_DISPATCH  6  // dispatch pointer is not the stock one (somebody else hooked it)
#define RVX_SYNC_E_OBJ_MATRIX    7
#define RVX_SYNC_E_OBJ_DIMS      8
#define RVX_SYNC_E_OBJ_STATE     9  // current state outside 0..states-1
#define RVX_SYNC_E_SLOT_EMPTY    10 // Mem1Path slot is 0: Canon's raw path is not set up
#define RVX_SYNC_E_SLOT_VALUE    11 // Mem1Path slot holds something else than the stock callback
#define RVX_SYNC_E_RESTORE       12 // at stop the EvfState dispatch pointer was not ours any more
#define RVX_SYNC_E_CHAIN_LOST    13 // at stop the Mem1Path slot was not ours any more (Canon rewrote it)
#define RVX_SYNC_E_SILENT        14 // hook installed but no frame signal for RVX_SYNC_SILENT_MS

#define RVX_SYNC_PRIO       0x10   // "rvx_sync" task: one above the worker, posts events only
#define RVX_SYNC_STACK      0x1000
#define RVX_SYNC_WAIT_MS    500
#define RVX_SYNC_SILENT_N   4      // 4 x 500 ms without a signal = stop the recording

extern int rvx_sync_src;
extern int rvx_pool_watch, rvx_pool_stop_below, rvx_pool_period, rvx_pool_walk_max;

/* Telemetry, read with `rd &<name>`.  Reset when a recording starts unless noted. */
extern volatile int rvx_sync_active;     // source in use by the current recording (0, 1, 2)
extern volatile uint32_t rvx_sync_ticks; // signals counted in Canon's context
extern int rvx_sync_core_ignored;        // core vsync callbacks ignored while source 1 / 2 was active
extern int rvx_sync_missed;              // signals that arrived before the task handled the previous one
extern int rvx_sync_refused;             // installs refused since boot
extern int rvx_sync_restore_fail;        // restores that found a foreign pointer, since boot
extern int rvx_sync_silent;              // recordings stopped because the source went silent, since boot
extern int rvx_sync_last_err;            // RVX_SYNC_*
extern uint32_t rvx_sync_last_val;       // the value that failed the check named by rvx_sync_last_err
extern int rvx_pool_armed;               // 1 = the watchdog may stop this recording
extern int rvx_pool_free_start, rvx_pool_free_last, rvx_pool_free_min;
extern int rvx_pool_checks;
extern int rvx_pool_tripped;             // 0, or 1 + the level that stopped the recording

// Worker task only.
// Installs the source selected by rvx_sync_src.  Returns the source in use (0 after a refusal).
int rvx_sync_install(void);
// Puts Canon's pointer back if it is still ours.  Safe to call when nothing is installed.
void rvx_sync_uninstall(void);
// The recording is INACTIVE: the core's vsync callback drives the module again.
void rvx_sync_session_end(void);
void rvx_sync_report(void);

// min(free containers, limit); read-only bounded walk, callable from any task (never from a hook).
uint32_t rvx_pool_free(uint32_t limit);
void rvx_pool_session_start(void);
// Call once per frame handled by the worker.  Returns 1 when the recording must end now.
int rvx_pool_frame_check(void);

// event_pusher.c
unsigned int rvx_frame_tick(void); // the per-frame work, whatever the source
void rvx_request_stop(void);       // asks the worker to stop, without blocking the caller

#endif // _rvx_sync_h_
