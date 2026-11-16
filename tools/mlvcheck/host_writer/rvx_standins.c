// Host stand-ins for the ML globals/functions that raw_vidx/mlv_3.c pulls in.
//
// Unlike mlv_3_sample_code/function_overrides.c (which uses dummy values), these
// are filled with the values the M50 II core reports in 1080p LiveView (docs/35,
// src/raw.c), so that the MLV produced on the host carries the same headers the
// camera would write.  Compiled with the SAME ML headers as mlv_3.c so the struct
// layouts are the real ones (-m32, -DMODULE).

#include <stdint.h>
#include <string.h>
#include <limits.h>

#include "dryos.h"
#include "raw.h"
#include "lens.h"

#include "rvx_standins.h"

int af_mode = 0;
uint32_t camera_model_id = 0x80000453; // EOS M50 Mark II (assumed; not read from HW here)
char camera_model[32] = "Canon EOS M50m2";
char camera_serial[32] = "0000000000";

// src/raw.c initialiser + CAM_COLORMATRIX1 for CONFIG_M50 / CONFIG_M50mk2
struct raw_info raw_info = {
    .api_version = 1,
    .bits_per_pixel = 14,
    .black_level = 1024,
    .white_level = 13000,
    .cfa_pattern = 0x02010100,
    .calibration_illuminant1 = 1,
    .color_matrix1 = { 8532, 10000,  -701, 10000, -1167, 10000,
                      -4095, 10000, 11879, 10000,  2508, 10000,
                       -797, 10000,  2424, 10000,  7010, 10000},
    .dynamic_range = 1100,
};

struct raw_capture_info raw_capture_info = {
    .sensor_res_x = 6000,
    .sensor_res_y = 4000,
    .sensor_crop = 162,
    .binning_x = 3, .skipping_x = 0,   // raw.c non-zoom LV branch
    .binning_y = 1, .skipping_y = 2,
    .offset_x = SHRT_MIN,
    .offset_y = SHRT_MIN,
};

struct lens_info lens_info;

// same arithmetic as raw_set_geometry() in src/raw.c
void rvx_sim_set_geometry(void *buffer, int width, int height,
                          int skip_left, int skip_right, int skip_top, int skip_bottom,
                          int black, int white)
{
    raw_info.buffer = buffer;
    raw_info.width = width;
    raw_info.height = height;
    raw_info.pitch = raw_info.width * raw_info.bits_per_pixel / 8;
    raw_info.frame_size = raw_info.height * raw_info.pitch;
    raw_info.active_area.x1 = skip_left;
    raw_info.active_area.y1 = skip_top;
    raw_info.active_area.x2 = raw_info.width - skip_right;
    raw_info.active_area.y2 = raw_info.height - skip_bottom;
    raw_info.jpeg.x = 0;
    raw_info.jpeg.y = 0;
    raw_info.jpeg.width = raw_info.width - skip_left - skip_right;
    raw_info.jpeg.height = raw_info.height - skip_top - skip_bottom;
    raw_info.black_level = black;
    raw_info.white_level = white;

    memset(&lens_info, 0, sizeof(lens_info));
    strncpy(lens_info.name, "EF-M15-45mm f/3.5-6.3 IS STM", sizeof(lens_info.name) - 1);
    lens_info.focal_len = 15;
    lens_info.focus_dist = 100;
    lens_info.aperture = 35;
    lens_info.iso = 100;
    lens_info.iso_analog_raw = 72;
    lens_info.wb_mode = 0;
    lens_info.kelvin = 5200;
    lens_info.WBGain_R = 2000;
    lens_info.WBGain_G = 1024;
    lens_info.WBGain_B = 1600;
}

int rvx_sim_pitch(void) { return raw_info.pitch; }

// simulated microsecond uptime clock; the caller advances it per frame
static uint64_t sim_us = 60ULL * 1000 * 1000; // 60 s after boot
void rvx_sim_advance_us(uint64_t us) { sim_us += us; }

uint64_t get_us_clock(void)
{
    sim_us += 7; // every call costs a little time
    return sim_us;
}

void LoadCalendarFromRTC(struct tm *tm_out)
{
    struct tm tm = {0};
    tm.tm_sec = 30; tm.tm_min = 23; tm.tm_hour = 6;
    tm.tm_mday = 19; tm.tm_mon = 6; tm.tm_year = 126; // 2026-07-19 06:23:30
    tm.tm_wday = 0; tm.tm_yday = 199;
    *tm_out = tm;
}

int get_current_shutter_reciprocal_x1000(void)
{
    return 50 * 1000; // 1/50 s
}

// raw_vid.h routes SEND_LOG_DATA_STR here on this branch
extern int rvx_host_log(const char *s);
void m50ii_uart_str(const char *s)
{
    rvx_host_log(s);
}
