# 41 - The raw_vidx recording module: architecture, hardware history, known bugs

Written 2026-11-16 during an offline sprint. No camera was attached. Everything described as
"hardware" below comes from the UART log of earlier sessions (`tools/mlserial/run/uart.jsonl`,
2026-07-14 to 07-22); nothing in this document was re-run on the camera, and none of the fixes on
branch `offline/C4` has ever executed on it.

This document replaces the raw_vidx parts of the chat-memory notes
(`memory/m50ii-symbol-shift-boot-crash.md`, `memory/ml-m50ii-port-status.md`) and the daily notes
(`.remember/today-2026-07-18..22.done.md`). Where those notes and this document disagree, this
document is the corrected version; the reasons are in section 5 and in `docs/42-record-time-crashes.md`.

Terms used throughout:

- **ML**: Magic Lantern. **Core**: the `autoexec.bin` image built from `src/` and
  `platform/M50mk2.101/`. **Module**: a separately linked `.mo` file the core loads from the card.
- **LiveView (LV)**: the camera's live sensor preview. **LV raw**: the mode in which Canon's
  firmware also writes the undemosaiced sensor frame to a RAM buffer every frame; ML requests it
  with `raw_lv_request()`.
- **EDMAC**: Canon's DMA engine; a *channel* is one DMA stream.
- **MLV**: Magic Lantern's raw video container, a sequence of typed blocks (file header `MLVI`,
  metadata blocks, one `VIDF` block per frame).
- **vsync hook / CBR_VSYNC**: a callback ML invokes once per LV frame.
- **Attempt N**: row N of the recording-attempt table in `docs/offline/C7.md` section (4).
  **db boot N**: boot number in `tools/mlserial/run/mllog.db`, the numbering the old notes use.

Line numbers refer to branch `walkback-milestone`, commit `a10b16c23`, unless a different branch is
named.

## 1. Architecture as it stands (walkback-milestone a10b16c23)

### 1.1 Files

| File (`modules/raw_video/raw_vidx/`) | Lines | Role |
|---|---|---|
| `raw_vid.c` | 43 | module entry: creates the queue, calls `init_event_pusher`, `init_worker` (`raw_vid.c:20-27`) |
| `event_pusher.c` | 500 | producer: key-press and vsync callbacks, event pool, posts events |
| `worker.c` | 677 | consumer: one task that owns all session state, copies frames, drives the file |
| `mlv_3.c`, `mlv_3.h` | 794, 59 | MLV block writer; uses no file I/O itself |
| `raw_vid.h`, `raw_vid_worker.h` | 69, 33 | event types, queue length, priorities, buffer sizes, UART logging macros |
| `mlv_3_sample_code/` | - | host test programs for `mlv_3.c` |

`STRUCTURE.txt` in the same directory states the design intent: one DryOS message queue is the only
synchronisation between the asynchronous callbacks and a single worker task.

### 1.2 Tasks and callbacks

| Name | Kind | Priority (lower number = higher priority) | Stack | Created at | Does |
|---|---|---|---|---|---|
| `raw_v_worker` | task, permanent | 0x11 (`raw_vid_worker.h:16`) | 0x1000 | `worker.c:675` | receives events, copies each frame, starts flushes, opens and closes the file |
| `raw_write` | task, one per buffer flush | 0x1A (`raw_vid_worker.h:23`) | 0x800 | `worker.c:377` | one `FIO_WriteFile` of a full crop buffer (`worker.c:161-215`) |
| `stop_com` | task, transient | 0x12 (`WORKER_PRIO + 1`) | 0x800 | `event_pusher.c:102, 277, 381, 392, 488` | posts COMMAND_STOP without blocking a callback |
| `raw_vid_vsync_cbr` | callback `CBR_VSYNC` | runs in the caller's task | - | `event_pusher.c:310, 498` | builds one IMAGE_DATA event per frame |
| `raw_vid_keypress_cbr` | callback `CBR_KEYPRESS` | runs in the caller's task | - | `event_pusher.c:226, 499` | start/stop on `MODULE_KEY_LV` or `MODULE_KEY_REC` (`event_pusher.c:260`) |

Where the vsync callback runs (Observed in source and ROM, not at run time): the core calls
`module_exec_cbr(CBR_VSYNC)` at `src/state-object.c:98` from its state-object hook, which on this
port fires on `EVF_STATE` input 6 in state 3 (`src/state-object.c:225`), with
`EVF_STATE = *(0x10108)` (`platform/M50mk2.101/include/platform/state-object.h:7`). Report A2
shows that this object is Canon's `EvfDevState`, that its transitions run in Canon's `EvfDev` task
(priority 0x0E, 200-slot message queue, creation at ROM 0xE065B630-0xE065B640), and that input 6 in
state 3 is `evfdevUpdateParam`, not a readout-done event. So everything the vsync callback does
runs inside the EvfDev task and delays that task's queue.

Canon task priorities for comparison (report A11, single-source, not independently verified
except EvfCap and EvfDev; the Fwrite and MovWriter value 0x15 is traced from callers, not a
constant at the create site, D7-critic section 4 item 4): EvfCap 0x0D, EvfDev 0x0E, DAF and MovieRecorder
0x11, EIS 0x13, PropMgr 0x14, Canon's own bulk writers Fwrite/MovWriter 0x15, GuiMainTask 0x17,
FileMgr 0x19, DOSDriver 0x1A. The worker at 0x11 therefore outranks most of Canon's LV support
tasks while it copies; the writer at 0x1A outranks almost nothing.

### 1.3 Queue and events

- Queue `raw_vid_q`, 20 entries (`raw_vid.c:20`, `RAW_VID_Q_LEN` at `raw_vid.h:27`).
- Events come from a static pool of 20 `struct raw_vid_event` (`event_pusher.c:66`), guarded by
  `events_sem`; the pool lookup takes that semaphore with a 2000 ms timeout
  (`event_pusher.c:98`), which means the vsync callback can block for up to 2 s inside EvfDev.
- Event types (`raw_vid.h`): `COMMAND_START`, `COMMAND_STOP`, `IMAGE_DATA` (source pointer, source
  pitch, crop x/y/w/h, byte size). `EMPTY` and `PREPARING` are pool states and are never posted.
- Recording state lives in the worker: `ERROR`, `INACTIVE`, `ACTIVE` (`worker.c:19`). The vsync
  callback returns immediately unless the state is `ACTIVE` (`event_pusher.c:334` region, the
  `get_recording_state()` test).

### 1.4 Buffers

- **Crop buffers**: two, allocated at start with `fio_malloc`, 16 MiB each, reduced in 1 MiB steps
  down to 4 MiB if allocation fails, aligned to 0x1000 (`worker.c:57-137`,
  `raw_vid_worker.h:28-31`). They are addressed through the uncacheable alias
  (`worker.c:136-137`). The worker fills one; when the next frame does not fit it switches to the
  other and hands the full one to a `raw_write` task (`worker.c:315-377`).
- **Capture buffers**: two LV-frame-sized `fio_malloc` blocks allocated inside the vsync callback
  (`event_pusher.c:374, 384`). They exist for the original double-buffer design (redirect Canon's
  raw EDMAC channel to alternate buffers). With the default toggles they are allocated but never
  read.
- **Frame source**: with the default toggle the event's source pointer is
  `UNCACHEABLE(raw_info.buffer)` (`event_pusher.c:433-434`), i.e. the single buffer Canon writes
  every frame. Hardware logs show it at 0x6BC61CC8 with geometry 2096 x 1164, pitch 0xE54
  (`memory/m50ii-symbol-shift-boot-crash.md`, db boot 212). Reading a buffer Canon is rewriting
  means a copy slower than one frame period mixes two frames (tearing); this was accepted
  deliberately (`event_pusher.c:44-48`).
- **Crop**: x = 200, y = 100 (`event_pusher.c:421-422`), 1792 x 896 pixels, 14 bits per pixel
  (`BPP`, `raw_vid.h:29`). One frame is 1792 * 896 * 14 / 8 = 2,809,856 bytes; with the VIDF header
  (0x20) and padding (0x60) a 16 MiB buffer holds 5 frames = 14,049,920 bytes, the value the camera
  logged as `wbytes=0x00D66280` (C1, C4-review finding 2).
- **Copy**: a per-row `memcpy` in the worker task (`worker.c:431-435`), followed by a direct call
  of `edmac_cbr_w(NULL)`, which only gives `edmac_sem` back. The EDMAC copy
  (`edmac_copy_rectangle_cbr_start`) is no longer called.

### 1.5 Session sequence

Start (`worker.c:440-580`): allocate crop buffers; `raw_lv_request()` (`worker.c:480`); optional
"raw job ready" poll (`worker.c:494-510`, see 5.3); create `tstNN.mlv` with
`get_numbered_file_name("tst%02d.mlv", 99, ...)` and `FIO_CreateFile` (`worker.c:534-536`);
`start_mlv_session()` (`worker.c:557`); write the 572-byte header block set (`worker.c:571`); set
state `ACTIVE`.

Per frame: vsync callback posts IMAGE_DATA; worker writes a VIDF header into the crop buffer,
copies the crop, and flushes a full buffer through `raw_write`.

Stop (`worker.c:581-641`): seek to 0, rewrite the header with the frame count, `FIO_CloseFile`
(`worker.c:599-601`); only then `raw_lv_release()` (`worker.c:612`); free buffers;
`stop_mlv_session()` (`worker.c:641`). The close-before-release order was introduced after db boot
212, where a crash in the release path lost the only file with real frames.

### 1.6 Runtime toggles

All are plain `int` globals, changed from the serial shell with `wr &<name> <value>`.

| Toggle | Default | Where | Meaning |
|---|---|---|---|
| `rvx_src_from_raw_info` | 1 | `event_pusher.c:50` | 1: read `UNCACHEABLE(raw_info.buffer)`; 0: read the module's capture buffers (which the D8 redirect does not fill reliably) |
| `rvx_redirect_edmac` | 0 | `event_pusher.c:56` | 1: call `raw_lv_redirect_edmac()` every vsync (`event_pusher.c:476-477`); correlated with the assert storm, see doc 42 |
| `rvx_wait_raw_job` | 1 | `worker.c:24` | 1: poll two Canon RAM words before going ACTIVE; report A1 shows the poll tests constants (5.3) |
| `rvx_checkpoint` | 1 | branch `offline/C4` only, `worker.c:49` there | periodic header rewrite + close + reopen; reviewer recommends 0 for the first run (4.2) |

Branch `offline/C4` also adds two read-only counters, `rvx_dropped_queue` and `rvx_dropped_busy`.

### 1.7 Logging

`SEND_LOG_DATA_STR` and `RVX_HEX` write `[RVX] ...` lines through the core's direct UART writer
`m50ii_uart_str` (`raw_vid.h:12-22`). They do not go through Canon's `DryosDebugMsg`, so module
logging does not consume Canon's event containers or interrupt stack (A3, independent verification
item 11). Lines are not atomic and can interleave with Canon output.

### 1.8 File format as written

Observed by replaying the unmodified `mlv_3.c` and the `worker.c` logic on the host (report C1):

- Header set of 572 bytes: `MLVI` (version string `v3.0`, `file_flags = 4`), `RAWI` (0xB4), `RAWC`
  (0x20), `IDNT` (0x54), `EXPO` (0x28), `LENS` (0x60), `RTCI` (0x2C), `WBAL` (0x2C).
- One `VIDF` per frame: 0x20 header, 0x60 padding, 2,809,856 bytes of packed 14-bit data.
- `RAWI` describes the 2096 x 1164 source while frames are 1792 x 896; `mlv_dump` compensates
  (mlv_lite does the same).
- The frame rate field comes from `fps_get_current_x1000()` (`worker.c:553`). Report C4c argues
  that the core's timer base for this body is wrong (32 MHz instead of 27 MHz), so the recorded
  rate is probably wrong; C9-recommended takes LV as 25.0 fps. Not measured on hardware.
- Host `mlv_dump` decodes these files and the extracted DNGs match the source crop (C1 section 4).
  That proves the writer code, not the camera.

### 1.9 Build and shipping state

- `platform/M50mk2.101/modules.included` on this branch lists `bench`, `file_man`, `mlv_lite`,
  `raw_vidx`. Report C8 recommends removing `mlv_lite` (never load both recorders).
- A platform `make` does not refresh the staged copies of `raw_vidx.mo`; take the module from
  `modules/raw_video/raw_vidx/build/` (C4, "Trap").
- The module's md5 is not reproducible between builds because it embeds build strings
  (C4-review notes); do not use the md5 as identity.

## 2. Hardware history, attempt by attempt

Source: `docs/offline/C7.md` section (4), which lists 47 recording attempts (32 with `mlv_lite`, 15
with `raw_vidx`). Frame counts are lower bounds because module logging is rate-limited from module
build 81fd5d54 on. The "module state" column is taken from the memory notes; the UART log does not
record which module binary was on the card, so that column is Inferred, not Observed.

| # | UTC | db boot | Module state per the notes | Observed result (C7) |
|---|---|---|---|---|
| 33 | 07-18 22:40 | 196 | first raw_vidx build, async EDMAC copy, capture-buffer source | no frames, no abort |
| 34 | 07-18 22:41 | 196 | same | no frames; `couldn't take edmac_sem` x9, `crop_buf was NULL` x159; log goes silent |
| 35 | 07-19 00:05 | 203 | source changed to `raw_info.buffer` ("Change C"), CBR seed still on | MvRawProDs/RawPro/LvJob assert storm x8, log goes silent |
| 36 | 07-19 00:29 | 206 | seed switched off at run time | 1 frame copied (2,809,856 B); `edmac_sem` x9, `crop_buf NULL` x497; no abort, camera stays up |
| 37 | 07-19 04:00 | 207 | synchronous in-module copy (build 81fd5d54) | one `ImageController.c:931` assert, log goes silent |
| 38 | 07-19 04:05 | 209 | same | storm x6, log goes silent |
| 39 | 07-19 04:07 | 210 | same | data abort `pc=0x000E855E lr=0xE016402F` at once |
| 40 | 07-19 04:23 | 212 | same, `raw_lv_redirect_edmac` patched to return at run time | **5 frames copied, 14,049,920 B written**; queue full, self-stop; `EvfDevState.c:372/542` x32; abort `pc=0 lr=0xE05D1161` 0.4 s later. The file did not survive (no close before the crash) |
| 41 | 07-19 04:26 | 213 | same patch | storm x6, log goes silent |
| 42 | 07-19 10:50 | 215 | build 7ae71fbe: redirect off by default, close before release | 5 frames, 14,049,920 B written; EvfDevState x32; abort `pc=0 lr=0xE05D1161` |
| 43 | 07-19 10:56 | 217 | same | 5 frames, 14,049,920 B written; EvfDevState x32; abort `pc=0xE05B9FDA` |
| 44 | 07-19 12:22 | 220 | not recorded | storm x9, log goes silent |
| 45 | 07-19 13:07 | 224 | build with the "ready" poll | poll reports ready; 5 frames; no write logged; EvfDevState x32; abort `pc=0 lr=0xE05D1161` |
| 46 | 07-19 13:25 | 226 | not recorded | nothing copied, no abort |
| 47 | 07-22 00:58 | 367 | walkback core + module with all fixes of that date | poll reports ready; 5 frames; no write logged; one `Task.c:229` assert; abort `pc=0xE005DFA2` |

What the table establishes (Observed):

1. Six of 15 attempts copied frames; three logged a completed 14,049,920-byte card write.
2. Four attempts ended in the assert storm with no frames (35, 38, 41, 44).
3. Every attempt that copied 5 frames ended in an abort within 1 s of the last module log line.
4. `MLV closed (finalized)` appears 0 times in the UART history (C1 section 3). No raw_vidx file
   is known to have survived on a card, and none has been decoded.
5. Copy cadence: 20 frame intervals in 9.84 s, about 2.0 frames per second, about 5.7 MB/s
   (C7 section (5), A13). Card write: 42 MB of data in 1.00 s pooled over three writes, with an
   error of one 0.2-0.4 s host timestamp batch per write, so the true rate lies anywhere between
   about 17 and 280 MB/s. A7 estimates the card path at about 75 MB/s from firmware analysis.

Corrections to the memory notes:

- The note "redirect-off validated, storm = 0 (boots 214/217)" rests on two attempts (42, 43).
  Attempt 44, about 90 minutes later, stormed again. Whether attempt 44 ran a module with the
  redirect on is not recorded. "Redirect off kills the storm" is therefore not established.
- The note "poll fix: expect no storm" rests on two attempts (45, 47). The poll cannot have had an
  effect (5.3), so the absence of a storm in those two attempts is not evidence for the fix.
- Attempt 45 shows the EvfDevState burst with no card write logged. The note that the burst is
  triggered by the card write starving EvfDev (write task priority 0x9) does not cover that
  attempt. See doc 42 section 2.

## 3. Known bugs in a10b16c23

Found by host replay or code reading in this sprint; none observed directly on hardware except
where stated.

| Id | Bug | Evidence | Status on `offline/C4` |
|---|---|---|---|
| B1 | A clean stop never flushes the partly filled crop buffer; a clip shorter than 6 frames is a 572-byte header-only file, and the header frame count includes the lost frames | C1 "Mismatches", reproduced on host (`crop_camstop.mlv`: header says 8, file holds 5); consistent with the three hardware writes all being exactly one full buffer | fixed, `0530d6c7d` |
| B2 | VIDF timestamps are absolute uptime (`get_us_clock()`), all other blocks are relative to session start | C1 | not fixed on C4 (report C10 lists the C1 writer bugs as fixed on its integration branch) |
| B3 | Crop and pan offsets are never written; VIDF says crop 0,0 although the crop starts at 200,100 (`worker.c:30-33`) | C1 | not fixed on C4 |
| B4 | A frame is counted and its header written before `take_semaphore_now(edmac_sem)`; on failure a counted frame holds stale data | C1, C4-review finding 6; unreachable with the synchronous copy | open |
| B5 | `alloc_crop_bufs()` sets `end = start + buf_size` after aligning `start` up, overrunning the block by up to 0xFFF bytes | C1; not reachable with 5 frames per buffer | open |
| B6 | COMMAND_STOP rewrites the header and closes without waiting for a running `raw_write` task | C1 | fixed on the normal path (`0530d6c7d`), still present on timeout paths (C4-review finding 3) |
| - | The event taken on the first vsync is never posted or released | C4 commit table | fixed, `81b7ff105` |
| - | When the other crop buffer is still being written, the worker sets `crop_buf = NULL` and silently discards every later frame (`worker.c:335-340`); a failed post spawns a stop and leaks the event | C4; matches the hardware pattern "5 frames then queue full, self-stop" | fixed, `a1f74a6e0` (drop and count) |
| - | A failed start leaves `crop_buf` non-NULL, so every later start is rejected until reboot | C4 | fixed, `ba29d4865` |
| - | `prevent_recording` is set by every stop and cleared only at module init: a second recording in one boot receives no frames | C4; consistent with attempt 46 | fixed, `01bc47079` |
| - | `video_frame_count` is never reset between sessions | C4 | fixed, `0530d6c7d` |
| - | Capture buffers (about 8.6 MB) stay allocated when no vsync sees the stop | C4 host test | fixed, `f8191a47a` |
| - | Stale comment about a `m50ii_seed_*` symbol that no longer exists in the core | C4 | fixed, `6a74a067a` |

Design problems that are not coding bugs (see doc 42 for the evidence):

- **Copy speed.** The crop x offset of 200 pixels makes each source row start at a byte offset of
  350, which is 2 modulo 4, so the linked `memcpy` runs its byte loop; both source and destination
  are uncached. Report A13 derives about 5.7 MB/s from the logs against a need of 67.4 MB/s for
  1792 x 896 x 14 bit at 24 fps. A13 proposes x = 192 or 208 as a module-only test.
- **Hook object.** The vsync hook sits on EvfDevState input 6, state 3, which is a set-parameter
  event (A2; F1 adds a selectable alternative).
- **Work inside the hook.** The vsync callback calls `fio_malloc`/`fio_free`
  (allocation at `event_pusher.c:374, 384`), may block 2 s on `events_sem` (`event_pusher.c:98`) and logs to the
  UART, all inside Canon's EvfDev task (C4-review finding 9, A2).
- **Priorities.** The copy runs at 0x11, above Canon's EIS, PropMgr, Imager and file tasks; the
  writer at 0x1A is the lowest-priority party of the pipeline (A11, not independently verified).
- **"Ready" poll.** Tests constants (5.3).

## 4. Branch offline/C4

### 4.1 What it contains

Worktree `/mnt/data/WORK/eos/wt/C4`, 8 commits on `a10b16c23` (`81b7ff105` .. `f8191a47a`), changing
only `worker.c`, `event_pusher.c` and `raw_vid_worker.h` in the module (357 insertions, 97
deletions). The fixes are listed in section 3. In addition:

- `write_sem` serialises the worker and the write task; stop waits for the running write, writes
  the remaining buffers oldest first through the low-priority write task, then finalises and closes
  before `raw_lv_release()`. The header count comes from `frames_on_disk`, advanced only after a
  complete `FIO_WriteFile` (`0530d6c7d`).
- Checkpoint (`c55b53e4d`, toggle `rvx_checkpoint`, default 1): after each full buffer, rewrite the
  header with the current count, close, reopen with `FIO_OpenFile(O_RDWR|O_SYNC)` and seek to the
  end, so that a crash leaves a decodable file.
- The module imports one more core symbol, `FIO_OpenFile` (0xcf555 in the baseline `.sym`).
- Host behavioural test `tools/raw_vidx_hosttest/run.sh <module source dir>`: 68 checks pass on
  C4; 9 of 10 tests fail on unmodified `a10b16c23` (C4 "Host test"). The test is single-threaded
  and its semaphores cannot time out.

### 4.2 Review result (C4-review)

The eight commits are correct on their normal paths and the scope is clean. The reviewer's verdict
is **not ready to flash as is**:

1. HIGH: after one `write_sem` timeout the recording drops every frame until stop
   (C4 `worker.c:516-520` with `463-478`). Reachable once the checkpoint makes the write task hold
   the semaphore for more than 1 s.
2. HIGH: the default-on checkpoint puts seek + header write + close + open + seek on the write path
   every 5 frames (about 5 times a second at 24 fps), on a card path that the logs show needing
   0.2-0.4 s per buffer already; `FIO_SeekSkipFile(fp, 0, SEEK_END)` is unchecked, and if it does
   not land at the end the next buffer overwrites the header. Recommendation: `rvx_checkpoint = 0`
   for the first hardware run, or checkpoint on a time interval.
3. MEDIUM: on a stop timeout the worker still closes the file and frees buffers under a running
   write.
4. MEDIUM: the result of `task_create` is ignored while `write_sem` is held.
5. MEDIUM: a short card write leaves a truncated block in the middle of the file.

Report C10 describes an integration branch (`offline-integrate1`) that claims to deal with every
C4-review finding and the C1 writer bugs; use C10, not this section, for the state of that branch.

### 4.3 Core layout note

Built on a branch named `offline/C4`, the core `.sym` differs from the baseline in 53 symbols by 8
bytes, because the Makefile embeds the branch name in the image. Built on an 18-character branch
name the `.sym` is byte-identical to the baseline (md5 `d2f46ba5e091fa02d53a8aae9969442f`). The
module fixes themselves move no core symbol. Details in `docs/43-boot-stability-and-layout.md`
section 5.

## 5. Established, inferred, open

### 5.1 Established (Observed)

- The module can copy real sensor data into its buffers and complete a 14 MB `FIO_WriteFile` on
  this camera (attempts 40, 42, 43; pixel values and geometry in the memory note for db boot 212).
- No attempt ended cleanly and no file was recovered.
- The host writer path produces files `mlv_dump` decodes correctly (C1).
- The bugs of section 3 marked "reproduced on host" exist in the source.

### 5.2 Inferred

- The "5 frames then queue full" pattern is the combination of a 2 frames/s copy, the
  `crop_buf = NULL` stall and the 20-entry queue, not a card-speed limit (C4, C7, A13).
- The crashes after frames were copied are caused by Canon tasks being starved or delayed by the
  module (copy at 0x11 and work inside the EvfDev hook) until Canon's event queues and its shared
  event-container pool run out (A2, A3, A11, C9-recommended). See doc 42.

### 5.3 Withdrawn

- **"Canon builds the movie-raw job asynchronously; poll `*(*(0xE07B9ED4)+0x30) == 1` and
  `*(*(0xE0761240)+0x30) != 0` before consuming"** (`worker.c:484-510`). Report A1, confirmed by
  its independent verification (claim 7): the first word is an argument stored at creation time
  (ROM 0xE07B9B58), the second is an EDMAC channel number (ROM 0xE0760A9C). Both are constant from
  LV start. The poll carries no information.
- **"The CPU copy runs off the capture path, so it is safe"** (`worker.c:420` comment). It is off
  the vsync callback, but at priority 0x11 it still delays Canon tasks (A11).
- **"Write priority 0x9 to 0x1A fixes the EvfDevState flood."** The 0x9 value was above EvfDev
  (A11 confirms the ordering), but the flood also appears in an attempt with no write logged (45).

### 5.4 Open

- Whether any raw_vidx file decodes when written by the camera.
- Which frame-ready signal to use: EvfDevState 6/3 (current), EvfState input 5 at `*(0x8E8C)`
  (A2, F1), or the EDMAC channel-3 completion callback slot in Mem1Path (A1).
- Copy speed after the alignment fix and through the cacheable alias (A13; all rates except
  5.7 MB/s are estimates).
- Cost of the checkpoint on the real card (C4-review finding 2).
- The actual LV frame rate and the correct timer base (C4c).

## 6. Pointers

| Topic | Report |
|---|---|
| Attempt table, rates, assert bursts | `docs/offline/C7.md` (regenerate with `tools/logmine/regen.sh`) |
| Module fixes and host test | `docs/offline/C4.md`, `tools/raw_vidx_hosttest/` |
| Review of the fixes | `docs/offline/C4-review.md` |
| Host MLV lane, writer bugs B1-B6, validator | `docs/offline/C1.md`, `tools/mlvcheck/` |
| Integration branch for the next hardware session | `docs/offline/C10.md` |
| mlv_lite versus raw_vidx | `docs/offline/C8.md` |
| Recording design for the next steps | `docs/offline/C9-recommended.md` |
| Copy speed | `docs/offline/A13.md` |
| LV raw path, Mem1Path, completion rings | `docs/offline/A1.md` |
| EvfDevState versus EvfState, hook alternatives | `docs/offline/A2.md`, `docs/offline/F1.md` |
| Task priorities | `docs/offline/A11.md` |
| Record-time crashes | `docs/42-record-time-crashes.md` |
| Boot reliability and core layout | `docs/43-boot-stability-and-layout.md` |
