#ifndef _raw_vid_worker_h_
#define _raw_vid_worker_h_

#include "dryos.h"

void init_worker(struct msg_queue *q);
enum recording_state get_recording_state(void);

// Priority for the worker tasks.  WORKER_PRIO controls the queue monitor.
// WORKER_WRITE_PRIO only the task that flushes data to disk.
// This is a compromise.  Lower values are higher priority.
// Too high priority and the GUI becomes very laggy.
// Too low and it limits effective write bandwidth.
// These specific values were empirically determined on 200D and are
// unlikely to work well on all cams.  More testing is needed.
#define WORKER_PRIO 0x11
// [M50 II] 0x9 (a 200D value) is HIGHER priority than Canon's EvfDev/LiveView tasks, so the big
// per-buffer FIO_WriteFile STARVES the EvfDev task for the duration of the write -> its message
// queue overflows -> LiveViewDrive::EvfDevState.c L:372/542 nullptr assert FLOOD the instant the
// write finishes -> the camera stalls and can't cleanly stop/close the MLV (frames lost). RE'd via
// Ghidra: the asserts are EvfDev TryPostEvent(evfdev+0x8) failures from Canon's LvComPack path.
// Drop the write task well below Canon's LV tasks (~0x0-0x13) so EvfDev keeps draining mid-write.
#define WORKER_WRITE_PRIO 0x1A

// How long COMMAND_STOP waits for one buffer write to card to finish before it gives
// up on the buffered frames.  A full 16 MB buffer at a slow 5 MB/s is about 3 s.
#define STOP_WRITE_WAIT_MS 10000
// How long a frame may wait for the previous "raw_write" task to exit before its
// buffer flush is skipped.  The task is past its FIO_WriteFile() by then.
#define FLUSH_WRITE_WAIT_MS 1000

// Used to get space for our local buffers.  This is the initial
// size attempted, we reduce from their until success.
// Pick a nice round value; this is the aligned size, not underlying size.
#define INITIAL_CROP_BUF_SIZE (16 * 1024 * 1024)
#define CROP_BUF_ALIGN 0x1000 // aligned for when FIO_Write() reads from here (best alignment untested)
#define MIN_CROP_BUF_SIZE (4 * 1024 * 1024)
#define CROP_BUF_STEP_SIZE (1 * 1024 * 1024)

#endif // _raw_vid_worker_h_
