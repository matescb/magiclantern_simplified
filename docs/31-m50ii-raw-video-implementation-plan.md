# 31 — M50 Mark II Raw Video: Comprehensive Implementation Plan

Status at authoring: **all read-only research + RE complete; no port code written yet.**
This is the executable spec. It is self-contained — a fresh session can run it using only
this file plus its two companions. Lives outside the git repo, so ROM addresses are fine here.

Gate facts: importers/callers = none (planning doc, sibling to docs/28/29/30, consumed by future
sessions); affected API/data schemas = none; user instruction = "Based on findings prepare
comprehensive raw video implementation plan" (via /ecc:plan).

## Companion docs (read first)
- `docs/29-d8-raw-reference-lineage.md` — the D8 raw recipe; **§E = M50 mk1 primary reference**.
- `docs/30-m50ii-raw-stub-resolution.md` — the 28/30 resolved gang100 (1.0.1) addresses.
- Clones under `/mnt/data/WORK/eos/research/`: `bilal_M50_rawvideo` (PRIMARY, M50.110),
  `kitor_edmac_sx740`, `kitor_raw_draft_v2` (R.180), `m6ii_raw_video`.

## Objective
Stock `mlv_lite` recording 14-bit raw video (MLV) on the Canon EOS M50 Mark II (DIGIC 8,
K468, fw 1.0.1), building on the working menu + LiveView + stable boot. Intermediate,
independently-valuable milestone: a single raw DNG (proves the pipeline without the vsync hook).

## Operating constraints (unchanged, in force)
- NEVER update camera firmware (keep 1.0.1).
- Only the user cold-boots the camera (BRICK RISK). Keep a known-good autoexec on a 2nd card.
- ROM addresses stay in code/local docs only, never in public/forum/Discord posts (matches upstream ML).
- Work on fork branch `matescb/magiclantern_simplified@m50mk2-port`.
- QEMU is a compile/smoke gate only — it does NOT model D8 EDMAC, the sensor, or memory pressure.

## Why this is now a bounded port (not novel RE)
bilalfakhouri's `M50_rawvideo` branch achieved working `mlv_lite` on the M50 mk1 — the SAME
DIGIC 8 family, and our port started from the M50.110 stubs. The approach: hand-rolled D8
EDMAC (`edmac-d8.c`), non-SLURP buffer path (Canon fills its own buffer via `lv_set_mm`/
`lv_save_raw`, ML reads the EDMAC channel MMIO and mem2mem-copies frames), and simply enabling
the EVF_STATE hooks for per-frame vsync. The infamous EvfCap crash was a symptom of an
incomplete pipeline, not the hook — so it is expected to resolve once the full pipeline exists.

Every ROM address R1→R3 needs is resolved (docs/30). Only 2 refinements are deferred
(SHAD_GAIN, FPS_CONFIRM) and both tune empirically on-target without blocking bring-up.

================================================================================
PATTERNS TO MIRROR (grounded)
================================================================================
| Category | Source | Pattern |
|---|---|---|
| New platform C file | `research/bilal_M50_rawvideo/platform/M50.110/edmac-d8.c` | hand-rolled D8 mem2mem; copy near-verbatim incl. 2GB handling |
| Stub block style | our `platform/M50mk2.101/stubs.S` (existing MMU/mem cluster) | `NSTUB(addr, name)` lines |
| ROM hook (if ever needed) | `src/patch_mmu.c:861` GraphicsConvertPosType NOP (M50mk2-gated) | — raw needs NONE (empty mmu_patches ok) |
| DMA buffer alloc | our `function_overrides.c` `_alloc_dma_memory` C wrappers + `UNCACHEABLE()` | the path raw buffers use |
| Config flags | `research/bilal_M50_rawvideo/platform/M50.110/{features,internals}.h` | the exact enable set |
| Single-frame test | stock `src/debug.c` `dump_img_task()` `#ifdef CONFIG_RAW_LIVEVIEW` | DNG capture template |
| Validation loop | prior port work: build → stage autoexec.bin → user cold-boot → UART log | per-phase HW proof |

================================================================================
PHASE R0 — Build infrastructure & memory gate
================================================================================
GOAL: modules load on hardware without a DryOS pool panic. This is the first real risk.
FILES:
- `platform/M50mk2.101/Makefile`: `CONFIG_MODULES := n → y`, `CONFIG_TCC := n → y`.
- `platform/M50mk2.101/function_overrides.c`: REMOVE the `module_*` no-op stubs (they exist
  only because the bring-up build had modules off) so the real `module_task` links.
- `platform/M50mk2.101/modules.included`: start with ONE tiny module (`file_man` or `bench`)
  to measure headroom BEFORE adding mlv_lite.
VALIDATE: build → QEMU boot to big.8 → HW cold-boot. Confirm `ml_started=1`, the module_task
runs, the tiny module appears in the ML modules menu, and NO DryOS pool panic (LED-13).
Capture `_bss_end` vs Canon pool headroom from the UART log.
RISK: memory budget — enabling modules/TCC + (later) raw buffers competes with the tight pool
that previously panicked at MMU pages=4. MITIGATION: measure with one tiny module first; if it
panics, reduce `ml_reserved_mem` / revisit MMU-page count before proceeding — do not add mlv_lite
until headroom is proven.
ROLLBACK: revert the two Makefile flags + restore module_* stubs.

================================================================================
PHASE R1 — DIGIC 8 EDMAC engine   (R1a RE = DONE; this is R1b wiring)
================================================================================
GOAL: a working RAM→RAM EDMAC mem2mem copy on hardware.
FILES:
- NEW `platform/M50mk2.101/edmac-d8.c` — copy from `research/bilal_M50_rawvideo/platform/
  M50.110/edmac-d8.c` VERBATIM, including the 2GB handling (drops `& 0x1FFFFFFF` mask, inverts
  the UNCACHEABLE test) and constants: `mem2mem_devices{0,7}`, `mem2mem_resources{0x100AD,
  0x100BB}`, RD_CH 46, WR_CH 13, boomer selector `0x373f0126`, MODE 0, WAIT 50ms.
- `platform/M50mk2.101/Makefile.platform.default` (or Makefile): add `edmac-d8.o` to
  `ML_SRC_EXTRA_OBJS`.
- `platform/M50mk2.101/internals.h`: `#define CONFIG_EDMAC_MEMCPY_D8`; add `CONFIG_MALLOC_STRUCT_V2`
  if not already present (verify against current tree).
- `platform/M50mk2.101/stubs.S`: ADD (gang100 1.0.1 addresses, from docs/30):
    NSTUB(0xe05b848e, edmac_set_address)         NSTUB(0xe05b8862, edmac_set_size)
    NSTUB(0xe05b88fc, edmac_set_transfer_mode)   NSTUB(0xe05b9690, StartEDmac_maybe)
    NSTUB(0xe05b95e4, ConnectReadEDmac_maybe)    NSTUB(0xe05b96b8, edmac_reset_channel)
    NSTUB(0xe05b5720, edmac_reset_boomer_vdkick) NSTUB(0xe0683ca0, edmac_select_boomer)
    NSTUB(0xe05b7c48, RegisterEDmacCompleteCBR)  NSTUB(0xe05b7f68, UnregisterEDmacCompleteCBR)
    NSTUB(0xe05b8f38, edmac_reset_packunpack_mode) NSTUB(0xe05b80c2, edmac_stop_boomer_maybe)
    NSTUB(0xe06b934e, PwrMng_WakeSubChips)       NSTUB(0xe06b937c, PwrMng_SuspendSubChips)
    NSTUB(0xE05BE4BE, CreateResLockEntry)        NSTUB(0xe05be8a6, LockEngineResources)
    NSTUB(0xe05be740, UnLockEngineResources)     NSTUB(0xe05cc5fe, CreateEventFlag_strictly)
  (Delete/Wait/Set/ClearEventFlag, SGI_HANDLERS_PTR, MALLOC_STRUCT_ADDR already present — verified.)
- `platform/M50mk2.101/function_overrides.c`: un-NOP `RegisterEDmacCompleteCBR` /
  `UnregisterEDmacCompleteCBR` / `UnLockEngineResources` (now real stubs); make `shamem_read`
  a direct load with a 0xDxxxxxxx-range guard (port M50 mk1 body). Leave SetEDmac /
  ConnectWriteEDmac / ConnectReadEDmac / StartEDmac / AbortEDmac as NOP (unused on this path).
VALIDATE: add a mem2mem self-test to `src/debug.c` `run_test()` (Don't-Click-Me) — allocate two
DMA buffers, fill src, `edmac_copy_rectangle_cbr_start(...)`, compare dst==src on HW. Success =
EDMAC engine live. (Note: `PwrMng_WakeSubChips` MUST run before any EDMAC MMIO — edmac-d8.c does
this; if the cam hard-locks, that's the first suspect.)
ROLLBACK: gate edmac-d8.o + the stubs behind a flag; revert internals.h define.

================================================================================
PHASE R2 — raw_info params + single DNG   (⭐ first "raw works" proof)
================================================================================
GOAL: one raw frame captured to a valid DNG, WITHOUT the per-frame vsync hook.
FILES:
- `platform/M50mk2.101/features.h`: `#define FEATURE_PICSTYLE`, `#define CONFIG_RAW_LIVEVIEW`.
- `platform/M50mk2.101/internals.h`: `#define CONFIG_NO_DEDICATED_MOVIE_MODE` (verify not already).
  Do NOT define CONFIG_MEM_2GB (M50 mk1 doesn't; 2GB handled in edmac-d8.c). Keep
  CONFIG_PROP_REQUEST_CHANGE undef'd.
- `platform/M50mk2.101/consts.h`: add
    `#define RAW_LV_EDMAC_CHANNEL_ADDR 0xd0420200`  (confirmed)
    `#define SHAD_GAIN_REGISTER 0xd0008030`         (PLACEHOLDER/from-200D — flag; tune later)
    FPS regs (see R2 fps below).
- `src/raw.c` (shared): ensure the M50mk2 D8 hunks are active — the non-SLURP
  `#ifdef CONFIG_DIGIC_VIII` `call("lv_set_mm",1)` branch in `raw_lv_enable`; the M50/M50mk2
  color-matrix + dynamic-range guards; skip_top=34 / skip_left=88 (1080p; FIXME for other modes).
  Port these from the M50 mk1 raw.c (same guards, add CONFIG_M50mk2 where it says CONFIG_M50).
- `platform/M50mk2.101/fps-engio_per_cam.c/.h`: port M50 mk1 — `TG_FREQ_BASE` (M50 mk1 = 27000000;
  VERIFY for M50 II via ADTG/measurement), `FPS_REGISTER_A 0xd0406198`, `FPS_REGISTER_B 0xd04061a4`,
  `FPS_REGISTER_CONFIRM_CHANGES 0xC0F06000` (PLAUSIBLE; validate empirically). Wire `get_fps_register_a/b`.
VALIDATE: stock `debug.c dump_img_task()` (already `#ifdef CONFIG_RAW_LIVEVIEW`): in LiveView,
trigger it → `raw_lv_request()` + `raw_update_params()` + read `raw_info.buffer` → `save_dng()` to
card. Inspect the DNG on desktop: correct WxH, real Bayer image (not garbage), sane black/white.
This proves buffer discovery + params. NO vsync hook yet.
RISK: SHAD_GAIN wrong (deferred) may skew highlight/gain; black/white levels & skip values may need
tuning (M50 mk1 shipped 1080p-only skips). These are image-quality tuning, not blockers.
ROLLBACK: undef CONFIG_RAW_LIVEVIEW → back to R1 state.

================================================================================
PHASE R3 — EVF_STATE vsync hook + mlv_lite recording
================================================================================
GOAL: mlv_lite records a raw clip to card.
FILES:
- NEW `platform/M50mk2.101/include/platform/state-object.h`: `#define EVF_STATE
  (*(struct state_object**)0x10108)` (confirmed HIGH, docs/30) + any sibling defs M50 mk1 has.
- `platform/M50mk2.101/internals.h`: `#define CONFIG_STATE_OBJECT_HOOKS`,
  `#define CONFIG_EVF_STATE_SYNC` (remove the stale commented-out crash note).
- `platform/M50mk2.101/modules.included`: add `mlv_lite` (+ `mlv_play` for on-cam playback).
- `src/state-object.c` is generic — installs `stateobj_lv_spy` on EVF_STATE; `vsync_func()` fires
  on `input==5 && old_state==5` → `module_exec_cbr(CBR_VSYNC)` = mlv_lite frame grab. No new code.
VALIDATE: movie mode → ML menu → mlv_lite → record a few seconds → stop → play in mlv_play (or copy
MLV to desktop, open in MLV App). Success = raw video on the M50 II.
RISK (the one residual): EvfCap crash when hooking EVF_STATE. Precedent says it resolves once the
full pipeline (R1+R2) is present. MITIGATION: R2's single-DNG path already works WITHOUT the hook,
so a crash here is isolated to the hook; debug by diffing our EVF_STATE init path vs M50 mk1's
(both call the same CreateStateObject-equivalent). Movie-mode keycodes also differ from photo mode
(known) — capture if menu nav misbehaves in movie mode.
ROLLBACK: undef CONFIG_STATE_OBJECT_HOOKS → back to the working R2 still-raw state.

================================================================================
MASTER FILES-TO-CHANGE
================================================================================
| File | Phase | Action |
|---|---|---|
| `platform/M50mk2.101/Makefile` | R0 | CONFIG_MODULES/TCC := y |
| `platform/M50mk2.101/function_overrides.c` | R0,R1 | drop module_* NOPs; un-NOP EDMAC CBR/UnLock; real shamem_read |
| `platform/M50mk2.101/modules.included` | R0,R3 | tiny module → mlv_lite |
| `platform/M50mk2.101/edmac-d8.c` (NEW) | R1 | copy M50.110 verbatim |
| `platform/M50mk2.101/Makefile.platform.default` | R1 | +edmac-d8.o |
| `platform/M50mk2.101/stubs.S` | R1 | +18 EDMAC/pwrmng/reslock/evtflag stubs (docs/30) |
| `platform/M50mk2.101/internals.h` | R1,R2,R3 | EDMAC_MEMCPY_D8, MALLOC_STRUCT_V2, NO_DEDICATED_MOVIE_MODE, STATE_OBJECT_HOOKS, EVF_STATE_SYNC |
| `platform/M50mk2.101/features.h` | R2 | FEATURE_PICSTYLE, CONFIG_RAW_LIVEVIEW |
| `platform/M50mk2.101/consts.h` | R2 | RAW_LV_EDMAC, SHAD_GAIN(ph), FPS regs |
| `platform/M50mk2.101/fps-engio_per_cam.c/.h` | R2 | port M50 mk1 |
| `src/raw.c` | R2 | activate M50mk2 D8 hunks |
| `src/debug.c` | R1,R2 | mem2mem self-test; DNG capture |
| `platform/M50mk2.101/include/platform/state-object.h` (NEW) | R3 | EVF_STATE 0x10108 |

================================================================================
VALIDATION LADDER (each on real hardware)
================================================================================
build → QEMU boot gate (big.8) → stage autoexec.bin to EOS_DEVELOP card → user cold-boot → UART log.
- R0: modules load, no pool panic.
- R1: EDMAC mem2mem self-test passes (dst==src).
- R2: single raw DNG, valid on desktop.  ⭐
- R3: MLV clip records + plays.  🎯

================================================================================
RISKS
================================================================================
| Risk | Likelihood | Mitigation |
|---|---|---|
| R0 memory budget / DryOS pool panic | MED | measure with 1 tiny module first; tune ml_reserved_mem/MMU pages before mlv_lite |
| PwrMng ordering hard-lock (EDMAC MMIO before wake) | LOW | edmac-d8.c handles it; first suspect if R1 locks |
| EvfCap crash on EVF_STATE hook | MED (residual) | R2 still-raw works without it; isolated debug vs M50 mk1 |
| SHAD_GAIN / FPS_CONFIRM wrong | LOW (cosmetic) | deferred; empirical tune; M50 mk1 shipped SHAD wrong and worked |
| Image params (black/white/skip) off for non-1080p | MED | port M50 mk1 1080p values first; add modes later |
| Movie-mode keycodes differ (menu nav) | LOW | known; capture if needed |
| ROM address in public post | LOW | keep in code/local docs only |
| Brick on cold-boot | MED | user-only boot; known-good 2nd card |

================================================================================
DEFERRED / KNOWN-UNFINISHED (inherit from M50 mk1)
================================================================================
- SHAD_GAIN_REGISTER (empirical), FPS_REGISTER_CONFIRM_CHANGES (validate on-target).
- 4K/crop skip values (M50 mk1 is 1080p-only); `raw_lv_request_bpp` D8 behavior not fully understood.
- TG_FREQ_BASE value for M50 II (verify vs M50 mk1's 27 MHz).

================================================================================
ACCEPTANCE
================================================================================
- [ ] R0: a module loads on HW, no panic, headroom recorded.
- [ ] R1: EDMAC mem2mem self-test passes on HW.
- [ ] R2: a single raw DNG opens correctly on desktop.  ⭐ (minimum "raw works")
- [ ] R3: an MLV clip records and plays back.  🎯 (goal)
- [ ] Diagnostics gated off for the keeper build; changes committed to m50mk2-port.
- [ ] docs/30 addresses that were PLACEHOLDER/deferred are annotated with on-target results.

================================================================================
EXECUTION LOG
================================================================================
## 2026-07-12 — R0 done (HW mem-gate passed) + R1 wired (QEMU-green)
**R0 (modules + memory gate): DONE.** Makefile CONFIG_MODULES/TCC:=y. Enabling alone pushed
_bss_end 0x14fa00→0x15fe00 → 2 MMU pages starved Canon → boot HALTED before pre.0 (QEMU-repro,
matching the patch_mmu.c warning). FIX: MMU_MAX_REMAP_PAGES 2→1 (GCPT NOP dropped as redundant;
big.5 root-fixed by the _alloc_dma_memory C wrappers; non-fatal assert handler still the net).
_bss_end back to bit-identical 0x14fa00. HW: booted, ML menu works, modules enumerate (bench,
file_man). NOTE the first HW boot crashed in module_task→TCC error1 (0xC833C, NULL deref +0x94)
because the SD reported "Not mounted" (efat_openfent:-2) — a LOOSE CARD, not code; a firmly-seated
re-boot enumerated modules fine. Module *loading* via TCC still to be proven on the return boot.

**R1 (D8 EDMAC engine): WIRED, QEMU-GREEN.** Files: NEW platform/M50mk2.101/edmac-d8.c (ported
from M50.110, hand-rolled boomer mem2mem; mem2mem_CBR param changed to void* for our -Werror);
internals.h +CONFIG_EDMAC_MEMCPY_D8; Makefile +edmac-d8.o; stubs.S +18 THUMB_FN (docs/30 addrs);
function_overrides.c: un-NOP Register/UnregisterEDmacCompleteCBR + UnLockEngineResources (→ real
stubs), shamem_read now a real 0xDxxxxxxx-guarded direct read; debug.c: mem2mem self-test in
run_test() ("Don't click me"), gated CONFIG_M50mk2 && CONFIG_EDMAC_MEMCPY_D8, all buffers via
UNCACHEABLE(), reports PASS/FAIL to UART + NotifyBox. Build: links clean, edmac_copy_rectangle_
cbr_start single-def, _bss_end still 0x14fa00 (edmac added text not bss). QEMU: pre.0→big.8,
modules init runs, no EDMAC crash (dormant until self-test). Image md5 add241d1.

**Button (VIEW→toggle): DEFERRED — needs keycode.** ML modules toggle on BGMT_PRESS_SET (SET);
M50mk2's only mapped button (0x0A) sends BGMT_Q (submenu/info) so it can't enable modules from
the menu. User wants VIEW→BGMT_PRESS_SET, SET/Q→BGMT_Q. But the VIEW/PLAY keycode in this GUI
mode is UNKNOWN (0x9F was wrongly assigned then dropped as noise). So: M50II_INPUT_TRACE=1 in
init.c (ON) to capture VIEW's real keycode from the [ML] BTN UART line on the return boot; then
finalize gui.c (add M50II_KEY_VIEW → BGMT_PRESS_SET) and turn the trace back OFF. Modules are
pre-enabled another way meanwhile:

**Module enable (bypasses the button):** ML loads a module iff ML/SETTINGS/<name>.en exists
(empty file = enabled; config_flag_file_setting_load = FIO_GetFileSize==0). Staging creates
ML/SETTINGS/file_man.en + bench.en so they load on the first boot regardless of the button.

## Return-boot test plan (one boot validates R0-load + R1 + captures VIEW)
1. Boot → File Manager appears (modules LOADED via TCC = R0 fully validated; the path that
   crashed on the loose card).
2. ML menu → Debug → "Don't click me" → NotifyBox "EDMAC mem2mem: PASS" + UART "[R1] EDMAC
   mem2mem self-test PASS" (= R1 EDMAC engine works on silicon).
3. Press the VIEW button → grep UART for "[ML] BTN" to read its keycode → finalize VIEW→toggle.
NOT committed to m50mk2-port yet (await this HW confirmation).

## 2026-07-12 — STEP A done (channel-creation RE): self-test "failure" fully explained, R2 de-risked
QUESTION: does R2/R3 need us to explicitly *create* EDMAC mem2mem channels 46/13, or does Canon?
METHOD: (1) Ghidra gang100 — traced the DAT_e05b996c channel table. Entry = 8 bytes: +0 = ptr to
per-channel descriptor struct, +4 = state bitmask (bit0 = CREATED, checked by edmac_reset_channel
@0xe05b96b8 → assert Engine::Edmac.c:0x77c). ALL 8 functions that reference DAT_e05b996c only READ
+4 or write the descriptor; NONE sets the CREATED bit — it's set by a RegisterEDmac-style creator
that takes the table/entry as an arg (not via this symbol). (2) mk1 reference source (decisive):
our edmac-d8.c == mk1 verbatim; `mem2mem_emdac_copy_d8` calls `edmac_reset_channel(46/13)` as its
FIRST action (line 111-112) — i.e. it ASSERTS-AND-ASSUMES the channel exists, never creates it.
(3) mk1 callers of edmac_copy_rectangle_cbr_start / edmac_memcpy_res_lock = ONLY mlv_lite, mlv_rec,
raw_vid/worker, selftest — every one is the active raw pipeline or the selftest module; NONE is in
cold init.
CONCLUSION:
- Canon creates channels 46/13 as part of the ACTIVE RAW pipeline (imaging/streaming). ML must NOT
  create them (mk1 doesn't). => our cold-photo-standby self-test FAIL is fully expected; NOT a bug.
- **R2's single DNG does NOT exercise EDMAC** — stock dump_img_task does a plain CPU
  `memcpy(buf, raw_info.buffer, frame_size)` (debug.c:216) after raw_lv_request(). So R2 validates
  buffer discovery + raw_info params INDEPENDENTLY of the EDMAC engine. No channels needed for R2.
- **R1's EDMAC engine gets its true validation in R3** (mlv_lite frame grab), during active raw
  streaming where channels 46/13 are guaranteed live. The self-test remains a dormant dev tool.
IMPACT ON PLAN: the "channels not created" item is CLOSED (not a blocker, no code change). Proceed
A→B(commit)→C(R2 single DNG)→D(R3 mlv_lite). No CreateEDmac call to add.

## 2026-07-12 — STEP B done (milestone committed) + R2 wired & staged (QEMU-green, awaiting HW DNG)
STEP B: committed R0+R1+VIEW as `ecd2f853c` on m50mk2-port (9 source files; edmac-d8.o/.d
excluded). Input trace M50II_INPUT_TRACE 1->0 (init.c). Keeper build b1de2caf, QEMU pre.0->big.8
+ ml_started, 8 known non-fatal asserts (KerQueue/MPIDR/GUI.c), no panic.

R2 (CONFIG_RAW_LIVEVIEW single-DNG path): WIRED, QEMU-GREEN, STAGED to card. Edits:
- features.h: +FEATURE_PICSTYLE +CONFIG_RAW_LIVEVIEW.
- internals.h: +CONFIG_NO_DEDICATED_MOVIE_MODE (did NOT add STATE_OBJECT_HOOKS/EVF_STATE_SYNC =
  R3; did NOT add CONFIG_MEM_2GB).
- consts.h: +RAW_LV_EDMAC_CHANNEL_ADDR 0xd0420200 (confirmed) +SHAD_GAIN_REGISTER 0xd0008030
  (placeholder from 200D/mk1 — tune later). Without RAW_LV_EDMAC, raw.c fell through to the wrong
  DIGIC-V default 0xC0F26200.
- fps-engio_per_cam.h: uncommented + set the D8 FPS block A=0xD0406198 B=0xD04061A4
  CONFIRM=0xD0406190 (mk1 values; A/B match docs/30; CONFIRM is mk1's, better than docs/30's
  C0F06000 guess — same 0xD04061xx block; validate on-target).
- fps-engio_per_cam.c: get_fps_register_a/_default/_b stubs (return 1) -> direct MMIO reads (mk1).
- raw.c: (1) added the M50/M50mk2 LV skip hunk skip_top=34/skip_left=88 (1080p, mk1); (2) added the
  missing `#elif defined(CONFIG_DIGIC_VIII)` PACK32_MODE+MODE_*BIT branch (our raw.c predated mk1's
  D8 branch — only had 200D/6D2/7D2; M50mk2 fell through -> MODE_* undefined build error). PACK32_MODE
  0xd0008094 placeholder; EngDrvOut is NOP here so bit depth defaults to Canon's stream.
- state-object.c (shared): RELOCATED vsync_counter + wait_lv_frames OUT of the
  CONFIG_STATE_OBJECT_HOOKS guard (increment stays inside). Reason: raw_lv_enable()/dump_img_task()
  call wait_lv_frames, which was hook-gated; hooks are deferred to R3. With hooks off it now falls
  back to its bounded time-based timeout (enough to settle LV for a single grab). Purely additive
  for other platforms (hooks-on behaviour unchanged).
BUILD: bda35f4c, 255616 B. _bss_end 0x14fd00 = +768 B over the R1 keeper 0x14fa00 — negligible
(2-page value was 0x15fe00, panic was 0x180700), memory budget FINE. QEMU pre.0->big.8+ml_started,
only known asserts + a benign raw_ init line, no new crash (raw dormant until LiveView+dump).
STAGED to /dev/sdb1 EOS_DEVELOP (prior R1 build 1fc6232d backed up -> scratchpad/
autoexec_oncard_before_r2.bin). Modules kept enabled. NOT committed yet (await HW DNG).

HW TEST (R2 ⭐): cold-boot -> half-press shutter to enter LiveView -> FN opens ML menu -> Debug ->
"Dump image buffers" (SET). It runs raw_lv_request()+raw_update_params(), memcpy's raw_info.buffer,
save_dng() to card. Then inspect the .DNG on desktop: correct WxH, real Bayer (not garbage/noise),
sane black/white. QEMU can't model the sensor so this is HW-only. If DNG is garbage: check UART for
"LV raw buffer: <addr> (WxH)" + skip values; likely raw_info.buffer addr (RAW_LV_EDMAC) or bit depth
(PACK32_MODE placeholder). If it works -> commit R2, then R3 (mlv_lite).

## 2026-07-13 — R2 first HW result: property spam (FEATURE_PICSTYLE) fixed; DNG path hangs
HW test of R2 (build bda35f4c, photo mode). Findings from hw_menuinput.log + card:
1. PROPERTY SPAM (user's complaint): ~55x "[PROPAD] GetPropID(0)=0x02050072" + "[PROPST]
   PROPST_DeliverIcuProperty 0x800000d3" during menu use. ROOT CAUSE: FEATURE_PICSTYLE ->
   picstyle.c PROP_HANDLERs register prop_register_slave for ~11 picstyle props (PROP_PICTURE_STYLE
   0x80000028, PROP_PICSTYLE_SETTINGS_* 0x80000066+) which map to 0x0205xxxx ICU props the M50 II
   does NOT expose; Canon's ICU delivery loop (FUN_e01c5c6a @0xe01c5c6a, iterates 47 props) fails
   GetPropID(0x02050072) each broadcast. 0 hits in pre-R2 logs => introduced by R2. FIX: disabled
   FEATURE_PICSTYLE (features.h), kept CONFIG_RAW_LIVEVIEW. Not needed for raw. Rebuild a02b1dc8;
   _bss_end dropped back to bit-identical 0x14fa00 (picstyle.c static data removed).
2. DNG: the dump task RAN (card has M50mk2/PH-LV/LCD/{LV,HD}-000.422 + PH-UNK — two runs) but
   produced NO .DNG anywhere, not even dump_img_task's empty-DNG fallback. .422 are written BEFORE
   the raw section => the raw path (raw_lv_request -> raw_update_params -> save_dng) HUNG/failed
   before the fallback. NO CPU abort in the log => a hang (Canon call blocking or a loop), not a
   crash. Tested in PHOTO mode (PH-LV/PH-UNK) — raw LiveView really wants MOVIE mode.
   DIAGNOSTIC BUILD 5f10b55f: added M50mk2-gated NotifyBox markers in dump_img_task raw section
   (lv=? -> "lv_request done" -> "params=%d buf=%x WxH fs=%d" -> "save_dng done") so the next HW
   test localizes the hang WITHOUT UART (CONFIG_DEBUGMSG=0). Staged to card; stale .422 cleared.
   Interpreting the next test: hangs at "request..." => lv_set_mm/lv_save_raw blocks; params=0 =>
   buffer discovery failed (RAW_LV_EDMAC/mode); params=1 but no "save_dng done" => save_dng crash;
   sane buf/WxH + "save_dng done" => success (check DNG on desktop).

---

## DUPLEX UART — serial debug console (2026-07-14)

**Why:** collapse the edit→build→SD→cold-boot→capture loop into a live REPL. Directly
unblocks (a) menu-nav dial decode (inject keycodes / dump the 0xB7 rotation struct live)
and (b) R2 raw-EDMAC hunt (md/edmac the 0xd0420200 region live while LiveView streams).

### RX register map — recovered from ROM (channel 0, base 0xC0800000)
Decoded from Canon's own polled reader `FUN_e007d878` and RX ISR `FUN_e007da46`
(siblings of `uart_write_bytes` @0xE007D814 via the base-ptr table `DAT_e007db44`):

| reg | addr | meaning |
|-----|------|---------|
| TX data (write) | 0xC0800000 | already used by m50ii_uart_str |
| RX data (read)  | 0xC0800004 | `*dst = (char)*(base+4)` |
| STATUS          | 0xC0800014 | bit0(0x1)=RX ready, bit1(0x2)=TX ready, bits 0x38=RX errors |
| ACK read        | 0xC0800014 | write 1 after reading data |
| ACK error       | 0xC0800014 | write 0x38 to clear framing/overrun |
| MASK/enable     | 0xC0800018 | interrupt mask |

RX primitive (no ISR hook): poll STATUS; if bit0, read 0xC0800004, write 1 to STATUS.

### CONTENTION RISK (what Phase 0 tests)
`FUN_e007da46` is a Canon RX ISR that drains 0xC0800004 into its own ring. If it's live
on shipped fw, it races our poll for bytes. Phase-0 echo test disambiguates:
 - clean echo         => Canon ISR dormant, poll mode safe → proceed to Phase 1 console.
 - no echo, heartbeat keeps printing => ISR stealing bytes → read Canon ring / mask 0xC0800018.
 - no banner / hang   => worse; revert with backup card.

### Phase 0 build a65b31535bc9b4790eed2996b823cd76 (STAGED, awaiting HW cold-boot)
 - NEW FILE src/serial_shell.c: poll-mode RX echo task ("ml_serial", prio 0x1f), banner +
   per-byte echo + ~3s "alive rx_total" heartbeat. Body guarded by CONFIG_M50mk2 (empty TU
   elsewhere). Added serial_shell.o to platform/Makefile ML_SRC_OBJS_00.
 - init.c: M50II_SERIAL_SHELL gate (default 1, keeper OFF); m50ii_serial_shell_start() called
   in boot_post_init_task beside the ml_in task.
 - QEMU gate PASS: reaches big.8/ml_started (same depth as prior keeper stepB), banner prints,
   41 alive heartbeats, rx_total=0 (no serial-in in QEMU, expected). No new asserts.
 - Prior card build 648a835a (dial-decode) backed up to scratchpad/autoexec_oncard_before_serial.bin.

### Phase 0 RESULT: loopback works on HW (2026-07-14)
Duplex confirmed end-to-end; Canon RX ISR is dormant -> poll mode safe. Green light for console.

### Phase 1 build 5e10d6bda80bbae9607476721b9cf85e (STAGED, awaiting HW)
serial_shell.c is now a line-reader + dispatcher (CR/LF + backspace, echo, "ml> " prompt):
  help | ver | md <addr> [nwords] | rd <addr> | edmac | key <code>
  wr/call gated behind M50II_SERIAL_UNSAFE (default 0). md bounded to 0x100 words.
'key <code>' injects into m50ii_handle_canon_gui_event(0,code,0) -> live menu-nav debugging
  (e.g. key 0x40=UP, 0x42=DOWN, 0x22=FN, 0xB7=dial). 'edmac' dumps 0xd0420200 x0x10 for R2.
QEMU gate PASS: big.8, "serial console ready", tier1-diag END, no new asserts. Prior echo
build a65b3153 backed up to scratchpad/autoexec_oncard_before_serial.bin.

### Two-sided debug harness (2026-07-14) — build 3bd5e57f0063cd3f3b818ba93ce96737 STAGED
Goal: operator cold-boots ONCE, then Claude drives the camera over /dev/ttyUSB0 (no hand-typing),
collapsing the edit->build->SD->reboot->capture loop into a live session.

CAMERA (src/serial_shell.c v2): @seq framing + =ok/=err @seq terminators; async [ML] logs stay
distinct. Commands: help ver md rd dump(hex+CRC32) edmac watch key wr call load exec. Only hard
rail = no ROM writes (wr/load refuse addr>=0xE0000000, confirmed rom start). exec = cache-sync +
call a thumb blob staged in a RAM scratch (arbitrary code without a rebuild). QEMU gate PASS
(big.8, "serial harness ready", no new asserts).

HOST (tools/mlserial/, outside firmware repo):
 - mlserald.py: pyserial daemon, owns the port, timestamped uart.log + structured uart.jsonl,
   seq-correlated replies, reboot-resilient reopen loop, Unix-socket control (socket in /tmp,
   md5(RUN_DIR) name to dodge AF_UNIX 108-char limit).
 - mlserial.py: CLI I invoke -- daemon start/status, ping, cmd, key <name>, dump <a> <len> <f>
   (CRC-verified), edmac-scan (host heuristic over md), watch, log [-f], symbolize (nearest sym
   from magiclantern.sym; auto-decodes crash addrs).
 - PROTOCOL.md documents the wire contract.
VALIDATED end-to-end 5/5 against a PTY fake camera (scratchpad/fake_cam.py + harness_test.sh):
ping/md/key/ROM-guard/dump+CRC all correct. Awaiting single HW cold-boot to go live.

### R2 RAW-LV EDMAC CHANNEL FOUND LIVE via serial harness (2026-07-14, build ea367b21)
Used the working duplex harness (call/md/watch/dump) to solve the R2 buffer-discovery blocker
WITHOUT any rebuild:
 - `call('lv_save_raw', 1)` works via Canon call-by-name @0xe05cbf83 (ML `call` stub); returns 1.
   (Staged the "lv_save_raw" string into the harness scratch buf, then `call 0xe05cbf83 <str> 1`.)
 - M50 II EDMAC channels are at 0xd0420000 + N*0x100; the ram_addr (buffer-ptr) reg is at
   channel_base + 0xa0.
 - Before lv_save_raw: ch2 (0xd04202a0) = 0. AFTER lv_save_raw(1): ch2 = 0x6BC61CC8 (uncached RAM).
   => **RAW LV EDMAC = channel 2; ram_addr register = 0xd04202a0** (NOT 0xd0420200, which is the base).
 - Dumped 0x6BC61CC8: 77 distinct bytes / 253 nonzero of 256 = real packed 14-bit raw sensor data.
 - Buffer pointer is STABLE (single fixed buffer Canon overwrites in place), so 0xd04202a0 read
   once gives the raw frame address.
ROOT CAUSE of the earlier DNG hang CONFIRMED: consts.h RAW_LV_EDMAC_CHANNEL_ADDR=0xd0420200 read the
channel BASE (=0) not the ram_addr (+0xa0), AND lv_save_raw was never enabled -> raw_update_params
got a null buffer -> hung before the DNG fallback.
FIX for the R2 build: (1) RAW_LV_EDMAC ram_addr = 0xd04202a0; (2) ensure call('lv_save_raw',1) runs
before raw_update_params (CONFIG_RAW_LIVEVIEW path). Other live channels @ +0xa0 (movie LV baseline):
ch1=0x6AC2FB00 ch3=0x6BA1962E ch4=0x7EDxxxxx ch5=0x7EDxxxxx ch7=0x43C1CC00(disp) ch10=0x43C2xxxx(disp).

### R2 pipeline FULLY VALIDATED live (2026-07-14, harness) — ready for DNG build
Confirmed the raw address const does NOT need changing:
 - CONFIG_DIGIC_8X IS defined for M50mk2 (internals-common.h:25 <- CONFIG_DIGIC_VIII), so the
   edmac_mmio struct with ram_addr @ +0xa0 is used. raw.c reads &raw_lv_edmac->ram_addr =
   0xd0420200 + 0xa0 = 0xd04202a0. So RAW_LV_EDMAC_CHANNEL_ADDR=0xd0420200 is CORRECT as-is.
 - Live via harness: call('lv_set_mm',1)=ret0 (select RAW vs default YUV) + call('lv_save_raw',1)=ret1.
   ch2 ram_addr(0xd04202a0)=0x6BC61CC8 (uncached raw buffer). Geometry reg 0xd0420250=0x048B0E54
   => xb=0x0E54=3668 bytes/line (=>2096 px @14bit), yb=0x048B=1163 (~1164 lines). Legit raw frame.
 - raw_lv_enable() already calls lv_set_mm(1)+lv_save_raw(1) on D8, and the DNG path gates on
   lv_raw_enabled. So the earlier DNG hang (params=0/buf=0) was PHOTO mode (no raw stream) — NOT
   the address. In MOVIE mode the buffer+geometry are valid.
NEXT (Lane B): build a raw-DNG capture build, test IN MOVIE MODE, pull DNG, validate on desktop.
No consts change needed; just ensure the DNG trigger runs raw_lv_enable + is exercised in movie mode.

### R2 DNG path TRIGGERED LIVE via harness (2026-07-14) — save_dng is the remaining issue
Spawned the existing dump_img_task on-camera with NO rebuild:
  call run_in_separate_task(dump_img_task,0)  ->  call 0xd4d25 0xd6f25 0  (ret = task handle 0x1554013C)
(dump_img_task @0xd6f24, run_in_separate_task @0xd4d24, from ELF nm; thumb ptrs +1.)
Result: the run_test task ran, then UART showed:
  [FIO] ERROR @run_test Write(fd=3, pvBuffer=0, dwSize=0) L:528
=> save_dng was reached (so raw_update_params returned non-zero / m50ii_rup true) but a strip
   Write got a NULL buffer / 0 size. The FIO warning MAY be non-fatal (other Write calls could
   have written the real DNG data). NEXT: check the card for RAW-*.DNG. If present -> R2 milestone
   (first M50 II raw DNG), inspect/validate. If absent -> debug save_dng: inspect raw_info live
   (buffer @+0x04, height +0x08, width +0x0c, pitch +0x10, frame_size +0x14) after calling
   raw_update_params (0xe5731) via the harness; likely frame_size/pitch or a strip pointer = 0.
Address const 0xd0420200 CONFIRMED correct (no change needed). Everything committed (49b89e783/d9614bca4).

### R2 DNG result: NO valid DNG (save_dng writes 0 bytes) — corrected
Card check after the live trigger: only M50mk2/MV-1080/LCD/RAW-000.DNG exists and it is 0 bytes
dated 2027-06-04 = STALE (prior session). No RAW-001 today. So save_dng did NOT write image data;
the [FIO] Write(pvBuffer=0,dwSize=0) was fatal to the output.
LIKELY CAUSE (debug.c M50mk2 path, ~line 231): after the guarded raw_update_params() succeeds, the
code calls raw_update_params() AGAIN unconditionally after wait_lv_frames(3)+raw_set_dirty(); if that
2nd call fails/zeroes raw_info (buffer/frame_size -> 0), malloc(0)+memcpy(0) then save_dng writes 0.
RESUMABLE NEXT STEP (harness, needs reboot): find raw_info addr (from build/location.map or by
disassembling raw_update_params @0xe5731 which stores into it), then via harness:
  call 0xe5831 (raw_lv_request) -> wait -> call 0xe5731 (raw_update_params) -> md <raw_info> 8
  (fields: +04 buffer, +08 height, +0c width, +10 pitch, +14 frame_size). Confirm which field is 0.
Then fix the M50mk2 dump_img_task path (guard the 2nd raw_update_params; ensure frame_size/buffer
stay valid) and/or save_dng strip write. Pipeline inputs (channel/buffer/geometry/lv_save_raw) ALL
proven good; only the raw_info->save_dng handoff remains.

### R3 mlv_lite: EVF_STATE CONFIRMED via Ghidra (2026-07-14)
EVF_STATE = *(struct state_object**)0x10108. Proof: 0x10108 is written by FUN_e069a640 with a
CreateStateObject result (FUN_e05bb31a(name,0,matrix@0xe0bbe0f0,0xf,4)); the name ptr *(matrix-4)
=*(0xe0bbe0ec)=0xe137f588 -> string "EvfDevState". So define in
platform/M50mk2.101/include/platform/state-object.h:  EVF_STATE = *(struct state_object**)0x10108.
NOTE: CreateStateObject args (...,0xf,4) => the exact readout-done transition (R.180 uses
input==5 && old_state==5) may differ on M50 II EvfDevState (n_states/n_inputs 4/0xf); verify the
real vsync transition at runtime via the harness (watch EvfDevState transitions) or a state logger.
mlv_lite recipe (docs/29 §B): internals.h += CONFIG_STATE_OBJECT_HOOKS + CONFIG_EVF_STATE_SYNC +
CONFIG_EDMAC_MEMCPY_R; state-object.h EVF_STATE; enable mlv_lite module build; raw.c side already
done+validated. RISK: EvfCap crash when hooking EVF_STATE is UNRESOLVED on non-R D8 (SX740/M6II);
mk1 M50 reportedly worked -> mk2 likely OK but test with backup card ready.

### R3 EVF_STATE — AMBIGUITY to resolve at runtime (2026-07-14)
platform/M50mk2.101/include/platform/state-object.h currently has EVF_STATE=*(0x6cec) but 0x6cec
has ZERO xrefs in ROM = unverified guess (hooking a non-state-object crashes at boot).
Ghidra-confirmed state object: 0x10108 = "EvfDevState" (written by CreateStateObject in FUN_e069a640).
BUT "EvfDevState" may be the device/mode state, not the per-frame readout state the vsync hook needs
(there are also ROM strings "EVF_STATE"@e06ef188 and "EvfState"@e137558c). So which object carries the
evfReadOutDone transition is UNRESOLVED.
RESOLUTION (harness, needs boot): read candidate globals live, verify each points to a struct whose
+type field = "StateObject", then install a logging spy / watch the object's current_state field
during LiveView to see which one advances once per frame. Pick that object + read its transition
matrix to get the exact (input,old_state) for the readout event (R.180 uses 5,5; M50 II EvfDevState
was created with n=(0xf,4) so it likely differs). THEN set EVF_STATE + the D8 vsync branch and build.
Do NOT enable CONFIG_STATE_OBJECT_HOOKS with an unverified address (boot-crash risk).
mlv_lite module IS present at modules/raw_video/mlv_lite (no port needed). state-object.h template =
one #define (cf 200D.101). Remaining after EVF_STATE resolved: internals enables + modules.included +
build + EvfCap-crash test.

### R3 EVF_STATE — resolved via harness: BOTH candidates wrong, need per-frame object (2026-07-14)
Read both candidates live (state_object struct: +0=type"StateObject" +4=name +14=max_inputs
+18=max_states +1c=current_state):
 - *(0x10108) = 0x00DCC0B8: VALID StateObject "EvfDevState", matrix 0xe0bbe0f0, max(0xf,4),
   current_state STUCK at 3 even with LiveView actively streaming (display EDMAC ch10 0xd0420aa0
   flips 0x43C40E30<->0x43C28C00 per frame). => EvfDevState is the DEVICE/mode state, NOT the
   per-frame readout object. WRONG for the vsync hook.
 - *(0x6cec) = 0xE0A6F070: type field = 0x33A69C00 (not a "StateObject" ptr) => NOT a state object.
   The existing header EVF_STATE=*(0x6cec) is a bad guess; hooking it would crash.
NEXT (find the per-frame object): enumerate CreateStateObject (FUN_e05bb31a) call sites in Ghidra
and their store-to-global targets; the readout object's name is likely "EvfState"/"EVF_STATE"
(strings @ e137558c / e06ef188) not "EvfDevState". OR harness-scan: read candidate state globals,
find the one whose current_state (+0x1c) cycles once per frame during LV; then read its matrix to
get the exact (input,old_state) for the readout transition (R.180 used 5,5 but M50 II states=0..3).
Set EVF_STATE=*(that global) + D8 vsync branch, then enable CONFIG_STATE_OBJECT_HOOKS + build mlv_lite.

### R3 EVF_STATE RESOLVED via statespy harness command (2026-07-14) — 0x10108 IS CORRECT
Added `statespy <objptr>` to serial_shell.c: hooks a state_object's transition ptr (+0x0c) with a
recorder (count + last input/old/new into globals; no UART in the transition ctx), poll via `statespy`.
Result on EvfDevState (*(0x10108)=0x00DCC0B8): count climbed ~130/s during LiveView with the dominant
transition input=6, old_state=3 -> new_state=3 (a SELF-transition -> current_state stays 3, which is
why the earlier current_state watch wrongly looked "static"). Hooking it 3s did NOT crash LV (clean
un-hook) => EvfCap crash risk LOW for this object.
CONCLUSION: EVF_STATE = *(struct state_object**)0x10108 (EvfDevState) IS the per-frame vsync object.
D8 vsync branch: (self==EVF_STATE && input==6 && old_state==3) -> vsync_func(). (~130/s may be >1/frame;
refine trigger empirically if mlv_lite over-captures.) The prior "0x10108 static/wrong" note is SUPERSEDED.
Proceed: state-object.h EVF_STATE=*(0x10108); state-object.c D8 branch; internals CONFIG_STATE_OBJECT_HOOKS
+CONFIG_EVF_STATE_SYNC; modules.included mlv_lite; build+test.

### R3 state-object hook ENABLED + NULL-safe (2026-07-14, build 276592251cfc9537c865578ff022246e)
Changes: state-object.h EVF_STATE=*(0x10108); internals.h +CONFIG_STATE_OBJECT_HOOKS +CONFIG_EVF_STATE_SYNC;
state-object.c: D8 vsync branch (self==EVF_STATE && input==6 && old_state==3)->vsync_func(); + NULL-safe
install (stateobj_start_spy guards NULL; state_init bounded-waits ~10s for EVF_STATE before hooking).
WHY null-safe: QEMU never creates EvfDevState so *(0x10108)=0 -> state_init crashed (null deref @state_init
pc=0xE1538, DFAR=0). Bounded wait => QEMU skips (reaches big.8), HW installs when object appears.
Still NO mlv_lite module yet (testing the hook in isolation first). QEMU gate PASS (big.8, no abort).
vsync_counter @ 0x0014c360 (verify per-frame climb on HW via harness rd).
AWAITING HW TEST: flash 27659225, boot to movie LV, harness: rd 0x14c360 twice ~1s apart -> should climb
~130/s if vsync fires. If boot OK + counter climbs => hook works, add mlv_lite module next. Backup card ready.

### R3 Phase A+B — HW-VALIDATED via harness (2026-07-14, build 27659225) ✅ KEYSTONE FOUND
PHASE A (vsync hook): booted movie LV, harness `rd 0x14c360` (vsync_counter) climbs monotonically
at ~23.6/s (≈24fps = ~ONE call/frame, NOT the feared 130/s -> no over-capture). Camera fully
responsive, LV did not crash. EVF_STATE(*(0x10108)) hook CONFIRMED firing per-frame on HW. EvfCap
crash risk did NOT materialize.

PHASE B (raw_info live, no rebuild): raw_info@0x103b7c was ALL ZERO at rest (buffer=0, w/h/pitch/
frame_size=0) = static-init default -> this is the exact 0-byte-DNG/0-frame root. Drove the OFFICIAL
enable: `call raw_lv_request()` @0x000e5961 (thumb). Result:
  - EDMAC ram_addr 0xd04202a0: 0 -> 0x6BC61CC8 (raw stream came up)
  - raw_info populated EXACTLY: buffer=0x2BC61CC8 (cacheable mirror), height=1164(0x48C),
    width=2096(0x830), pitch=3668(0xE54=2096*14/8), frame_size=4269552(0x4125F0=3668*1164),
    bpp=14, black=2047, white=16200 (autodetected). lv_raw_enabled=1.
  - raw buffer @0x6BC61CC8 holds live varied 14-bit packed sensor data (full frame).
KEYSTONE INSIGHT: the vsync hook is what makes the WHOLE raw pipeline work. With hook OFF,
wait_lv_frames(2) (in raw_lv_enable) timed out -> stream never settled -> raw_update_params read an
idle EDMAC channel -> zeros -> 0-byte DNG (R2 #17). With hook ON, wait_lv_frames is real ->
raw_update_params gets valid registers -> perfect raw_info. This unblocks BOTH single-DNG and MLV.
D8 enable path (raw_lv_enable): lv_set_mm(1) [select RAW, default YUV] + lv_save_raw(1). raw_lv_request
@0x000e5960, raw_update_params @0x000e5860, RAW_LV_EDMAC_CHANNEL_ADDR=0xd0420200 (+0xa0=ram_addr).
NEXT (Phase C): add mlv_lite to platform/M50mk2.101/modules.included + build (non-slurp/CBR_VSYNC path,
D8 edmac_copy_rectangle_cbr_start, uncompressed-first) + QEMU gate + flash + capture MLV clip.

### R3 Phase C+D — module ENABLED + loads, but TCC LINK FAILS (2026-07-14) ⛔ NEW BLOCKER
FLASH: card AUTOEXEC.BIN=27659225 (verified hook) + ML/modules/mlv_lite.mo (md5 9e6b9db5, 73156B,
verified from fresh remount). NOTE flaky USB reader: mount opt errors=remount-ro flips FS ro on any
I/O glitch -> writes silently revert (page-cache md5 can falsely "match"); ALWAYS verify from a fresh
remount. Different USB port cured it (card re-enumerated sdb1->sdc1).
ENABLE: mlv_lite needs ML/SETTINGS/<name>.en marker (module.c:602 config_flag_file_setting_load;
absent => not loaded). Skipped the copy_included_modules.py step (minimized writes), so no marker.
FIX w/o card shuffle: harness `call toggle_module_enabled(2)` @0x000cd379 (idx2=mlv_lite) -> writes
ML/SETTINGS/mlv_lite.en via camera FIO (the [FIO]Remove ecode=0x100b log = harmless pre-delete of
absent file), enabled flipped 0->1 (module_list[2].enabled @0x110350=1). toggle_module_enabled@0xcd378,
get_config_dir="ML/SETTINGS/", module_list@0x110134 stride 0xB8 (name@+0 filename@+9 status@+0x49
valid@+0xA8 enabled@+0xAC error@+0xB0 visible@+0xB4).
REBOOT -> mlv_lite LOAD ATTEMPTED but TCC link FAILS: on-screen + last_error_msg@0x1464f4 =
"No allocator for 1.7GB|TMP at libtcc.c:226, module_task." (libtcc.c:226 = tcc_malloc's
AllocateMemory(size), MEM_TEMPORARY=shoot_malloc). module_list[2]: enabled=1 valid=0 error=0.
Camera STAYS ALIVE (bench/file_man loaded, ML+harness responsive). ~1.7GB is a GARBAGE size (no
tcc alloc is legitimately that big; mlv_lite.o text42K/bss30K, .mo sane sections).
RULED OUT: (a) stale/corrupt .mo (md5 verified on card); (b) .mo section sizes garbage (readelf: all
sane, 18 sections); (c) missing .module_deps mis-parse (bench/file_man ALSO lack .module_deps + load
fine, so tcc_load_offline_section handles missing section; module.c:1430 NULL-guards it); (d) real OOM
(1.7GB not a real link need).
DISTINGUISHING FEATURE of mlv_lite vs bench/file_man: much bigger + has a .config_vars section
(+.rel.config_vars) holding its dozens of CONFIG_INTs. bench/file_man have .module_strings but NO
.config_vars. => SUSPECT: the .config_vars processing OR the full tcc_load_object_file/relocation of
the large module computes a garbage size. load_data()@tccelf.c:2337 does tcc_malloc(size); size flows
from section headers / a count. tcc_add_file @module.c:669 is the real load.
NEXT (fresh TCC dive): (1) get EXACT failing size (instrument tcc_malloc to bmp_printf/UART the size+
a return-addr, or add a guard printing size at libtcc.c:226) to identify the caller; (2) inspect
.config_vars handling in tcc-glue/module.c (module_config load) for a count/size read from the .mo;
(3) check relocate_section / set_elf_sym (tccelf.c:348 new_syms=tcc_malloc(nb_syms*16)) for an nb_syms
blow-up when linking a large module against the card .sym; (4) confirm card ML/modules/<MODEL>.sym is
the CURRENT one (I copied only the .mo, not the .sym; addresses identical to 27659225 so should match,
but verify). Likely fix = a small loader guard OR a build-time .mo change. mlv_lite.o/.mo build OK.
EVERYTHING UPSTREAM PROVEN: hook fires (Phase A), raw_info perfect + raw streaming (Phase B). Only the
module LINK stands between us and MLV capture.

### R3 mlv_lite LINK ROOT-CAUSED + FIXED — MODULE LOADS ON M50 II (2026-07-14) ✅✅✅
The "No allocator for 1.7GB" was a DOWNSTREAM ARTIFACT, not the root cause. Instrumented
tcc_relocate_ex (tcc/tccrun.c — NOTE the CORE build compiles tcc/ NOT modules/tcc/; Makefile:653
cp -r ../../tcc build/) with a reboot-persistent capture array g_tccdbg[]/g_tccdbg_n read over the
harness. Findings: the section size loop = 0x1A800 (106KB, SANE — NOT the balloon); capture stopped
right after at relocate_syms => the failure is UNRESOLVED SYMBOLS. Static diff (nm mlv_lite.mo UND
syms vs build/magiclantern.sym) found the 8 MISSING symbols mlv_lite needs but this port gates OUT:
  srm_malloc_suite, srm_free_suite            (SRM off: CONFIG_MEMORY_SRM_NOT_WORKING, mem.c:1064)
  picstyle_get_current_{name,sharpness,contrast,saturation,color_tone}  (FEATURE_PICSTYLE off)
  ErrCardForLVApp_handler                     (Canon LV-error dialog handler, no M50mk2 stub)
relocate_syms(tccelf.c:422) tcc_error_noabort("undefined symbol") on these -> nb_errors ->
tcc_relocate_ex returns -1 -> "failed to link modules". FIX: 8 safe compat stubs in
platform/M50mk2.101/function_overrides.c (srm->NULL so mlv_lite falls back to shoot_malloc; picstyle
->neutral defaults; ErrCardForLVApp_handler=dummy thunk, &it never matches a real dialog handler).
Rebuilt (autoexec c50add74 + matching M50mk2_101.sym b841f2e6 — MUST flash BOTH, addresses shift
every build; card mlv_lite.mo/.en unchanged). RESULT on HW (needed one clean reboot to run the module
loader): module_list[2]=mlv_lite valid=1 enabled=1 error=0; info=0x010DC708 cbr=0x010DC67C
config=0x010DC5F4 (all non-null = init ran + CBRs/config registered). g_tccdbg: post-size-loop
0x1A800, FINAL size to tcc_malloc = 0x24800 (146KB, +40KB from 4 reloc sections x10240) — NO 1.7GB.
mlv_lite IS ALIVE on the M50 II. Current addrs (build c50add74): module_list@0x110438 (stride 0xB8),
raw_info@0x103d3c, vsync_counter@0x14c360, g_tccdbg@0x110104. NEXT: enable raw video + trigger record
(menu via harness keys OR find module raw_video_enabled/start-rec via config@0x010DC5F4 walk) ->
capture .MLV -> host decode. TODO cleanup: strip g_tccdbg instrumentation from tcc/tccrun.c (+revert
the stray modules/tcc/tccrun.c edit) for the keeper; commit the function_overrides.c shim.

### R3 mlv_lite RAW VIDEO RUNS + recording-trigger findings (2026-07-14)
✅ RAW VIDEO ENABLES + PREVIEWS ON M50 II HW! set_config_var("raw.video.enabled",1) via harness
(set_config_var@0x000da720, thumb call 0xda721, stage name in scratch w/ `load`) = SAFE (no crash,
heartbeat kept ticking). User in movie LiveView selected RAW video 640x360 29.62fps -> sees the
CROPPED raw preview live (ML overlays NOT compositing during LV = separate cosmetic OSD gap). The
earlier "crash on enable" was at a LARGER res/other state; 640x360 crop is safe. raw_info@0x103d3c.
RECORDING TRIGGER: the physical REC button does NOT reach mlv_lite (Canon H.264 recording runs
instead) — mlv_lite's keypress/movie hook isn't wired to the M50 II record event. Recording is
toggled by static `raw_start_stop()` (mlv_lite.c:3966; when idle: gui_stop_menu()+task_create(
raw_video_rec_task) -> SAFE to call from any task). To call a MODULE (static) symbol by address:
raw_start_stop is static (.mo strips it) but IS in mlv_lite.o @0x43fc (.text); raw_rec_init @0x450
(.text, both .o and .mo). So raw_start_stop_runtime = raw_rec_init_runtime + 0x3FAC.
⚠️ MISTAKE MADE: tried raw_rec_init_runtime = *(module_info.init) reading @ module_list[2].info+0x0C
(0x010DC708+0x0C=0x010DC714) but that read 0x00000000 (WRONG offset assumption for module_info_t
{api4b, name@+4, long_name@+8, init@+0xC} OR module_list[2].info!=0x010DC708). Computed 0x3FAC and
CALLED 0x3FAD -> jumped to garbage -> CAMERA HUNG (power-cycle). LESSON: SANITY-CHECK computed module
addrs are in 0x010Dxxxx BEFORE calling. CORRECT next approach: anchor off module_list[2].cbr
(=0x010DC67C, a module_cbr_t[] {name,symbol,type,handler@+0xC,ctx}=20B each) — read a handler (e.g.
raw_rec_vsync_cbr @.o/.mo offset 0x7520) runtime addr, then text_delta = handler_rt - 0x7520,
raw_start_stop_rt = 0x43fc + text_delta. VERIFY raw_start_stop_rt is ~0x010Dxxxx, then call |1.
Then wait, call again to stop (RAW_FINISHING), pull .MLV, host decode (raw2dng/mlv tools).
Module base region ~0x010DC000. Recording path (raw_video_rec_task) is UNTESTED on M50 II — may crash
(buffer alloc/EDMAC/card-write); capture via daemon uart.log + abort handler.

### R3 recording path CRASHES on M50 II (2026-07-14) — NEXT DEBUG TARGET
raw_start_stop address SOLVED + validated: text_delta = handler_rt - cbr_offset = 0x010C3E70
(CONSISTENT across all 5 mlv_lite CBR handlers read from module_list[2].cbr table @0x010CE144;
module_cbr_t = {name,symbol,type,handler@+0xC,ctx}=20B; types 6/9/8/3/4/0x8000 =
VSYNC/VSYNC_SETPARAM/KEYPRESS_RAW/SHOOT_TASK/DISPLAY_FILTER/named). raw_start_stop_rt =
0x43fc + delta = 0x010C826D (thumb). NB module base SHIFTS per boot (0x010CExxx this boot vs
0x010DCxxx prior) + module_info.init is NULL (mlv_lite has no MODULE_INIT) — always re-derive delta
from the live cbr table, never assume.
CALL RESULT: with raw ACTIVELY STREAMING (640x360 crop preview up, raw_info.buffer=0x2BC61CC8),
`call 0x010C826D` -> CAMERA HUNG (no "ret", no trace on uart.log). So the RECORDING ENGINE crashes,
not the trigger. raw_start_stop when idle does gui_stop_menu() + task_create(raw_video_rec_task).
Two suspects: (a) gui_stop_menu() called from ml_serial (REQUIRES GuiMainTask) deadlocks/faults; OR
(b) raw_video_rec_task (the real record path: buffer alloc via shoot_malloc since SRM stubbed to
NULL, D8 edmac_copy_rectangle_cbr_start per frame, FIO .MLV writes) faults on first real use. The
hang printed nothing = likely a fault before/without the abort handler, or a deadlock.
NEXT SESSION PLAN: (1) don't call raw_start_stop from ml_serial — instead wire the M50 II REC button
to mlv_lite's keypress path (proper context) OR add a tiny ML-core helper task that calls
raw_start_stop from a safe context. (2) Instrument raw_video_rec_task entry + each setup step
(printf/UART) so the fault point is captured. (3) Consider gui_stop_menu the culprit first (cheap:
patch raw_start_stop path or test a variant that skips it). (4) Verify shoot_malloc can provide the
~4MB frame buffers under M50 II memory (SRM disabled). Everything up to recording is PROVEN: hook,
raw_info, raw preview all live; module loads; only the record engine remains.

### R3 OBSERVABILITY build (2026-07-14) — durable crash breadcrumbs
Approved plan: stabilize + get visibility. Grounding found the key latent tool: ML already writes
ML/LOGS/console.log PER-LINE (console.c:105 FIO open+write+close each console_puts) — SURVIVES a hang
— but it's gated by CONSOLE_DEBUG which was #undef'd (console.c:17). ALSO: CONFIG_CRASH_LOG is
#undef'd on M50 II (features.h:50) [not enabled yet — risk of clashing with the existing init.c UART
abort handler]. Module symbols resolvable at runtime via module_get_symbol() (module.h:290) [future
`modsym` harness cmd to stop hand-computing addrs — the cause of one self-crash].
CHANGE MADE: console.c:17 -> #define CONSOLE_DEBUG under #ifdef CONFIG_M50mk2 (per-line card log).
Built autoexec 3788486d (addresses shift every build -> MUST flash autoexec + matching M50mk2_101.sym
together; mlv_lite.mo/.en unchanged; g_tccdbg instrumentation still in tcc/tccrun.c, harmless).
HOW TO USE: flash 3788486d + its .sym -> boot -> reproduce the record crash (enable RAW 640x360, call
raw_start_stop via harness OR press REC once wired) -> on hang, pull card -> read ML/LOGS/console.log:
the LAST line = where raw_video_rec_task died ("Starting raw recording...", then buffer/EDMAC/FIO
prints up to the fault). Also check ML/LOGS/console.log from ANY prior crash boot (it's overwritten
only on the NEXT ML boot, console.c:91). Remaining plan (future): CONFIG_CRASH_LOG, harness `state`/
`modsym`/safe-`call`, flight-recorder ring, then fix raw_video_rec_task + wire REC button.

### R3 CORRECTION (2026-07-14) — raw PREVIEW is NOT working (over-claimed earlier)
User correction: the "640x360 cropped video in LiveView" was CANON's normal LV crop (from the
movie/res mode), NOT mlv_lite's raw preview. So earlier notes saying "RAW VIDEO RUNS + PREVIEWS" are
WRONG. ACCURATE verified status:
  ✅ mlv_lite LOADS + registers menu/CBRs/config (harness: module_list[2] valid=1 error=0, info/cbr/
     config non-null). SOLID.
  ✅ raw_video_enabled can be SET (set_config_var) without an immediate crash; heartbeat kept ticking.
  ✅ (ML-core level, Phase B) raw_info + EDMAC populate correctly when raw_lv_request() is called
     manually (2096x1164 14-bit, buffer 0x6BC61CC8). This proves the ML-core raw primitives, NOT that
     mlv_lite's own preview/record pipeline works.
  ⚠️ raw PREVIEW does NOT render on screen (mlv_lite's preview path / DISPLAY_FILTER CBR
     raw_rec_update_preview not functioning — no ML overlay compositing over LV either).
  ⛔ recording (raw_start_stop -> raw_video_rec_task) crashes/hangs.
So mlv_lite loads + enables cleanly, but neither PREVIEW nor RECORD actually works yet. The preview
failure + the record crash may share a root (mlv_lite's raw setup path on D8 / display integration).
The console.log observability build (3788486d) is the tool to see where both fail. Do NOT claim raw
video "works" until a real .MLV decodes on the host.

### R3 record-task crash MAP (2026-07-14) — console.log -> code location (use with build 3788486d)
raw_video_rec_task(card_index=0) setup order + its printf breadcrumbs (mlv_lite.c):
  3372 RAW_PREPARING; 3375 cbr PREPARING; 3378 init_vsync_vars; 3398 (h264 proxy, usually OFF);
  3415 powersave_prohibit; 3417 wait_lv_frames(2); 3420 raw_update_params [fail->NotifyBox"Raw detect
  error"]; 3428 update_resolution_params; **3429 setup_buffers()**; 3430 setup_bit_depth; 3434
  get_next_raw_movie_file_name; 3440 FIO_CreateFile [fail->"File create error"]; 3450/3451 mlv chunk
  headers (FIO write) [fail->"Card Full"]; **3457 hack_liveview(0)**; 3463 beep; 3473 msg_queue_post;
  3479 RAW_RECORDING (enables vsync CBR -> per-frame D8 edmac copy).
setup_buffers printf markers: "Setting up buffers (frame size..,fullres size..)"(1572); "Trying
double buffering (shoot..)"(1588); "Trying double buffering (SRM)"(1593 -> SRM stub NULL); "Falling
back to single buffering"(1599); "Could not allocate full-size buffers"(1610, graceful ret 0).
=> CONSOLE.LOG LAST-LINE -> CULPRIT:
  ends ~"Starting raw recording..." only        -> gui_stop_menu()/task_create (very early)
  ends in/after "Setting up buffers"/"..double.." -> setup_buffers: 4MB shoot_malloc w/ SRM off, or
      ASSERT(slots[0].ptr)@975 / ASSERT(fullsize_buffers[0])@976
  ends after buffer msgs, before "File create"   -> **hack_liveview(0)** = TOP SUSPECT: it EngDrvOut's
      DIGIC-4/5-style display regs 0xc0f0xxxx (lv-img-engio.c:779-804, many NOT gated by CONFIG_DIGIC_V)
      which on D8/M50 II are wrong/unmapped -> likely fault. D8 needs different regs OR hack_liveview
      must be a controlled no-op on this body (like piggyback_canon_menu was).
  ends after file creation                       -> vsync CBR raw_rec_vsync_cbr / D8
      edmac_copy_rectangle_cbr_start per-frame copy (first frame).
SEPARATE ISSUE: raw PREVIEW + ML-overlay-over-LV both fail = ML display compositing over Canon LV not
working on M50 II (raw_rec_update_preview / DISPLAY_FILTER). NOT needed to RECORD (mlv_lite records
blind, writes .MLV). Prioritize the record crash; ignore preview for first clip.
NEXT: flash 3788486d + .sym -> enable RAW 640x360 -> trigger raw_start_stop -> pull card -> read
ML/LOGS/console.log last line -> jump to the culprit above. If hack_liveview: gate/adapt it for D8.
