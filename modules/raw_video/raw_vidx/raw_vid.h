#ifndef _raw_vid_h_
#define _raw_vid_h_

#undef RAW_VID_LOG
//#define RAW_VID_LOG 1

/* [M50 II debug] Route the module's telemetry to the SERIAL UART (core m50ii_uart_str @0xcf1ef,
 * resolved at module load), because send_log_data_str only reaches the on-screen ML console, which
 * is invisible over serial -- that blindness cost us hours. Every existing SEND_LOG_DATA_STR call
 * now emits "[RVX] <msg>" over UART; grep run/uart.log (or `mllog`) for "[RVX]". RVX_HEX(tag,val)
 * logs a hex value. Not atomic (may interleave with Canon output) but greppable. */
extern void m50ii_uart_str(const char *);
#define SEND_LOG_DATA_STR(s) do { m50ii_uart_str("[RVX] "); m50ii_uart_str(s); } while (0)
static inline void rvx_hex(const char *tag, uint32_t v)
{
    char b[11]; const char *h = "0123456789ABCDEF";
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = h[(v >> ((7 - i) * 4)) & 0xF];
    b[10] = 0;
    m50ii_uart_str("[RVX] "); m50ii_uart_str(tag); m50ii_uart_str("="); m50ii_uart_str(b); m50ii_uart_str("\n");
}
#define RVX_HEX(tag, v) rvx_hex((tag), (uint32_t)(v))

// While we do send an event for every frame pushed,
// the queue receiving them doesn't need to be very long,
// because we must keep up, or we will drop frames.
#define RAW_VID_Q_LEN 20

#define BPP 14

enum event_type
{
    // Events with these types should never be posted to the queue.
    EMPTY = 0, // We zero the event to init or re-use it, so this value must be explicit.
    PREPARING, // Reserved, but otherwise empty; used to limit time spent in critical section.

    // COMMAND_START sets creation_time, type, and the window to record: w, h, x, y
    // (the same values every following IMAGE_DATA event will carry).
    // COMMAND_STOP only needs creation_time and type.  Other fields are ignored.
    COMMAND_START, // Producer / event_handler requests start.  Consumer / worker may reject the request.
    COMMAND_STOP,  // As above, but stop.

    // This event must set valid values for all fields.
    IMAGE_DATA, // Some image data to save to file.
};

struct raw_vid_event
{
    uint32_t creation_time;
    enum event_type type;
    char *src; // points to image data if capturing

    uint32_t orig_frame_width;
    uint32_t x; // x offset within the frame, for cropping
    uint32_t y; // y as above
    uint32_t w; // our crop width
    uint32_t h; // our crop height

    int32_t size; // size calculated by producer, in bytes
};

enum recording_state
{
    ERROR,
    // RECORDING // can't use this name because propvalues.h #defines it
    INACTIVE,
    ACTIVE
};

#endif // _raw_vid_h_
