#include "module.h"
#include "dryos.h"

#include "raw.h"
#include "edmac-memcpy.h"
#include "fps.h"

#include "raw_vid.h"
#include "raw_vid_worker.h"
#include "raw_vid_event_pusher.h"
#include "mlv_3.h"
#include "rvx_step1.h"

static struct msg_queue *event_q = NULL;
static FILE *raw_vid_fp = NULL;

// ensures we never attempt an edmac copy while the last is in progress
static struct semaphore *edmac_sem = NULL;

// Serialises use of raw_vid_fp between the worker and the "raw_write" task.
// The worker takes it before handing a full buffer to a "raw_write" task; that task
// gives it back when its FIO_WriteFile() has returned and the buffer is marked empty.
// So whoever holds it knows no card write is in flight, which is what COMMAND_STOP
// needs before it flushes the remaining data and closes the file.
static struct semaphore *write_sem = NULL;

// Frames whose complete VIDF block has been written to the file in this session.
// Only changed by whoever holds write_sem.  This, not the number of frames
// accepted into the buffers, is what goes into the MLV file header.
static uint32_t frames_on_disk = 0;

// Bytes of frame data known to be in the file after the headers (same rule as above).
static uint64_t bytes_on_disk = 0;

// Set by the worker while it handles COMMAND_STOP (and left set if that stop wedged).
static volatile int stopping = 0;

// A card write of this session came back short (card full, I/O error), or a checkpoint
// could not put the file back into a writable state.  Appending anything after that
// would leave a truncated block in the middle of the file, so from then on nothing more
// is written: frames are discarded until the user stops, and the stop only rewrites the
// header and closes.  Set by whoever holds write_sem; reset on COMMAND_START.
// Read-only telemetry: `rd &rvx_write_failed`.
volatile int rvx_write_failed = 0;

// A "raw_write" task was still running when COMMAND_STOP gave up waiting for it.  The
// file handle and both crop buffers are then deliberately left alone (that task is
// still using them) and no further recording is started in this boot.
// Read-only telemetry: `rd &rvx_wedged`.
volatile int rvx_wedged = 0;

// Per-session limiters for messages that would otherwise repeat at frame rate.
static struct
{
    int fp_null, crop_buf_null, edmac_sem, geometry, checkpoint, cbr_w, flush_wait, task_fail;
} log_n;

// MLV file name and in-memory copy of the file headers for the current session.
// Written by the worker in COMMAND_START (before any "raw_write" task exists);
// after that only touched by whoever holds write_sem.
static char mlv_name[20] = {0};
static char *header_buf = NULL;
static uint32_t header_size = 0; // MLV lib returns real size here

/* [M50 II] Crash-safe checkpoints. After each full buffer has been written, rewrite the header
 * with the number of frames now in the file, CLOSE the file and reopen it for append.
 * Why: on this camera a recording can wedge (assert storm / EvfDevState flood) before
 * COMMAND_STOP runs. FIO_CloseFile is what commits the directory entry and FAT chain; the first
 * clip that reached the card on HW (boot 212, 13.4 MB written) vanished completely after the
 * power-cycle because the file was never closed. With checkpoints a wedge costs at most the
 * frames since the last full buffer.
 * Cost: seek + header write + close + open + seek after EVERY buffer (5 frames at 1792x896, so
 * about 5 times a second), on a card path that hardware logs show is already at its limit, and
 * the rest of the clip goes through an O_RDWR|O_SYNC handle instead of the FIO_CreateFile one.
 * None of this has run on the camera.
 * Default 0 (OFF): the file is closed once, at stop. Runtime A/B: `wr &rvx_checkpoint 1`
 * (takes effect from the next full buffer; safe to flip while recording). */
int rvx_checkpoint = 0;

static enum recording_state recording_state = INACTIVE;

// [M50 II] Burst mode (rvx_burst_frames, see rvx_step1.h): capture into RAM only, write after.
// Worker-only state.  burst_target is latched on COMMAND_START; 0 = continuous recording.
static uint32_t burst_target = 0;
// The burst is complete (target reached or both RAM buffers full): frames still in the queue
// are ignored and the worker runs its own stop.
static int burst_done = 0;
static int self_stop = 0;
static struct raw_vid_event self_stop_event;

/* [M50 II] Poll Canon's async raw-job "ready" flags after raw_lv_request() before consuming frames,
 * to avoid the intermittent MvRawProDs/RawPro/LvJob ring-overflow assert STORM. Default 1 (on).
 * Runtime A/B: `wr <resolved-addr> 0`. */
int rvx_wait_raw_job = 1;

/* Frames the worker dropped in the current / most recent session because both crop buffers were
 * busy (the card write of the other buffer had not finished when this one filled up).
 * Reset on COMMAND_START; logged at stop. Read-only telemetry: `rd &rvx_dropped_busy`. */
int rvx_dropped_busy = 0;

/* Frames discarded in the current / most recent session because the file could not take them:
 * after a failed card write (rvx_write_failed) or with no open file. Reset on COMMAND_START;
 * logged at stop. Read-only telemetry: `rd &rvx_dropped_nofile`. */
int rvx_dropped_nofile = 0;

/* Frames dropped in the current / most recent session because the copy of the previous frame
 * still held edmac_sem. Cannot happen with the synchronous CPU copy; kept for the day the copy
 * goes asynchronous again. Reset on COMMAND_START; logged at stop. */
int rvx_dropped_copy = 0;

static struct mlv_session session = {
    .fps = 0,
    .res_x = 0,
    .res_y = 0,
    .crop_offset_x = 0, // filled in on COMMAND_START from the requested window
    .crop_offset_y = 0,
    .pan_offset_x = 0,
    .pan_offset_y = 0,
    .image_data_alignment = EDMAC_ALIGNMENT
};

enum recording_state get_recording_state(void)
{
    return recording_state;
}

// lv_save_raw trick (which we trigger indirectly via raw.c, raw_lv_request(),
// has given us a buffer of whatever size DryOS wants for fullsize LV.  We can
// crop this down with edmac_copy_rectangle_cbr_start(), DMA based and fast enough
// to do each frame.
struct crop_buf 
{
    char *_buf; // Unaligned, underlying buffer, used with malloc/free.
    char *start; // Start of aligned buf, for DMA ops, etc.
    // cur == start is how the worker recognises a buffer as free, and it tests that
    // without a lock while a "raw_write" task may be resetting the buffer.  That is only
    // correct if the task stores frames = 0 BEFORE cur = start.  Both are volatile so the
    // compiler keeps the two stores in source order.
    char * volatile cur; // Position that data should write to.
    char *end; // Do not write to this location or later.
    volatile uint32_t frames; // Number of complete VIDF blocks between start and cur.
};

// Finds two large, aligned blocks, of equal size,
// for accumulating potentially cropped frame data in,
// before writing to disk.
static void alloc_crop_bufs(struct crop_buf crop_bufs[])
{
    if (crop_bufs[0]._buf != NULL
        || crop_bufs[1]._buf != NULL)
    {
        SEND_LOG_DATA_STR("ERROR: non-null _buf in alloc_crop_bufs()\n");
        return;
    }
    
    uint32_t buf_size = INITIAL_CROP_BUF_SIZE + CROP_BUF_ALIGN;
    char *buf0 = NULL;
    char *buf1 = NULL;
    while (buf_size > MIN_CROP_BUF_SIZE)
    {
        buf0 = fio_malloc(buf_size);
        if (buf0 != NULL)
        {
            buf1 = fio_malloc(buf_size);
            if (buf1 != NULL)
            {
                break;
            }
            else
            {
                fio_free(buf0);
                buf0 = NULL;
            }
        }

        buf_size -= CROP_BUF_STEP_SIZE;
    }

    if (buf_size < MIN_CROP_BUF_SIZE)
    {
        SEND_LOG_DATA_STR("Couldn't find good crop bufs\n");
        // These should both already be null, but it's a big memory leak
        // if we get it wrong!
        if (buf0 != NULL)
        {
            SEND_LOG_DATA_STR("ERROR: buf0 unexpectedly NULL\n");
            fio_free(buf0);
            buf0 = NULL;
        }
        if (buf1 != NULL)
        {
            SEND_LOG_DATA_STR("ERROR: buf1 unexpectedly NULL\n");
            fio_free(buf1);
            buf1 = NULL;
        }
        return;
    }

    // Here we have two large enough bufs.

    crop_bufs[0]._buf = buf0;
    crop_bufs[1]._buf = buf1;

    if ((uint32_t)buf0 % CROP_BUF_ALIGN != 0)
    {
        crop_bufs[0].start = buf0 + (CROP_BUF_ALIGN - (uint32_t)buf0 % CROP_BUF_ALIGN);
    }
    else
    {
        crop_bufs[0].start = buf0;
    }
    if ((uint32_t)buf1 % CROP_BUF_ALIGN != 0)
    {
        crop_bufs[1].start = buf1 + (CROP_BUF_ALIGN - (uint32_t)buf1 % CROP_BUF_ALIGN);
    }
    else
    {
        crop_bufs[1].start = buf1;
    }
    // Do we want to include the unaligned end portion in the above sizes?
    // I'm assuming not, it's a small amount, and would rarely contain data
    // before we write out.

    // We use these for DMA ops, which prefer / require
    // the uncacheable address.
    crop_bufs[0].start = UNCACHEABLE(crop_bufs[0].start);
    crop_bufs[1].start = UNCACHEABLE(crop_bufs[1].start);

    // The allocation is buf_size bytes and start may have been moved up by as much as
    // CROP_BUF_ALIGN - 1 to align it, so only buf_size - CROP_BUF_ALIGN bytes are
    // guaranteed to follow start.  This used to be start + buf_size, which put end up to
    // CROP_BUF_ALIGN - 1 bytes past the allocation.
    const uint32_t usable_size = buf_size - CROP_BUF_ALIGN;
    crop_bufs[0].cur = crop_bufs[0].start;
    crop_bufs[0].end = crop_bufs[0].start + usable_size;
    crop_bufs[0].frames = 0;
    crop_bufs[1].cur = crop_bufs[1].start;
    crop_bufs[1].end = crop_bufs[1].start + usable_size;
    crop_bufs[1].frames = 0;
}

// Returns both crop buffers to the allocator and clears the structs,
// so alloc_crop_bufs() can be used again.
static void free_crop_bufs(struct crop_buf crop_bufs[])
{
    for (int i = 0; i < 2; i++)
    {
        if (crop_bufs[i]._buf != NULL)
            fio_free(crop_bufs[i]._buf);
        crop_bufs[i]._buf = NULL;
        crop_bufs[i].start = NULL;
        crop_bufs[i].cur = NULL;
        crop_bufs[i].end = NULL;
        crop_bufs[i].frames = 0;
    }
}

// Old DryOS used to trigger this, new DryOS
// seems to only trigger the write complete cbr.
static void edmac_cbr_r(void *unused_ctx)
{
}

// Triggers when EDMAC write operation is complete.
static void edmac_cbr_w(void *unused_ctx)
{
    edmac_copy_rectangle_adv_cleanup();
    // [M50 II] confirm completion fires; rate-limited so it doesn't flood at frame rate.
    if (log_n.cbr_w < 5) { SEND_LOG_DATA_STR("cbr_w: give sem\n"); log_n.cbr_w++; }
    give_semaphore(edmac_sem); // signal edmac copy rect as safe to use
}

// Writes the used part of crop_buf to the file, then marks the buffer empty.
// Synchronous.  Caller must hold write_sem.
static void write_buf(struct crop_buf *crop_buf)
{
    #ifdef RAW_VID_LOG
    uint32_t buf_size = 100;
    char log_buf[buf_size];
    #endif

    if (crop_buf == NULL)
    {
        SEND_LOG_DATA_STR("ERROR: buf struct for disk write was NULL\n");
        return;
    }

    if (crop_buf->_buf == NULL
        || crop_buf->start == NULL
        || crop_buf->cur == NULL
        || crop_buf->end == NULL)
    {
        SEND_LOG_DATA_STR("ERROR: some buf member for disk write was NULL\n");
        return;
    }

    // COMMAND_STOP cannot close this handle under us: it takes write_sem,
    // which our caller holds, before touching the file.
    if (raw_vid_fp == NULL)
    {
        SEND_LOG_DATA_STR("ERROR: raw_vid_fp was NULL before write\n");
        return;
    }

    uint32_t size = crop_buf->cur - crop_buf->start;
    if (size == 0)
        return;

    if (rvx_write_failed)
    {
        // An earlier write of this session failed part-way.  Do not append after the
        // truncated block; give the buffer back empty.  frames first, see struct crop_buf.
        crop_buf->frames = 0;
        crop_buf->cur = crop_buf->start;
        return;
    }

    #ifdef RAW_VID_LOG
    uint32_t start_time = get_ms_clock();
    #endif
    SEND_LOG_DATA_STR("worker: writing crop_buf -> card\n");
    RVX_HEX("wbytes", size);
    int written = FIO_WriteFile(raw_vid_fp, crop_buf->start, size);
    SEND_LOG_DATA_STR("worker: FIO_WriteFile done\n");
    if (written == (int)size)
    {
        frames_on_disk += crop_buf->frames;
        bytes_on_disk += size;
    }
    else
    {
        // Card full or write error.  These frames are not counted, so the header
        // frame count stays within what is really in the file.  Part of this buffer may
        // be in the file; nothing more is appended after it (rvx_write_failed), so the
        // damage is a cut-off block at the very end, which readers treat like a crash.
        rvx_write_failed = 1;
        SEND_LOG_DATA_STR("ERROR: short card write, frames in this buffer lost, no more data will be written; stop the recording\n");
        RVX_HEX("written", written);
    }
    #ifdef RAW_VID_LOG
    uint32_t end_time = get_ms_clock();
    uint32_t data_rate_x100 = (uint32_t)((float)size / 1024 / 1024
                              * 1000 * 100 / (end_time - start_time));

// FIXME this sometimes lags out and we stall.  I can think of two obvious things to try.
// One: stop creating this func in a task every flush, keep it resident and use sems.
// Two: use more buffers, this averages out the required write data rate.

    snprintf(log_buf, buf_size, "Buf flush data rate, MB/s: %d.%02d\n",
             data_rate_x100 / 100,
             data_rate_x100 % 100);
    send_log_data_str(log_buf);
    #endif

    // reset buf so it's good to use again.  frames first, see struct crop_buf.
    crop_buf->frames = 0;
    crop_buf->cur = crop_buf->start;
}

// Makes the file on the card complete and self-consistent as of now: header with the
// current frame count, closed, then reopened with the position at the end so recording
// carries on.  Caller must hold write_sem.  Only used with rvx_checkpoint = 1.
//
// Every step is checked.  If the file cannot be brought back to "open, positioned at
// its end", it is left closed (what is on the card is then a valid, finalised file up
// to this point) and the session stops writing (rvx_write_failed).
static void checkpoint_file(void)
{
    if (raw_vid_fp == NULL || header_buf == NULL || header_size == 0 || rvx_write_failed)
        return;

    if (update_video_frame_count(frames_on_disk, header_buf, header_buf + header_size))
        return;
    FIO_SeekSkipFile(raw_vid_fp, 0, SEEK_SET);
    int header_written = FIO_WriteFile(raw_vid_fp, header_buf, header_size);
    FIO_CloseFile(raw_vid_fp);
    if (header_written != (int)header_size)
    {
        SEND_LOG_DATA_STR("ERROR: checkpoint could not rewrite the header, recording cannot continue\n");
        raw_vid_fp = NULL;
        rvx_write_failed = 1;
        return;
    }

    // Same way src/fio-ml.c FIO_CreateFileOrAppend() reopens a file for appending.
    // We must not fall back to FIO_CreateFile() as that does: it would truncate.
    // raw_vid_fp is only reassigned once the new handle is ready; nobody else may
    // use it meanwhile, as we hold write_sem.
    FILE *fp = FIO_OpenFile(mlv_name, O_RDWR | O_SYNC);
    if (fp == NULL)
    {
        // What is on the card is a valid file; we just cannot add to it.
        SEND_LOG_DATA_STR("ERROR: checkpoint could not reopen mlv, recording cannot continue\n");
        raw_vid_fp = NULL;
        rvx_write_failed = 1;
        return;
    }
    // The next buffer is written wherever this seek leaves the position.  If that is not
    // the end of the file, it would overwrite the header and the start of the clip, so
    // do not trust it: compare with what we know we have written.
    int64_t pos = FIO_SeekSkipFile(fp, 0, SEEK_END);
    if (pos != (int64_t)(header_size + bytes_on_disk))
    {
        SEND_LOG_DATA_STR("ERROR: checkpoint seek-to-end landed elsewhere, recording cannot continue\n");
        RVX_HEX("seek pos", (uint32_t)pos);
        RVX_HEX("expected", (uint32_t)(header_size + bytes_on_disk));
        FIO_CloseFile(fp);
        raw_vid_fp = NULL;
        rvx_write_failed = 1;
        return;
    }
    raw_vid_fp = fp;

    if (log_n.checkpoint < 3) { RVX_HEX("checkpoint, frames on disk", frames_on_disk); log_n.checkpoint++; }
}

// Entry point of the "raw_write" task.  The worker took write_sem on our behalf
// before creating the task; we give it back when the write is finished.
static void write_buf_task(struct crop_buf *crop_buf)
{
    write_buf(crop_buf);
    // Not while stopping: COMMAND_STOP finalises and closes right after this anyway.
    if (rvx_checkpoint && recording_state == ACTIVE && !stopping)
        checkpoint_file();
    give_semaphore(write_sem);
}

// DryOS task_create() returns (handle << 1) on success and a small odd error code
// (1, 3 or 5 in the M50 II 1.0.1 ROM, 0xE05F40BA) when the task could not be created.
static int task_create_failed(uint32_t ret)
{
    return (ret & 1) != 0;
}

// Checkpoints add FIO_OpenFile (a path buffer on the stack) and Canon's open / close
// call depth to the write task, which the 0x800 stack has never carried on hardware.
static uint32_t write_task_stack(void)
{
    return rvx_checkpoint ? WORKER_WRITE_STACK_CHECKPOINT : WORKER_WRITE_STACK;
}

// Hands a full buffer to a new "raw_write" task.
// wait_ms: how long to wait for the previous "raw_write" task to give write_sem back;
// 0 means do not wait at all.
// Returns 0 if the task now owns write_sem and the buffer.  Otherwise nothing has
// changed (write_sem is not held) and the caller must try again later.
static int start_flush(struct crop_buf *crop_buf, int wait_ms)
{
    int err = (wait_ms > 0) ? take_semaphore(write_sem, wait_ms)
                            : take_semaphore_now(write_sem);
    if (err)
        return 1;

    uint32_t ret = task_create("raw_write", rvx_prio_clamp(rvx_write_prio), write_task_stack(),
                               write_buf_task, crop_buf);
    if (task_create_failed(ret))
    {
        // Nobody will give write_sem back for us.
        give_semaphore(write_sem);
        if (log_n.task_fail < 5) { SEND_LOG_DATA_STR("ERROR: could not create raw_write task, will retry\n"); log_n.task_fail++; }
        return 2;
    }
    return 0;
}

// For COMMAND_STOP: write out whatever is in crop_buf and wait for it.
// The write runs in a "raw_write" task like every other buffer flush, so large card
// writes always happen at WORKER_WRITE_PRIO (see raw_vid_worker.h for why that matters).
// Caller must hold write_sem.  Returns 0 with write_sem held again; non-zero if the
// write did not finish in time, in which case write_sem is NOT held.
static int flush_buf_and_wait(struct crop_buf *crop_buf)
{
    if (crop_buf->start == NULL || crop_buf->cur == crop_buf->start)
        return 0; // nothing to write

    uint32_t ret = task_create("raw_write", rvx_prio_clamp(rvx_write_prio), write_task_stack(),
                               write_buf_task, crop_buf);
    if (task_create_failed(ret))
    {
        // Last resort: write from the worker itself, at the worker's priority.  We
        // still hold write_sem, which is all write_buf() needs.
        SEND_LOG_DATA_STR("ERROR: could not create raw_write task at stop, writing from the worker\n");
        write_buf(crop_buf);
        return 0;
    }
    return take_semaphore(write_sem, STOP_WRITE_WAIT_MS);
}

// This is the heart of worker.c.
//
// This runs as a task whenever the module is loaded,
// regardless of whether a recording is in progress.
//
// It is the only consumer of the message queue. 
static void worker(void)
{
    #ifdef RAW_VID_LOG
    uint32_t buf_size = 100;
    char log_buf[buf_size];
    uint32_t start_time = 0;
    uint32_t session_data_size = 0;
    #endif

    struct crop_buf crop_bufs[2] = {NULL};
    struct crop_buf *crop_buf = NULL;
    // A full buffer whose "raw_write" task could not be started yet.  It is always the
    // buffer we are NOT filling.  Retried on every following frame, and written by
    // COMMAND_STOP at the latest.
    struct crop_buf *pending_flush = NULL;
    uint32_t video_frame_count = 0;

    // space for MLV file header
    const uint32_t header_buf_size = 1024; // wants to be big enough for combined headers,
                                           // this file shouldn't know their size, it's okay to just guess.
    header_buf = malloc(header_buf_size);
    if (header_buf == NULL)
    {
        SEND_LOG_DATA_STR("ERROR: couldn't malloc for header_buf\n");
        return; // worker task dies; raw video recording becomes impossible
    }

    TASK_LOOP
    {
        int err = -1;
        struct raw_vid_event *event = NULL;
        if (self_stop)
        {
            // Burst complete: handle a stop of our own before looking at the queue again.
            // Frames already queued are discarded afterwards (recording is INACTIVE by then).
            self_stop = 0;
            memset(&self_stop_event, 0, sizeof(self_stop_event));
            self_stop_event.type = COMMAND_STOP;
            event = &self_stop_event;
        }
        else
        {
            msg_queue_receive(event_q, &event, 0);
        }

        if (event == NULL)
        { // shouldn't happen, we only post valid events
            SEND_LOG_DATA_STR("ERROR: worker, event was NULL\n");
            continue;
        }

        switch (event->type)
        {
        case IMAGE_DATA:
            if (recording_state != ACTIVE)
            {
                // After a burst stopped itself, frames queued before the stop arrive here.
                if (!burst_done)
                    SEND_LOG_DATA_STR("ERROR: trying to process frame while recording not ACTIVE\n");
                break;
            }

            if (burst_done)
                break; // burst complete, stop is on its way

            if (event->src == NULL)
            {
                SEND_LOG_DATA_STR("ERROR: event->src was NULL\n");
                break;
            }

            if (raw_vid_fp == NULL || rvx_write_failed)
            {
                // Nothing more can go into this file (see rvx_write_failed).  Discard and
                // count until the user stops.  Only NULL-ness of raw_vid_fp is tested
                // here; the handle itself is never used without write_sem.
                // Rate-limited: this repeats for every frame.
                if (log_n.fp_null < 5) { SEND_LOG_DATA_STR("ERROR: file not writable, frame discarded\n"); log_n.fp_null++; }
                rvx_dropped_nofile++;
                break;
            }

            #ifdef RAW_VID_LOG
            snprintf(log_buf, buf_size, "Image prod time, cons time, size: %d, %d, %d\n",
                     event->creation_time, get_ms_clock(), event->size);
            send_log_data_str(log_buf);
            #endif

            if (crop_buf == NULL)
            {
                // FIXME we should stop here.
                // [M50 II] Rate-limit: this used to flood the UART and make serial unusable.
                if (log_n.crop_buf_null < 5) { SEND_LOG_DATA_STR("ERROR: crop_buf was NULL at dst check\n"); log_n.crop_buf_null++; }
                break;
            }

            // A full buffer is still waiting for its write task: try again, without
            // waiting.  (This is what was missing when one failed attempt left the
            // buffer full for the rest of the recording.)
            // (Not in burst mode: there the full buffer stays in RAM until the stop.)
            if (burst_target == 0 && pending_flush != NULL && start_flush(pending_flush, 0) == 0)
                pending_flush = NULL;

            if (event->size > crop_buf->end - crop_buf->start)
            {
                // For simplicity, alloc_crop_bufs() ensures the bufs are equal size.
                // Thus if we hit this, we can't ever save the data, there is no point
                // swapping bufs.  We must give up.
                // FIXME we don't yet handle stopping except via user pressing Rec 2nd time
                // (when added we need to audit for other places).
                SEND_LOG_DATA_STR("ERROR: event size too large for empty crop buf\n");
                break;
            }

            // Here we have src data, and an output file.
            // We want to do a (potentially cropped) copy from
            // src to dst.  We accumulate frames into a larger buffer
            // before triggering flush to disk.

            // Make sure the copy of the previous frame is finished BEFORE anything is
            // written into the buffer or counted.  This check used to come after the
            // VIDF header was written and the frame counted, so a failure left a counted
            // frame with stale image data in the file and lost the pending buffer flush.
            err = take_semaphore_now(edmac_sem);
            if (err)
            {
                // If this happens, it means we're not managing to save a frame
                // before the next vsync event in event_pusher.c tries to capture one.
                // We drop the frame rather than stop recording.
                // [M50 II] Rate-limited (was flooding the UART).
                if (log_n.edmac_sem < 5) { SEND_LOG_DATA_STR("ERROR: worker couldn't take edmac_sem, frame dropped\n"); log_n.edmac_sem++; }
                rvx_dropped_copy++;
                break;
            }
            // From here on every path must give edmac_sem back: the copy at the end of
            // this case does (edmac_cbr_w), the early exits below do it themselves.

            char *dst = NULL;
            struct crop_buf *flush_src = NULL;
            // write the header of a vidf block, which returns us an aligned
            // location to write the image data.
            dst = write_video_frame_header(event->size, crop_buf->cur, crop_buf->end);
            if (dst == NULL)
            {
                // The prior IMAGE_DATA was the last that fit in this buffer,
                // we must swap.  And we can trigger flush to disk of prior in use buffer.
                #ifdef RAW_VID_LOG
                snprintf(log_buf, buf_size, "Swapping bufs, time: %d\n", get_ms_clock());
                send_log_data_str(log_buf);
                #endif
                
                flush_src = crop_buf; // this triggers later flush to disk

                if (crop_buf == &crop_bufs[0])
                    crop_buf = &crop_bufs[1];
                else
                    crop_buf = &crop_bufs[0];

                if (crop_buf->cur != crop_buf->start && burst_target != 0)
                {
                    // Burst mode: both RAM buffers are full, so the burst ends here, short
                    // of its target.  Stay on the newer buffer so the stop writes the two
                    // in capture order.
                    crop_buf = flush_src;
                    SEND_LOG_DATA_STR("burst: RAM full before the frame target, stopping\n");
                    burst_done = 1;
                    self_stop = 1;
                    block_recording();
                    give_semaphore(edmac_sem);
                    break;
                }
                if (crop_buf->cur != crop_buf->start)
                {
                    // The other buffer is not free, so there is nowhere to put this
                    // frame.  Drop it and count it, and stay on the full buffer: nothing
                    // was written to it for this frame, so a later frame simply retries
                    // the swap, and the flush of the full buffer is triggered then.
                    //
                    // This used to set crop_buf = NULL, which silently discarded every
                    // following frame until the user stopped, and lost the full buffer.
                    //
                    // Usually the other buffer is being written to card (its write task
                    // resets cur when done).  But it may also be full with no write task
                    // at all, because starting one failed earlier.  Then nothing would
                    // ever empty it and every frame would end up here; so start it now,
                    // this time waiting a little so a lower-priority task holding
                    // write_sem gets to run.
                    if (pending_flush != NULL
                        && start_flush(pending_flush, FLUSH_WRITE_WAIT_MS) == 0)
                    {
                        pending_flush = NULL;
                    }
                    crop_buf = flush_src;
                    if ((rvx_dropped_busy & 0x3F) == 0) // rate-limited, ~every 64th drop
                        SEND_LOG_DATA_STR("frame dropped (both crop bufs busy, card write behind)\n");
                    rvx_dropped_busy++;
                    give_semaphore(edmac_sem);
                    break;
                }

                dst = write_video_frame_header(event->size, crop_buf->cur, crop_buf->end);
                if (dst == NULL)
                {
                    // Does not fit an empty buffer either.  Stay on the full one, so its
                    // frames are still written at stop in the right order.
                    SEND_LOG_DATA_STR("ERROR: couldn't fit image in either buffer\n");
                    crop_buf = flush_src;
                    give_semaphore(edmac_sem);
                    break;
                }
            }
            #ifdef RAW_VID_LOG
            session_data_size += (dst - crop_buf->cur + event->size);
            #endif
            // The frame is accepted.  The image data is copied below, before the worker
            // looks at another event.
            char *block_start = crop_buf->cur; // where this frame's VIDF header begins
            crop_buf->cur = dst + event->size;
            crop_buf->frames++;
            video_frame_count++;

            // We hold edmac_sem, so the copy rect from the prior IMAGE_DATA has completed
            // and the buffer that just filled up can safely go to the card.
            if (flush_src != NULL)
            {
                // The previous "raw_write" task, if any, has already emptied its buffer
                // (or we would not have been able to swap to it).  Without checkpoints
                // it is a few instructions away from giving write_sem back; with
                // rvx_checkpoint = 1 it may be inside checkpoint_file() for much longer.
                // So wait only briefly, and if that is not enough keep the buffer as
                // pending: the next frames retry, and COMMAND_STOP writes it otherwise.
                pending_flush = flush_src;
                if (burst_target != 0)
                {
                    // burst mode: no card I/O during capture, the stop writes it
                }
                else if (start_flush(pending_flush, FLUSH_WRITE_WAIT_MS) == 0)
                    pending_flush = NULL;
                else if (log_n.flush_wait < 5)
                {
                    SEND_LOG_DATA_STR("WARN: could not start card write for full buffer yet, will retry\n");
                    log_n.flush_wait++;
                }
            }

            int line_size = event->w * BPP/8; // width in bytes
            #ifdef RAW_VID_LOG
            snprintf(log_buf, buf_size, "w, h, sz: %d, %d, %d\n",
                     event->w, event->h, line_size * event->h);
            send_log_data_str(log_buf);
            #endif

            // FIXME:
            // We should stop using edmac_copy() directly.
            // - Not all cams have it.
            // - It has alignment requirements we can hide by having a small
            //   unaligned copy with normal memcpy at the start,
            //   simplifying MLV file format (and removing need to align buffers!).
            // - This high-level code shouldn't care *how* copies occur,
            //   only that they do.
            //
            // The extra cost for doing a memcpy() to align should be tiny,
            // alignment requirement is 0x10 on Digic 5.

            // [M50 II raw_vidx bring-up] dispatch geometry, first few frames only.
            {
                if (log_n.geometry < 3)
                {
                    RVX_HEX("wk.dst",        (uint32_t)dst);
                    RVX_HEX("wk.cbuf.start", (uint32_t)crop_buf->start);
                    RVX_HEX("wk.cbuf.end",   (uint32_t)crop_buf->end);
                    RVX_HEX("wk.src",        (uint32_t)event->src);
                    RVX_HEX("wk.pitch",      raw_info.pitch);
                    RVX_HEX("wk.line_size",  line_size);
                    RVX_HEX("wk.h",          event->h);
                    log_n.geometry++;
                }
            }

            // [M50 II raw_vidx] CPU crop-copy, synchronous from the worker's point of view.
            // The async D8 cpu_copy worker behind edmac_copy_rectangle_cbr_start does NOT reliably
            // fire its completion CBR on this cam, and the mem2mem EDMAC engine storms (DAF/EIS),
            // so rows are copied with the CPU from UNCACHEABLE(raw_info.buffer) (Change C).
            // Where it runs (this task, or an "rvx_copy" task at rvx_copy_prio), whether it
            // pauses (rvx_copy_chunk_kb) and how it reads the source (rvx_fast_copy) is decided
            // in rvx_step1.c; with every toggle at its default this is the same per-row memcpy
            // in this task as before.  edmac_sem is released via edmac_cbr_w() when done.
            {
                struct rvx_copy_job job = {
                    .src = (uint32_t)event->src
                         + (uint32_t)((event->x + 7)/8*BPP)                  // src_x (bytes)
                         + (uint32_t)((event->y/2*2)) * raw_info.pitch,      // src_y * pitch
                    .dst = (uint32_t)dst,
                    .src_pitch = raw_info.pitch,
                    .line_size = (uint32_t)line_size,
                    .rows = event->h,
                };
                uint32_t flags = rvx_copy_frame(&job, event->vd_seq, event->t_us,
                                                video_frame_count - 1);
                if ((flags & RVX_REC_TORN) && rvx_drop_torn)
                {
                    // Canon wrote into the source while we copied: do not keep the frame.
                    // Its VIDF block is the last thing in the buffer, so taking it back is
                    // just moving cur.  (The MLV frame number it used stays unused.)
                    crop_buf->cur = block_start;
                    crop_buf->frames--;
                    video_frame_count--;
                    rvx_mark_last_dropped();
                }
                edmac_cbr_w(NULL); // adv_cleanup no-op on D8 + give_semaphore(edmac_sem)
            }

            if (burst_target != 0 && video_frame_count >= burst_target)
            {
                SEND_LOG_DATA_STR("burst: frame target reached, stopping\n");
                burst_done = 1;
                self_stop = 1;
                block_recording(); // the vsync hook stops queueing frames at once
            }

            break;

        case COMMAND_START:
            SEND_LOG_DATA_STR("Worker got start request.\n");
            if (event->w == 0 || event->h == 0)
            {
                SEND_LOG_DATA_STR("ERROR: start request with 0 area\n");
                break;
            }

            if (rvx_wedged)
            {
                // See COMMAND_STOP: the previous recording's file handle and buffers
                // are still owned by a card write that never came back.
                SEND_LOG_DATA_STR("ERROR: a card write of an earlier recording never finished, cannot record again in this boot\n");
                break;
            }

            if (recording_state == INACTIVE)
            {
                // Get mem to buffer our cropped frames.  We do this because 
                // card write speeds are improved if we pass only large, size aligned
                // buffers to FIO_WriteFile().  Optimal size and alignment probably
                // varies by cam, card (manufacturer, controller, etc), card format etc.
                //
                // We double buffer so we can record into one as we write out the other.
                // At this point, we don't know the required size as no image data has
                // been sent.
                if (crop_buf != NULL)
                {
                    SEND_LOG_DATA_STR("ERROR: crop_buf not null on COMMAND_START\n");
                    break;
                }
                alloc_crop_bufs(crop_bufs);
                if (crop_bufs[0].start == NULL
                    || crop_bufs[1].start == NULL)
                {
                    SEND_LOG_DATA_STR("ERROR: could not obtain crop bufs, cannot start\n");
                    break;
                }
                #ifdef RAW_VID_LOG
                snprintf(log_buf, buf_size, "buf[0] and buf[1] size: %d, %d\n",
                         crop_bufs[0].end - crop_bufs[0].cur,
                         crop_bufs[1].end - crop_bufs[1].cur);
                send_log_data_str(log_buf);
                #endif
                crop_buf = &crop_bufs[0];
                pending_flush = NULL;
                rvx_dropped_busy = 0;
                rvx_dropped_nofile = 0;
                rvx_dropped_copy = 0;
                rvx_write_failed = 0;
                memset(&log_n, 0, sizeof(log_n));
                // Per-session counters.  video_frame_count used to be reset only when the
                // worker task started, so the header of a second recording also counted
                // the frames of the first.
                video_frame_count = 0;
                frames_on_disk = 0;
                bytes_on_disk = 0;
                burst_target = rvx_burst_frames > 0 ? (uint32_t)rvx_burst_frames : 0;
                burst_done = 0;
                self_stop = 0;

                SEND_LOG_DATA_STR("worker: COMMAND_START -> raw_lv_request\n");
                // Set LV to raw
                raw_lv_request();
                SEND_LOG_DATA_STR("worker: raw_lv_request done\n");

                // [M50 II] Wait for Canon's ASYNC raw-job build before consuming frames.
                // raw_lv_request -> lv_save_raw(1) only sets a FLAG; Canon then builds the movie-raw
                // job (MvRawProDs Create -> RawPro connect -> drive start) asynchronously, a VARIABLE
                // number of VDs later. If our vsync CBR starts pulling frames before that build+drain
                // is up, Canon's 2-slot per-VD ring overflows -> Engine::MvRawProDs.c:293 /
                // Engine::RawPro.c:452 / LiveViewCore::LvJob.c:213 nullptr assert STORM (intermittent
                // ~50%, wedges the record). RE'd via Ghidra: MvRawProDs ctx = *(0xE07B9ED4), its
                // "created" flag at ctx+0x30 -> 1 when built; RawPro ctx = *(0xE0761240), RawPath
                // connection handle at ctx+0x30 -> nonzero when connected. Poll both (bounded) BEFORE
                // going ACTIVE. NOTE: wait_lv_frames(2) BEFORE lv_save_raw (the mk1 recipe) is the
                // WRONG gate -- the race is AFTER lv_save_raw. Runtime A/B: `wr &rvx_wait_raw_job 0`.
                if (rvx_wait_raw_job)
                {
                    volatile uint32_t *mvraw_pp  = (volatile uint32_t *)0xE07B9ED4;
                    volatile uint32_t *rawpro_pp = (volatile uint32_t *)0xE0761240;
                    int ready = 0;
                    for (int i = 0; i < 75; i++)   // up to ~75 * 20ms = 1.5s
                    {
                        uint32_t m = *mvraw_pp;
                        uint32_t p = *rawpro_pp;
                        if (m != 0 && *(volatile uint32_t *)(m + 0x30) == 1 &&
                            p != 0 && *(volatile uint32_t *)(p + 0x30) != 0)
                        { ready = 1; break; }
                        msleep(20);
                    }
                    if (ready)
                    {
                        SEND_LOG_DATA_STR("worker: raw job READY (MvRawProDs created)\n");
                        msleep(50); // let the drain run a couple VDs before we consume
                    }
                    else
                    {
                        SEND_LOG_DATA_STR("worker: WARN raw job not ready (timeout) -- proceeding\n");
                    }
                }

                // [M50 II] No EDMAC setup is needed here: frames are read straight from
                // raw_info.buffer and cropped with a CPU copy in IMAGE_DATA above, so this module
                // touches no EDMAC channel or completion-CBR table (and the core at this revision
                // has no CBR "seed" helper to call).

                // Get new save file, start the MLV session, write the file headers.
                //
                // By this point we hold two large crop buffers and one raw_lv_request().
                // If anything below fails we must give all of that back: recording_state
                // stays INACTIVE, so COMMAND_STOP would never do it for us, and a leftover
                // non-NULL crop_buf makes every later COMMAND_START bail out.
                const char *start_err = NULL;
                int file_opened = 0;
                int mlv_started = 0;

                if (raw_vid_fp != NULL)
                {
                    start_err = "ERROR: Worker tried to init save file but already in use\n";
                }
                else if (get_numbered_file_name("tst%02d.mlv", 99, mlv_name, sizeof(mlv_name)) < 0)
                {
                    start_err = "ERROR: Worker couldn't get mlv file name\n";
                }
                else if ((raw_vid_fp = FIO_CreateFile(mlv_name)) == NULL)
                {
                    start_err = "Worker couldn't create mlv file\n";
                }
                else
                {
                    file_opened = 1;

                    session.fps = fps_get_current_x1000();
                    session.res_x = event->w;
                    session.res_y = event->h;
                    // Where the recorded window starts in the full raw frame, for the
                    // VIDF blocks (focus pixel maps and the like need it).  Same
                    // convention as mlv_lite: crop = the 8x2-aligned position the copy
                    // really starts at (must match the src arithmetic in IMAGE_DATA),
                    // pan = the position that was asked for.
                    session.crop_offset_x = (event->x + 7) & ~7;
                    session.crop_offset_y = event->y & ~1;
                    session.pan_offset_x = event->x;
                    session.pan_offset_y = event->y;

                    if (start_mlv_session(&session))
                    {
                        start_err = "ERROR: failed to start mlv session\n";
                    }
                    else
                    {
                        mlv_started = 1;

                        // write standard header blocks
                        header_size = write_file_headers(header_buf, header_buf + header_buf_size);
                        if (header_size == 0)
                            start_err = "ERROR: failed to write mlv headers\n";
                        else if (FIO_WriteFile(raw_vid_fp, header_buf, header_size) != (int)header_size)
                            start_err = "ERROR: short write of mlv headers to card\n";
                    }
                }

                if (start_err != NULL)
                {
                    SEND_LOG_DATA_STR(start_err);
                    SEND_LOG_DATA_STR("worker: start failed, releasing file, raw mode and buffers\n");
                    if (file_opened)
                    {
                        // The (empty or header-only) file is closed, not deleted.
                        FIO_CloseFile(raw_vid_fp);
                        raw_vid_fp = NULL;
                    }
                    if (mlv_started)
                        stop_mlv_session();
                    raw_lv_release(); // pairs with the raw_lv_request() above
                    crop_buf = NULL;
                    free_crop_bufs(crop_bufs);
                    break;
                }

                #ifdef RAW_VID_LOG
                send_log_data_str("Recording is active\n");
                start_time = get_ms_clock();
                #endif
                rvx_session_start(session.res_x, session.res_y, session.crop_offset_x,
                                  session.crop_offset_y, session.fps);
                if (burst_target != 0)
                    RVX_HEX("burst mode, frame target", burst_target);
                recording_state = ACTIVE;
            }
            break;

        case COMMAND_STOP:
            SEND_LOG_DATA_STR("Worker got stop request.\n");
            // FIXME how to stop is more complicated than this,
            // need to flush, reset some pointers, block new start
            // until finished.
            if (recording_state == ACTIVE)
            {
                // [M50 II] Finalize + CLOSE the MLV FIRST, before raw_lv_release().
                // raw_lv_release() can fault on the D8 raw-LV teardown (LiveViewDrive::EvfDevState
                // nullptr); closing first means the frames already written are committed to a
                // valid, decodable file even if the teardown then wedges.

                // Step 1: wait for a card write that is in flight (or handed to a "raw_write"
                // task that has not run yet).  Without this the file was closed under the
                // write, and the buffer was freed below while FIO_WriteFile() still read it.
                stopping = 1;

                // [M50 II] Burst option rvx_burst_release_first: everything captured is in
                // RAM, so raw LiveView can be given back to Canon BEFORE the card is touched;
                // the write then runs with Canon's raw job off.  The price: if this release
                // faults (the EvfDevState teardown problem), nothing has reached the card yet.
                // Default 0 keeps the order used so far (close first, release last).
                int released_early = 0;
                if (burst_target != 0 && rvx_burst_release_first)
                {
                    SEND_LOG_DATA_STR("burst: raw_lv_release before the card write\n");
                    raw_lv_release();
                    released_early = 1;
                }

                int have_write_sem = (take_semaphore(write_sem, STOP_WRITE_WAIT_MS) == 0);

                // Step 2: write out what is still in the buffers.  Frames are accepted into
                // a buffer long before it fills up and is flushed, so without this up to a
                // whole buffer of frames (and always the last, partly filled one) never
                // reached the file.  Older data first: the buffer we are not filling can
                // only hold data if its flush could not be started.
                if (have_write_sem && raw_vid_fp != NULL)
                {
                    struct crop_buf *older = (crop_buf == &crop_bufs[0]) ? &crop_bufs[1]
                                                                         : &crop_bufs[0];
                    struct crop_buf *newer = (older == &crop_bufs[0]) ? &crop_bufs[1]
                                                                      : &crop_bufs[0];
                    if (flush_buf_and_wait(older) || flush_buf_and_wait(newer))
                        have_write_sem = 0;
                }
                // Step 3: finalise the header and close.
                if (!have_write_sem)
                {
                    // A "raw_write" task is still inside FIO_WriteFile() (or a checkpoint)
                    // on this handle, reading one of the crop buffers.  Closing the file
                    // or freeing the buffers under it is exactly what must not happen.
                    // So leave both alone, for good: the handle and about 32 MB stay
                    // allocated until reboot, and no further recording is started
                    // (rvx_wedged).  The header on the card keeps the count it had
                    // (0, or the last checkpoint), i.e. the file looks like a crash.
                    // 'stopping' stays set so the late task does not start a checkpoint.
                    rvx_wedged = 1;
                    SEND_LOG_DATA_STR("ERROR: card write did not finish at stop; file left open, buffers kept, reboot before recording again\n");
                }
                else
                {
                    if (raw_vid_fp != NULL)
                    {
                        // The count is the number of frames known to be in the file, not
                        // the number accepted, so the header never promises more frames
                        // than a reader will find.
                        err = update_video_frame_count(frames_on_disk, header_buf, header_buf + header_size);
                        if (err)
                            SEND_LOG_DATA_STR("ERROR: couldn't update video frame count (closing anyway)\n");
                        // write back the header (now with the final frame count); file is complete
                        FIO_SeekSkipFile(raw_vid_fp, 0, SEEK_SET);
                        if (FIO_WriteFile(raw_vid_fp, header_buf, header_size) != (int)header_size)
                            SEND_LOG_DATA_STR("ERROR: short write of final mlv header\n");
                        FIO_CloseFile(raw_vid_fp);
                        raw_vid_fp = NULL;
                        SEND_LOG_DATA_STR("worker: MLV closed (finalized)\n");
                    }
                    else
                    {
                        // A checkpoint already closed the file and could not reopen it.
                        SEND_LOG_DATA_STR("worker: MLV was already closed by a failed checkpoint\n");
                    }
                    RVX_HEX("frames on disk", frames_on_disk);

                    // Integrity summary, and with rvx_sidecar = 1 the per-frame records as a
                    // second file.  Only here: the MLV is closed and no write task is running.
                    extern int rvx_dropped_queue; // event_pusher.c
                    struct rvx_session_counts counts = {
                        .frames_stored = video_frame_count,
                        .frames_on_disk = frames_on_disk,
                        .dropped_queue = (uint32_t)rvx_dropped_queue,
                        .dropped_busy = (uint32_t)rvx_dropped_busy,
                        .dropped_nofile = (uint32_t)rvx_dropped_nofile,
                        .dropped_copy = (uint32_t)rvx_dropped_copy,
                    };
                    rvx_session_report(mlv_name, &counts);

                    give_semaphore(write_sem);
                    stopping = 0;
                }
                rvx_session_end();

                // restore LV away from raw. This ML asserts if not paired with raw_lv_request(),
                // but is safe; on M50 II it may fault (EvfDevState) -- the file is already closed.
                if (!released_early)
                    raw_lv_release();
                // TODO mlv_lite does restore_bit_depth(), we don't mess with this yet.

                #ifdef RAW_VID_LOG
                snprintf(log_buf, buf_size, "session time, data written: %d, %d\n",
                         get_ms_clock() - start_time, session_data_size);
                send_log_data_str(log_buf);
                session_data_size = 0;
                #endif

                // free crop buffers (not if a write task may still be reading them)
                crop_buf = NULL;
                pending_flush = NULL;
                if (!rvx_wedged)
                    free_crop_bufs(crop_bufs);

                RVX_HEX("frames accepted", video_frame_count);
                {
                    extern int rvx_dropped_queue; // event_pusher.c
                    RVX_HEX("dropped (queue full)", rvx_dropped_queue);
                }
                RVX_HEX("dropped (bufs busy)", rvx_dropped_busy);
                if (rvx_dropped_nofile)
                    RVX_HEX("discarded (file not writable)", rvx_dropped_nofile);
                if (rvx_dropped_copy)
                    RVX_HEX("dropped (copy busy)", rvx_dropped_copy);

                stop_mlv_session();
                recording_state = INACTIVE;
                SEND_LOG_DATA_STR("Recording is inactive\n");
            }
            // Sending a stop command sets prevent_recording in event_pusher.c, and only
            // we may clear it.  Nothing did, so after the first stop the vsync CBR
            // returned early for the rest of the boot: every later recording started,
            // captured no frames and produced a header-only file.
            // By now every IMAGE_DATA event queued ahead of this stop has been consumed.
            // Done for every stop command, also one that arrived while not recording.
            allow_recording();
            break;

        case EMPTY:
        case PREPARING:
            // These types should never be sent.  We will need to fix the cause.
            // But we still want to release the event storage.
            SEND_LOG_DATA_STR("ERROR: unexpected event type\n");
            break;
        }

        // Note that we must reach here from every case above,
        // or we will leak events and starve the queue.
        release_event(event);

        // no sleep needed, queue receive with timeout 0 is blocking.
    }

    free(header_buf);
    SEND_LOG_DATA_STR("Raw video worker stopping.\n");
}

void init_worker(struct msg_queue *q)
{
    event_q = q;
    edmac_sem = create_named_semaphore("edmac_raw_worker", SEM_CREATE_UNLOCKED);
    if (edmac_sem == NULL)
    {
        SEND_LOG_DATA_STR("ERROR: worker could not create edmac sem\n");
        return;
    }
    write_sem = create_named_semaphore("raw_vid_write", SEM_CREATE_UNLOCKED);
    if (write_sem == NULL)
    {
        SEND_LOG_DATA_STR("ERROR: worker could not create write sem\n");
        return;
    }
    if (rvx_step1_init())
        SEND_LOG_DATA_STR("WARN: no copy-task semaphore, rvx_copy_prio will be ignored\n");
    task_create("raw_v_worker", WORKER_PRIO, 0x1000, worker, NULL);
    allow_recording();
}
