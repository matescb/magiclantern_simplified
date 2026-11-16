# C9-recommended - Judged recording plan for raw video on the M50 II

Date: 2026-11-16. Offline judgement of two designs. No camera attached. Nothing here was run on
hardware and nothing here proves behaviour on the camera. No ML code was changed by this task.

## Goal

Judge `docs/offline/C9-safe-first.md` and `docs/offline/C9-throughput-first.md` against the firmware
findings of this sprint (A1, A2, A3, A5, A7, A8, A11, A12, A13, B5, C1, C2, C4, C4c, C7, C8; B3 does
not exist), score them, and give one merged ordered plan.

## Verdict

**Base: safe-first. Graft the copy-speed work, the fill-source architecture and the hardware-fill
route from throughput-first, and start from the C4 module instead of a rewrite.**

- Safe-first is the only one of the two that explains why every recording so far crashed (CPU-bound
  copy at priority 0x11, back to back, starving Canon tasks until the shared message pool is empty;
  A3 and A11 support this) and removes that cause before asking for anything new from Canon.
- Throughput-first has the better end state and the better arithmetic for continuous recording, but
  its first hardware steps keep a busy copy at 0x12/0x15 and its targets (F1, F2) depend on ten
  never-executed ROM functions and on a redirect mechanism that A1 and C8 make look unlikely.
- Both designs missed three sprint findings that change the first session: the copy is slow mainly
  because of a byte loop (safe-first), LiveView is 25.0 fps and the core's fps base is wrong (both),
  and QEMU cannot load modules (throughput-first's offline gate).

## What was built

Nothing new. This task read the reports and ran existing checks:

```
$ python3 tools/C9-safe-first/budget.py --test
budget.py self-test: 8 checks OK
$ python3 tools/C9-throughput-first/ringsim.py --test
selftest OK
$ tools/raw_vidx_hosttest/run.sh wt/C4/modules/raw_video/raw_vidx | tail -1
failed tests: 0
$ md5sum sim/C4/raw_vidx.mo
79cf1d98535edb25064787e7be1c38c8  sim/C4/raw_vidx.mo
```

Source facts re-checked in the main checkout (read-only, a10b16c23):
`event_pusher.c:98` `take_semaphore(events_sem, 2000)` in the vsync path, `:374/:384` `fio_malloc`
in the vsync path, `:421` `event->x = 200`, `mlv_3.c:725` absolute VIDF timestamp,
`worker.c:553` `session.fps = fps_get_current_x1000()`, `raw_vid_worker.h` priorities 0x11 / 0x1A.
The C4 worktree still has the semaphore (`:107`) and the two `fio_malloc` (`:407/:417`).
Every core symbol the two designs import is present in `tools/symgate/baseline/M50mk2_101.sym`
(`get_us_clock`, `GetMemoryInformation`, `memcpy64`, `task_create_ex`, `get_task_info_by_id`,
`FIO_Flush`, `FIO_OpenFile`, `get_free_space_32k`, `msg_queue_post`, `call`, `dcache_clean`,
`take_semaphore_now`, `m50ii_uart_str`, `is_cpu1_ready`).

## Results

### Assumption check: safe-first

Status: S = supported by a sprint report, C = contradicted, U = unverified (no report settles it).
"Supported" means static analysis or log mining agrees; none of it is a camera measurement unless
the row says "measured".

| # | Assumption | Status | Evidence |
|---|---|---|---|
| 1 | A copy slower than one frame period mixes exposures; the current stored frames are torn | S | A13 "Tearing" note; A1 (Canon rewrites the buffer every frame). Arithmetic only, no torn frame was ever inspected |
| 2 | The 6 MB/s copy rate is "uncached to uncached" | C (incomplete) | A13: the main cause is newlib memcpy's byte loop, because the source row address is 2 mod 4 (x=200). x=192 alone is estimated at 15-30 MB/s. Safe-first's 6 MB/s column in T2/T3 is therefore pessimistic and its plan has no alignment fix |
| 3 | `lv_save_raw(0)` only clears a flag; Canon then stops channel 3 each frame | S | A1: handler writes `[0x10238+0x40]`; `Mem1Path_SetParam` calls `edmac_stop_boomer_maybe(3)` when the flag is 0. Extra point from A1 that helps mode F: flag 0 is Canon's normal LiveView state, so toggling returns to a state the firmware runs in all the time |
| 4 | After the flag is cleared the buffer holds one complete, untouched frame | U | Nobody traced whether the frame in flight completes or how many VDs pass. This is H3 and stays the central unknown |
| 5 | Channel 3 is not one of the channels the MvRawPro/RawPro rings wait on, so stopping it cannot storm | S (expected), U at run time | A1: RawPro channels are `[0x292B0+0x30]` / `+0x34`, expected 0x29 / 0x4C. The run-time value was never read. If either reads 3, mode F is dead |
| 6 | Recording aborts come from starving Canon tasks until the TaskClass pool (1536 nodes, head 0x39B84) is empty | S (inferred) | A3 (stack-floor fault inside the "MsgQue OverFlow" path), A11 (0x11 outranks DAF peers, EIS, PropMgr, Imager, Fwrite, GUI, FileMgr), C7 attempt 47 (`Task.c` assert then abort). A11 caveat: tasks are unpinned on two cores, which weakens any starvation argument. Not measured |
| 7 | Hook runs inside EvfDev (0x0E, queue 200, about 3 messages per frame); blocking there stalls Canon | S | A2. Static |
| 8 | The READY poll carries no information | S | A1 |
| 9 | Defects D1, D5, D6, D7 exist | S | C4 reproduced D1/D6/D7 with a host test and fixed them; C1 confirms D5 (`TIMESTAMP_ABSOLUTE`). D2 and D3 confirmed in source by this task and are NOT fixed in C4 |
| 10 | D2 can block EvfDev for up to 2 s | S, but rare | The semaphore is a short mutex around the event array; 2 s is the timeout, reached only if the worker holds it that long |
| 11 | Priorities: writer 0x1A, copy 0x1C sit below Canon's LV tasks | S for "below LV band", U for 0x1C | A11: LV band is 0x0D-0x14, Canon writers 0x15, DOSDriver 0x1A, CSMgrTask 0x1C or 0x1E. A copy at 0x1C is level with card services; if `FIO_WriteFile` hands work to CSMgrTask the copy can delay the write. A11 recommends 0x15/0x16 for throughput |
| 12 | Card is not the limit up to 1792x896 at 24 fps (75 MB/s measured) | S, marginal | A7 measured 75.1/75.7 MB/s on one card, short run. At the real 25 fps (row 16) 1792x896 needs 70.4 MB/s = 94 % of that |
| 13 | RAM: 32 MiB proven, 48 MiB "hoped" | S / U | A8: 63 MB shoot pool once measured; taking all of it reproduced the AE deadline storm. 2 x 16 MiB + 2 x 4.27 MB allocated together once (boot 212). 48 MiB plus Canon's needs is untested |
| 14 | `rvx_probe` "callable over serial" | C (as worded) | A8 (docs/32:1141) and the WiFi memory note: an allocation or blocking callee run through the serial `call` never returns. The probe must spawn its own task |
| 15 | Cacheable source after a per-line CP15 invalidate; 32-byte lines; possible L2 | S for the mechanism, U for speed and SMP | A13: line size 32 B, L1 d-cache 16 KB, Canon has an invalidate-by-range at 0xE05D088C (use it instead of inline CP15). RAM is outer non-cacheable, so the L2 guess does not apply to RAM. Whether maintenance is broadcast to the other core is unknown |
| 16 | LV rate 23.976 fps | C | C4c: logged vsync counters give 24.98 +- 0.13 per second (25p). All per-second figures are 4.3 % higher |
| 17 | fps for the MLV header in mode L/C = "LV fps / N" | C (hidden dependency) | The module reads `fps_get_current_x1000()`; with `TG_FREQ_BASE` 32 MHz it reads about 18.5 % high (A9, B5, C4c). Fixing the base is a core change. Mode F's "compute fps from timestamps" avoids it and should be used in every mode |
| 18 | `FIO_Flush` on an open file may commit the directory entry | U | A7 lists the tag; C4 declined to use it and added close/reopen checkpoints instead (never run on hardware) |
| 19 | Chaining the Mem1Path complete-CBR slot `[0x1029C]` gives a frame-ready signal | S (static), U at run time | A1 check 9. Runs in interrupt context; the slot was never read on hardware. With raw armed but ImagerState `[0x93A4+0xc]` = 0 the Canon function does nothing, so chaining is cheap |
| 20 | Health-watchdog addresses (0x23804, 0x292B0, 0x39B84, LvJob slots) | S (static), U at run time | A1, A2, A3. Never read on the camera |
| 21 | Offline gates G2/G3 (`raw_vidx_hosttest`, `mlv_check.py`) exist and work | S | C4, C1; host test re-run here: `failed tests: 0` |

### Assumption check: throughput-first

| # | Assumption | Status | Evidence |
|---|---|---|---|
| 1 | Card sets the ceiling: SDR104, 75 MB/s write, bus limit about 93 MB/s, no overclock headroom | S | A7 (measured, one card, 16 MB buffers, short run) |
| 2 | Full 2096x1164 14-bit cannot be continuous | S | A7; 106.8 MB/s at 25 fps |
| 3 | 2096x786 (69.1 MB/s) and 1792x896 (67.5 MB/s) fit 70-75 MB/s | C at 25 fps | C4c: LV is 25.0 fps, so they need 72.1 and 70.4 MB/s, i.e. 94-96 % of the one measured card rate with nothing left for FAT updates. A realistic full-width target at 25p is about 2096x720 (66.1 MB/s) |
| 4 | A per-frame CPU copy cannot carry 67 MB/s | S | A7 ("copy costs as much time as the card write"), A13 table (C: 80-150 MB/s estimated, not measured) |
| 5 | F0: cacheable alias readable after `0xE05D088C(addr,size)`; `memcpy64` with x=192 | S (static), U at run time | A13. Speed (40-80 MB/s estimate), callability from task context and cross-core staleness are unmeasured |
| 6 | Step 1 prediction "copy time falls 3x or more with x=192" | S (estimate) | A13 check 4. Not measured |
| 7 | `rvx_fill` at 0x12 on CPU1, else 0x15 on CPU0; other tasks "CPU 0" | 0x15 S; 0x12 C; CPU column U | A11 recommends copy 0x15; 0x12 outranks EIS, AlgsMgr, PropMgr, Imager, which is the situation rows 6 of the other table blames for the aborts. A11: plain `task_create` tasks are unpinned, so "CPU 0" is not a property the module gets for free |
| 8 | `task_create_ex` takes a CPU argument | S | A11: same arguments as `task_create` plus cpu on the stack, cpu >= 2 rejected; Canon uses cpu 0 and cpu 1. What else runs on CPU1 is unknown |
| 9 | F1: mem2mem failed because of wrong stubs; real select is 0xE05B802E with 0x373F0126 on ch13 | S (two independent static reads) | A5 and B5 agree. A5 also shows `0xE05B9690` on ch13 clobbers reg +8. The correct mk2 start sequence is unknown, so F1 is a research item, not a build step |
| 10 | F1 needs no resource lock conflict | S (partly) | A12: `{0x100AD, 0x100BB}` are locked by no Canon path; the ML `LockEngineResources` stub is the try-lock variant. Which resources ch13/ch46 really need is unknown |
| 11 | F2 M2/M3: writing 0xD04202A0 can redirect Canon's raw write into a ring slot | C (likely) | A1: `Mem1Path_SetParam` calls `edmac_set_address(3, param[8])` every frame, so a register write is overwritten unless it lands after SetParam and before the transfer. C8: the double buffer "the D8 redirect never fills". Only M1 (feed `param[8]`) fits, and its source variable is not located |
| 12 | The earlier redirect storm was "un-phased" and may not recur | U | A2 supports "6/3 is not readout-done"; A1 cannot explain why touching channel 3 would stop the RawPro ring. The storm cause is open |
| 13 | Chained ch3-complete CBR as frame event; `*(0x10290)` as Canon's record of the address | S (static), U at run time | A1 |
| 14 | 40 MiB ring is safe; never the whole pool | S / U | A8. 40 MiB matches the single boot-212 allocation; that recording crashed for other reasons, no AE storm logged |
| 15 | FSU accepts an uncached 512-aligned pointer; rejected writes are silently skipped; 31 MiB cap per call; `*(0x640C)` write time | S (static + log lines) | A7 |
| 16 | 2 GiB chunk roll; retune stall each 4 GiB | S for the 4 GiB hook (inferred), U for cost | A7 |
| 17 | Offline gate "QEMU smoke test confirms the module still loads to big.8" | C | C2: "Module loading does not start in QEMU"; the model never mounts B:. QEMU proves nothing about a module |
| 18 | "Build in a private worktree" then gate the `.mo` against the unchanged core `.sym` | S with a trap | C4/C8: a core built on a branch whose name is not 18 characters long moves 53 symbols by 8 bytes. Harmless for a module-only deploy, fatal if that core is ever flashed. C4: platform `make` leaves stale `.mo` copies; take the module from `modules/raw_video/raw_vidx/build/` |
| 19 | Stay on raw_vidx, not mlv_lite | S | C8 |
| 20 | Sentinel in ring slots proves that a DMA landed | U, sound | No report covers it; it is a host-testable idea with no Canon dependency |
| 21 | Step 1 pass criterion "no EvfDevState burst after the last write" | U | C7: every attempt that copied frames ended in that burst or a `Task.c` assert. Hook hygiene addresses the EvfDev queue (A2) but step 1 still copies at full speed with no throttle |
| 22 | In-slot VIDF header with padding is accepted by MLV readers | U | Not checked against `mlv_check.py` / `mlv_dump` (C1 has the tools to do it) |
| 23 | 12/10-bit Canon-side packing, lossless | U | A9: the bit-depth register literals 0xD0008094 / 0xD0008030 do not exist in the ROM; no path found |

### Scores

1 = worst, 5 = best. The percentages are my judgement from the evidence above, not computed values.
Both are capped by boot reliability: C7 gives 48 % of cold starts reaching big.8 on the walkback-size
core, so any session needs several power cycles per attempt.

| Criterion | safe-first | throughput-first | Reason |
|---|---|---|---|
| First decodable clip in the next hardware session | 4 (about 60 % for a file that validates; about 35 % for an untorn, correctly timed one) | 3 (about 45 % for a file that validates; about 20 % untorn) | Safe-first's first clip needs only calls that already ran on the camera plus a clean close, with the copy throttled and at low priority. Its untorn clip needs one unverified behaviour (H3). Throughput-first's first clip keeps an unthrottled copy; its untorn clip needs the cache path and a copy that finishes in half a frame |
| Firmware risk (5 = least) | 5 | 2 | Safe-first: no MMIO write, no EDMAC call, one Canon eventproc toggled. Throughput-first: step 3 cache maintenance on Canon's buffer and a 0x12 task, step 5 engine programming through unexecuted ROM functions, step 6-7 writes into Canon's channel 3 state from interrupt context, a path that stormed before |
| Path to continuous recording | 2 | 4 | Safe-first reaches continuous only for small crops and leaves the hardware fill as a list. Throughput-first has the slot/ring/writer design, stall simulation, chunk roll and the right observation that the CPU copy must go; it loses a point for F2 (row 11) and the 25 fps margins (row 3) |
| ML-core change required (5 = none) | 5 | 5 | Both are module-only. Both silently depend on the core's wrong fps base; the merged plan removes that dependency inside the module |

## Recommended plan

One module (`raw_vidx`), no core change, never a core built from an `offline/*` branch on the card.
Standing offline gate for every module build (called G below): `tools/raw_vidx_hosttest/run.sh
<module dir>` with 0 failed tests; harness output through `tools/mlvcheck/mlv_check.py`;
`tools/symgate/check_module_syms.py` against `tools/symgate/baseline/M50mk2_101.sym`; `.mo` taken
from `modules/raw_video/raw_vidx/build/`, md5 written down and committed before it goes near a card.
QEMU is not part of the module gate (C2).

Origin column: S = safe-first, T = throughput-first, N = added from sprint findings.

| # | Step | Origin | Offline test | Hardware test | Decision it unlocks |
|---|---|---|---|---|---|
| 0 | **Read-only probes, stock walkback build, no module change.** A1 checks 1-4, 6-8; A2 checks 1, 2, 4, 5, 6; A3 checks 1-3; A13 checks 1-3; A11 check 1; `rd 0xD0406198`, `rd 0xD04061A4` (fps base, B5 check 4); A5 check 1 | S (H1) + N | Script the probe list for `mlserial` and a parser that prints pass/fail per expectation; run it against a canned reply file | Movie-mode LV, about 15 minutes | (a) `*(0x10290)` equals the raw buffer: Mem1Path is the raw path, else every later step is re-planned. (b) `[0x292B0+0x30]`, `+0x34` are not 3: mode F allowed. (c) ring counters and pool head readable: watchdog can be enabled. (d) cached alias equals uncached after invalidate: step 5 allowed. (e) timer values: real fps base |
| 1 | **Bench task in the module** (own task, never run through serial `call`): A13 variants (a)-(e) on 4 MB with `get_us_clock`; one 16 MiB `FIO_WriteFile` then `rd 0x640C`; `GetMemoryInformation`; allocate 32, 40, 48 MiB, hold 30 s each, free; print Canon task priorities | S step 0 + T step 0 + N (A8) | G; host build of the timing loop; parser for the one-line `@ML>` results | Run once in movie LV; watch 30 s for "Over the Deadline" lines after each allocation | Copy-rate column for every budget table; ring size (32 vs 40 vs 48 MiB); card rate on this card; whether A11's static priority table is right |
| 2 | **Base module = C4 (`offline/C4` f8191a47a) plus patch P1.** P1: hook does `vd_count++` only (no semaphore, no `fio_malloc`, no UART); crop x = 192; copy moved out of the 0x11 worker into a capture task that copies 256 KiB then `msleep(10)`; VIDF timestamp relative and taken at capture; fps computed from `vd_count` over `get_us_clock`, not from the core; one summary line per second, none per frame; burst mode S (capture until the ring is full, then write, then close before `raw_lv_release`). Capture task 0x1B, writer 0x1A | S steps 1-2, built on C4; x=192 and per-second summary from T; fps from N (C4c) | G plus new harness cases: hook makes no blocking call (stub asserts), STOP delivered while every frame is dropped (T), 1-, 5-, 6-, 11-frame clips with header count = frames in file, timestamps relative and monotonic, source row address is 0 mod 4 | REC, wait for the burst to end; pull or WiFi-fetch the file. 1280x720, 32 MiB ring = 20 frames | **First decodable clip.** Pass = `mlv_check.py` clean, count matches, `ver` answers 30 s later, `rd 0x39B84` non-zero. If the abort within 1 s of the last write is gone: starvation was the cause (A3/A11), keep throttled low-priority copy as a rule. If it remains: run C7 check 4 (close and return without `raw_lv_release`) and A3 check 4 (zero `0x5158`) before anything else |
| 3 | **Freeze test, by hand first.** `call lv_save_raw 0`; read four 16-byte probes of the uncached buffer twice, 1 s apart; re-arm; repeat 20 times; then 200 toggles at 3 Hz | S (H3), probe form from N | None possible (Canon behaviour) | Moving subject in frame; compare probes on the host | Buffer stable while the flag is 0 and no assert after 200 toggles: mode F is allowed. Probes change: mode F is dead, skip to step 5 |
| 4 | **Mode F clip.** Freeze, copy, verify, re-arm, fixed pacing. Per-frame self-check: hash eight sparse 64-byte probes of the crop before and after the copy; a difference means Canon wrote during the copy, the frame is dropped and counted (sensor noise makes consecutive frames differ even for a static scene) | S step 3; self-check adapted from T's sentinel idea | Harness "sensor" thread rewriting a buffer at 25 Hz with a frame counter in every row unless frozen; assert each stored frame carries one counter and that the probe check catches every injected overwrite | Moving subject (stopwatch); 1280x720, about 2-5 fps depending on step 1's copy rate | **First coherent, correctly timed clip** (a time-lapse). Also validates the probe check on real data, which steps 5-7 rely on instead of pulling the card |
| 5 | **Fast copy (T's F0).** Source through the cacheable alias after Canon's range invalidate `0xE05D088C`, `memcpy64`, destination per step 1's best variant (cacheable destination needs `dcache_clean` range before the write) | T step 3, S step 4 | G; unit test that every pointer passed to `FIO_WriteFile` is uncached or 32-byte aligned (A7) | On a frozen frame (step 3 state): copy both ways, byte-compare 20 times, time both | Copy of 1280x720 in under 20 ms (about 80 MB/s): live capture at 25 fps is possible, go to step 6. 25-80 MB/s: live capture at 960x540 or every 2nd frame. Any byte difference: keep the uncached source, stay on mode F, move step 8 forward |
| 6 | **Mode L burst.** Copy starts a tunable delay after the vsync tick; probe check drops torn frames; RAM burst, write after | S step 5 | Step-4 simulator with freeze off; sweep copy rate and delay, tear-free region must match the budget table at 25 fps | Delay sweep 0-35 ms in 5 ms steps at 640x360, then 960x540, then 1280x720; read the dropped-frame count from the summary line | A delay window with zero torn frames at least 10 ms wide: vsync timing is good enough. No window: chain the counting-only function into `[0x1029C]` (A1 check 9, restore on stop) and repeat with that signal |
| 7 | **Continuous (mode C).** Writer runs during capture, 16 MiB writes from contiguous slots, drop-and-count on ring full, stop after N consecutive drops, 2 GiB chunk roll, watchdog on step-0 addresses (log only at first), priority A/B from A11 check 4: (capture 0x1B, writer 0x1A) versus (0x15, 0x16) | T step 2 ring and writer design, S step 7 | Harness with a rate-limited fake card at 20/40/75 MB/s and 250 ms stalls; time to first drop within 10 % of `ringsim.py` at 25 fps; multi-chunk file validates | 960x540 for 60 s, then 1280x720 (40.4 MB/s at 25 fps) for 5 minutes | **First continuous recording.** Which priority pair survives; real sustained card rate with FAT updates; whether the EvfDev pending count (A2 check 4) stays near 0 |
| 8 | **Hardware-fill probes, no recording.** A5 checks 2, 3, 5 (real select on ch13: expect `0xD042327C` = 0x26, no assert); B5 check 1; per-frame log of `0xD04202A0` against `*(0x10290)`; serial RAM search for the buffer address to find the `param[8]` source | T steps 4 and "next checks" 7-8 | Geometry unit test against `first_raw_capture.raw` cropped on the host | LV idle, photo mode first (A5 note on sleeping sub-chips) | Select works without assert: build F1. `*(0x10290)` re-written every frame from a RAM word that can be found: F2 mechanism M1 exists; otherwise F2 is parked |
| 9 | **F1 mem2mem crop** on ML's own channels 46/13 through module-local ROM pointers, pattern-buffer self-test first, sentinel per slot, automatic fall-back to step 5's copy | T step 5 | Sentinel state machine (complete / torn / missing) in the harness | Pattern copy byte-exact 100 times with LV idle; then 1792x840 or 2096x720 class | Continuous above 40 MB/s. At 25 fps the ceiling on the measured card is about 66-70 MB/s |
| 10 | F2 flip (only through M1), reduced bit depth, Speed Class file mode | T steps 6-8 | per item | per item | Full-width zero-copy recording; each item needs its own reverse engineering first |

Order rules:

- Steps 0-2 are the next hardware session. Step 2 does not depend on steps 0-1 passing, only on
  nothing in step 0 contradicting item (a).
- Step 3 costs minutes and decides between the cheap route to a correct clip (4) and the fast-copy
  route (5-6). Do it in the same session if step 2 passed.
- Steps 8-9 are independent of 3-7 once step 2 works and can be interleaved when a session has time.
- Ring size stays at 32 MiB until step 1 shows 40 MiB holds for 30 s without AE deadline lines.

## Firmware facts that most change the plan

1. **The slow copy is an alignment accident plus uncached aliases** (A13): source rows are 2 mod 4,
   so memcpy runs its byte loop. x = 192 and the cacheable alias are module-only changes. Every
   "6 MB/s" table in both designs is a floor, not a property of the camera. Unmeasured.
2. **`lv_save_raw` is one flag, and flag 0 is Canon's normal state** (A1): the MvRawPro / RawPro /
   LvJob asserts mean Canon's EDMAC completions were starved, not that the flag raced. This is what
   makes the freeze mode a low-risk experiment and the READY poll worthless.
3. **The hook runs in EvfDev (0x0E, 200-deep queue); the worker at 0x11 outranks most of Canon's LV
   helpers; all TaskClass messages share 1536 nodes; the post-recording abort is that pool running
   dry** (A2, A3, A11). Hence: integer-only hook, throttled copy below the LV band.
4. **LiveView is 25.0 fps and the core's `TG_FREQ_BASE` is wrong** (C4c, A9, B5): budgets rise 4.3 %,
   1792x896 and 2096x786 become 94-96 % of the measured card rate, and the header fps must be
   computed inside the module or it reads about 18.5 % high.
5. **Card: SDR104 already, 75 MB/s measured once, no headroom; FSU silently skips a write from a
   cacheable unaligned buffer** (A7). The continuous ceiling is about 66-70 MB/s whatever the
   architecture; pointer rules belong in a host test.
6. **Canon re-programs channel 3's address from `param[8]` every frame** (A1), and the redirected
   double buffer was never filled (C8). A register write is the wrong mechanism for a buffer flip;
   only feeding `param[8]` could work, and that variable is not found.
7. **mem2mem failed because two stubs point into `Engine::Outer.c`** (A5, B5 independently). The
   hardware copy is a real candidate again, callable from a module, but its start sequence is unknown.
8. **Memory: about 63 MB shoot pool, taking all of it reproduces the AE storm, and an allocation
   run from the serial shell never returns** (A8). Ring 32-40 MiB; probes in their own task.
9. **Tooling limits** (C2, C4, C8, C7): QEMU cannot load modules; a core built on an `offline/*`
   branch is not symbol-identical; stale `.mo` copies sit in the platform build tree; about half of
   cold starts on the walkback-size core do not reach big.8.

## Limitations

- This is a paper judgement. I re-ran two self-tests and one host test and re-read a few source
  lines; I did not re-derive any ROM address and did not open the ROM.
- Statuses rest on the sibling reports, which are static analysis or log mining. "Supported" is not
  "measured on the camera" except where a row says so (card rate, memcpy bench, 25 fps counters).
- The probabilities in the score table are my estimates, with no model behind them.
- C7 was read in part (headline findings, per-build table, recording attempts, rates); the assert
  tables and the verbatim dumps were skipped. A4, A6, A9, A12, C1, C2, C5, C6 were read as summaries.
- The probe-hash coherence check in step 4 is new in this report. It assumes sensor noise makes any
  two frames differ in eight 64-byte probes; that is plausible for 14-bit raw but untested, and clipped
  (fully white or black) probe locations would defeat it.
- Patch P1 is specified, not written. The C4 module it builds on has passed a host test only.
- Mode F rests on H3. If it fails, the first untorn clip waits for step 5, which rests on cache
  behaviour nobody has measured either.

## Next hardware checks

In session order. Commit the exact module source and note the `.mo` md5 before each deploy.

1. Step 0 probe script in movie-mode LV on the stock build. Stop and re-plan if `*(0x10290)` is 0
   after `lv_save_raw 1`, or if `[0x292B0+0x30]` / `+0x34` reads 3.
2. Step 1 bench task: five copy variants, 16 MiB write plus `rd 0x640C`, 32/40/48 MiB allocation hold.
3. Step 2 module (C4 + P1), 1280x720 burst of 20 frames. Check the file with `mlv_check.py`, then
   `ver` after 30 s and `rd 0x39B84`.
4. If the abort after the last write is still there: C7 check 4 and A3 check 4, nothing further.
5. Step 3 freeze probes by hand, then 200 toggles.
6. Step 4 mode F clip of a stopwatch; compare displayed time steps with VIDF timestamp steps.
7. Three clips in a row without a reboot; `GetMemoryInformation` before and after must match.
8. If time remains: A5 checks 1-3 and 5 in photo-mode LV (step 8).
