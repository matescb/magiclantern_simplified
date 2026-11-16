#include "module.h"
#include "dryos.h"

#include "raw.h"
#include "edmac-memcpy.h"
#include "fps.h"

#include "raw_vid.h"
#include "raw_vid_worker.h"
#include "raw_vid_event_pusher.h"
#include "mlv_3.h"

static struct msg_queue *event_q = NULL;
static FILE *raw_vid_fp = NULL;

// ensures we never attempt an edmac copy while the last is in progress
static struct semaphore *edmac_sem = NULL;

static enum recording_state recording_state = INACTIVE;

/* [M50 II] Poll Canon's async raw-job "ready" flags after raw_lv_request() before consuming frames,
 * to avoid the intermittent MvRawProDs/RawPro/LvJob ring-overflow assert STORM. Default 1 (on).
 * Runtime A/B: `wr <resolved-addr> 0`. */
int rvx_wait_raw_job = 1;

/* Frames the worker dropped in the current / most recent session because both crop buffers were
 * busy (the card write of the other buffer had not finished when this one filled up).
 * Reset on COMMAND_START; logged at stop. Read-only telemetry: `rd &rvx_dropped_busy`. */
int rvx_dropped_busy = 0;

static struct mlv_session session = {
    .fps = 0,
    .res_x = 0,
    .res_y = 0,
    .crop_offset_x = 0, // FIXME we want these correct so mlvapp can do FPM stuff, etc
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
    char *cur; // Position that data should write to.
    char *end; // Do not write to this location or later.
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

    crop_bufs[0].cur = crop_bufs[0].start;
    crop_bufs[0].end = crop_bufs[0].start + buf_size;
    crop_bufs[1].cur = crop_bufs[1].start;
    crop_bufs[1].end = crop_bufs[1].start + buf_size;
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
    static int cbrw_n = 0;
    if (cbrw_n < 5) { SEND_LOG_DATA_STR("cbr_w: give sem\n"); cbrw_n++; }
    give_semaphore(edmac_sem); // signal edmac copy rect as safe to use
}

static void write_buf_to_disk(struct crop_buf *crop_buf)
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
    
    // FIXME we probably want a sem to remove the potential
    // for COMMAND_STOP to close this handle before we start
    // the write.
    if (raw_vid_fp == NULL)
    {
        SEND_LOG_DATA_STR("ERROR: raw_vid_fp was NULL before write\n");
        return;
    }

    #ifdef RAW_VID_LOG
    uint32_t start_time = get_ms_clock();
    #endif
    SEND_LOG_DATA_STR("worker: writing crop_buf -> card\n");
    RVX_HEX("wbytes", (uint32_t)(crop_buf->cur - crop_buf->start));
    FIO_WriteFile(raw_vid_fp, crop_buf->start, crop_buf->cur - crop_buf->start);
    SEND_LOG_DATA_STR("worker: FIO_WriteFile done\n");
    #ifdef RAW_VID_LOG
    uint32_t end_time = get_ms_clock();
    uint32_t data_rate_x100 = (uint32_t)((float)(crop_buf->cur - crop_buf->start) / 1024 / 1024
                              * 1000 * 100 / (end_time - start_time));

// FIXME this sometimes lags out and we stall.  I can think of two obvious things to try.
// One: stop creating this func in a task every flush, keep it resident and use sems.
// Two: use more buffers, this averages out the required write data rate.

    snprintf(log_buf, buf_size, "Buf flush data rate, MB/s: %d.%02d\n",
             data_rate_x100 / 100,
             data_rate_x100 % 100);
    send_log_data_str(log_buf);
    #endif

    // reset buf so it's good to use again
    crop_buf->cur = crop_buf->start;
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

    char mlv_name[20] = {0};

    struct crop_buf crop_bufs[2] = {NULL};
    struct crop_buf *crop_buf = NULL;
    uint32_t video_frame_count = 0;

    char *header_buf = NULL; // space for MLV file header
    const uint32_t header_buf_size = 1024; // wants to be big enough for combined headers,
                                           // this file shouldn't know their size, it's okay to just guess.
    uint32_t header_size = 0; // MLV lib returns real size here
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
        msg_queue_receive(event_q, &event, 0);

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
                SEND_LOG_DATA_STR("ERROR: trying to process frame while recording not ACTIVE\n");
                break;
            }

            if (event->src == NULL)
            {
                SEND_LOG_DATA_STR("ERROR: event->src was NULL\n");
                break;
            }

            if (raw_vid_fp == NULL)
            {
                SEND_LOG_DATA_STR("ERROR: fp was NULL at attempt to write\n");
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
                static int cbnull_n = 0;
                if (cbnull_n < 5) { SEND_LOG_DATA_STR("ERROR: crop_buf was NULL at dst check\n"); cbnull_n++; }
                break;
            }

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

                if (crop_buf->cur != crop_buf->start)
                {
                    // The other buffer is still being written to card (the write task
                    // resets cur when it is done), so there is nowhere to put this frame.
                    // Drop it and count it, and stay on the full buffer: nothing was
                    // written to it for this frame, so a later frame simply retries the
                    // swap, and the flush of the full buffer is triggered then.
                    //
                    // This used to set crop_buf = NULL, which silently discarded every
                    // following frame until the user stopped, and lost the full buffer.
                    crop_buf = flush_src;
                    if ((rvx_dropped_busy & 0x3F) == 0) // rate-limited, ~every 64th drop
                        SEND_LOG_DATA_STR("frame dropped (both crop bufs busy, card write behind)\n");
                    rvx_dropped_busy++;
                    break;
                }

                dst = write_video_frame_header(event->size, crop_buf->cur, crop_buf->end);
                if (dst == NULL)
                {
                    SEND_LOG_DATA_STR("ERROR: couldn't fit image in either buffer\n");
                    break;
                }
            }
            #ifdef RAW_VID_LOG
            session_data_size += (dst - crop_buf->cur + event->size);
            #endif
            crop_buf->cur = dst + event->size; // cur after image data, which is not yet written
            video_frame_count++;

            err = take_semaphore_now(edmac_sem);
            if (err)
            {
                // If this happens, it means we're not managing to save a frame
                // before the next vsync event in event_pusher.c tries to capture one.

                // [M50 II] Rate-limited (was flooding the UART).
                static int sem_err_n = 0;
                if (sem_err_n < 5) { SEND_LOG_DATA_STR("ERROR: worker couldn't take edmac_sem\n"); sem_err_n++; }
                // FIXME this break means we're choosing to drop frames
                // rather than stop recording, I think just stopping is better.
                // Maybe re-imp the option from mlv_lite.
                break;
            }

            // here we know the copy rect from the prior IMAGE_DATA has completed,
            // as we have edmac_sem, so we can safely flush if one of the buffers was full.
            if (flush_src != NULL)
            {
                task_create("raw_write", WORKER_WRITE_PRIO, 0x800,
                            write_buf_to_disk, flush_src);
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
                static int wk_log_n = 0;
                if (wk_log_n < 3)
                {
                    RVX_HEX("wk.dst",        (uint32_t)dst);
                    RVX_HEX("wk.cbuf.start", (uint32_t)crop_buf->start);
                    RVX_HEX("wk.cbuf.end",   (uint32_t)crop_buf->end);
                    RVX_HEX("wk.src",        (uint32_t)event->src);
                    RVX_HEX("wk.pitch",      raw_info.pitch);
                    RVX_HEX("wk.line_size",  line_size);
                    RVX_HEX("wk.h",          event->h);
                    wk_log_n++;
                }
            }

            // [M50 II raw_vidx] SYNCHRONOUS inline CPU crop-copy.
            // The async D8 cpu_copy worker behind edmac_copy_rectangle_cbr_start does NOT reliably
            // fire its completion CBR on this cam, so edmac_sem never returns and every frame drops
            // (the "couldn't take edmac_sem" -> "crop_buf NULL" flood). We run in the dedicated
            // worker task (off the capture/vsync path), so a plain per-row memcpy here is safe and
            // deterministic; release edmac_sem directly via edmac_cbr_w() when done. This also
            // avoids the mem2mem EDMAC engine (DAF/EIS storm) and the core-side sync change (which
            // shifts symbols and destabilises boot). src = UNCACHEABLE(raw_info.buffer) (Change C).
            {
                uint32_t sp    = raw_info.pitch;                                   // src stride
                uint32_t dp    = (uint32_t)line_size;                              // dst stride
                uint32_t sbase = (uint32_t)event->src
                               + (uint32_t)((event->x + 7)/8*BPP)                  // src_x (bytes)
                               + (uint32_t)((event->y/2*2)) * sp;                  // src_y * pitch
                uint32_t dbase = (uint32_t)dst;
                for (int row = 0; row < (int)event->h; row++)
                    memcpy((void*)(dbase + (uint32_t)row * dp),
                           (void*)(sbase + (uint32_t)row * sp),
                           (uint32_t)line_size);
                edmac_cbr_w(NULL); // adv_cleanup no-op on D8 + give_semaphore(edmac_sem)
            }

            break;

        case COMMAND_START:
            SEND_LOG_DATA_STR("Worker got start request.\n");
            if (event->w == 0 || event->h == 0)
            {
                SEND_LOG_DATA_STR("ERROR: start request with 0 area\n");
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
                rvx_dropped_busy = 0;

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

                // Get new save file
                if (raw_vid_fp != NULL)
                {
                    SEND_LOG_DATA_STR("ERROR: Worker tried to init save file but already in use\n");
                    break;
                }

                if (get_numbered_file_name("tst%02d.mlv", 99, mlv_name, sizeof(mlv_name)) > -1)
                {
                    raw_vid_fp = FIO_CreateFile(mlv_name);
                    if (raw_vid_fp == NULL)
                    {
                        SEND_LOG_DATA_STR("Worker couldn't create mlv file\n");
                        break;
                    }
                }
                else
                {
                    SEND_LOG_DATA_STR("ERROR: Worker couldn't get mlv file name\n");
                    break;
                }

                // FIXME from here onwards we want to cleanup the file if we error.
                // Really, we need to revisit error conditions in all states,
                // and ensure we handle correctly (e.g. flushing pending data).

                session.fps = fps_get_current_x1000();
                session.res_x = event->w;
                session.res_y = event->h;

                err = start_mlv_session(&session);
                if (err)
                {
                    SEND_LOG_DATA_STR("ERROR: failed to start mlv session\n");
                    break;
                }

                // write standard header blocks
                header_size = write_file_headers(header_buf, header_buf + header_buf_size);
                if (header_size == 0)
                {
                    SEND_LOG_DATA_STR("ERROR: failed to write mlv headers\n");
                    break;
                }
                FIO_WriteFile(raw_vid_fp, header_buf, header_size);

                #ifdef RAW_VID_LOG
                send_log_data_str("Recording is active\n");
                start_time = get_ms_clock();
                #endif
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
                // valid, decodable file even if the teardown then wedges. On a frame-count update
                // error we still close (never leave the handle dangling / data unflushed).
                if (raw_vid_fp != NULL)
                {
                    err = update_video_frame_count(video_frame_count, header_buf, header_buf + header_size);
                    if (err)
                        SEND_LOG_DATA_STR("ERROR: couldn't update video frame count (closing anyway)\n");
                    // write back the header (now with the final frame count); file is complete
                    FIO_SeekSkipFile(raw_vid_fp, 0, SEEK_SET);
                    FIO_WriteFile(raw_vid_fp, header_buf, header_size);
                    FIO_CloseFile(raw_vid_fp);
                    raw_vid_fp = NULL;
                    SEND_LOG_DATA_STR("worker: MLV closed (finalized)\n");
                }
                else
                {
                    SEND_LOG_DATA_STR("ERROR: unexpected fp state in worker\n");
                }

                // restore LV away from raw. This ML asserts if not paired with raw_lv_request(),
                // but is safe; on M50 II it may fault (EvfDevState) -- the file is already closed.
                raw_lv_release();
                // TODO mlv_lite does restore_bit_depth(), we don't mess with this yet.

                #ifdef RAW_VID_LOG
                snprintf(log_buf, buf_size, "session time, data written: %d, %d\n",
                         get_ms_clock() - start_time, session_data_size);
                send_log_data_str(log_buf);
                session_data_size = 0;
                #endif

                // free crop buffers
                crop_buf = NULL;
                if (crop_bufs[0]._buf != NULL)
                {
                    fio_free(crop_bufs[0]._buf);
                    crop_bufs[0]._buf = NULL;
                    crop_bufs[0].start = NULL;
                    crop_bufs[0].cur = NULL;
                    crop_bufs[0].end = NULL;
                }
                if (crop_bufs[1]._buf != NULL)
                {
                    fio_free(crop_bufs[1]._buf);
                    crop_bufs[1]._buf = NULL;
                    crop_bufs[1].start = NULL;
                    crop_bufs[1].cur = NULL;
                    crop_bufs[1].end = NULL;
                }

                extern int rvx_dropped_queue; // event_pusher.c
                RVX_HEX("frames accepted", video_frame_count);
                RVX_HEX("dropped (queue full)", rvx_dropped_queue);
                RVX_HEX("dropped (bufs busy)", rvx_dropped_busy);

                stop_mlv_session();
                recording_state = INACTIVE;
                SEND_LOG_DATA_STR("Recording is inactive\n");
            }
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
    task_create("raw_v_worker", WORKER_PRIO, 0x1000, worker, NULL);
    allow_recording();
}
