#include "module.h"
#include "dryos.h"
#include "raw.h" // raw_info global, raw_lv_redirect_edmac()
#include "zebra.h" // liveview_display_idle()

// Ugly - we use propvalues.c to hold various globals
// and functions that are nothing to do with properties,
// just global state for various camera things,
// including raw video stuff.
// E.g. is_movie_mode(), set_recording_custom(),
// various RECORDING macros.
#include "propvalues.h"

#include "raw_vid.h"
#include "raw_vid_event_pusher.h"
#include "raw_vid_worker.h"
#include "rvx_sync.h"

#if RAW_VID_Q_LEN < 5
    #error "RAW_VID_Q_LEN too short"
#endif

// This is about 56MB/s at 24fps
//#define MLV_3_CROP_WIDTH 1600
//#define MLV_3_CROP_HEIGHT 900

// This is 64.3MB/s at 24fps
#define MLV_3_CROP_WIDTH 1792
#define MLV_3_CROP_HEIGHT 896

// This is 67.8MB/s at 24fps
//#define MLV_3_CROP_WIDTH 1888
//#define MLV_3_CROP_HEIGHT 896

// This is 70.2MB/s at 24fps
//#define MLV_3_CROP_WIDTH 2048
//#define MLV_3_CROP_HEIGHT 856

// This is 73.5MB/s at 24fps
// (and nice round numbers)
//#define MLV_3_CROP_WIDTH 2048
//#define MLV_3_CROP_HEIGHT 896

// Top-left corner of the recorded window within the full raw frame, in pixels.
// The worker starts the copy at x rounded up to a multiple of 8 and y rounded down to
// a multiple of 2 (whole 14-bit groups, and the Bayer row phase).
// TODO allow choosing offset and crop?
#define MLV_3_CROP_X 200
#define MLV_3_CROP_Y 100

/* [M50 II raw_vidx bring-up] Capture-source selector.
 *   1 = read the raw frame directly from the Canon-written buffer UNCACHEABLE(raw_info.buffer)
 *       (single-buffering; proven to hold a real movie-LV frame in docs/35's single-DNG capture).
 *   0 = read from the redirected double buffer `frame` (capture_frames[]), which on D8 the LV EDMAC
 *       redirect does NOT reliably fill (the sensor only writes raw_info.buffer).
 * Default 1. Runtime A/B: `wr &rvx_src_from_raw_info 0`. Accepts single-buffer tearing to get a
 * first decodable clip; double-buffering is a later refinement. */
int rvx_src_from_raw_info = 1;

/* [M50 II] Whether to redirect Canon's LV raw write-EDMAC to our own capture_frames buffer.
 * Default 0 (OFF): with rvx_src_from_raw_info we read raw_info.buffer directly, and redirecting
 * the channel (raw_lv_edmac->ram_addr @0xd0420200) fights Canon's lv_save_raw movie-raw job ->
 * MvRawProDs/RawPro/LvJob null-context + interrupt storm. Runtime A/B: `wr &rvx_redirect_edmac 1`. */
int rvx_redirect_edmac = 0;

/* Frames the producer (vsync CBR) dropped in the current / most recent session because the worker
 * was behind: no free event slot, or the queue rejected the post. Reset when a session captures its
 * first frame; logged by the worker at stop. Read-only telemetry: `rd &rvx_dropped_queue`. */
int rvx_dropped_queue = 0;

/* [M50 II] Recorded window, runtime-selectable.  Defaults = the former compile-time values.
 * Read when REC starts a recording and fixed for that recording.  Width must be a multiple of
 * 8 pixels (whole 14-byte groups), height even; a window that does not fit the raw frame stops
 * the recording on its first vsync.  A13: with x = 200 every source row starts 2 bytes off a
 * word boundary and memcpy runs its byte loop; `wr &rvx_crop_x 192` avoids that.
 * The plan's first clip: `wr &rvx_crop_w 1280`, `wr &rvx_crop_h 720`, `wr &rvx_crop_x 192`. */
int rvx_crop_w = MLV_3_CROP_WIDTH;
int rvx_crop_h = MLV_3_CROP_HEIGHT;
int rvx_crop_x = MLV_3_CROP_X;
int rvx_crop_y = MLV_3_CROP_Y;

/* Vsync hooks seen while the current / most recent session was active, whether or not the
 * frame was queued.  Each IMAGE_DATA event carries the value at its vsync, so frames that were
 * never copied show up as gaps.  Read-only telemetry. */
volatile uint32_t rvx_vsyncs = 0;

// Window of the session in progress, latched from rvx_crop_* by the REC key.
static uint32_t sess_w = MLV_3_CROP_WIDTH, sess_h = MLV_3_CROP_HEIGHT;
static uint32_t sess_x = MLV_3_CROP_X, sess_y = MLV_3_CROP_Y;

static struct msg_queue *event_q = NULL;

// We post these events to the msg queue.  Since we push a pointer,
// we need some storage that will live until the msg has been consumed.
//
// Don't access these events directly, only use get_event() / release_event().
// This handles the sem to avoid races.
// Only the worker receives events; single consumer reduces code complexity.
// The worker releases every event it received.  The producer releases only events it
// took but never posted (first vsync of a session, rejected post), which nobody else
// can hold.
static struct raw_vid_event events[RAW_VID_Q_LEN] = {0};
static struct semaphore *events_sem = NULL;

// Local var to rapidly stop pushing image data to worker.
// Should not be accessed directly.  send_stop_command() uses this.
//
// Should not need a sem, since this side only ever changes it 0 -> 1,
// only worker changes 1 -> 0, via allow_recording().  And worker
// flushes any queued events before making that change.
static int prevent_recording = 1;

// Set by allow_recording(), consumed by the vsync CBR: the next frame it handles
// is the first frame of a new session, whatever its frame counter says.
static int session_reset_pending = 0;

static void send_stop_command(void); // forward declaration

// Returns a raw_vid_event in the PREPARING state,
// or returns NULL if all events are in use.
// That should only happen if the worker cannot service our requests fast enough,
// probably because we can't save to card fast enough.
//
// We use a static array of events, and .type == EMPTY to signify
// if the event can be re-used.  Therefore, caller must never set type
// of the obtained event to EMPTY, as this means it can be reassigned
// in a racey manner.
//
// Caller can modify other parts of the event before posting.
// Caller must never modify any part of an event after posting;
// the receiver is responsible for releasing the event
// (which it will do with release_event()).
struct raw_vid_event *get_empty_event(void)
{
    static size_t last_index = 0;
    struct raw_vid_event *event = NULL;

    int err = take_semaphore(events_sem, 2000);
    if (err)
    {
        SEND_LOG_DATA_STR("ERROR: get_next_event() sem timeout\n");
        task_create("stop_com", WORKER_PRIO + 1, 0x800, send_stop_command, NULL);
        return NULL;
    }

    // This is an optimisation; we test the event after the last one
    // we assigned.  Most of the time usage should be sequential.
    if (last_index < RAW_VID_Q_LEN - 1)
        last_index++;
    else
        last_index = 0;

    if (events[last_index].type == EMPTY)
    {
        event = &events[last_index];
    }
    else
    {
        // simple linear search, it's a small contiguous array
        int i = 0;
        while (i < RAW_VID_Q_LEN)
        {
            if (events[i].type == EMPTY)
            {
                event = &events[i];
                break;
            }
            i++;
        }
    }

    if (event != NULL)
    {
        memset(event, 0, sizeof(*event));
        event->type = PREPARING;
    }

    give_semaphore(events_sem);
    return event;
}

// Allows the provided event to be re-used.
// After calling this, the caller must not use
// the event again.
void release_event(struct raw_vid_event *event)
{
    // Here we only zero the type.  This allows get_event() to re-use the event,
    // and that func zeroes the complete event.
    // This removes a source of race conditions due to ARM weak memory model.
    event->type = EMPTY;
}

// This should only be called by worker.c,
// when it wishes to allow this file to start a new recording.
// The worker calls it at init and after it has handled each COMMAND_STOP.
void allow_recording(void)
{
    session_reset_pending = 1;
    prevent_recording = 0;
}

void block_recording(void)
{
    prevent_recording = 1;
}

void init_event_pusher(struct msg_queue *q)
{
    // Not much init, most of the work is done by the CBR funcs.
    event_q = q;
    events_sem = create_named_semaphore(NULL, SEM_CREATE_UNLOCKED);
}

// Send stop command, handling the possiblity that the queue
// may be full.
//
// Also sets the flag to immediately disallow sending image data
// from this file to the queue.  This allows the queue to empty,
// should it be full.
//
// Probably works if called directly, but it's expected you'd
// use task_create().  That allows this function to be slow as it
// attempts retries, etc, without blocking whatever context you
// trigger it from.  We need to handle problems encountered in
// timing critical CBR contexts.
static void send_stop_command(void)
{
    prevent_recording = 1;
    SEND_LOG_DATA_STR("Sending stop command to worker\n");

    const int max_tries = 10;
    int tries = 0;
    struct raw_vid_event *event = get_empty_event();
    while (event == NULL && tries < max_tries)
    { // should happen only if the queue is completely full,
      // we must wait and hope the worker can process some events.
        msleep(50); // more time than one frame at 24 fps,
                    // probably enough we only wait once.
        event = get_empty_event();
        tries++;
    }
    if (tries == max_tries)
    {
        SEND_LOG_DATA_STR("ERROR: could not get event for stop command\n");
        return;
    }

    event->creation_time = get_ms_clock();
    event->type = COMMAND_STOP;

    tries = 0;
    int err = msg_queue_post(event_q, (uint32_t)event);
    while (err && tries < max_tries)
    {
        msleep(50);
        // FIXME: use msg_queue_post_with_timeout()?
        err = msg_queue_post(event_q, (uint32_t)event);
        tries++;
    }
    if (tries == max_tries)
    {
        SEND_LOG_DATA_STR("ERROR: could not post stop command event\n");
    }
}

// Here we judge if the keypress should start or stop recording.
// We also suppress some events during recording.
//
// This is a slightly simplified copy of the logic in mlv_lite.c
//
// We must return 1 if we are not suppressing the event;
// that is, if we want to give other code a chance to handle the event.
static unsigned int raw_vid_keypress_cbr(unsigned int key)
{
    SEND_LOG_DATA_STR("keypress_cbr\n");
    RVX_HEX("key", key);
    if (!is_movie_mode()) // in propvalues.c
    {
        SEND_LOG_DATA_STR("bail: not movie mode\n");
        return 1;
    }

    // [M50 II debug] idle-guard RELAXED: the serial handle_module_keys trigger must fire even when
    // the ML console overlay makes liveview_display_idle() false. Log it, but do NOT bail.
    if (!liveview_display_idle())
        SEND_LOG_DATA_STR("warn: LV not idle (guard relaxed)\n");

    // if you somehow managed to start recording H.264, let it stop
    if (RECORDING_H264)
    {
        SEND_LOG_DATA_STR("bail: RECORDING_H264\n");
        return 1;
    }

    // Fetch recording state when rec key pressed.
    // Theoretically this can change before we use it,
    // but the window is small and the worst case outcome is
    // not a real problem; we either start or stop recording inappropriately.
    enum recording_state recording_state = get_recording_state();

    // block the zoom key while recording
    if (recording_state == INACTIVE
        && key == MODULE_KEY_PRESS_ZOOMIN)
        return 0;

    // start/stop recording with the LiveView key
    int rec_key_pressed = (key == MODULE_KEY_LV || key == MODULE_KEY_REC);

    if (rec_key_pressed)
    {
        SEND_LOG_DATA_STR("REC key pressed.\n");

        if (event_q == NULL)
        {
            // not initialized; block the event
            return 0;
        }

        if (recording_state == ACTIVE)
        {
            // We wish to stop recording.
            SEND_LOG_DATA_STR("Sending stop com due to rec keypress.\n");
            // task_create to avoid blocking in key handling cbr:
            task_create("stop_com", WORKER_PRIO + 1, 0x800, send_stop_command, NULL);
        }
        else if (recording_state == INACTIVE)
        {
            // Trying to start recording.
            struct raw_vid_event *event = get_empty_event();
            if (event != NULL)
            {
                event->creation_time = get_ms_clock();
                event->type = COMMAND_START;
                // FIXME we should take these from menu config when we have that
                if (rvx_crop_w < 64 || rvx_crop_h < 16 || rvx_crop_x < 0 || rvx_crop_y < 0
                    || (rvx_crop_w & 7) || (rvx_crop_h & 1))
                {
                    SEND_LOG_DATA_STR("ERROR: rvx_crop_w/h/x/y not usable (w multiple of 8, h even), not starting\n");
                    release_event(event);
                    return 0;
                }
                sess_w = (uint32_t)rvx_crop_w;
                sess_h = (uint32_t)rvx_crop_h;
                sess_x = (uint32_t)rvx_crop_x;
                sess_y = (uint32_t)rvx_crop_y;
                event->w = sess_w;
                event->h = sess_h;
                // so the worker can put the window position into the MLV frame headers
                event->x = sess_x;
                event->y = sess_y;
                msg_queue_post(event_q, (uint32_t)event);
                SEND_LOG_DATA_STR("posted COMMAND_START\n");
            }
            else
            {
                SEND_LOG_DATA_STR("ERROR: no empty event for START\n");
            }
            // If we were trying to start recording and there was an earlier
            // error, we won't try to send the start signal.  Which is okay.
        }

        // In other states, we ignore the button
        return 0;
    }

    return 1;
}

// src/raw.c makes this fire every LV vsync (which I guess is tied to every sensor capture?).
// In any case, it works to get 60fps captures on 200D.
// [M50 II] The per-frame work.  Called from the core's vsync callback (rvx_sync_src = 0, in
// Canon's EvfDev task) or from the "rvx_sync" task (sources 1 and 2, see rvx_sync.c).
unsigned int FAST rvx_frame_tick(void)
{
    static int count = 0; // frame count, reset every recording session
    struct raw_vid_event *event = NULL;

    // The cam will capture sensor data for display on LV.
    // Without needing to modify *what* the cam captures (which is hard),
    // we can change *where* the data goes.  This happens to be easy
    // because of how EDMAC works (and which parts we understand well).
    //
    // We don't know precisely when the EDMAC capture will finish, only
    // that it is in the future.  But we do know it must finish before
    // the next vsync, or LV would hang.
    //
    // Therefore we double buffer capture, changing where cam saves the
    // data every vsync. That means we know the older captured frame has
    // completed capture.
    //
    // We message worker.c with the details of this captured frame.
    // It optionally crops out a region of interest, and saves to disk.

    static char *capture_frames[2] = {NULL};
    static char *frame = NULL; // current capture_frame in use

    if (get_recording_state() != ACTIVE || prevent_recording)
    {
        // Either no recording, or the consumer still thinks recording is active but
        // something on this, the producer side, has requested stop (the recording has
        // very recently stopped; consumer will re-enable recording when it is ready).
        // Reset session.
        //
        // The buffers used to be freed only in the second case.  If the worker went
        // INACTIVE between two vsyncs that case was never seen, and both capture
        // buffers (about 2 x 4.3 MB) stayed allocated until the next recording.
        count = 0;
        if (capture_frames[0] != NULL)
        {
            fio_free(capture_frames[0]);
            capture_frames[0] = NULL;
        }
        if (capture_frames[1] != NULL)
        {
            fio_free(capture_frames[1]);
            capture_frames[1] = NULL;
        }
        return 0;
    }

    if (session_reset_pending)
    {
        // Normally the branch above has already reset count while the worker was
        // busy stopping.  If the worker finished the stop between two vsyncs we
        // never saw that state, so do not rely on it.
        session_reset_pending = 0;
        count = 0;
    }

    // In worker.c we request LV changes to raw mode, which takes some time.
    // When it occurs, raw_info global struct gets updated with the raw dimensions.
    if (raw_info.width == 0
        || raw_info.height == 0)
    {
        return 0;
    }

    count++;
    if (count == 1)
    { // per recording-session setup
        SEND_LOG_DATA_STR("Video starting.\n");
        rvx_dropped_queue = 0;
        rvx_vsyncs = 0;

        // The window must lie inside the raw frame, or the worker would copy from beyond
        // Canon's buffer.  (x is rounded up to 8, as the worker does.)
        if (((sess_x + 7) & ~7u) + sess_w > (uint32_t)raw_info.width
            || sess_y + sess_h > (uint32_t)raw_info.height)
        {
            SEND_LOG_DATA_STR("ERROR: recording window does not fit the raw frame, stopping\n");
            prevent_recording = 1; // at once; the stop task below only runs later
            task_create("stop_com", WORKER_PRIO + 1, 0x800, send_stop_command, NULL);
            return 0;
        }
    }
    rvx_vsyncs++;

    // [M50 II] The two full-frame capture buffers (2 x 4.3 MB) are only used when frames are
    // not read from raw_info.buffer or when the EDMAC redirect is on.  With the defaults
    // neither is the case, so do not take 8.5 MB out of the pool Canon's LiveView shares (A8),
    // and do not call the allocator from the vsync hook, when nothing will use the memory.
    if (count == 1 && (!rvx_src_from_raw_info || rvx_redirect_edmac))
    {

        // We lie a little to increase height, we want to slightly over-allocate.
        // Old code does this, not quite sure why, perhaps DMA block size related?
        const uint32_t lv_raw_size = raw_info.width * (raw_info.height + 2) * BPP/8;
        // Free leftovers of a session whose stop we never observed (see above),
        // so they are not leaked and the size matches the current raw_info.
        if (capture_frames[0] != NULL)
        {
            fio_free(capture_frames[0]);
            capture_frames[0] = NULL;
        }
        if (capture_frames[1] != NULL)
        {
            fio_free(capture_frames[1]);
            capture_frames[1] = NULL;
        }
        capture_frames[0] = fio_malloc(lv_raw_size);

        if (capture_frames[0] == NULL)
        {
            SEND_LOG_DATA_STR("Could not alloc capture_frames[0]\n");

            // This is likely unrecoverable, stop recording
            task_create("stop_com", WORKER_PRIO + 1, 0x800, send_stop_command, NULL);
            return 0;
        }
        capture_frames[1] = fio_malloc(lv_raw_size);
        if (capture_frames[1] == NULL)
        {
            SEND_LOG_DATA_STR("Could not alloc capture_frames[1]\n");
            fio_free(capture_frames[0]);
            capture_frames[0] = NULL;

            // This is likely unrecoverable, stop recording
            task_create("stop_com", WORKER_PRIO + 1, 0x800, send_stop_command, NULL);
            return 0;
        }

        frame = capture_frames[0];
    }
    else if (count == 1)
    {
        frame = NULL; // nothing allocated; a pointer left from an earlier session is stale
    }

    event = get_empty_event();
    if (event == NULL)
    {
        // [M50 II] Queue full = the consumer (worker) can't keep up with LV fps -- the uncacheable
        // sync crop-copy is slow (~2.8MB/frame). DO NOT stop here: on HW that made the stop cascade
        // uncleanly -- with the queue full, COMMAND_STOP couldn't be posted, so the file never got a
        // clean close (frames lost), and raw_lv_release's EvfDevState flood then wedged the cam.
        // Instead DROP this frame (classic MLV's "skip frames"): the worker catches up, the queue
        // drains, and the record continues at a lower effective fps until a clean stop is posted.
        if ((rvx_dropped_queue & 0x3F) == 0)   // rate-limited log (~every 64th drop)
            SEND_LOG_DATA_STR("frame dropped (queue full, consumer behind)\n");
        rvx_dropped_queue++;
        return 0;
    }

    event->creation_time = get_ms_clock();
    event->type = IMAGE_DATA;

    event->orig_frame_width = raw_info.pitch;

    event->x = sess_x; // x offset within the frame, for cropping
    event->y = sess_y; // y as above
    event->w = sess_w;
    event->h = sess_h;
    event->vd_seq = rvx_vsyncs;
    event->t_us = (uint32_t)get_us_clock();

    int32_t frame_size = event->w * event->h * BPP/8;
    event->size = frame_size;

    // Send prior captured data to worker for cropping
    // and writing.
    // [M50 II] Prefer the Canon-written buffer directly (single-buffering); the D8 LV EDMAC
    // redirect does not reliably fill the `frame` double buffer. See rvx_src_from_raw_info.
    if (rvx_src_from_raw_info)
        event->src = (char *)UNCACHEABLE(raw_info.buffer);
    else if (frame != NULL)
        event->src = frame;

    // [M50 II raw_vidx bring-up] geometry + capture-buffer sample, first few frames only.
    {
        static int ep_log_n = 0;
        if (ep_log_n < 3)
        {
            RVX_HEX("rawinfo.w",     raw_info.width);
            RVX_HEX("rawinfo.h",     raw_info.height);
            RVX_HEX("rawinfo.pitch", raw_info.pitch);
            RVX_HEX("rawinfo.buf",   (uint32_t)raw_info.buffer);
            RVX_HEX("ev.src",        (uint32_t)event->src);
            if (event->src != NULL)
            {
                volatile uint32_t *sp = (volatile uint32_t *)event->src;
                RVX_HEX("src[0]", sp[0]);
                RVX_HEX("src[1]", sp[1]);
            }
            ep_log_n++;
        }
    }

    // Swap buffer in use
    if (frame == capture_frames[0])
    {
        frame = capture_frames[1];
    }
    else
    {
        frame = capture_frames[0];
    }

    // Make the next LV EDMAC capture go to updated buffer.
    // [M50 II] DISABLED by default: with Change C we read raw_info.buffer directly, so we do NOT
    // need the double buffer. Worse, raw_lv_redirect_edmac() writes raw_lv_edmac->ram_addr at
    // 0xd0420200 -- the very channel Canon's lv_save_raw movie-raw job manages -- which hijacks it
    // out from under Canon and triggers the intermittent Engine::MvRawProDs/RawPro/LvJob null-context
    // + [kern] interrupt STORM that wedges the camera (HW: boots 203/209). Leaving Canon's channel
    // alone lets its raw job run and keep filling raw_info.buffer, which is what we read.
    // Runtime A/B: `wr &rvx_redirect_edmac 1`.
    if (rvx_redirect_edmac && frame != NULL)
        raw_lv_redirect_edmac(frame);

    // Don't send request on first vsync, we've set up for EDMAC capture
    // but it hasn't had time to run.
    if (count > 1)
    {
        int err = msg_queue_post(event_q, (uint32_t)event);
        if (err)
        {
            // The queue rejected the post.  Same situation as "no empty event" above
            // (consumer behind), and the same policy: drop this frame, count it, keep
            // recording.  The event was never queued, so the worker will not release
            // it; we must, or the slot leaks.
            release_event(event);
            if ((rvx_dropped_queue & 0x3F) == 0)
                SEND_LOG_DATA_STR("frame dropped (queue post failed)\n");
            rvx_dropped_queue++;
        }
    }
    else
    {
        // The event taken above is deliberately not posted on the first vsync.
        // Nobody else will ever see it, so we must hand the slot back ourselves,
        // or it stays PREPARING forever and the pool shrinks by one per session.
        release_event(event);
    }

//ASSERT(frame_size_uncompressed % 4 == 0);

    return 0;
}

/* Core vsync callbacks ignored because the current recording runs on sync source 1 or 2.
 * Compared with rvx_sync_ticks this gives the rate of both sources from one recording. */
int rvx_sync_core_ignored = 0;

static unsigned int FAST raw_vid_vsync_cbr(unsigned int unused)
{
    if (rvx_sync_active)
    {
        // A module-installed source drives this recording; two sources must not both feed it.
        rvx_sync_core_ignored++;
        return 0;
    }
    return rvx_frame_tick();
}

void rvx_request_stop(void)
{
    task_create("stop_com", WORKER_PRIO + 1, 0x800, send_stop_command, NULL);
}

MODULE_CBRS_START()
    MODULE_CBR(CBR_VSYNC, raw_vid_vsync_cbr, 0)
    MODULE_CBR(CBR_KEYPRESS, raw_vid_keypress_cbr, 0)
MODULE_CBRS_END()
