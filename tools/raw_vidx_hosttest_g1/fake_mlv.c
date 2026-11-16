/* Minimal stand-in for modules/raw_video/raw_vidx/mlv_3.c, used by the host test only.
 * Same API (mlv_3.h), trivially simple block layout so harness.c can walk the file:
 *   file header : "MLVI" u32 size(=64) u32 video_frame_count ... zero padding
 *   frame block : "VIDF" u32 size u32 frame_number, padding up to alignment, image data
 * The real mlv_3.c is NOT exercised by these tests. */
#include "dryos.h"
#include "mlv_3.h"

#define FAKE_HDR_SIZE 64
#define FAKE_VIDF_HDR 32

int fake_mlv_session_in_progress = 0;
static uint32_t frame_number = 0;
static uint32_t alignment = 1;

int start_mlv_session(struct mlv_session *session)
{
    if (fake_mlv_session_in_progress)
        return -1;
    fake_mlv_session_in_progress = 1;
    frame_number = 0;
    alignment = session->image_data_alignment ? session->image_data_alignment : 1;
    return 0;
}

int stop_mlv_session(void)
{
    if (!fake_mlv_session_in_progress)
        return -1;
    fake_mlv_session_in_progress = 0;
    return 0;
}

int write_file_headers(char *start, char *end)
{
    if (start == NULL || end == NULL || end - start < FAKE_HDR_SIZE)
        return 0;
    memset(start, 0, FAKE_HDR_SIZE);
    memcpy(start, "MLVI", 4);
    uint32_t size = FAKE_HDR_SIZE;
    memcpy(start + 4, &size, 4);
    return FAKE_HDR_SIZE;
}

char *write_video_frame_header(uint32_t image_size, char *start, char *end)
{
    if (start == NULL || end == NULL)
        return NULL;
    uintptr_t img = ((uintptr_t)start + FAKE_VIDF_HDR + alignment - 1) / alignment * alignment;
    char *image_start = (char *)img;
    if (image_start + image_size > end)
        return NULL;
    uint32_t size = (uint32_t)((image_start + image_size) - start);
    memset(start, 0, image_start - start);
    memcpy(start, "VIDF", 4);
    memcpy(start + 4, &size, 4);
    memcpy(start + 8, &frame_number, 4);
    frame_number++;
    return image_start;
}

int update_video_frame_count(uint32_t count, char *start, char *end)
{
    if (start == NULL || end == NULL || end - start < FAKE_HDR_SIZE)
        return -1;
    memcpy(start + 8, &count, 4);
    return 0;
}
