# Offline sprint index (2026-11-16)

This file lists every report written during the offline sprint of 2026-11-16 on the Magic Lantern
(ML) port to the Canon EOS M50 Mark II, firmware 1.0.1. It gives each report's headline finding and
what happened when the finding was checked.

**No camera was attached during the sprint.** Every finding below comes from static reading of the
ROM dump (`m50ii_dumper/dump_1.0.1/gang100.bin`, loaded at 0xE0000000), from ML source, from host
builds and host tests, or from UART logs captured on the camera between 2026-07-14 and 2026-07-22
(`tools/mlserial/run/uart.jsonl`). None of it has been re-run on hardware. Code on the sprint
branches has been built but never flashed.

How to read the "Verification" column:

- **Independently verified** means the report ends with an "Independent verification" section
  written by a second agent that re-derived the claims. The verdict words used there are CONFIRMED,
  REFUTED and UNVERIFIABLE (true or false cannot be decided statically). The column names the
  refuted claims so that they are not repeated.
- **Self-checked** means the author re-disassembled or re-ran their own evidence; there was no
  second agent.
- **Not verified** means the report was not checked by anyone else.

The list reflects the files present in `docs/offline/` when this index was written. Reports that
were announced but not present at that time are listed at the end.

The synthesis of the firmware reports for a new reader is `docs/38-firmware-map-raw-video.md`. The
merged recording plan is `C9-recommended.md`.

## A - firmware analysis (ROM 1.0.1)

| Report | Subject | Headline finding | Verification |
|---|---|---|---|
| [A1](A1.md) | LiveView raw job, Mem1Path, completion rings | `lv_save_raw(1)` builds no job; it sets one flag in Mem1Path (context 0x10238, +0x40), and `Mem1Path_SetParam` (0xE0754298) re-programs EDMAC write channel 3 each frame. The MvRawProDs / RawPro / LvJob asserts are overflow checks of 2-slot rings that only Canon's EDMAC-complete callback chain (0xE0760A30) drains. The flags the module polled (0x23800+0x30, 0x292B0+0x30) are constants, so "ready" carried no information. | Independently verified. Claims 1-3, 5, 7 CONFIRMED. "Only" popper and "only" consumer (claims 4, 8) UNVERIFIABLE. **REFUTED: claim 6**, the inference that the soft-assert sink at [0x22D9C] explains the repeating storm (the UART log shows the other file strings). |
| [A2](A2.md) | EvfDevState versus EvfState | `*(0x10108)` is EvfDevState; its input 6 in state 3 is `evfdevUpdateParam`, not a readout-done event. The object that corresponds to the M50 mk1 `EVF_STATE` is EvfState at `*(0x8E8C)` (matrix 0xE0B5050C), whose input 5 is `evfReadOutDoneInterrupt`. | Independently verified. Claims 1-3, 5-7, 9 CONFIRMED (one naming correction, sender list incomplete). Identity with mk1 UNVERIFIABLE. **REFUTED as argued: claim 8**, the "about 3 EvfDev messages per frame" model based on a 3:1 log ratio. |
| [A3](A3.md) | Crashes inside Canon's debug-message formatter | The signatures `pc=0 / lr=0xE05D1161` and the memcpy variant are one fault: CPU0's 4 KB stack at 0xDF000000-0xDF001000 overflows while `DryosDebugMsg` (0xE05D10C4) is nested in the "MsgQue OverFlow" path of `TryPostEvent`, triggered by an empty event-container free list (head at RAM 0x39B84). A third signature (NULL `%s` in vsnprintf 0xE005DB9C) is unrelated. | Independently verified. Claims 1-5, 7 CONFIRMED. "Interrupt stack" and recursion beyond one round UNVERIFIABLE. **REFUTED in part: claim 11** (the raw_vidx grep was too narrow). |
| [A4](A4.md) | PropertyMgr boot crash `pc=0xE021B8FC lr=0xE04037A1 dfar=0x46` | The registrant that causes the crash was **not identified**. It is not tied to the ML marker `big.1c` (last marker was `big.1b` in 21 of 29 dumps). No resolved ROM0 `prop_register_slave` call site passes a `_prop_cleanup` tail-caller as token callback. | Independently verified. Frame chain, registers, counts, record layout CONFIRMED. **REFUTED: claim 5** ("Canon RAM code at 0x0225xxxx-0x0226xxxx is not in gang100.bin" - it is, see F5) and the completeness part of claim 4 ("0 of 118 covers Canon"). Continued in F5. |
| [A5](A5.md) | EDMAC channel map, boomer selector, mem2mem | The stubs `edmac_select_boomer` 0xE05B752E and `edmac_reset_boomer_vdkick` 0xE05B7520 are `Engine::Outer.c` functions indexed by port number, not EDMAC functions. The real per-channel select is 0xE05B802E (`Engine::Edmac.c`); the unmodified mk1 selector 0x373F0126 passes its checks for channel 13. Inferred: the mem2mem copy never routed read channel 46 into write channel 13. | Independently verified. All checked claims CONFIRMED; none refuted. Function identity by positional homology and the "stale frames" cause are UNVERIFIABLE statically. Applied on branch `offline-edmacstubs` (F2). |
| [A6](A6.md) | Movie-record state machine, card mounting | The addresses docs/32 called `vcmMovieFormatMount` (0xE0202328) and `fmMountCard` (0xE02023C0) are debug strings, not code. The real fmMountCard (0xE0202106) / fmUnmountCard (0xE0202612) are FileMgr state handlers, and the `vcmMovieFormatMount` calls are compiled out. Record start is GUI-driven through `handleMovieButton` 0xE071FE72 and `IsEnableMovieStart` 0xE0547518; no mount code is on that path. | Independently verified. Claims 1-4, 6, 7, 9, 10 CONFIRMED. **PARTLY REFUTED: claim 5** - the descriptor layout at 0xE0BB79BC is right, but 0xE024A8FC does not log "mvrRecStart In", so that function's name is not established. Drive-index reading and the `IsEnableMovieStart` patch are UNVERIFIABLE. |
| [A7](A7.md) | Card write path and speed | The camera already runs the card at UHS-I SDR104 (hardware log), clock index 9 = 195 MHz. ML bench measured 75.1 / 75.7 MB/s write and 90.4 / 93.9 MB/s read on one card. The FSU layer rejects a write buffer that is cacheable and not 32-byte aligned (0xE0085180-0xE0085194). One device call is capped at 0xF800 sectors. | Independently verified. Main claims CONFIRMED. **REFUTED: claim 2b** ("no DDR50 string exists" - there are two, at 0xE015FB7C and 0xE015FC8C) and **claim 12** ("registration function 0xE00E8362 has no caller" - it is called at 0xE00AF47C). |
| [A8](A8.md) | Memory pools, SRM | Only the shoot-memory suite is wired up (about 63 MB measured earlier in movie LiveView). The commented SRM stubs are mk1 addresses; the real pair on 1.0.1 is 0xE057B7BA (allocate) / 0xE057DA00 (free). | Independently verified. All claims CONFIRMED; none refuted. Link between the 0x2AB1000 area and the "1stJob" buffer UNVERIFIABLE. |
| [A9](A9.md) | Raw geometry, frame timer | The frame-timer clock is about 27 MHz, not 32 MHz, so `TG_FREQ_BASE` should be 27000000. Canon's eight LiveView raw geometries are in a ROM table at 0xE1359338; for 2096x1164 Canon's window is left 92 / top 38 / 2000x1126. | Independently verified. No claim refuted. Corrections: records are 11 words (stride 0x2C); table B has no located code reference, so the drive-mode-to-resolution map is a hypothesis; the 27 MHz conclusion rests on mk1 measurements. Applied in C4c. |
| [A10](A10.md) | Sensor and engine register writes | `engio_write` = 0xE06CAEB2 (plain register/value list, no shadow memory). One CMOS serial writer, 0xE06C2F98; there is no ADTG path in this ROM. Canon left callback slots at RAM 0x194B4 / 0x194B8 and logging eventprocs for observation. | Not verified. |
| [A11](A11.md) | Canon task table and priorities | 184 task-creation sites recovered. Canon's LiveView core runs at priorities 0x0D-0x14; Canon's bulk writers at 0x15. raw_vidx's copy worker at 0x11 outranks PropMgr, EIS and the file tasks. Recommendation: copy 0x15, write 0x16. | Not verified. |
| [A12](A12.md) | Engine resource locks | `{0x100AD, 0x100BB}` are locked by no Canon path in this ROM. The stub `LockEngineResources` = 0xE05BE8A6 is the non-blocking try-lock; the blocking lock is 0xE05BE6A6. `DeleteResLockEntry` = 0xE05BE5A2. Recommendation: no lock for the CPU-copy path. | Not verified by a second agent; A14b, A14c and F2 reached the same conclusion about the try-lock independently. |
| [A13](A13.md) | Why the frame copy is slow | The copy ran in memcpy's byte loop because the source row address is 2 mod 4 (crop x=200), and both source (0x6BC61CC8) and destination are in the non-cacheable alias. Measured mean 0.49 s per frame, about 5.7 MB/s. Fixes: crop x a multiple of 16 px; read through the cacheable alias after a d-cache invalidate (0xE05D088C). | Independently verified. All claims CONFIRMED; none refuted. Speed estimates for the fixes are not measured. |
| [A14a](A14a.md) | Stub audit: kernel, tasks, memory, FIO, debug | 9 stubs are wrong (blind mk1 copies), among them `task_max` (should be 0x4108) and `_request_RPC` (0xDF002C32 lands inside an instruction; real entry 0xDF002C4A). `tools/verify_stubs.py` skips all 0xDF00xxxx stubs. | Not verified. Applied on branch `offline-stub-fixes` (F7). |
| [A14b](A14b.md) | Stub audit: GUI, display, properties, EDMAC, misc | 11 stubs wrong or very likely wrong; six are on the raw-video EDMAC path. `LiveViewApp_handler` points at another dialog's handler (real pair 0xE0444058 / slot 0xE9F0). | Not verified. EDMAC part agrees with A5 (verified). Applied in F2 and F7. |
| [A14c](A14c.md) | Missing functions and guessed constants | Found: `debug_assert`, `dm_set_store_level`, `DeleteResLockEntry`, display on/off, real `_LoadCalendarFromRTC`. `LV_OVERLAYS_MODE` should be MEM(0x15998). `LVAE_STRUCT` and `LVAE_ISO_STRUCT` are mk1 values that occur nowhere in this ROM. | Not verified. Applied in part in F7. |
| [A15](A15.md) | ROM1 and boot-time RAM images | ROM1 (`gang200.bin`) holds no code. The three RAM images are at ROM0 file offsets 0x1394358, 0x13BB074, 0x14995A4. All seven RAM pointers raw video depends on are 0 at boot. Tool: `docs/offline/tools/raminit.py`. | Not verified. |

## B - Discord mining and mk1 comparison

| Report | Subject | Headline finding | Verification |
|---|---|---|---|
| [B1](B1.md) | DIGIC 8 LiveView raw capture in the community dump | The enable recipe `lv_set_mm 1` + `lv_save_raw 1` is hardware-confirmed on EOS R. The "DIGIC 8 crash" (`MvRawProDs.c` assert, task EvfDev) has been seen on R and RP since 2021 without raw video and was never root-caused. | Not verified (citations are file:line into the dump). |
| [B2](B2.md) | Crop modes and sensor registers | On M50 mk1 only 14-bit raw at Canon's 1080p stream (2096x1164 buffer) has been recorded; 10/12-bit is unsolved on DIGIC 7/8. | Not verified. |
| [B3](B3.md) | Card speed and throughput | DIGIC 8 UHS-I bodies cap at about 90 MB/s in bench; nobody has overclocked SD on DIGIC 8; lossless compression does not exist on any DIGIC 7/8 port. | Not verified. Consistent with A7's hardware-log numbers. |
| [B4](B4.md) | History of the M50 / M50 II raw effort | mk1 raw is a straight copy of kitor's `edmac_sx740` code with two fixes. Untried here: three small SRM allocators, the `edmaclog` module, an FS-ready gate instead of a fixed sleep. | Not verified. The author states the coverage limits. |
| [B5](B5.md) | Line-level diff: working mk1 port versus our tree | mk1's raw enablement is 9 files / 63 lines. Our boomer stubs point into a different ID space (same finding as A5). Three drifts to revert: `raw_lv_request_bpp` early return, `TG_FREQ_BASE`, per-recording diagnostics in mlv_lite. | Not verified as a whole; the boomer finding is confirmed by A5's verification. |

## C - tooling, code and design

| Report | Subject | Headline finding | Verification |
|---|---|---|---|
| [C1](C1.md) | Host MLV lane | Host `mlv_dump` builds; the camera's own `mlv_3.c` writer was replayed on the host with the real captured frame; `tools/mlvcheck/mlv_check.py` validates MLV files. Replay found writer bugs (B1: a clean stop loses the frames still in the buffer; B2-B6). | Self-checked (41 tests pass). Bugs fixed on `offline/C4` and `offline-integrate1` (C10). |
| [C2](C2.md) | Reproducible QEMU harness | `tools/qemu/` builds an SD image from the current build and smoke-tests boot to the marker `big.8` plus the serial shell; passed twice (about 65 s wall). Found stale modules on the base image. | Self-checked. QEMU only; says nothing about hardware. |
| [C3](C3.md) | QEMU model toward the raw path | Stage A only: Canon storage does not come up in the emulator; loading modules and writing an MLV in QEMU were not reached. | Self-checked negative result. |
| [C3b](C3b.md) | QEMU past the "EEPROM stage" | The stall is serial-flash identification, not the EEPROM. Canon's init stage 1 now runs, then both cores park before PropertyMgr is created. Storage does not come up, no module was loaded, and the patched binary is worse for ML smoke tests than the C3 one. | Self-checked negative result. |
| [C4](C4.md) | raw_vidx correctness fixes (module only) | Branch `offline/C4`, 8 commits: stop path and mid-record wedge leave a decodable file; host behavioural test `tools/raw_vidx_hosttest/`. | Reviewed in C4-review: fixes correct on normal paths, **not ready to flash as is**. |
| [C4-review](C4-review.md) | Review of `offline/C4` | Two HIGH findings: after one write-semaphore timeout every later frame is dropped; the default-on checkpoint puts a header rewrite on the write path. Core layout claim confirmed with an 18-character branch name. | Is itself the verification of C4. Findings addressed in C10. |
| [C4c](C4c.md) | Frame-timer base, selector comment, edmac-d8.c guard | Branch `offline/C4c`, 3 commits: `TG_FREQ_BASE 27000000` for M50 II; logged vsync counters give 24.98 fps (25p), not 23.976. | Self-checked; agrees with A9 (verified). |
| [C5](C5.md) | Symbol-stability gate | `tools/symgate/` compares a core `.sym` with the booting baseline, checks module imports and card images. Finding: `build_id = "<commit> <branch>"` is embedded in the binary, so the branch-name length moves 53 core symbols. | Self-checked. Confirmed independently by C4-review and C10 (19 characters move 53 symbols, 18 do not). |
| [C6](C6.md) | Does a core relink break boot? | The logs do not support "symbol shift itself breaks boot": the `pc=0xE021B8FC` streak (boots 330-359) hit both core layouts and started and ended with changes to the card. | Not verified by a second agent; C7 reached the same conclusion from the same logs. |
| [C7](C7.md) | Crash catalogue from the UART logs | 457 cold starts; 216 of 410 effective boots (53 %) reached `big.8`; 36 distinct abort signatures; 47 recording attempts. The `0xE021B8FC` crash follows something outside AUTOEXEC.BIN. Regenerate with `tools/logmine/regen.sh`. | Self-checked (8 unit tests). Tables are generated from logs. |
| [C8](C8.md) | What to do with mlv_lite | Drop `mlv_lite` from `modules.included` and keep raw_vidx as the only recorder; mlv_lite still reads the unfilled double buffer and calls `raw_lv_redirect_edmac` every vsync. Branch `offline/C8`. | Not verified. Picked into C10. |
| [C9-safe-first](C9-safe-first.md) | Recording design, least firmware risk | The current module cannot produce a correctly timed clip: one 2.81 MB copy at about 6 MB/s spans about 11 sensor frames. | Judged in C9-recommended. |
| [C9-throughput-first](C9-throughput-first.md) | Recording design, maximum throughput | The card sets the ceiling: full 2096x1164 14-bit needs more than the bus can carry, so it cannot be continuous. | Judged in C9-recommended (several assumptions marked contradicted, for example the frame rate is 25 fps). |
| [C9-recommended](C9-recommended.md) | **Merged recording plan** | Base on safe-first, graft the copy-speed work and hardware-fill route from throughput-first, start from the C4 module. Each assumption of both designs is marked supported, contradicted or unknown. | Is itself the judgement of the two designs. Not hardware-tested. |
| [C10](C10.md) | Integration branch for the next hardware session | Branch `offline-integrate1` (18 characters), 14 commits: C4 + all review findings + C1 writer bugs + C4c + C8. Core `.sym` byte-identical to the booting baseline, which means the layout is identical, **not** that the core is unchanged: the staged `autoexec.bin` contains one core code change, the timer base constant `TG_FREQ_BASE` 32000000 -> 27000000 (3 bytes at file offset 0x18E20, C10-review finding M2). Staged card directory `sim/C10/card`. | Self-checked (17 host tests, symbol gate PASS). A C10-review report was not present when this index was written. |
| [C10-review](C10-review.md) | Review of `offline-integrate1` | Safe to flash as a first attempt if the first step is module only (keep the booting `autoexec.bin` and `.sym`). No HIGH findings. Open: M1 a stop that times out leaves the file unclosed, M2 the core constant above, M3 a failed card write is not shown to the user, L3 VIDF timestamps taken at handling time. The physical card was not checked. | This is the second-agent review of C10. |
| [C11](C11.md) | Build-id layout hazard, host mlv_dump | Branch `offline-buildid-fx`: fixed-width `build_id` so the layout no longer depends on branch-name length; in-tree host builds; provisional M50 / M50 II entries for `mlv_dump`. | Self-checked. |
| [C12](C12.md) | First plan steps as module code | Branch `offline-plan-step1`, tip 09a4aaf4b, one commit on top of C10 (not equal to C10's tip): priority toggles, throttled copy, burst to RAM, frame-integrity counters, A13 fast copy off by default. Creates one task per frame (never run on the camera). Hook hygiene not done. Takes A11's priority table and A13's address as written. | Not verified; host test only (single-threaded model). |

## F - follow-up reports

| Report | Subject | Headline finding | Verification |
|---|---|---|---|
| [F1](F1.md) | Selectable vsync hook source | Re-confirms A2 from the ROM. Branch `offline-evfstate-1`: global `m50ii_vsync_src` selects the existing EvfDevState 6/3 hook (default) or EvfState input 5. Describes the Mem1Path channel-3 callback as a third option. | Self-checked (re-derivation of A2; symbol gate output in `F1-symgate.txt`). |
| [F2](F2.md) | Corrected EDMAC stubs | Branch `offline-edmacstubs`: `edmac_select_boomer` -> 0xE05B802E, `edmac_reset_boomer_vdkick` -> 0xE05B7FB6, `StartEDmac_maybe` -> 0xE05B82AE, `ConnectReadEDmac_maybe` -> 0xE05B7A32 (medium confidence). `LockEngineResources` deliberately left on the try-lock. | Self-checked function by function (section 3). Not checked by a second agent. |
| [F4](F4.md) | Event-container pool exhaustion | The pool behind A3's crash is 1536 containers at RAM 0xBEE18-0xC4E18 with no free counter; a container is in use while its event sits unread in a queue. Gives rules for raw_vidx (nothing slow in the state-object hook, no CPU-bound ML task above Canon's LiveView tasks). | Not verified. |
| [F5](F5.md) | PropertyMgr boot crash, continued | The crash is **still not identified**. Removes candidates (including registrations in the RAM blob copied from ROM 0xE13BB074) and specifies a probe that names the culprit in one crashing boot. | Not verified. |
| [F7](F7.md) | Corrected stubs outside the EDMAC block | Branch `offline-stub-fixes`, 19 commits: 13 stubs changed, 9 added, 1 constant corrected. Core layout unchanged. Two commits are expected to change LiveView behaviour (`LV_OVERLAYS_MODE`, `LiveViewApp_dialog`). | Self-checked against a stub baseline (`docs/stub-baseline-2026-11-16.txt`). |

## G - second follow-up round

| Report | Subject | Headline finding | Verification |
|---|---|---|---|
| [G1](G1.md) | Module-only frame-sync sources, event-pool watchdog | Branch `offline-plan-step2`, tip a015861a2, one commit on top of C12 (C10 -> C12 -> G1). Latest recording branch. Whether the new sync sources are safe or fire once per complete frame is open. | Not verified; host test against fake Canon objects. |
| [G2](G2.md) | Abort-handler dump for `pc=0xE021B8FC` | Branch `offline-abortdump1`, tip 741c021f6, two commits on `a10b16c23`; variant B puts 744 bytes in a new section `.abx`. F5's probe as code. Changes the core. | Not verified; built and symbol-gated, never run. |
| [G3](G3.md) | Hardware probe pack | `tools/hwprobe/hwprobe.py` runs the read-only probes the reports ask for; expected values are static predictions. | Self-checked (25 unit tests on canned replies). |

## D - documentation and critique

| Report | Subject | Headline finding | Verification |
|---|---|---|---|
| [D2](D2.md) | Permanent docs from the memory notes | Wrote the numbered docs that replace knowledge held only in chat-memory notes, corrected by the sprint findings. | Not verified. |
| [D3](D3.md) | This index and the README rewrite | - | Corrected by H2 after D7-critic. |
| [D5](D5.md) | Merged Ghidra label proposals | `labels/MERGED.csv`, 361 rows with conflicts marked; nothing written to Ghidra. | Not verified. |
| [D6](D6.md) | Port history | Produced `docs/40-port-history.md`. | Not verified. |
| [D7-critic](D7-critic.md) | **Completeness critique** | No blocker has a hardware-tested fix; ten documentation corrections; eight unresolved contradictions; five single-source findings spot-checked (`task_max` 0x4108 confirmed, A11's writer priorities found to be caller-supplied). | Is itself the check; D5 was not examined. |

## H, R - close-out stage

| Report | Subject | Headline finding | Verification |
|---|---|---|---|
| [H2](H2.md) | Report on the documentation corrections asked for by D7-critic. | Applied the corrections D7-critic asked for; see the report for done / not done. | Not verified. |

## Other files in this directory

| Path | Content |
|---|---|
| `labels/*.csv` | Proposed Ghidra labels (address, name, evidence) from reports A1-A15 and B1-B5. Proposals only; nothing was written to the Ghidra project. |
| `tools/raminit.py` | Looks up the boot-time value of a RAM address from the ROM images (A15). |
| `F1-symgate.txt` | Symbol-gate output for the F1 branch. |
| `backup2/` | Copies of the files edited by H2 (README, INDEX, docs 38, 41, 42, 43, A11, C10, C12) taken before those edits. |
| `backup/` | Copies of `docs/README.md` and docs 30, 32, 36, 37 taken before the edits of 2026-11-16. |
| `D3.md` | Report on this index and the README rewrite. |

## Announced but never produced

Reports F3 and F6. (C10-review, C12 and C3b, listed here earlier as absent, exist and are in the
tables above. `offline-plan-step1` is at 09a4aaf4b, one commit ahead of C10's tip 9633ac828.)

Reading caution added at close-out: A11 (Canon task priorities) is single-source. D7-critic
found that the Fwrite and MovWriter priorities (0x15) are not constants at the create sites
(0xE02F2736 `mov r1,r5`, 0xE04A76DA `mov r1,r4`); the value comes from tracing callers and was
not repeated. `task_max` is 0x4108, not 0x40CC (A14a, F7, D7-critic).
