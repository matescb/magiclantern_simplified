# C9-safe-first - Recording architecture for raw_vidx on the M50 II (least-firmware-risk variant)

Date: 2026-11-16. Offline design. No camera attached; nothing here was run on hardware. No ML code was
changed. The only thing built is a host-side budget calculator.

## Goal

Design how `modules/raw_video/raw_vidx` should reach a first **decodable, correctly timed** MLV clip on the
M50 II with the least firmware risk (module-only, no Canon EDMAC/CBR/MMIO writes), then list the steps to
continuous recording. Deliver data flow, sizing arithmetic, task layout, Canon calls with their unverified
points, a frame-budget table, failure handling, and an ordered plan with an offline and a hardware test per step.

## Short version

1. The current module cannot produce a correctly timed clip, for a reason that is arithmetic, not a bug:
   it copies 2.81 MB per frame at a measured ~6 MB/s from a buffer Canon overwrites every ~42 ms. One copy
   spans ~11 sensor frames, so every stored frame is a mix of about 11 exposures. Any design must make
   the copy shorter than one frame period, or stop Canon from overwriting the buffer during the copy.
2. Safe-first answer: **mode F (freeze-frame)**. Clear the `lv_save_raw` flag so Canon stops refreshing the
   buffer, copy the now-static frame slowly at low priority into a RAM ring, set the flag again, repeat.
   When the ring is full or REC is pressed, leave raw off and write the ring to the card. Result: whole
   frames at about 1.4 fps (1792x896) to 4.8 fps (640x360) with true per-frame timestamps. Whether clearing
   the flag really leaves an intact frame is unverified and is hardware check H3.
3. The path to full frame rate is a faster copy, not a cleverer queue. The ML bench measured cached memcpy at
   ~80 MB/s on this body; the 6 MB/s figure is uncached-to-uncached. Reading through the cacheable alias
   after invalidating it, into a cacheable destination, is the one change that could make a 1280x720 or
   1792x896 live copy fit in a frame period (table T3).
4. Card speed is not the bottleneck for anything up to 1792x896 at 24 fps: 75 MB/s write was measured on
   hardware (A7). Copy speed and RAM (32 MiB proven, 48 MiB hoped) are.
5. Seven defects in the current module would spoil even a perfect capture path (section "Defects found").
   Step 1 of the plan fixes them and needs no new Canon behaviour.

## What was built

| Path | What |
|---|---|
| `/mnt/data/WORK/eos/tools/C9-safe-first/budget.py` | frame-size / ring / fps / card arithmetic used for every table below. `python3 budget.py` prints the tables, `python3 budget.py --test` runs 8 checks |
| `/mnt/data/WORK/eos/sim/C9-safe-first/budget.out` | the table output pasted below |

```
$ python3 /mnt/data/WORK/eos/tools/C9-safe-first/budget.py --test
budget.py self-test: 8 checks OK
```

## Inputs and how far each can be trusted

| Fact | Value | Source | Trust |
|---|---|---|---|
| LV raw frame | 2096x1164, 14-bit packed, pitch 3668 B, 4,269,552 B | docs/35, `first_raw_capture.raw` | measured |
| Buffer | one Canon buffer, 0x2BC61CC8 cacheable / 0x6BC61CC8 uncached (0x6BC41CC8 also seen, boot 203) | docs/35, memory note | measured; address varies between boots, never hard-code it |
| Refresh | every LV frame, ~24-25 fps | task text | assumed 23.976 in tables |
| Uncached-to-uncached copy | 5.1-6.2 MB/s (range 4.3-8.0) | C7 rates table, UART cadence of 5 frames | lower bound, coarse (0.4 s timestamp steps) |
| Cached memcpy | 76-83 MB/s | A7, ML bench on HW 07-18 | measured, but cached source and destination |
| Card write | 75.1 / 75.7 MB/s, 16 MiB buffers, card "SD16G" | A7, ML bench on HW | measured on one card |
| RAM | ~63 MB shoot memory; 2 x 16 MiB contiguous `fio_malloc` succeeded (crop_buf 0x46331000-0x47332000) | task text, memory note | 32 MiB proven, more is not |
| EvfDev task | priority 0x0E, queue 200, ~3 messages per frame; EvfCap 0x0D | A2 | static analysis |
| vsync hook | fires on EvfDevState input 6 / state 3 = `evfdevUpdateParam`, inside the EvfDev task, before readout of that frame | A2 | static analysis |
| `lv_save_raw(n)` | writes a flag (Mem1Path ctx 0x10238 +0x40); per frame `Mem1Path_SetParam` programs EDMAC ch3 when the flag is set, else stops ch3 | A1 | static analysis |
| MvRawPro/RawPro/LvJob "storm" | Canon's EDMAC completion callbacks stopped or starved while VD continued; not an `lv_save_raw` race | A1 | inferred |
| pc=0 / 0xE005DFA2 aborts during recording | global TaskClass message pool (1536 nodes, head 0x39B84) exhausted | A3 | inferred from dumps |
| "READY" poll in worker.c:494-517 | reads two constants; proves nothing | A1 | static analysis |
| mem2mem EDMAC | failed because of two wrong stubs, not hardware | A5 | inferred |

The last real recording attempt in the log (attempt 47, boot 367) still aborted 0.2 s after a copy completed
(C7). So the module as it stands is not stable even for short clips.

## Why the current design cannot give a correct clip

- Frame period at 23.976 fps = 41.7 ms. Copy of 2,809,856 B at 6 MB/s = 468 ms = 11.2 frame periods.
- Canon writes the whole 4.27 MB buffer once per period, top to bottom, so its write front moves at
  >= 102 MB/s. A 6 MB/s reader is lapped about 11 times per frame. Each stored frame is ~11 horizontal bands
  from 11 consecutive exposures. A static scene looks fine (which is why the single DNG of docs/35 looked
  right); anything moving is torn.
- The worker runs at priority 0x11 and never sleeps while the queue is non-empty, and the queue is always
  full because frames arrive 11x faster than they are consumed. On the core it runs on, every task with a
  numerically higher priority value (lower priority) gets no CPU for the whole recording. If any Canon
  TaskClass consumer sits there (EIS/DAF handlers are candidates; the log shows `[EIS] ... TryPostEvent`
  floods before the aborts), its messages pile up in the shared 1536-node pool and A3's abort follows. This
  is an inference from A2/A3 plus the code; it fits "crashes a fraction of a second after a few copies"
  better than the per-site explanations in the memory note. Hardware check H2 tests it.

## Defects found in the current module (read of walkback-milestone a10b16c23)

| # | Where | Defect | Effect |
|---|---|---|---|
| D1 | worker.c COMMAND_STOP (581-645) | the partly filled crop buffer is never written; only full 16 MiB buffers are flushed, at swap time | a clip shorter than 6 frames writes headers only; every clip loses its tail; the header frame count then exceeds the frames in the file |
| D2 | event_pusher.c:98 in `get_empty_event`, called from the vsync hook | `take_semaphore(events_sem, 2000)` inside the EvfDev task | can block Canon's EvfDev queue for up to 2 s (200 slots = ~65 frames, A2) |
| D3 | event_pusher.c:369-394 | two `fio_malloc` of 4.27 MB inside the hook on the first frame; with redirect off they are never used | allocation in the EvfDev task; 8.5 MB of shoot memory wasted |
| D4 | event_pusher.c:369, 409, 443-453; raw_vid.h:13 | `SEND_LOG_DATA_STR`/`RVX_HEX` in the hook write to the UART synchronously | ~40 characters at 115200 baud = 3.5 ms per line inside EvfDev |
| D5 | mlv_3.c:725 | `vidf_block.timestamp = get_us_clock()` is absolute (every other block subtracts `session_start_timestamp`) and is taken when the worker gets to the frame, not at capture | VIDF timestamps are wrong by the queue delay and by the session start offset |
| D6 | worker.c:335-343 | if the other buffer is still being written at swap time, `crop_buf = NULL` and every later frame is dropped silently; recording never stops | hung "recording" |
| D7 | worker.c:377 and 197-215 | a new `raw_write` task per flush resets `crop_buf->cur` with no lock; COMMAND_STOP can close the file under it | race between writer and stop |
| D8 | worker.c:494-517 | READY poll on `*(0xE07B9ED4)+0x30`, `*(0xE0761240)+0x30` | no information (A1); adds up to 1.5 s, harmless otherwise |
| D9 | worker.c:136-137, 197 | buffers are passed to `FIO_WriteFile` already `UNCACHEABLE` | correct today only because the copy also writes through the uncached alias; if the copy destination becomes cacheable the pointer handed to `FIO_WriteFile` must be the cacheable one (docs/35 pitfall) |

## Architecture

### Modes

| Mode | Idea | Coherent frames | fps | New Canon behaviour relied on |
|---|---|---|---|---|
| S (step 2) | copy from the live buffer into RAM, write after stop | no (torn unless the scene is static) | whatever the copy gives | none |
| **F (step 3, first real target)** | freeze buffer with `lv_save_raw(0)`, copy, re-arm | yes, if H3 passes | 1-5 (T2) | flag toggle leaves the last frame intact |
| L (step 5) | fast cached copy from the live buffer, started at frame-ready, small crop | yes, if the copy finishes inside the safe window | 24 or 12 | cache invalidate of Canon's buffer; a frame-ready signal |
| C (step 7) | mode L plus writing while capturing | yes | 24 or 12, unlimited length | none beyond L |

All modes share the same ring, writer, file format and failure handling. Only the capture task's inner loop changes.

### Data flow (mode F)

```
 sensor -> Canon CapPath -> Mem1Path_SetParam (per frame, Canon)
                               | flag [0x10238+0x40] == 1 : program EDMAC ch3 -> Canon raw buffer
                               | flag == 0               : stop ch3 (buffer left as it is - UNVERIFIED)
                               v
                     Canon raw buffer 2096x1164x14bit   (never written, redirected or re-registered by ML)
                               |
 EvfDev task (0x0E)            |                                   rvx_cap task (prio 0x1C)
 vsync hook:                   |                                   loop per frame:
   vd_count++            ------+---- vd_count (read only) ---->      1 wait until vd_count >= armed_at + 3
   (nothing else)              |                                     2 call("lv_save_raw", 0)
                               |                                     3 wait 2 vsyncs; t_cap = us clock at step 2
                               +---- rows of the crop  ------->      4 copy crop -> ring slot, 256 KiB chunks,
                                     (uncached alias, or             msleep(10) between chunks, health check
                                      cached alias after             5 slot.state = FULL (timestamp, frame no)
                                      invalidate in step 4)          6 call("lv_save_raw", 1); armed_at = vd_count
                                                                     7 stop if ring full / REC / health bad
                                                                         |
                                 RAM ring: 2 (or 3) x 16 MiB blocks      v
                                 slot = VIDF header + pad + payload, 4 KiB aligned
                                                                         |
                               rvx_wr task (prio 0x1A, resident, sleeps on a semaphore)
                                 after capture stops (mode F/S/L) or while capturing (mode C):
                                 MLVI/RAWI/... headers -> FULL slots in order -> rewrite header -> close
                                                                         v
                                                                 B:/.../tstNN.mlv
```

Rules that hold in every mode:

- The hook does integer work only: no semaphore, no allocation, no logging, no `task_create`, no queue of
  event structs. One `vd_count++` and, from step 5 on, one `give_semaphore`.
- ML never writes an EDMAC register, never calls RegisterEDmacCompleteCBR, never touches channel 3, 0x29,
  0x4C or the channel in `[0x292B0+0x30]` (A1), and does not use the mem2mem engine until step 8.
- No ML task involved in recording runs above (numerically below) 0x1A, and every loop that can run for more
  than ~40 ms sleeps on purpose.
- The file is opened, written and closed by one task only.

### Buffers and sizing

Ring: reuse the allocation pattern that has worked on hardware (two `fio_malloc(16 MiB + 4 KiB)`); try a
third block and accept failure. Drop the two unused 4.27 MB `capture_frames` (D3). Budget against ~63 MB:

| Item | Size | Note |
|---|---|---|
| ring block 0, 1 | 2 x 16 MiB = 33.6 MB | proven to allocate (boot 212) |
| ring block 2 (optional) | 16 MiB = 16.8 MB | unproven; 50.3 MB total leaves ~12 MB to Canon |
| header buffer | 1 KiB | `malloc`, as now |
| removed: capture_frames | -8.5 MB | D3 |

A frame never straddles two blocks. Slot = 32-byte VIDF header + padding + payload, rounded up to 4 KiB so
the payload is 4 KiB aligned (satisfies the FSU buffer rule and keeps writes sector aligned, A7).
Slots per block = floor(16 MiB / slot):

| Crop (14-bit) | payload B | slot B | slots / 16 MiB | 32 MiB ring | 48 MiB ring |
|---|---|---|---|---|---|
| 2096x1164 | 4,269,552 | 4,272,128 | 3 | 6 | 9 |
| 1792x896 | 2,809,856 | 2,813,952 | 5 | 10 | 15 |
| 1280x720 | 1,612,800 | 1,613,824 | 10 | 20 | 30 |
| 960x540 | 907,200 | 909,312 | 18 | 36 | 54 |
| 640x360 | 403,200 | 405,504 | 41 | 82 | 123 |

(T1 below counts the ring as one contiguous area, so it shows 7/11 and 11/17 for the two largest crops; the
per-block numbers in this table are the ones to implement.)

Queue: none between hook and capture task in modes S/F (the capture task polls `vd_count` with `msleep(10)`).
Between capture and writer one counting semaphore plus the slot states (`FREE -> FILLING -> FULL -> WRITING ->
FREE`), single producer and single consumer, indices advanced only by their owner. The 20-entry event queue
remains only for START/STOP commands from the key handler. Ring full is the only back-pressure point and it
is handled in the capture task, never in the hook.

Width must be a multiple of 8 pixels and the x offset a multiple of 8 pixels (8 px = 14 bytes), the y offset
even (keeps RGGB phase); the current `(x+7)/8*BPP` and `y/2*2` already do this.

### Tasks and priorities

| Task | Priority | Owner | Role | Blocking allowed |
|---|---|---|---|---|
| EvfCap | 0x0D | Canon | EvfState machine | - |
| EvfDev | 0x0E | Canon | runs our vsync hook | never, from ML code |
| key handler context | Canon GUI | Canon | posts START/STOP to `raw_vid_q` (non-blocking post 0xE05B55EE) | no |
| raw_v_worker | 0x11 (as now) | ML | command dispatcher only: START allocates and wakes rvx_cap, STOP sets `stop_req`. No copying, no file I/O | short |
| rvx_wr | 0x1A | ML | resident writer; only task that calls FIO on the MLV | yes (card) |
| rvx_cap | 0x1C | ML | capture loop and copy | yes; sleeps 10 ms per 256 KiB |

0x1A was chosen in the last module revision for the writer after the 0x09 writer starved EvfDev; it has not
been validated on hardware (the memory note ends with "NEXT: flash b4bb3762"). 0x1C for the copy is a
guess that it sits below Canon's LV consumers; H2 measures the real Canon priorities. If Canon tasks exist
at 0x1D-0x1F that must run during LV, the deliberate sleeps are what protects them, not the priority.

Copy duty cycle in mode F: 256 KiB at 6 MB/s = 44 ms run, then `msleep(10)`. A 2.81 MB frame is 11 chunks =
0.47 s + 0.11 s. Budget table T2 assumes 80% duty.

### Timing ("correctly timed")

- Each slot stores `t_cap - t_session_start` in microseconds (`get_us_clock`, ML core, 0xE0B35 in the
  baseline sym), taken in the capture task when the frame is known complete (mode F: at the freeze call;
  mode L: at the frame-ready signal). `write_video_frame_header` gets the timestamp as an argument (fixes D5).
- `MLVI.sourceFpsNom/Denom`: in mode L/C with skip N it is the LV fps / N. In mode F the cadence is set by
  the copy, so the header is written at stop with `1000 * (frames-1) * 1e6 / (t_last - t_first)` and the
  capture loop paces itself to a fixed period (e.g. every 16th vsync for 1792x896 = 1.4985 fps) so the
  cadence is constant, not just on average. A frame that misses its vsync is dropped and counted; VIDF
  `frameNumber` keeps the nominal index so tools can see the gap.
- `videoFrameCount` in the header = frames actually written to the file, counted by the writer (fixes D1).

### Canon calls used, and what is unverified about each

| Call / access | Address | Used for | Unverified |
|---|---|---|---|
| `call("lv_set_mm", 1)`, `call("lv_save_raw", 1)` via `raw_lv_request()` | call 0xE05CBF82; handlers 0xE0753D86, 0xE06C0D04 (A1) | start LV raw | whether the LV core supplies a buffer (`param[8]`) in every movie mode; `rd 0x10290` should equal the buffer (A1 check 2) |
| `call("lv_save_raw", 0)` / `(1)` from rvx_cap | same | freeze / re-arm in mode F | **the central unknown**: that the frame in flight completes and stays intact; how many VDs until ch3 stops; that re-arming needs no settle beyond 2-3 VDs; that toggling ~3x per second for a minute upsets nothing (the handler also clears `[0x19094+0xc]` and `+0x10`, meaning unknown) |
| read `raw_info.buffer/width/height/pitch` once at start | ML core 0x104434 | geometry and source | `raw_lv_release()` zeroes `raw_info.buffer`, so the module must keep its own copy and must not call `raw_update_params()` during a clip |
| vsync CBR (`CBR_VSYNC`) | state-object.c:225, EvfDevState 6/3 | frame tick | it is `evfdevUpdateParam`, not readout-done; other senders exist at mode change (A2). Good enough as a tick, not as a frame-ready signal |
| `msg_queue_post` | 0xE05B55EE | START/STOP commands | none (same function Canon's TryPostEvent uses, A2) |
| `fio_malloc` 16 MiB x2 / x3 | ML core over `_AllocateMemory` 0xE05BEE20 (DMA pair reimplemented in function_overrides.c) | ring | third block; fragmentation after several sessions; real free amount (`GetMemoryInformation` 0xE05BECAA) |
| `FIO_CreateFile`, `FIO_WriteFile`, `FIO_SeekSkipFile`, `FIO_CloseFile` | 0xE05B163E, 0xE05B1A00, 0xE05B1942, 0xE05B1C1A | file | seek-to-0 then rewrite header then close has never completed on hardware (every run wedged first); efat flush timing unknown (A7); whether `FIO_Flush` 0xE05B2F3A commits the directory entry without closing |
| `task_create`, `msleep`, `take/give_semaphore` | 0xE05F40BA, 0xE05BA37A, 0xE05D0DA8, 0xE05D0E22 | tasks | `msleep` granularity (assumed 10 ms); whether ML tasks are confined to CPU0 |
| health reads (RAM only): `[0x39B84]` pool free head, `[0x23804]`, `[0x292B0]` ring counts, six LvJob slots at 0x50AF8 + n*0x188 | A1, A3 | abort capture before Canon asserts | all addresses are static-analysis results, never read on hardware |
| step 4 only: data-cache invalidate by address over Canon's buffer (CP15 c7,c6,1 per 32-byte line) | inline in the module | fast copy | cache line size; whether an L2 cache exists and needs its own invalidate; cost of ~88k CP15 writes per 2.8 MB; effect on Canon tasks that read the same buffer |
| step 6 only: Mem1Path completion slot `[0x1029C]` (expect 0xE06AD51B) chained to a module function | A1 check 9 | frame-ready signal | runs in interrupt context; whether the slot is called every frame; one RAM pointer write, but into Canon state |

Not used at all in steps 1-7: `raw_lv_redirect_edmac`, `edmac_copy_rectangle_*`, RegisterEDmacCompleteCBR,
any MMIO write, any ROM patch, any ML core change.

## Frame budget

Generated by `budget.py` (decimal MB; ring counted as contiguous).

#### T1 frame size, required rate, ring capacity

| option | bytes/frame | MB/s @23.976 | MB/s @11.988 | MB/s @5.994 | frames in 32 MiB | frames in 48 MiB | burst s @23.976 (32/48) |
|---|---|---|---|---|---|---|---|
| full 2096x1164 | 4,269,552 | 102.4 | 51.2 | 25.6 | 7 | 11 | 0.29 / 0.46 |
| current 1792x896 | 2,809,856 | 67.4 | 33.7 | 16.8 | 11 | 17 | 0.46 / 0.71 |
| 1280x720 | 1,612,800 | 38.7 | 19.3 | 9.7 | 20 | 31 | 0.83 / 1.29 |
| 960x540 | 907,200 | 21.8 | 10.9 | 5.4 | 36 | 55 | 1.50 / 2.29 |
| 640x360 | 403,200 | 9.7 | 4.8 | 2.4 | 82 | 124 | 3.42 / 5.17 |
| 1792x896 12-bit (CPU repack) | 2,408,448 | 57.7 | 28.9 | 14.4 | 13 | 20 | 0.54 / 0.83 |
| 1792x896 10-bit (CPU repack) | 2,007,040 | 48.1 | 24.1 | 12.0 | 16 | 25 | 0.67 / 1.04 |
| 1280x720 10-bit (CPU repack) | 1,152,000 | 27.6 | 13.8 | 6.9 | 29 | 43 | 1.21 / 1.79 |

#### T2 mode F (freeze, copy, re-arm): effective fps by copy rate (80% duty, 3 frames re-arm)

| option | copy 6 MB/s | copy 25 MB/s | copy 80 MB/s |
|---|---|---|---|
| full 2096x1164 | 0.99 fps | 2.95 fps | 5.21 fps |
| current 1792x896 | 1.41 fps | 3.76 fps | 5.92 fps |
| 1280x720 | 2.17 fps | 4.86 fps | 6.65 fps |
| 960x540 | 3.18 fps | 5.87 fps | 7.18 fps |
| 640x360 | 4.78 fps | 6.88 fps | 7.61 fps |

Mode F clip length with a 32 MiB ring: 1792x896 = 10 frames = 7 s of real time at 1.4 fps; 1280x720 = 20
frames = 9 s; 640x360 = 82 frames = 17 s.

#### T3 mode L (copy from the live buffer): what fits in one frame period

| copy rate | max coherent bytes per frame period | largest listed 14-bit option that fits |
|---|---|---|
| 6 MB/s | 250,250 | none (< 640x360) |
| 25 MB/s | 1,042,709 | 960x540 |
| 80 MB/s | 3,336,670 | current 1792x896 |

This is an upper bound. The real window is the time between Canon finishing the crop's last row and Canon
reaching the crop's first row in the next frame; it needs the frame-ready signal of step 6 to use fully.
At 6 MB/s live capture at full rate is impossible at any useful size; with skip 2 or 4 the window does not
grow (Canon still overwrites every frame), only the write load drops.

#### T4 continuous writing: time until a 32 MiB ring fills (48 MiB in brackets), copy assumed not limiting

| option | fps | need MB/s | card 20 MB/s | card 40 MB/s | card 80 MB/s |
|---|---|---|---|---|---|
| full 2096x1164 | 23.976 | 102.4 | 0.4 s (0.6 s) | 0.5 s (0.8 s) | 1.5 s (2.3 s) |
| full 2096x1164 | 11.988 | 51.2 | 1.1 s (1.6 s) | 3.0 s (4.5 s) | sustained |
| full 2096x1164 | 5.994 | 25.6 | 6.0 s (9.0 s) | sustained | sustained |
| current 1792x896 | 23.976 | 67.4 | 0.7 s (1.1 s) | 1.2 s (1.8 s) | sustained |
| current 1792x896 | 11.988 | 33.7 | 2.5 s (3.7 s) | sustained | sustained |
| current 1792x896 | 5.994 | 16.8 | sustained | sustained | sustained |
| 1280x720 | 23.976 | 38.7 | 1.8 s (2.7 s) | sustained | sustained |
| 1280x720 | 11.988 | 19.3 | sustained | sustained | sustained |
| 960x540 | 23.976 | 21.8 | 19.2 s (28.7 s) | sustained | sustained |
| 960x540 | 11.988 | 10.9 | sustained | sustained | sustained |
| 640x360 | 23.976 | 9.7 | sustained | sustained | sustained |
| 1792x896 12-bit | 23.976 | 57.7 | 0.9 s (1.3 s) | 1.9 s (2.8 s) | sustained |
| 1792x896 10-bit | 23.976 | 48.1 | 1.2 s (1.8 s) | 4.1 s (6.2 s) | sustained |
| 1280x720 10-bit | 23.976 | 27.6 | 4.4 s (6.6 s) | sustained | sustained |

(Rows that are sustained at every card speed are omitted; `budget.py` prints all of them.)
"80 MB/s" is above the 75 MB/s measured; 1792x896 at 23.976 fps needs 67.4 MB/s, i.e. 90% of the measured
card rate with nothing left for FAT updates, so treat it as marginal. A CPU copy at 80 MB/s shares the CPU
and memory bus with the write path, so the two rates do not simply add up (A7 makes the same point).

#### T5 write-after-stop: flush time for a full ring

| ring | card 20 MB/s | card 40 MB/s | card 80 MB/s |
|---|---|---|---|
| 32 MiB | 1.7 s | 0.8 s | 0.4 s |
| 48 MiB | 2.5 s | 1.3 s | 0.6 s |

Bit depth: 10/12-bit by CPU repacking costs more CPU per byte than a plain copy, so it only helps once the
card, not the copy, is the limit. It is not on the safe-first path. A Canon-side lower bit depth
(`lv_set_mm` variants, PACK32 mode) would be free but is unexplored on this body.

## Failure handling

| Event | Detection | Action |
|---|---|---|
| Ring full (modes S/F/L) | capture task finds no FREE slot | normal end of burst: stop capturing, leave raw off, wake writer. Never detected in the hook |
| Ring full (mode C) | same | drop the frame, count it, keep `frameNumber` running; if more than N consecutive drops (config, default 12) end the clip cleanly |
| Card slow | writer measures each `FIO_WriteFile` with `get_us_clock`; logs MB/s once per block after the clip | mode C: drops as above. Other modes: only the flush takes longer |
| Write error / short write | `FIO_WriteFile` return != size | stop capture, do not write further blocks, rewrite header with the count of frames known written, close. Log once |
| Card full | `get_free_space_32k` before START and before each block | refuse START below ring size + 1 MiB; in mode C end the clip one block early |
| Canon pipeline unhealthy | every copy chunk: `[0x39B84] == 0`, `[0x23804] >= 2`, `[0x292B0] >= 2`, all six LvJob slots busy | stop copying at once, `lv_save_raw(0)`, sleep 200 ms, then write what is in RAM. Disabled by a module variable until H1 confirms the addresses |
| Canon asserts / abort during capture | not detectable from ML | modes S/F/L hold frames in RAM, so a wedge before the flush loses the clip. Mitigation: mode F writes after every K frames while frozen (the buffer is frozen anyway, so writing costs no frames), default K = one 16 MiB block, and calls `FIO_Flush` after each block if H6 shows it commits the directory entry |
| STOP while capturing | key handler posts STOP; dispatcher sets `stop_req` | capture task finishes or abandons the current slot (abandon in F: slot back to FREE), leaves raw off, wakes writer |
| STOP while writing | `stop_req` already set; second REC press | ignored until the writer reports CLOSED. The writer never aborts mid-block; the header rewrite and close always run. START is refused until state is INACTIVE |
| START while a previous clip is still flushing | state != INACTIVE | refused with one log line |
| `raw_lv_release()` faults on teardown | - | called last, after the file is closed (already the order in the walkback build). In mode F raw is already off, so only ML's reference count changes |
| Allocation failure | `fio_malloc` returns NULL | 3 -> 2 blocks silently; fewer than 2 x 4 MiB: refuse START. Everything allocated in the dispatcher task, never in the hook |
| Mode change / LV stops during capture | `vd_count` does not advance for 500 ms | treat as STOP |
| Power cut | - | data since the last flushed block is lost; the file on the card stays decodable up to the last header rewrite only if step 1's periodic header checkpoint is enabled |

## Implementation plan

Every step is module-only (`modules/raw_video/raw_vidx`). For every step run the same three offline gates:
(G1) `tools/symgate/check_module_syms.py` against the baseline core `.sym` so the module imports nothing the
booting core lacks (C5; all imports named in this plan were checked present in `build/M50mk2_101.sym`:
`call`, `get_us_clock`, `msg_queue_post`, `take/give_semaphore`, `FIO_*`, `GetMemoryInformation`,
`get_free_space_32k`, `raw_lv_request/release`, `m50ii_uart_str`); (G2) the host harness
`tools/raw_vidx_hosttest/run.sh <module dir>` (written by another agent in this sprint; I did not run it);
(G3) decode the harness's output file with `tools/mlvcheck/mlv_check.py`. Step-specific tests are extra.

| # | Change | Offline test | Hardware test | Pass criterion |
|---|---|---|---|---|
| 0 | No recording. Add a `rvx_probe` entry callable over serial: prints `GetMemoryInformation`, largest block, Canon task priorities (walk task ids with `get_task_info_by_id`), and times four 2.8 MB copies with `get_us_clock`: uncached->uncached, uncached->cacheable, cacheable->cacheable (stale, speed only), 16 MiB `FIO_WriteFile` (then `rd 0x640C`, A7) | G1; host build of the timing loop | H1, H2 below | numbers recorded; decides copy rate column in T2/T3 and whether 0x1C is below Canon's LV tasks |
| 1 | Fix D1-D7 with no change to the capture method: hook reduced to `vd_count++`; dispatcher/capture/writer split; flush the tail on STOP; timestamp passed in; frame count = frames written; resident writer; remove capture_frames and the READY poll; one UART line per clip phase, none per frame | G2 cases `short_clip`, `stop_right_after_swap`, `queue_full`, `slow_card` must pass and G3 must report frames in file == header count for a 1-, 5-, 6- and 11-frame clip | REC, wait 3 s, REC. Pull card | a `tstNN.mlv` exists, `mlv_check.py` clean, frame count matches, camera still responds to `ver` 30 s later |
| 2 | Mode S: RAM ring (2-3 x 16 MiB), capture until full, then write. Crop 1280x720 default. Copy in 256 KiB chunks with `msleep(10)` | host: ring state machine under random producer/consumer delays (extend harness); slot table above reproduced by a unit test | static test chart, record one burst | 20 frames decode; first and last frame identical within noise (static scene); no assert in UART; `[0x39B84]` non-zero after |
| 3 | Mode F: freeze / copy / re-arm, fixed pacing, per-frame capture timestamps, fps written at stop | host: synthetic "sensor" thread that overwrites a buffer at 24 Hz with a frame counter in every row unless frozen; assert every captured frame has one counter value and timestamps are monotonic with the configured period | H3 below: moving target (stopwatch on a phone, or a fan) | no band discontinuities in any frame (tear check script, to be written, row-pair difference profile); stopwatch digits advance by the VIDF timestamp difference +-1 LV frame |
| 4 | Fast copy: cacheable destination (pass the cacheable pointer to `FIO_WriteFile`, D9) and, as a second switch, cacheable source after a per-line invalidate of the crop rows | host: none meaningful (cache behaviour); G1-G3 for regressions | H4: on a frozen frame, copy by both methods, compare byte for byte, time both | identical data 20/20 times; copy rate >= 25 MB/s. If the data differs even once, keep the uncached source |
| 5 | Mode L at reduced crop, skip N: copy starts on the vsync tick + a tunable delay, RAM burst, write after | host: the step-3 simulator with freeze disabled, sweep copy rate and delay, confirm the tear-free region matches T3 | H5: moving target, sweep delay 0-40 ms in 5 ms steps, 640x360 then 960x540 | a delay range with zero torn frames exists and is >= 10 ms wide; else stay on mode F |
| 6 | Frame-ready signal: chain the Mem1Path completion slot `[0x1029C]` to a module function that only gives a semaphore, restore on stop and on module unload. One RAM pointer write | host: none; code review that the chained function is interrupt-safe (no logging, no blocking) | A1 check 7 first (`rd 0x1029C` = 0xE06AD51B), then count semaphore gives per second | gives/s == LV fps +-1 for 60 s, no asserts; then repeat H5 using the signal in place of the delay |
| 7 | Mode C: writer runs during capture, 16 MiB (4 AU) blocks, drop on ring full, clean end after N drops | host: T4 reproduced by the harness with a rate-limited fake card at 20/40/80 MB/s; time-to-full within 10% of `budget.py` | 60 s clip at 960x540 skip 1, then 1280x720 | dropped == 0 at 960x540; EvfDev queue count (`call 0xE05BA079 TC`, A2 check 4) stays below 20 throughout |
| 8 | Beyond safe-first, each needs its own RE first: (a) mem2mem EDMAC with A5's corrected select/enable stubs to replace the CPU copy; (b) find where the LV core takes `param[8]` and alternate two ML buffers there (the mk1 double-buffer idea without touching the EDMAC register); (c) hook EvfState 5/5 at `*(0x8E8C)` (core change, A2); (d) Canon Speed Class file mode (A7); (e) lower bit depth or lossless | per item | per item | - |

Steps 1-3 need nothing from Canon that has not already run on this camera except the flag toggle in step 3.
Step 2 can be skipped once step 3 works; it exists so that the ring and writer are proven before the one
new Canon behaviour is introduced.

## Next hardware checks

In this order; all before or alongside the step named.

- **H1 (step 0, read-only).** In movie-mode LV, before loading raw_vidx: A1 checks 1-4 and 6-8 (`rd 0x10278 0x30`,
  `rd 0x292B0 0x70`, ring counts ten times, LvJob slots, `rd 0x1029C 8`, `rd 0x22D9C 12`) and A2 checks 1, 2, 4, 5.
  Confirms or kills every address the health watchdog uses.
- **H2 (step 0).** `rvx_probe`: free memory and largest block; priorities of all tasks; the four timings.
  Then repeat the starvation question directly: run a 2 s busy loop at priority 0x11 with LV up and watch
  `[0x39B84]` and the UART for `TryPostEvent`; repeat at 0x1C. If 0x11 floods and 0x1C does not, the starvation
  explanation above is confirmed.
- **H3 (step 3, the key one).** With LV raw on and a moving target: `call lv_save_raw 0`, wait 100 ms, dump the
  buffer twice 1 s apart with `dump_img_task` (docs/35 route). Pass = the two dumps are byte-identical and
  neither is torn. Also `rd 0x10278 0x30` before/after to see +0x40, +0x58, +0x60 change. Then toggle 0/1
  200 times at 3 Hz and check for asserts and that the buffer address is unchanged.
- **H4 (step 4).** Cached-after-invalidate copy equals uncached copy on a frozen frame, 20 repetitions, with timing.
- **H5 (step 5).** Delay sweep for tear-free live copies at 640x360 and 960x540.
- **H6 (step 1/3).** Does `FIO_Flush` after a block make the file survive a power cut with the right length?
  Write 16 MiB, flush, cut power, inspect the card.
- **H7 (any step).** Three clips in a row without reboot: allocation still succeeds, no leak
  (`GetMemoryInformation` before and after equal).

## Limitations

- Design only. No module code was written, built or tested; the plan's offline tests are specified, not run.
  `tools/raw_vidx_hosttest` and `tools/mlvcheck` are other agents' work that I located but did not execute.
- Mode F rests on one unverified Canon behaviour (H3). If clearing the flag blanks, frees or keeps refreshing
  the buffer, mode F is dead and the first coherent clip has to wait for the fast copy (step 4) at a small crop.
- The starvation explanation for the recording aborts is an inference from A2/A3 and the code, not a
  measurement. The 0x1C priority and the sleep duty cycle are guesses until H2.
- Copy rates of 25 and 80 MB/s in the tables are assumptions. Only ~6 MB/s (uncached both sides, coarse) and
  ~80 MB/s (cached both sides) have been observed. The A13 copy-speed and A8 memory reports did not exist
  when this was written; the 32/48 MiB ring and all T2/T3 columns should be re-read against them.
- LV fps is taken as 23.976; the task states 24-25. All per-second figures scale linearly.
- Card speed 75 MB/s is one bench on one 16 GB card; no data for a filling or fragmented card.
- A mode F clip at 1-5 fps is a time-lapse, not video. It meets "decodable and correctly timed"; it does not
  meet "raw video" in the usual sense. That starts at step 5.
- The health-watchdog addresses and the Mem1Path slot come from static analysis by other agents (A1, A3) and
  have never been read on the camera.
