// rvx_host_mlv: produce an MLV on the host with the SAME writer code raw_vidx
// uses on the camera (modules/raw_video/raw_vidx/mlv_3.c, linked unmodified).
//
// This file replays the data path of raw_vidx/worker.c:
//   COMMAND_START  -> start_mlv_session(), write_file_headers(), write header
//   IMAGE_DATA     -> write_video_frame_header() into one of two crop buffers,
//                     per-row crop memcpy with the worker's exact src arithmetic,
//                     swap + flush when a frame no longer fits
//   COMMAND_STOP   -> update_video_frame_count(), seek 0, rewrite header, close
// The image source is a single packed 14-bit LiveView frame dumped from the
// camera (tools/mlserial/captures/first_raw_capture.raw), repeated N times.
//
// --stop camera : exactly what worker.c COMMAND_STOP does today (the frames still
//                 sitting in the current crop buffer are NOT flushed).
// --stop flush  : what a fixed worker would do (flush the tail, then finalise).
// --stop crash  : no finalise at all (camera died before close); only complete
//                 buffer flushes reach the file, frame count stays 0.
// --cut N       : additionally chop N bytes off the end (torn last write).

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mlv_3.h"
#include "rvx_standins.h"

// values from raw_vidx (raw_vid.h, raw_vid_worker.h, src/edmac-memcpy.h)
#define BPP 14
#define EDMAC_ALIGNMENT 0x80
#define INITIAL_CROP_BUF_SIZE (16 * 1024 * 1024)
#define CROP_BUF_ALIGN 0x1000

static int verbose = 0;
int rvx_host_log(const char *s)
{
    if (verbose)
        fputs(s, stderr);
    return 0;
}

struct crop_buf
{
    char *_buf;
    char *start;
    char *cur;
    char *end;
};

static FILE *fp = NULL;
static uint32_t flushed_bytes = 0;

static void write_buf_to_disk(struct crop_buf *b)
{
    size_t n = b->cur - b->start;
    if (n && fwrite(b->start, n, 1, fp) != 1)
    {
        perror("fwrite");
        exit(2);
    }
    flushed_bytes += n;
    fprintf(stderr, "flush: %zu bytes\n", n);
    b->cur = b->start;
}

static void usage(const char *me)
{
    fprintf(stderr,
        "usage: %s -i frame.raw -o out.mlv [-n frames] [-W src_w] [-H src_h]\n"
        "          [-w crop_w] [-h crop_h] [-x off_x] [-y off_y] [-f fps_x1000]\n"
        "          [-b black] [-B white] [-s camera|flush|crash] [-c cut_bytes]\n"
        "          [-S crop_buf_size] [-v]\n"
        "defaults: 2096x1164 source, 1792x896 crop at 200,100 (raw_vidx), 8 frames\n", me);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL, *stop = "flush";
    int frames = 8, src_w = 2096, src_h = 1164;
    int w = 1792, h = 896, x = 200, y = 100; // event_pusher.c
    int fps_x1000 = 29970, black = 2047, white = 16200;
    long cut = 0;
    uint32_t buf_size = INITIAL_CROP_BUF_SIZE + CROP_BUF_ALIGN; // alloc_crop_bufs()
    int opt;

    while ((opt = getopt(argc, argv, "i:o:n:W:H:w:h:x:y:f:b:B:s:c:S:v")) != -1)
    {
        switch (opt)
        {
        case 'i': in = optarg; break;
        case 'o': out = optarg; break;
        case 'n': frames = atoi(optarg); break;
        case 'W': src_w = atoi(optarg); break;
        case 'H': src_h = atoi(optarg); break;
        case 'w': w = atoi(optarg); break;
        case 'h': h = atoi(optarg); break;
        case 'x': x = atoi(optarg); break;
        case 'y': y = atoi(optarg); break;
        case 'f': fps_x1000 = atoi(optarg); break;
        case 'b': black = atoi(optarg); break;
        case 'B': white = atoi(optarg); break;
        case 's': stop = optarg; break;
        case 'c': cut = atol(optarg); break;
        case 'S': buf_size = strtoul(optarg, NULL, 0); break;
        case 'v': verbose = 1; break;
        default: usage(argv[0]); return 1;
        }
    }
    if (!in || !out)
    {
        usage(argv[0]);
        return 1;
    }
    if (x + w > src_w || y + h > src_h)
    {
        fprintf(stderr, "crop %dx%d+%d+%d does not fit %dx%d\n", w, h, x, y, src_w, src_h);
        return 1;
    }

    // ---- the "Canon raw buffer" ----
    uint32_t src_size = (uint32_t)src_w * src_h * BPP / 8;
    char *src = malloc(src_size);
    FILE *fi = fopen(in, "rb");
    if (!src || !fi || fread(src, src_size, 1, fi) != 1)
    {
        fprintf(stderr, "cannot read %u bytes from %s\n", src_size, in);
        return 1;
    }
    fclose(fi);

    // raw.c: M50/M50mk2 skip_top 34, skip_left 88
    rvx_sim_set_geometry(src, src_w, src_h, 88, 0, 34, 0, black, white);

    // ---- alloc_crop_bufs() ----
    struct crop_buf crop_bufs[2];
    for (int i = 0; i < 2; i++)
    {
        crop_bufs[i]._buf = aligned_alloc(CROP_BUF_ALIGN, buf_size);
        if (!crop_bufs[i]._buf)
            return 1;
        crop_bufs[i].start = crop_bufs[i]._buf;
        crop_bufs[i].cur = crop_bufs[i].start;
        crop_bufs[i].end = crop_bufs[i].start + buf_size;
    }
    struct crop_buf *crop_buf = &crop_bufs[0];

    // ---- COMMAND_START ----
    fp = fopen(out, "wb");
    if (!fp)
    {
        perror(out);
        return 1;
    }

    struct mlv_session session = {
        .fps = fps_x1000,          // fps_get_current_x1000()
        .res_x = w,                // event->w
        .res_y = h,                // event->h
        .crop_offset_x = 0,        // worker.c leaves these 0
        .crop_offset_y = 0,
        .pan_offset_x = 0,
        .pan_offset_y = 0,
        .image_data_alignment = EDMAC_ALIGNMENT
    };

    const uint32_t header_buf_size = 1024;
    char *header_buf = malloc(header_buf_size);
    if (start_mlv_session(&session))
        return 3;
    uint32_t header_size = write_file_headers(header_buf, header_buf + header_buf_size);
    if (header_size == 0)
        return 3;
    fwrite(header_buf, header_size, 1, fp);

    // ---- IMAGE_DATA x frames ----
    uint32_t video_frame_count = 0;
    int32_t event_size = w * h * BPP / 8; // event_pusher.c
    uint64_t frame_us = 1000000000ULL / (uint64_t)fps_x1000;

    for (int f = 0; f < frames; f++)
    {
        rvx_sim_advance_us(frame_us);

        if (event_size > crop_buf->end - crop_buf->start)
        {
            fprintf(stderr, "ERROR: event size too large for empty crop buf\n");
            break;
        }

        struct crop_buf *flush_src = NULL;
        char *dst = write_video_frame_header(event_size, crop_buf->cur, crop_buf->end);
        if (dst == NULL)
        {
            flush_src = crop_buf;
            crop_buf = (crop_buf == &crop_bufs[0]) ? &crop_bufs[1] : &crop_bufs[0];
            dst = write_video_frame_header(event_size, crop_buf->cur, crop_buf->end);
            if (dst == NULL)
            {
                fprintf(stderr, "ERROR: couldn't fit image in either buffer\n");
                break;
            }
        }
        crop_buf->cur = dst + event_size;
        video_frame_count++;

        if (flush_src != NULL)
            write_buf_to_disk(flush_src); // camera: "raw_write" task

        // worker.c synchronous inline crop copy, same arithmetic
        int line_size = w * BPP / 8;
        uint32_t sp = rvx_sim_pitch();
        uint32_t dp = (uint32_t)line_size;
        uint32_t sbase = (uint32_t)src
                       + (uint32_t)((x + 7) / 8 * BPP)
                       + (uint32_t)((y / 2 * 2)) * sp;
        uint32_t dbase = (uint32_t)dst;
        for (int row = 0; row < h; row++)
            memcpy((void *)(dbase + (uint32_t)row * dp),
                   (void *)(sbase + (uint32_t)row * sp),
                   (uint32_t)line_size);
    }

    // ---- COMMAND_STOP ----
    uint32_t pending = crop_buf->cur - crop_buf->start;
    if (!strcmp(stop, "flush"))
    {
        write_buf_to_disk(crop_buf);
        pending = 0;
    }
    if (strcmp(stop, "crash"))
    {
        if (update_video_frame_count(video_frame_count, header_buf, header_buf + header_size))
            return 4;
        fseek(fp, 0, SEEK_SET);
        fwrite(header_buf, header_size, 1, fp);
    }
    stop_mlv_session();
    fclose(fp);

    if (cut > 0)
    {
        FILE *t = fopen(out, "rb");
        fseek(t, 0, SEEK_END);
        long sz = ftell(t);
        fclose(t);
        if (cut < sz && truncate(out, sz - cut))
            perror("truncate");
    }

    fprintf(stderr,
            "%s: stop=%s frames_counted=%u header=%u flushed=%u unflushed_at_stop=%u cut=%ld\n",
            out, stop, video_frame_count, header_size, flushed_bytes, pending, cut);
    return 0;
}
