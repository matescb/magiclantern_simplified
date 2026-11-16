# C9-throughput-first - Recording architecture for raw video on the M50 II

Offline design, 2026-11-16. No camera attached. Nothing here was run on hardware; every Canon address
below comes from static analysis in the sibling reports (A1, A2, A5, A7, A8, A13) or from the UART
history, and is marked with its status.

## Goal

A concrete, buildable plan for `modules/raw_video/raw_vidx` that reaches **sustained continuous**
recording at the highest practical resolution, module-only (no ML core change), with the firmware
unknowns that gate each stage named and ordered.

## Verdict in five lines

1. The card, not the camera, sets the ceiling: SDR104 is already negotiated and the ML bench measured
   75 MB/s write (A7). Full 2096x1164 14-bit at 23.976 fps is 102.4 MB/s, above even the bus limit
   (about 93 MB/s). It cannot be continuous on this body by any architecture.
2. The highest continuous 14-bit modes that fit 70-75 MB/s are **2096x786 (69.1 MB/s, full sensor
   width, zero-copy)** and **1792x896 (67.5 MB/s, hardware crop)**. Both have under 10 % margin.
3. A per-frame CPU copy cannot carry those modes: the bench memcpy is about 80 MB/s (A7), so a
   67 MB/s stream would use 84 % of one core at a priority above most Canon tasks. The copy has to go.
4. Target architecture: one ring of frame slots and one resident writer, with a pluggable *fill
   source*: F0 cached CPU copy (bring-up, up to about 40 MB/s), F1 EDMAC mem2mem crop on ML's own
   channels, F2 buffer flip (Canon's channel 3 writes straight into ring slots, no copy at all).
5. Stay on raw_vidx. Its queue-based structure is the right base, a host test harness already exists
   for it (`tools/raw_vidx_hosttest`), and mlv_lite's slot logic is tied to the EDMAC redirect and
   SRM paths that are the open questions here.

## What was built

| path | what |
|---|---|
| `/mnt/data/WORK/eos/tools/C9-throughput-first/ringsim.py` | frame-budget arithmetic and a discrete-time ring/writer simulation (drops, stalls). `python3 ringsim.py` prints the tables below; `python3 ringsim.py --test` runs the self-test |
| `/mnt/data/WORK/eos/sim/C9-throughput-first/tables.md` | captured output of the run used in this report |
| this file | the design |

No ML source was changed, no worktree was created and nothing was built for the camera.

```
$ python3 ringsim.py --test
selftest OK
```

The self-test checks the frame sizes against known values (4,269,552 B = `first_raw_capture.raw`,
2,809,856 B = current crop), slot counts, and that the analytic overflow time agrees with the simulation.

## Results

The design below is the result; the executed part is the budget and simulation output quoted in
"Frame budget". Sections: inputs, data flow, slot layout, sizing, tasks, Canon calls, frame budget,
failure handling, unknowns, implementation plan.

### Inputs used (and how solid each is)

| input | value | source | status |
|---|---|---|---|
| LV raw frame | 2096x1164, 14-bit packed, pitch 3668 B, 4,269,552 B | docs/35 | measured on hardware |
| Canon buffer | 0x6BC61CC8 (uncached alias), 0x2BC61CC8 (cacheable) | docs/35, A13 | measured |
| raw path | Mem1Path (ctx 0x10238) re-programs EDMAC write channel 3 each frame from `param[8]` | A1 | static only |
| current copy | 5.7 MB/s; byte loop because source rows are 2 mod 4, both ends uncached | A13 | rate measured (coarse), cause inferred |
| memcpy / memcpy64 | 81 / 82 MB/s (ML bench, cacheable buffers) | A7 quoting UART log | measured |
| card write | 75.1 / 75.7 MB/s, 16 MB buffers, "SD16G" FAT32 card, SDR104 | A7 quoting UART log | measured, short run, one card |
| write cap per block-device call | 0xF800 sectors = 31 MiB; per-drive semaphore held for the whole call | A7 | static only |
| FSU buffer rule | uncached address accepted; cacheable must be 32-byte aligned; rejected writes are silently skipped | A7 | static + log lines |
| shoot memory | about 63 MB in movie LV; taking all of it correlated with an AE deadline storm | docs/32 via A8 | measured once |
| proven allocation | 2 x 16 MB `fio_malloc` plus 2 x 4.27 MB during the boot-212 recording | memory note | measured (about 40 MB total) |
| Canon LV tasks | EvfCap priority 0x0D, EvfDev 0x0E, 200-deep queues, about 3 EvfDev messages per frame | A2 | static only |
| vsync hook | runs inside the EvfDev task (input 6, state 3 = evfdevUpdateParam) | A2 | static + behaviour |

The task brief gives card speeds of 20/40/80 MB/s as assumptions. The tables use those; note that the
only measurement is 75 MB/s, so the "80" column is slightly optimistic for the card that was tested.

## Data flow

```
 sensor -> Canon LV pipeline -> EDMAC write ch3 (Canon-owned, never reconfigured by ML)
                                     |
          F2 flip:  ch3 target = slot[k] frame area  (Canon's DMA fills the ring directly)
          F0/F1:    ch3 target = Canon default buffer 0x6BC61CC8
                                     |
   frame-complete event (per frame, O(1), never blocks)
     source A: vsync CBR in EvfDev task (today)       source B: chained Mem1Path ch3-complete CBR
                                     |
        +----------------------------+-----------------------------+
        | F0: rvx_fill task          | F1: rvx_fill task           | F2: no fill task
        | dcache-invalidate range,   | program ML's own EDMAC      | verify sentinel, pick next
        | word-aligned cached copy   | pair (rd ch46 -> wr ch13),  | free slot, arm it as the
        | of ROI into slot[k]        | wait own completion sem     | next ch3 target
        +----------------------------+-----------------------------+
                                     |
                 slot[k] = { VIDF header | pad | payload }  state FILLED
                                     |
              write queue (DryOS msg queue, 32 entries, slot index + frame number)
                                     |
   rvx_wr (resident, low priority): FIO_WriteFile(uncached, 512-aligned, 4 KiB multiple)
        F0/F1: one call per run of memory-contiguous slots (up to 16 MiB)
        F2:    one call per frame (2.9 - 4.3 MB), rows y0..y0+h of the slot
                                     |
              free ring (lock-free SPSC of slot indices) -> back to the producer
                                     |
                 B:/ ... .MLV, .M00, .M01 ... (chunked, each chunk closed when full)

   rvx_ctl (existing worker task): START / STOP / ERROR state machine only, own 8-entry queue
   rvx_wd  (inside rvx_ctl, 100 ms tick): health counters -> throttle or emergency stop
```

## Slot layout

All slot addresses are the uncached alias (0x4xxxxxxx). Writes go to the card from the uncached
pointer, so `FIO_WriteFile` skips its whole-cache `sync_caches()` (docs/35 pitfall applies the other way
round here: nothing in a slot is ever written through the cacheable alias, so there is nothing to sync).

**Copy / DMA slot (F0, F1)** - size = block size, slots packed back to back in one allocation:

```
 +0            VIDF header (32 B)
 +32           frameSpace pad up to +0x200
 +0x200        payload (w*h*bpp/8), start is 512-aligned (EDMAC wr and FSU both satisfied)
 ...           tail pad to a 4 KiB multiple
```
Consecutive queued slots are contiguous in memory and in the file, so the writer merges them.

**Flip slot (F2)** - size = 0x1000 + align_up(4,269,552, 0x1000) = 4,276,224 B:

```
 +0x0000       head-room page (only used when y0 = 0)
 +0x1000       F = frame base given to channel 3 (Canon's own buffer is only 8-byte aligned, so
               any base works); Canon writes all 1164 rows here
   F + y0*3668 - k    VIDF header, written by ML after the frame is complete. It overwrites the
                      tail of row y0-1, which is not recorded. k = 32 + frameSpace, chosen once per
                      session so this address is 512-aligned.
   F + y0*3668        first recorded row
   F + (y0+h)*3668    end of payload; the block is extended into the following rows up to a
                      4 KiB multiple (those bytes are padding in the file)
```
Vertical crop costs nothing in flip mode: the writer just writes fewer rows. Horizontal crop is not
possible without a copy, so flip modes are always 2096 wide (including Canon's border columns; MLV
`RAWI` describes the active area).

**Sentinel.** When a slot returns to the free ring, ML stores a 16-byte magic at the middle of the
first and of the last recorded row. At frame-complete both must be gone. First gone, last present =
torn frame, drop it. Both present = the DMA never landed here (the "stale frame" failure seen with
mem2mem): drop, count, and after 3 in a row stop the fill source and fall back. This makes F1 and F2
self-verifying on hardware without pulling the card.

## Buffer and queue sizing

Ring budget: **40 MiB default** (the amount already allocated at once during the boot-212 recording),
48 MiB as a stretch setting, never the whole pool (A8: grabbing all shoot memory reproduced the AE storm).
Allocation is done in `rvx_ctl` at START, never in the hook: try one contiguous block, fall back to
2 x 20 MiB, then 16 MiB steps as `alloc_crop_bufs()` does today. Slots never straddle allocations.

Arithmetic (from `ringsim.py`):

- flip slot 4,276,224 B -> 41,943,040 / 4,276,224 = **9 slots**, 2 reserved (one being filled by
  Canon, one armed as the next target) -> 7 queueable = 0.29 s of footage.
- 1792x896 copy/DMA slot 2,813,952 B -> **14 slots**, 1 reserved -> 13 queueable = 0.54 s.
- 1280x720 slot 1,613,824 B -> 25 slots, 24 queueable = 1.0 s.

Queues:

| queue | type | length | why |
|---|---|---|---|
| free ring | lock-free single-producer single-consumer array of slot indices | 32 (power of two >= max slots 31) | hook or ISR must be able to take a slot without any semaphore |
| write queue | DryOS `msg_queue` | 32 | longer than the slot count, so a post can never fail for lack of room |
| control queue | DryOS `msg_queue` (today's `event_q`) | 8 | START / STOP / ERROR only. Frames no longer share it, so STOP cannot be starved by a full frame queue (a failure the comments in `event_pusher.c` record from hardware) |

The 20-entry `events[]` array, its semaphore (`take_semaphore(events_sem, 2000)` is a blocking call
inside the EvfDev task today) and the per-flush `task_create("raw_write")` all go away.

## Tasks and priorities

DryOS: lower number = higher priority. Canon: EvfCap 0x0D, EvfDev 0x0E (A2, static).

| context | priority | CPU | does | must not |
|---|---|---|---|---|
| frame-complete hook (vsync CBR in EvfDev, later the chained ch3-complete CBR in interrupt context) | 0x0E / ISR | 0 | read counters, pop free ring, write one MMIO word (F2), `msg_queue_post` | take semaphores, allocate, log to UART, copy pixels |
| `rvx_fill` (F0/F1 only) | 0x12 on CPU1 if affinity works (U8), else 0x15 on CPU0 | 1 or 0 | F0: invalidate + copy; F1: program own EDMAC pair, wait own semaphore (bounded 30 ms) | touch channel 3, 0x29, 0x4C or any Canon CBR |
| `rvx_ctl` (today's `raw_v_worker`) | 0x11 | 0 | START/STOP state machine, file create/close, chunk roll, watchdog tick | per-frame work |
| `rvx_wr` (resident) | 0x1A | 0 | `FIO_WriteFile` only | hold the drive semaphore longer than about 250 ms (cap one call at 16 MiB) |

CPU share of F0 on one core = stream rate / copy speed. At the 80 MB/s bench figure: 1280x720 = 48 %,
1792x896 = 84 %. That is why F0 is a bring-up stage and not the target, unless CPU1 is usable.

## Canon calls and addresses used

Status key: HW = exercised on the camera in the UART history; ST = static analysis only; UNV = never
checked. All ROM functions are Thumb (call with the odd address). None of these needs a core change:
the module calls ROM addresses through its own function-pointer constants.

| stage | call / address | use | status and what is still unverified |
|---|---|---|---|
| all | `raw_lv_request()` -> `call("lv_set_mm",1)`, `call("lv_save_raw",1)` | arm Canon's raw path | HW. A1: these only set Mem1Path flags; whether the LV core supplies `param[8]` in every video mode is UNV |
| all | `raw_lv_release()` | disarm | HW, but every recording so far ended in an EvfDevState burst within 1 s of the last write (C7); cause not isolated |
| all | `msg_queue_create/post/receive` | queues | HW |
| all | `FIO_CreateFile`, `FIO_WriteFile`, `FIO_SeekSkipFile`, `FIO_CloseFile` | file | WriteFile of 14 MB HW. A clean close after a recording has never been observed. 512-aligned uncached pointer accepted: ST (A7) |
| all | `fio_malloc` (2 x 16 MB) | ring memory | HW at that size; 40 MiB as one or two blocks UNV |
| all | `get_us_clock` | timing | in core `.sym`; accuracy UNV |
| all | Canon write-time word `*(0x640C)` (microseconds of last FSUBlockWrite) | free write-speed probe | ST (A7) |
| watchdog | `*(0x23804)`, `*(0x292B0)` ring counts (healthy 0 or 1); six LvJob slots at 0x50AF8 + n*0x188 | detect the MvRawPro/RawPro/LvJob overflow before it storms | ST (A1) |
| watchdog | EvfDev pending count: `rd 0x9B48` -> D, `rd D+8` -> TC, `0xE05BA078(TC)` | detect EvfDev queue growth toward 200 | ST (A2) |
| F0 | `0xE05D088C(addr, size)` d-cache invalidate by range | read Canon's buffer through 0x2BC61CC8 | ST (A13). Callable from task context, effect on the other core, and real copy speed all UNV |
| F0 | `memcpy64` (core symbol) | word copy, needs 4-byte aligned pointers | HW (bench). ROI x must make the row offset a multiple of 4 (x = 192 gives 336 B) |
| F0 | `task_create_ex` (0xE05F42D2) with a CPU argument | run the copy on CPU1 | in core `.sym`; argument order and what Canon runs on CPU1 UNV |
| F1 | `0xE05B7B42` prepare channel, `0xE05B848E` set address, `0xE05B849A` set geometry, `0xE05B88FC` transfer mode, `0xE05B802E` EDMAC boomer select (ch13, 0x373F0126), `0xE05B80C2` disconnect, `0xE05B7A6C` / `0xE05B7B08` enable / disable leaf, `0xE05B7C48` / `0xE05B7F68` register / unregister complete CBR (own channels 13 and 46 only) | hardware crop copy | ST (A5). The core's `edmac_select_boomer` 0xE05B752E is the wrong function (it is an Outer.c port select); the module must bypass the core's `edmac-d8.c`. Correct start sequence for a mk2 write channel, resource-lock ids and achievable MB/s are UNV. Earlier "stale frames + storm" was obtained with the wrong function |
| F2 | channel 3 address register `0xD04202A0` (reg base 0xD0420200 + 0xA0, = `raw_lv_edmac->ram_addr`) | read back which buffer Canon programmed; in mechanism M2/M3 also written | read HW; write HW-tested once, un-phased, and it stormed |
| F2 | `*(0x10290)` Mem1Path +0x58, Canon's record of the address it programmed this frame | slot identity | ST (A1) |
| F2 | `*(0x1029C)` / `*(0x102A0)` Mem1Path complete-CBR slot and argument (expected 0xE06AD51B) | chain a module function to get an exact per-frame "write complete" event | ST (A1). Runs in interrupt context. Whether anything re-registers the slot at mode change is UNV |
| F2 | the upstream variable that feeds `param[8]` | let Canon program channel 3 with ML's slot by itself | not located (U1) |

Hard rule kept from A1: the module never calls RegisterEDmacCompleteCBR, reset, abort or select on
channel 3, 0x29, 0x4C or the channel number found at `*(0x292B0+0x30)`.

### How the flip is commanded (F2), in order of preference

- **M1 - feed Canon's own programming.** Overwrite the RAM word the LV core passes as `param[8]`.
  Canon then calls its own `edmac_set_address(3, ...)` at its own time. No MMIO race, no CBR change.
  Needs U1.
- **M2 - write 0xD04202A0 from the chained ch3-complete CBR.** The transfer of frame N has just
  ended, so the channel is idle. Whether Canon's per-frame `Mem1Path_SetParam` for frame N+1 runs
  before or after this point decides whether the write sticks. Needs U2.
- **M3 - write 0xD04202A0 from an EvfState `evfReadOutDoneInterrupt` hook** (state object `*(0x8E8C)`,
  input 5), installed by the module on that object. This is the M50 mk1 timing. Needs U3.

In every mechanism the module does not assume where a frame landed: the completed slot is identified
by reading back `*(0x10290)` / 0xD04202A0 and confirmed by the sentinel. The flip latency (1 or 2
frames) is therefore measured, not guessed, and the "2 reserved slots" figure covers the worse case.

The existing evidence against redirecting is one configuration: the write was issued from the EvfDev
6/3 hook (not a readout-done event, A2), in builds that also had the CBR seed or the blocking in-hook
work, with n = 2-4 attempts (C7). The brief says to treat the storm as fact and this plan does: F2 is
the last stage, it is gated on read-only probes, and it auto-reverts. But it is the only route to
full-width recording without a copy, so it stays in the plan.

## Frame budget

Rate = block size x fps, block = VIDF header + pad + payload rounded to 4 KiB. A cell shows either
"continuous" or the seconds until the first dropped frame with a 40 MiB ring. "gate" names the
firmware unknown the mode depends on.

#### 23.976 fps, ring 40 MiB

| mode | source | payload B | block B | rate MB/s | ring slots (queueable) | card 20 MB/s | card 40 MB/s | card 80 MB/s | gate |
|---|---|---|---|---|---|---|---|---|---|
| 2096x1164 14b | flip | 4269552 | 4272128 | 102.4 | 9 (7) | 0.4 s | 0.5 s | 1.3 s | - |
| 2096x1080 14b | flip | 3961440 | 3964928 | 95.1 | 9 (7) | 0.4 s | 0.5 s | 1.8 s | - |
| 2096x896 14b | flip | 3286528 | 3289088 | 78.9 | 9 (7) | 0.4 s | 0.6 s | continuous | - |
| 2096x786 14b | flip | 2883048 | 2883584 | 69.1 | 9 (7) | 0.4 s | 0.7 s | continuous | - |
| 1920x1080 14b | dma | 3628800 | 3629056 | 87.0 | 11 (10) | 0.5 s | 0.8 s | 5.2 s | - |
| 1792x896 14b | dma | 2809856 | 2813952 | 67.5 | 14 (13) | 0.8 s | 1.3 s | continuous | - |
| 1280x720 14b | copy | 1612800 | 1613824 | 38.7 | 25 (24) | 2.1 s | continuous | continuous | - |
| 2096x1164 12b | dma | 3659616 | 3661824 | 87.8 | 11 (10) | 0.5 s | 0.8 s | 4.7 s | U6 pack mode |
| 2096x1164 10b | dma | 3049680 | 3051520 | 73.2 | 13 (12) | 0.7 s | 1.1 s | continuous | U6 pack mode |
| 1920x1080 10b | dma | 2592000 | 2592768 | 62.2 | 16 (15) | 0.9 s | 1.8 s | continuous | U6 pack mode |
| 2096x1164 14b lossless (ratio 0.57 assumed) | dma | 2433644 | 2437120 | 58.4 | 17 (16) | 1.0 s | 2.1 s | continuous | U7 lossless |

At 25 fps every rate is 4.3 % higher: 2096x896 14b becomes 82.2 MB/s and is no longer continuous at
80 MB/s (10.3 s); 1280x720 becomes 40.3 MB/s and overflows a 40 MB/s card after 112 s. A 48 MiB ring
adds 2-3 slots and changes no verdict (full tables in `sim/C9-throughput-first/tables.md`).

#### Highest continuous frame rate per mode

| mode | block B | fps at 20 MB/s | fps at 40 MB/s | fps at 70 MB/s | fps at 75 MB/s | fps at 80 MB/s |
|---|---|---|---|---|---|---|
| 2096x1164 14b | 4272128 | 4.7 | 9.4 | 16.4 | 17.6 | 18.7 |
| 2096x1080 14b | 3964928 | 5.0 | 10.1 | 17.7 | 18.9 | 20.2 |
| 2096x896 14b | 3289088 | 6.1 | 12.2 | 21.3 | 22.8 | 24.3 |
| 2096x786 14b | 2883584 | 6.9 | 13.9 | 24.3 | 26.0 | 27.7 |
| 1920x1080 14b | 3629056 | 5.5 | 11.0 | 19.3 | 20.7 | 22.0 |
| 1792x896 14b | 2813952 | 7.1 | 14.2 | 24.9 | 26.7 | 28.4 |
| 1280x720 14b | 1613824 | 12.4 | 24.8 | 43.4 | 46.5 | 49.6 |
| 2096x1164 10b | 3051520 | 6.6 | 13.1 | 22.9 | 24.6 | 26.2 |
| 1920x1080 10b | 2592768 | 7.7 | 15.4 | 27.0 | 28.9 | 30.9 |

Reading: a 20 MB/s card records nothing useful at 24 fps (about 800x450 14-bit at most); a 40 MB/s card
carries 1280x720 at 23.976 fps; only a card that really sustains 70 MB/s or more reaches the
2096x786 / 1792x896 class.

#### Stall tolerance (simulation, 23.976 fps, 40 MiB, card stalls 250 ms every 5 s, 120 s)

| mode | card MB/s | slots (queueable) | frames | dropped | max fill |
|---|---|---|---|---|---|
| 2096x896 14b | 80 | 9 (7) | 2782 | 96 | 7 |
| 2096x786 14b | 75 | 9 (7) | 2878 | 0 | 7 |
| 1792x896 14b | 75 | 14 (13) | 2878 | 0 | 7 |
| 1280x720 14b | 75 | 25 (24) | 2878 | 0 | 7 |
| 2096x1164 10b | 75 | 13 (12) | 2814 | 64 | 12 |
| 2096x1164 10b | 80 | 13 (12) | 2878 | 0 | 7 |

A 250 ms stall queues about 7 frames. The flip ring (7 queueable) survives it with nothing to spare,
so in flip mode the ring should use the 48 MiB setting (9 queueable) if U10 allows.

#### Fill-stage requirement (F0/F1)

| mode | bytes read per frame | mean MB/s | MB/s to finish within half a frame |
|---|---|---|---|
| 1920x1080 14b | 3628800 | 87.0 | 174.0 |
| 1792x896 14b | 2809856 | 67.4 | 134.7 |
| 1280x720 14b | 1612800 | 38.7 | 77.3 |

Canon overwrites its single buffer from the top every frame. A copy that starts at frame-complete and
reads top to bottom is tear-free only while it stays ahead of Canon's next write, which sweeps the
frame in under 41.7 ms (over 100 MB/s). In practice the fill must finish in about half a frame, which
only F1 (hardware) can do for the large modes. F0 at 80 MB/s is tear-safe up to about 1280x720.

## Failure handling

| event | detection | action |
|---|---|---|
| no free slot at frame-complete (card slow) | free ring empty | drop this frame, `dropped++`, keep Canon's target unchanged (flip: re-arm the slot just completed, i.e. overwrite the newest unqueued frame). Never stop from the hook. `rvx_ctl` stops the recording if dropped frames exceed the user limit (default: stop at the first drop for "continuous" mode, or never for "skip frames" mode) |
| card stall | `rvx_wd`: no write completion for 500 ms while the queue is non-empty | log once; frames drop as above. After 3 s: emergency stop |
| `FIO_WriteFile` returns short or error | return value != requested | one retry of the remainder; then emergency stop. The current code ignores the return value |
| FSU rejects the buffer | cannot be seen from the return path for certain (A7: write silently skipped) | prevented by construction: every pointer is uncached and 512-aligned. Host test asserts it |
| Canon ring counters at 2, LvJob pool exhausted, or EvfDev pending above 50 | `rvx_wd` 100 ms tick | throttle: fill source skips every other frame for 1 s. EvfDev pending above 120 or counters stuck for 300 ms: emergency stop |
| sentinel intact after frame-complete (F1/F2) | per frame | drop; 3 in a row -> revert Canon's default buffer (F2) or switch to F0 (F1), set `fill_failed`, continue or stop per setting |
| fill timeout (F1 own completion semaphore, 30 ms) | `take_semaphore` timeout in `rvx_fill` | disable own channel pair with the two leaf calls, drop frame, 3 in a row -> fall back to F0 |
| chunk reaches 2 GiB (FAT32 limit is 4 GiB; A7 also expects a retune pause at every 4 GiB written) | byte counter in `rvx_wr` | `rvx_wr` closes the chunk and creates the next (`.M00`, `.M01`, ...). The ring absorbs the pause; measured in step 2. A wedge then loses at most the open chunk |
| STOP while writing | control queue message | 1) producer flag `stopping`: no new frames queued. 2) F2: restore Canon's default address by the same mechanism and wait for 2 frame-complete events that report the default address. 3) writer drains the queue, bounded by queued bytes / 10 MB/s. 4) rewrite MLV header with the frame count, `FIO_CloseFile`. 5) only then `raw_lv_release()`. 6) free the ring. Steps 4 before 5 is the order already in `worker.c` |
| emergency stop | any of the above | same as STOP but step 3 is skipped (queue discarded) so the close happens within one write call |
| mode change, card door, power switch during recording | `raw_info` dimensions change; `lv` goes 0; existing shutdown CBR | emergency stop. F2 must never leave channel 3 pointing at a slot that is about to be freed: the ring is freed only after step 2 confirmed the default address, otherwise it is leaked on purpose |
| module unload / second START | state machine | START refused unless state is INACTIVE and the previous ring is freed or leaked-and-flagged |

## Firmware unknowns, in the order they gate work

| id | unknown | gates | cheapest probe (serial harness, LV in movie mode) |
|---|---|---|---|
| U5 | Is the cached alias readable after `0xE05D088C`; real copy MB/s; is the copy pre-empted | F0 | A13 checks 2, 3, 6: `md` both aliases, invalidate, compare; micro-benchmark variants timed with `get_us_clock` |
| U9a | Sustained write MB/s per card, per-call time (`rd 0x640C`), pause at chunk roll and at 4 GiB | every budget row | A7 checks 1 and 7 |
| U10 | Largest safe ring: 40 MiB and 48 MiB allocation in movie LV without the AE deadline storm; contiguity | ring size | allocate at START with recording disabled, watch for asserts for 30 s |
| U8 | `task_create_ex` CPU argument; what runs on CPU1 | F0 above 40 MB/s | create a counting task with cpu = 1, read the counter and `is_cpu1_ready` |
| U2 | Per-frame order of: ch3 write complete, `Mem1Path_SetParam`, address latch. Is `*(0x1029C)` stable. Does `*(0x10290)` track 0xD04202A0 | F2 (M2), and a better frame event for F0/F1 | A1 checks 1, 2, 7, 9; then a counting-only chained CBR |
| U4 | mk2 start sequence for a write channel with the real select function; resource locks; MB/s | F1 | A5 checks 1-6 on a pattern buffer, LV running but not recording |
| U1 | Which RAM word supplies `param[8]` | F2 (M1) | search RAM for 0x6BC61CC8 / 0x2BC61CC8 over serial, then static xref of the hits back from `CapPath_SetParam` 0xE06CFC2C |
| U3 | Can a module hook EvfState `*(0x8E8C)` without the "EvfCap crash" | F2 (M3) | A2 checks 1 and 3 (statespy on that object) |
| U6 | Canon-side 12/10-bit packing on DIGIC 8: `lv_set_mm` argument, channel PackUnpackInfo register, or pack on ML's own write channel in F1. Mem1Path computes line bytes as width*7/4, so the ch3 side looks fixed at 14-bit | 10/12-bit rows | read ch3 `transfer_mode` (+0xC0) and PackUnpackInfo (+0xD0) with `lv_set_mm` 0 and 1 |
| U7 | Any usable lossless encoder path on DIGIC 8 | lossless row | not started; no reference port has it |
| U9b | Speed Class create (`CreateSC` 0xE05B16FA, `EntSC`, `RecordStart`) as pre-allocation; `FIO_Flush` semantics; exFAT files above 4 GiB through ML's FIO | pre-allocated file, fewer chunk rolls | A7 check 5 (Canon's own write benchmark, plain vs Speed Class) |

There is no "set file size" call in this firmware (A7), so the "pre-allocated file" item of the
throughput wish list reduces to U9b. The base plan does not depend on it.

## Implementation plan

Each step is module-only and leaves a recordable module. Offline gate for every step: build in a
private worktree, `tools/symgate/check_module_syms.py` on the new `.mo` against the unchanged core
`.sym`, `tools/raw_vidx_hosttest/run.sh <worktree>/modules/raw_video/raw_vidx`, and the QEMU smoke
test (`tools/qemu/smoke_test.py`) to confirm the module still loads to big.8. Only the step-specific
tests are listed below.

| # | change | offline test | hardware test | pass criterion |
|---|---|---|---|---|
| 0 | Measurement only. Per-frame copy time and per-write time with `get_us_clock`, one `@ML>` summary line per second (frames, drops, ring fill, copy us, write us). Throwaway `rvx_bench` task: A13 copy variants (a)-(e), 64 MiB write in 4 and 16 MiB calls | host harness: summary line format parsed by a small script | run the bench in movie LV; `rd 0x640C` after a write | U5 and U9a answered with on-camera numbers |
| 1 | Hook hygiene. Remove semaphore, `fio_malloc` and UART logging from the vsync CBR; split control queue from frame queue; resident writer task; ROI x = 192 (word-aligned rows); check `FIO_WriteFile` return values | harness tests `queue_full`, `slow_card`, `stop_right_after_swap` still pass; new test: STOP is delivered while 100 % of frames are being dropped | 5 s recording at the current crop | file closes and persists; no EvfDevState burst after the last write; copy time per frame falls by 3x or more (A13 prediction) |
| 2 | Ring. Slot allocator (40 MiB), copy-slot layout, free ring, contiguous-run writes capped at 16 MiB, uncached 512-aligned pointers, 2 GiB chunk roll, drop policy | harness: files validate with `tools/mlvcheck/mlv_check.py`; assert on every write pointer alignment; `ringsim.py` slot counts equal the module's | 60 s at 1280x720 with the uncached copy (expect drops) | valid multi-chunk MLV, drop count equals the log, clean stop |
| 3 | F0 fast copy. Cacheable source after range invalidate, `memcpy64`, optional CPU1. Watchdog tick reading the A1/A2 health counters (log only) | harness with a fake Canon buffer that changes every frame: no torn frames at the modelled copy speed; byte-exact compare | 1280x720 for 5 minutes, then 1792x896 for 30 s | first continuous recording: 0 drops at 1280x720 for 5 minutes on a 40 MB/s or faster card; frames decode with matching greens (docs/35 check) |
| 4 | Sentinels plus passive flip probes: per frame log 0xD04202A0 and `*(0x10290)`; chain a counting-only function into `*(0x1029C)` behind a runtime toggle that restores the original word | harness: sentinel logic (complete, torn, missing) | toggle on in LV without recording, then while recording F0 | counter ticks once per frame, no new asserts for 60 s, original word restored on toggle off. Answers U2 |
| 5 | F1 hardware crop. Module-local EDMAC pair with the A5 function addresses; pattern-buffer self-test first; sentinel-verified; automatic fall-back to F0 | unit test of geometry (offsets, pitch, 8-pixel alignment) against `first_raw_capture.raw` cropped on the host | pattern self-test with LV idle; then 1792x896 | pattern copy byte-exact 100 times; then 1792x896 continuous for 60 s on the 75 MB/s card with 0 sentinel misses |
| 6 | F2 flip, capture only. Two ring slots plus Canon's default, mechanism chosen by step 4 (M2) or U1 (M1); sentinel-verified; auto-revert on any assert-counter rise; no card writes | harness: flip state machine with latency 1 and 2, drop handling never frees an armed slot | 10 s in LV, frames counted and one slot dumped to the card afterwards | sentinels overwritten in the expected slot every frame, health counters stay at 0 or 1, dumped frame decodes |
| 7 | F2 flip, recording. Flip slot layout, in-slot VIDF header, per-frame writes, 48 MiB ring | harness: MLV validity with in-slot headers and row sub-range; stall simulation | 2096x786 for 60 s, then 2096x896 | 0 drops at 2096x786 on the 75 MB/s card; file validates; clean stop with Canon's default address restored |
| 8 | Reduced bit depth (U6), then Speed Class file creation (U9b) | per feature | per feature | full-width 10-bit at 73 MB/s holds for 60 s |

Steps 0-3 depend on no unknown beyond U5 and already deliver continuous recording at 1280x720.
Steps 5 and 6 are independent of each other; do 5 first if step 4 shows the ch3 phase is unfavourable.

## Limitations

- Nothing here was compiled for or run on the camera. The plan is a design; the only executed
  artefact is the host arithmetic and simulation.
- The simulation models the card as a constant rate with optional periodic stalls. Real cards have
  irregular latency; the 75 MB/s figure is a short run on one 16 GB card.
- The 80 MB/s memcpy figure is for cacheable buffers in the ML bench, not for the cached-source to
  uncached-destination copy F0 needs. A13 estimates 40-80 MB/s for that case.
- All Canon addresses come from other agents' static analysis. I did not re-derive them. One check
  I did make: `struct edmac_mmio.ram_addr` in `src/edmac.h` (DIGIC 8 layout) sits at offset 0xA0,
  which matches A5's "set address writes reg +0xA0".
- The claim that the earlier redirect storm was un-phased rests on A2's reading of the hook object
  and on C7's attempt counts; it is an argument for re-testing under control, not evidence that the
  flip is safe.
- The in-slot header trick assumes MLV readers accept trailing padding inside a VIDF block. mlv_lite
  writes padded slots, but this was not checked against `mlv_check.py` or MLV App.
- A8 reports that taking all shoot memory broke AE timing. Whether 40 MiB is safely below that line
  is known from one recording only.
- The lossless ratio 0.57 is a placeholder from older-generation cameras, and no DIGIC 8 encoder path
  is known.

## Next hardware checks (ordered)

1. Step 0 bench: copy variants and 64 MiB writes timed on-camera; `rd 0x640C`. Decides F0's ceiling
   and the real card budget.
2. `rd 0x10278 0x30` and `rd 0xD04202A0` before and after `lv_save_raw 1`: does `*(0x10290)` equal
   0x6BC61CC8 and does it equal the register (A1 checks 1-2).
3. `rd 0x1029C 8`: expect 0xE06AD51B and its argument, stable across 10 reads and a mode change.
4. Ring-health baseline in idle LV: `rd 0x23804`, `rd 0x292B0`, the six LvJob words, EvfDev pending
   count (A1 check 4, A2 check 4).
5. Allocate 40 MiB then 48 MiB in movie LV with nothing else running; watch 30 s for AE deadline
   asserts.
6. Step 1 module: 5 s recording, confirm the file persists after a normal power-off.
7. A5 checks 2, 3, 5 (the real EDMAC select on ch13, expect register 0xD042327C = 0x26, no assert).
8. Serial RAM search for 0x6BC61CC8 and 0x2BC61CC8 to locate the `param[8]` source (U1).
9. Counting-only chained CBR at `*(0x1029C)` (step 4), then the two-slot flip capture (step 6).
