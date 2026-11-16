#ifndef _raw_vid_pusher_h_
#define _raw_vid_pusher_h_

#include "dryos.h"

void init_event_pusher(struct msg_queue *event_q);
void release_event(struct raw_vid_event *event);
void allow_recording(void);
// Worker only: make the vsync hook stop queueing frames now (undone by allow_recording()).
void block_recording(void);
// [H1] Worker only: the capture buffers the per-frame hook may not allocate or free itself.
void rvx_capture_alloc(void); // on CAPTURE_PREP, while recording
void rvx_capture_free(void);  // when no session is active
extern volatile int rvx_hook_flags; // what the hook could not print, see event_pusher.c
extern int rvx_dropped_nobuf;

#endif // _raw_vid_pusher_h_
