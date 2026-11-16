#ifndef _rvx_step1_h_
#define _rvx_step1_h_

/* [M50 II] First steps of the recording plan in docs/offline/C9-recommended.md, as runtime
 * toggles.  Everything here is module-only.  NOTHING here has run on the camera.
 *
 * All toggles are plain ints, changed over serial with `wr &<name> <value>`.  They are read
 * when a recording starts (COMMAND_START) unless noted, so flip them while not recording.
 *
 * name                    default  meaning
 * rvx_write_prio          0x1A     priority of the "raw_write" card-write task (0x1A = behaviour
 *                                  before this change).  A11 recommends 0x16.  Read per write.
 * rvx_copy_prio           0        0 = frame copy runs inside the worker at 0x11 (behaviour before
 *                                  this change).  Non-zero = the copy runs in its own "rvx_copy"
 *                                  task at this priority while the worker only waits.  A11
 *                                  recommends 0x15, the plan's step 2 uses 0x1B.  Read per frame.
 * rvx_copy_chunk_kb       0        0 = copy a frame without pause (behaviour before this change).
 *                                  N = msleep(rvx_copy_sleep_ms) after every N KiB copied, so
 *                                  Canon tasks of any priority get CPU.  Plan value: 256.
 * rvx_copy_sleep_ms       10       length of that pause.
 * rvx_burst_frames        0        0 = continuous recording (behaviour before this change).
 *                                  N = capture at most N frames into the two RAM buffers with no
 *                                  card write during capture, then stop by itself and write.
 * rvx_burst_release_first 0        burst only.  0 = write + close the file, then raw_lv_release()
 *                                  (order used so far).  1 = raw_lv_release() first, so Canon's
 *                                  raw job is off while the card is written.
 * rvx_integrity           1        per-frame record in RAM (timestamp, vsync number, copy time,
 *                                  probe hashes of the source before / after the copy and of the
 *                                  copy) plus a summary on UART at stop.  No card I/O.
 * rvx_sidecar             0        1 = write those records to <clip>.RVI after the MLV is closed.
 * rvx_drop_torn           0        1 = a frame whose source changed during the copy is not stored.
 * rvx_fast_copy           0        1 = read the source through the cacheable alias after Canon's
 *                                  d-cache invalidate-by-range (A13), memcpy64 where aligned.
 *                                  Do not enable before A13's hardware check 3 has passed.
 */

/* Canon tasks of the LiveView band run at 0x0D-0x14 (A11).  No task of this module that does bulk
 * work may be created above (numerically below) 0x15, whatever the toggle says. */
#define RVX_PRIO_MIN 0x15
#define RVX_PRIO_MAX 0x1F

#define RVX_COPY_STACK 0x1000

/* Eight probes of 64 bytes each, spread over the copied window from its first to its last row. */
#define RVX_PROBES 8
#define RVX_PROBE_BYTES 64

/* Sidecar file <clip>.RVI: one struct rvx_sidecar_header, then .n_records x struct rvx_frame_rec.
 * Little endian, every field uint32_t.  Reader: tools/mlvcheck/rvi_check.py. */
#define RVX_SIDECAR_MAGIC 0x49585652 /* "RVXI" */
#define RVX_SIDECAR_VERSION 1
#define RVX_MAX_RECS 2000

#define RVX_REC_TORN      1 /* source probes differ before / after the copy */
#define RVX_REC_DST_DIFF  2 /* copy differs from the source as read after the copy */
#define RVX_REC_DROPPED   4 /* not stored in the MLV (rvx_drop_torn) */
#define RVX_REC_FAST_COPY 8 /* copied through the cacheable alias */

struct rvx_frame_rec
{
    uint32_t stored_index; // index among the frames stored in the MLV; 0xFFFFFFFF if dropped
    uint32_t vd_seq;       // vsync number within the session; gaps = frames never copied
    uint32_t t_vsync_us;   // get_us_clock() low 32 bits in the vsync hook
    uint32_t t_copy_us;    // same clock when the copy started
    uint32_t copy_us;      // duration of the copy, pauses included
    uint32_t hash_before;  // source probes, read uncached, before the copy
    uint32_t hash_after;   // source probes, read uncached, after the copy
    uint32_t hash_dst;     // the same probe positions in the copy
    uint32_t flags;        // RVX_REC_*
};

struct rvx_sidecar_header
{
    uint32_t magic, version, header_size, record_size;
    uint32_t n_records;        // records that follow
    uint32_t records_lost;     // frames copied after the record array was full
    uint32_t w, h, x, y;       // recorded window (x, y as copied: 8- and 2-aligned)
    uint32_t vsyncs;           // vsync hooks seen while the session was active
    uint32_t frames_stored;    // frames accepted into the RAM buffers
    uint32_t frames_on_disk;   // frames in the MLV file
    uint32_t dropped_queue, dropped_busy, dropped_nofile, dropped_copy;
    uint32_t torn;             // frames with RVX_REC_TORN
    uint32_t dst_diff;         // frames with RVX_REC_DST_DIFF
    uint32_t copy_prio, write_prio, chunk_kb, sleep_ms;
    uint32_t burst_frames, release_first, fast_copy, drop_torn;
    uint32_t copy_task_fail;
    uint32_t first_t_us, last_t_us; // vsync times of the first and last record
    uint32_t fps_x1000_core;        // what the core reported (MLV header value)
    uint32_t fps_x1000_measured;    // from vd_seq and t_vsync_us of the first / last record
};

struct rvx_copy_job
{
    uint32_t src;       // first byte to copy, uncached address
    uint32_t dst;
    uint32_t src_pitch; // bytes from one source row to the next
    uint32_t line_size; // bytes to copy per row = destination pitch
    uint32_t rows;
};

extern int rvx_write_prio, rvx_copy_prio, rvx_copy_chunk_kb, rvx_copy_sleep_ms;
extern int rvx_burst_frames, rvx_burst_release_first;
extern int rvx_integrity, rvx_sidecar, rvx_drop_torn, rvx_fast_copy;
extern int rvx_torn, rvx_dst_diff, rvx_copy_task_fail;
extern volatile uint32_t rvx_vsyncs; // event_pusher.c
extern void (*rvx_dcache_inval)(uint32_t addr, uint32_t size);

int rvx_step1_init(void);
int rvx_prio_clamp(int prio);

// Session bookkeeping.  All called from the worker task only.
void rvx_session_start(uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint32_t fps_core);
// Copies one frame (inline or in the "rvx_copy" task) and records its integrity data.
// Returns the RVX_REC_* flags of the frame.
uint32_t rvx_copy_frame(const struct rvx_copy_job *job, uint32_t vd_seq, uint32_t t_vsync_us,
                        uint32_t stored_index);
// The frame recorded last was not kept after all.
void rvx_mark_last_dropped(void);
struct rvx_session_counts
{
    uint32_t frames_stored, frames_on_disk;
    uint32_t dropped_queue, dropped_busy, dropped_nofile, dropped_copy;
};
// UART summary; with rvx_sidecar also writes <mlv_name minus extension>.RVI.  The MLV must be
// closed.  mlv_name NULL = no file.
void rvx_session_report(const char *mlv_name, const struct rvx_session_counts *counts);
void rvx_session_end(void);

#endif // _rvx_step1_h_
