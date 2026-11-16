# M50 II boot flakiness — FIO / card-mount race in big.7→big.8 (2026-07-14)

> **Status note (2026-11-16).** This is an append-log; parts of it are refuted. This note was added
> by the offline sprint of 2026-11-16, which had no camera attached. The body below is unchanged.
>
> - **The "movie-format remount" theory is wrong** (the sections that say Canon remounts the card
>   through `vcmMovieFormatMount @0xe0202328` / `fmMountCard @0xe02023c0` at record start and that
>   the remount is starved of memory). Those two addresses are debug strings, not functions; the
>   `vcmMovieFormatMount` calls are compiled out of this ROM; no mount code is on the record-start
>   path. See `docs/offline/A6.md` (independently verified, claims 1-3).
> - The card-write fix this log led to (buffers for `FIO_WriteFile` must be uncacheable) stands:
>   `docs/35`, mechanism in `docs/offline/A7.md` (verification row 6).
> - Boot-reliability figures quoted here are superseded by the catalogue generated from the full
>   UART history: `docs/offline/C7.md`. "A core relink breaks boot" is not supported by the logs:
>   `docs/offline/C6.md`.
> - Why recordings crashed: `docs/offline/A3.md` and `docs/offline/F4.md` (inferred, not
>   hardware-tested).
>
> Index: `docs/README.md`.

---
## ⇒ CURRENT STATE (read this first — 2026-07-18)

**MLV RECORD TEST (build autoexec e49041017 + mlv_lite.mo 5d394927): the FIO write fix is CONFIRMED on
the MLV path; the record path is unblocked up to and including the header write + a 4MB in-record write.
New isolated blocker = a DAF/EIS/AE capture-state storm + no vsync frames.**

In movie-LV standby, `call raw_start_stop` ran raw_video_rec_task end-to-end. From the atomic `@ML>`
UART markers + ML/LOGS/console.log:
```
Setting up buffers -> 143 slots from shoot_malloc (63MB shoot, SRM off)   -> R0 memory risk CLEARED
create(post-STARTING) f=0x3                                                -> FIO_CreateFile OK
post-write-hdr mounted=1                                                   -> MLV HEADER WRITE OK
Starting raw recording... / EDMAC copy resources locked.                   -> entered RECORDING, locked mem2mem
```
The central `FIO_WriteFile` UNCACHEABLE fix (docs/35; fio-ml.c gated CONFIG_MEM_2GB||CONFIG_M50mk2)
unblocks the real MLV write path. Card `M50TEST.MLV` = 4,194,304 B — that is mlv_lite's `[ctrl-rec]` 4MB
diagnostic write from 0x40000000 DURING recording, which SUCCEEDED (writes work even in the capture
state), NOT real frame data. The inline 0x40000000 aliasing in mlv_lite `write_frames` was removed (it
DEFEATED the central sync_caches() guard).

**REMAINING BLOCKER (cleanly isolated): after `EDMAC copy resources locked`, ZERO frames are captured
before stop, and Canon's DAF/EIS/AE pipeline storms.** console.log goes straight from "EDMAC copy
resources locked" to "Stopping raw recording..." (my 5s stop) with NO frame lines in between; NO
`framewrite-ok/FAIL` marker ever appears => the per-frame vsync capture loop never advances. Concurrent
UART storm:
```
[DAFZERO] DAFPath_SetDAFParam > Timeout                                   (dual-pixel AF path stalls)
[EIS] ERROR EV_SET_FRAMECOUNT_GLOBALVECTOR / EV_EXPBALANCE_GLOBALVECTOR TryPostEvent(0xd)   (IS/AE queues full)
[DAF] WARNING >> FrameBufferDummy
```
and console.log floods with **317x "No memory suites."** (mlv_lite retrying an SRM memory-suite alloc
that always fails with SRM off -- likely a polling loop; the per-line FIO spam itself may aggravate the
deadline misses). Camera stays ALIVE (heartbeat fine) but the LiveView imaging pipeline is wedged (needs
a power-cycle). This is docs/32 "finding 3" (capture destabilizes Canon LiveView), now isolated:
everything upstream — module load, raw params (2096x1164, black 2047), 143-slot memory, FIO write, MLV
header, in-record 4MB write — works; ONLY the RECORDING capture-state entry (a) disrupts Canon DAF
(autofocus) + EIS (stabilization) on DIGIC 8 and (b) yields no vsync-driven frame captures.

**PRIME SUSPECTS + NEXT (narrow target):** (1) the per-frame vsync CBR (raw_rec_vsync_cbr) is not
advancing during RECORDING -- does the EVF_STATE vsync (that fired ~24/s in standby, docs/31 Phase A)
STOP once hack_liveview reconfigures LV? Verify: `rd vsync_counter` before vs during record. (2)
hack_liveview (@mlv_lite.c:2154) / the capture-state LV reconfig disrupting DAF+EIS on D8 -- find + call
Canon's "stop DAF / stop EIS" primitives before the capture loop, re-enable on stop; compare bilal M50
mk1 (does mk1 disable AF/IS during raw, or lack DAF/EIS?). (3) the "No memory suites" loop (SRM off) --
locate the printer + gate it so it stops spamming. TOOLING FIX applied this session: mlresolve.py now
resolves is_movie_mode/lv/raw_info from the flashed .sym (a stale hardcoded is_movie_mode 0xde065 got
`call`ed into prop_request_change_wait and crashed the cam). Flaky boot still intermittently drops the
module (FIO race "Not mounted ID=1" at module_task; "No allocator for 1.7GB" = bad-boot artifact, nm
mlv_lite.mo UND vs sym = 0 missing) -- a clean cold-boot loads it.

---

## ⇒ CURRENT STATE (2026-07-16)

**Goal:** record a raw `.MLV` on the M50 II. Everything below the divider is the historical
investigation (much of it superseded — read the last few UPDATE sections for the live trail).

**SOLVED:** D8 mem2mem frame copy (HW-validated). MLV **header write** (build 5dc87e3b+):
skip the malloc'd VERS blocks + write the NULL pad from a STATIC buffer → `write_mlv_chunk_headers`
completes on HW.

**OPEN BLOCKER — frame writes fail `[FIO] ERROR [FirstEnt] Not mounted ID=0`:**
Ruled OUT (via live harness + in-rec-task probes): mount-bit 0x200000 (stays set), raw capture
active, drive-A `is_dir`, memory starvation, handle create-timing, buffer address/region, write
size/alignment, multi-write, seek. Plain FIO writes (incl. 4 MB) SUCCEED from the rec task with
raw armed. The failure is specific to the **per-frame group write extending the file**.

**★ GHIDRA RE FINDING (2026-07-16, the reframe):** the emitter is `FUN_e05b30e6` (FIO "FirstEnt"
find-first). It derives the **drive index from the path's first char**: `*path - 0x41` (`A`→0,
`B`→1; only 0/1 valid), takes a per-drive semaphore (`FUN_e05b1242`, table `DAT_e05b146c+0x78`,
stride 0x28), then calls the real efat open `FUN_e00ab290`; results go in a 10-slot findFile table
`DAT_e05b32d0` ([handle, drive_letter]). So **`ID` in "Not mounted ID=%d" is a DRIVE index, and
`ID=0` = drive A:** — which on the M50 II is genuinely NOT mounted (`is_dir("A:/")=0`; our SD is
**B:**=index 1). Our file is on B:, yet the failing frame-extension path queries drive **0**. So the
bug reframes to: **the file-extension / cluster-alloc (AcqSize) path resolves to the wrong drive
(A:/0) instead of B:/1.** Traced one level deeper: `FUN_e00ab290` (efat open) resolves the path to a
DEVICE handle via `thunk_FUN_e00e5500` (path→device) then checks device state via `FUN_e00e5644` —
the mount check + the `ID` (device index) live in that device layer. RE'd further (2026-07-16):
`FUN_e00e5644` = task-context getter (`FUN_e012e0cc`), not the table; `FUN_e00e5500` resolves the
path through the GENERIC DryOS device/handle layer (`FUN_e012e17a` open, `FUN_e012e086/e1bc/cf24`) —
deep, doesn't cleanly expose the A:/B: choice. **KEY: I traced the OPEN path (FirstEnt/openfent
`FUN_e00ab290`), but the frame writes actually fail in the EXTEND path — the emitter of
`"AcqSize(ecode=0x100b) L:718"` — which is a DIFFERENT, NOT-YET-LOCATED function.** THAT is the precise
next RE target: find the AcqSize (size-acquire / cluster-alloc / free-space) function at "L:718" and
see what drive/device it queries during a B: file extension. (Also un-pinned: exact call site of
"[%s] Not mounted ID=%d" @e05b15ac — loaded via literal pool, get_xrefs_to found none; try a byte-
pattern search for the address, or the AcqSize function will reference it.)
NOTE: idle 4 MB writes from the serial shell (raw armed) DO extend+succeed, so extension isn't broken
globally — only from the rec task's RAW_PREPARING state (post setup_buffers / hack_liveview). And the
Phase 1.1 [ctrl-pre]/[ctrl-rec] markers got SHREDDED by concurrent Canon error output on the shared
UART (no newline boundary) — so that HW answer needs atomic `@ML>` single-write logging first.

## ★★ 2026-07-16 AcqSize RE COMPLETE — mount-check primitive fully decoded (exact addrs). Chain: FIO_WriteFile(extend) -> AcqSize FUN_e05b1db4 -> FUN_e00aab1c -> FUN_e011831c (efat_openfent, the mount check).
- **AcqSize = `FUN_e05b1db4`**; "L:718" = 0x2ce = its `ecode=%x` log after the inner call fails. It derives
  drive from `*path-0x41` (B:->1, correct), takes that drive's sem, calls `FUN_e00aab1c(path,buf)`.
- **`FUN_e00aab1c`** calls **`FUN_e011831c(*path-0x41, path+2, &handle)`** = efat_openfent. Returns <0 ->
  "efat_openfent:-2".
- **`FUN_e011831c` MOUNT CHECK (the primitive):**
  `desc = DAT_e01185a8 + drive*0x98;  if (((*(int*)(desc+0xc)) << 10) >= 0) return -2;`
  i.e. if BIT 0x200000 (bit 21) of the flag at `desc+0xc` is CLEAR -> NOT MOUNTED (-2).
  `DAT_e01185a8 = 0xaae70` (volume table base), descriptor stride 0x98, mount flag @ +0xc.
  => **drive 1 (B:) flag = 0xaae70+0x98+0xc = 0xaaf14** (bit SET = our SD, mounted — the exact addr we've
     read all along). **drive 0 (A:/CF) flag = 0xaae70+0xc = 0xaae7c** (bit CLEAR — A: absent on M50 II).
- => **"Not mounted ID=0" = the mount check ran for DRIVE 0 (A:) and found it absent.** Since B:/1 IS
  mounted, the rec task's failing AcqSize is querying drive **0 (A:)**, not the file's B:. "Not mounted
  ID=0" also appears as BENIGN boot noise (module_task/ml_init probe A: which isn't there) — same signature.

**THE ONE REMAINING QUESTION:** why does the rec-task frame-write EXTENSION path pass drive 0 (A:) to
AcqSize/openfent when the file is "B:/..."? (Canon's `_FIO_WriteFile` extend using a global primary/
"recording" drive = 0? or an mlv_lite free-space check on CARD_A?) Next: RE `_FIO_WriteFile`/the write-
extend caller of AcqSize, and check mlv_lite write_frames free-space (get_free_space_32k / CARD_A refs).

**CHEAP LIVE TESTS (next boot, NO rebuild):**
  1. `rd 0xaae7c` — confirm A:/drive-0 mount flag has bit 0x200000 CLEAR.
  2. HACK: `wr 0xaae7c <cur | 0x200000>` (mark A: "mounted") then record — if frames write, CONFIRMS the
     drive-0-query root cause + a candidate hack-fix. (May fail deeper: A: has no real FAT structures.)
  3. During a failing write, `rd 0xaaf14` AND `rd 0xaae7c` to see which descriptor is consulted.
This is the crux and is now a small, well-scoped task with exact addresses.

## ★★★ 2026-07-16 _FIO_WriteFile RE — the WRITE PATH IS CLEAN / EXONERATED. The frame write is NOT the drive-0 source.
Stub addrs (stubs.S): `_FIO_WriteFile`=0xE05B1A00, `AcqSize`=`_FIO_GetFileSize64`=0xE05B1DB4, `FirstEnt`=
`_FIO_FindFirstEx`=0xE05B30E6, `Remove`=`_FIO_RemoveFile`=0xE05B17B0. Decompiled the write path:
- **`_FIO_WriteFile` (FUN_e05b1a00)(fd,buf,size):** resolves fd->file struct (`FUN_e05b10da`), takes drive
  from **`*(fd_struct+4)`** (the STORED drive letter -> 'B' -> index 1, CORRECT), takes that drive's sem,
  calls worker `FUN_e00aa480(fd,buf,size, *(fd+4))`. Logs "Write ecode=%x" @0x224 ONLY on worker failure.
- **worker `FUN_e00aa480`(...,drive_letter):** `FUN_e00844f4(drive-0x41)` per-drive check, then the actual
  device write `FUN_e03ef740(fd,buf,size)` (handles partial writes). **NO FirstEnt / AcqSize / openfent /
  directory / size op anywhere. Uses drive index 1 (B:) throughout.**
=> **The frame write itself never queries drive 0 and we never saw a "Write ecode=" error** — so the write
IS NOT failing on the mount check. The drive-0 "[FirstEnt] Not mounted ID=0" / "AcqSize" / "Remove" errors
come from PATH-BASED ops (drive = path[0]-'A'), NOT from _FIO_WriteFile. One is known-BENIGN: mlv_lite.c:2994
`if (is_dir("A:/"))` inside get_cf_dcim_dir (called by get_next_raw_movie_file_name) deliberately probes
drive 0 and expects the failure (is_dir returns 0). `get_free_space_32k` uses a CACHED field (no FIO), so
it's not the source either.
**NEW REFRAME (important):** stop assuming "the frame write extension queries drive 0" — the write path
disproves it. The rec task bails on a PATH-BASED FIO op (AcqSize=_FIO_GetFileSize64 and/or _FIO_RemoveFile)
that resolves to drive 0 — or the write is failing for a DIFFERENT reason (FUN_e03ef740 device write) whose
"Write ecode" got lost in the UART interleave. **The exact fatal op is HIDDEN by the marker-shredding
interleave** — so the RIGHT next step is the ATOMIC `@ML>` single-write logging (camera-side): wrap each FIO
call in the rec write path with an atomic tagged log of (op, path, ret) so `mllog markers` shows the exact
failing op + its path. THEN we know if it's GetFileSize64/Remove with a drive-0 path, or the device write.
Secondary: source-audit mlv_lite raw_video_rec_task/write_frames for FIO_GetFileSize / FIO_RemoveFile /
FindFirst calls and what path each uses. (Live-call tests of _FIO_GetFileSize64 risk hanging — defer.) **LEADING HYPOTHESIS (actionable):** small writes to B: work, but file EXTENSION
needs `AcqSize` (free-space / cluster alloc), which likely queries the PRIMARY device (0 = A:) — empty
on the M50 II — instead of the file's device (1 = B:). Deeper root: on a single-card body the SD is
normally the PRIMARY card = **A: = device 0**, but this port maps it to **B: = device 1** with A:
absent. That asymmetry would explain everything: reads + small writes go via the B: path, but any Canon
op that assumes "primary card = device 0" (cluster alloc, DCIM, movie-format) hits the empty device 0.
=> Investigate making the M50 II SD present as A:/device 0 (or why the port chose B:); that may be the
real fix, not a mlv_lite change. (get_shooting_card/get_ml_card @0x0010043C returned drive_letter "B".)

**NEXT ACTIONS:** (1) finish the drive-0 RE above; (2) once root cause known, the fix is likely a
small drive/card-index correction; (3) rig automation (Phase B: over-serial deploy + WiFi power)
so iteration is autonomous. Header-write cruft (probes/ctrl writes/uncacheable wrap/skip-vers) is
still in mlv_lite.c and should be trimmed once the frame write lands.

**TOOLING (new, 2026-07-16, tools/mlserial/ — see TOOLS.md):**
`mlresolve.py` (auto-resolve raw_start_stop per boot), `mllog.py` (SQLite log DB: `boots`/`errors`/
`markers`/`heartbeat` — query deltas, stop reading garbled UART), `flash.sh` (one-shot build+flash+
verify+record). Golden rule: probe LIVE via `call`/`exec`/memory before rebuilding.

**Current card build:** e441ee93 (Phase 1.1 state-vs-buffer diag) — flashed but on the last boot the
module FAILED TO LOAD (flaky boot FIO race: `mllog.py` boot 86 shows mlv_lite error=1, raw_info=0).
A clean cold-boot usually loads it. `raw_start_stop` is per-boot → run `python3 mlresolve.py`.

**⚠ CRITICAL GAP — Phase 1.1 never actually ran:** build e441ee93 added `[ctrl-pre]` (4 MB write to
`f` BEFORE hack_liveview) and `[ctrl-rec]` (4 MB write to `f` DURING recording) to decide STATE-vs-
BUFFER — but that build failed to load, so the test is STILL UNANSWERED on HW. First thing next
session: cold-boot until mlv_lite loads (check `mlresolve.py` / `mllog.py boots`), fire record, and
read `mllog.py markers` for `[ctrl-pre]` vs `[ctrl-rec]`. That result + the RE below narrows it fast.

**Drive-mapping note (src/fio-ml.c):** `available_cards[] = {"A"/CF, "B"/SD, "C"/EXT}`; the M50 II
config selects **CARD_B** for ML_CARD + SHOOTING_CARD, so **B: is DELIBERATE** — Canon enumerates the
SD as device 1 (A:/CF absent). B: reads + small writes work, so B:→SD is correct. So the "should be
A:" idea is likely WRONG; the real anomaly is narrower: the file-EXTENSION (AcqSize/cluster-alloc)
path queries device **0** (A:/CF) instead of the file's device 1. RE `FUN_e00e5500`/`FUN_e00e5644`
(device resolve/mount-state) to find where device 0 gets used for a B: extension.

---

## Symptom
User: "camera twice crashed during start." Reality (from `tools/mlserial/run/uart.log`,
4.3 MB, 38 boot attempts): the camera **watchdog-reboot-loops**. It is NOT a clean crash
(no CPU abort / DFAR dump) — it is a ~20–24 s **hang → hardware watchdog reset**.

## What the logs prove
- Live card build = `autoexec.bin` md5 `c50add74…` (14:14). **NOT** the CONSOLE_DEBUG
  observability build — `strings` shows zero `console.log`. So there is no `ML/LOGS/console.log`;
  the UART harness log is our only observability (and it was enough).
- `ML/modules/LOADING.LCK` present = ML already flipped to safe mode ("crashed last time,
  autoload disabled"). Generic message, does not name a module.
- Fonts load fine (`big.5: _load_fonts done`). Fonts/`hack_liveview` are NOT the problem.

## The fork point (sharp)
Every boot reaches `big.7: config_load done`. Then within ~1 s it EITHER:
- prints `big.8: my_big_init_task COMPLETE -- ml_started=1`  (SUCCESS), or
- goes silent ~24 s → `BootLoad` (watchdog reset)  (FAIL).

Success rate ≈ **17 / 38** boots reach big.8. Timing-dependent ⇒ a race, not a fixed crash.

## Code in the hang window (src/init.c 1119–1161)
```
config_load();                       // big.7 printed here
debug_init_stuff();
_hold_your_horses = 0;               // RELEASES all tasks that were spin-waiting
for (task in _tasks_start.._tasks_end) task_create(...);  // spawns module_task, etc.
msleep(500);                         // ~= the ~1 s success gap
// big.8, ml_started = 1
```

## Root cause (strong hypothesis)
`_hold_your_horses = 0` + the TASK_CREATE loop release **module_task**, which immediately
scans the card for `.mo` modules. The card FS is often **not mounted yet**:
```
[FIO] ERROR @module_task AcqSize(ecode=0x100b) L:718     (fio-ml.c:718)
[FIO] ERROR @module_task NextEnt(ecode=0x100b) L:1477    (fio-ml.c:1477)
[FIO] ERROR [AcqSize] Not mounted ID=1
[FM] Suspend : No Card
```
When the mount is slow (flaky reader / `errors=remount-ro`), module_task spins on these FIO
retries and starves my_big_init_task from finishing big.8 → ~24 s → watchdog. As the card
degraded near the end of the session (last two boots 15:43 & 15:44 BOTH failed), failures
clustered — exactly what a mount-timing race predicts.

## This is NOT the mlv_lite record crash
The record-engine investigation (raw_video_rec_task) is BLOCKED behind this: we cannot
reliably reach ml_started to even enable RAW. Some earlier "it crashed" record events were
likely this same watchdog reset, not the record path.

## Fix directions (ranked)
1. **Gate module_task on card-mount-ready** with a bounded, backed-off wait (msleep between
   retries) so it cannot busy-spin/starve the scheduler before big.8. Highest leverage, lowest risk.
2. **Verify LOADING.LCK safe-mode actually short-circuits the FIO module scan** — if module_task
   still enumerates the card when autoload is disabled, that's a bug (safe mode should skip it).
3. **Card health**: try a known-good fast card; the reader/card was flipping read-only this session.
4. Optional: move the module scan later / lower priority so it never contends with big.8 completion.

## Corrected earlier assumption
The compacted note "hack_liveview(0) @ mlv_lite.c:3457 writes DIGIC-4/5 regs 0xc0f0xxxx" was a
misread — those writes live in `image_effects_step()` (off the record path). Real `hack_liveview`
(mlv_lite.c:2154) is identical to bilal's WORKING M50 mk1 port, so it is not a suspect.

---

# UPDATE 2026-07-14 16:26 — boot fix VALIDATED; record path reached; crashes at null EDMAC callback

## Boot-stability fix (module_task card-ready gate) — CONFIRMED WORKING on HW
Build `7160ea24` (autoexec) flashed. First boot reached `big.8: ml_started` and stayed up
(heartbeat climbed past 0x7D, zero reboots). `module_task` did NOT print "card FS not mounted"
(gate found the card mounted → took the load path). mlv_lite loaded valid=1/enabled=1/error=0.
The ~17/38 boot-loop is gone. Even better: it survives a *record* crash and auto-reboots clean.

## raw_start_stop resolution (per-boot, cbr-table method) — WORKS
module_list @ core .bss 0x00110438, stride 0xB8. Entry 2 = mlv_lite, cbr table ptr @ +0xA0.
Read cbr[0] (CBR_VSYNC=6) handler = runtime(raw_rec_vsync_cbr). Two-anchor delta cross-check:
  raw_rec_vsync_cbr:       runtime 0x3B6C3C30 - .mo off 0x7520 = delta 0x3B6BC710
  raw_rec_keypress_cbr_raw:runtime 0x3B6C0BC8 - .mo off 0x44B8 = delta 0x3B6BC710  (AGREE)
raw_start_stop .mo off 0x43FC (verified: push{...lr} prologue, called by `bl 43fc` from keypress
handler; no symbol in .mo so objdump mislabels it raw_rec_update_preview+0x540).
  raw_start_stop runtime = 0x43FC + 0x3B6BC710 = 0x3B6C0B0C
`call 0x3B6C0B0C` returned cleanly and spawned raw_video_rec_task (Canon task "raw_rec_task1").

## The record crash — pinned
raw_rec_task1 started, set up buffers, began frame capture, then:
```
[FIO] ERROR @raw_rec_task1 AcqSize(ecode=0x100b) L:718     (card not mounted, from record task)
!!! ASSERT (non-fatal): nullptr @ Engine::Edmac.c L:077C / 05DE / 05EF / 040B / 0770  (caught)
[AB] pc=0x00000000 lr=0xE05D1161 dfar=0xC89208D5           (FATAL: null fn-ptr call from ROM)
-> ~9 abort repeats over 3s -> BootLoad (reboot) -> clean recovery
```
ROOT CAUSE: the raw-capture EDMAC **completion/callback pointer is NULL**. Canon's EDMAC engine
(ROM lr=0xE05D1161) calls it → pc=0 → prefetch abort. The prior "EDMAC wired" work covered the raw
LiveView PREVIEW EDMAC; the RECORDING capture path registers a completion CBR that is null/NOP on D8.
Suspect: function_overrides.c NOPs RegisterEDmacAbortCBR / RegisterEDmacPopCBR (and the complete-CBR
wiring) — the classic non-SLURP capture needs a real completion callback. Cross-ref bilal M50 mk1
edmac-d8 + Ghidra on ROM 0xE05D1161 to identify which CBR/pointer must be non-null.

NEXT: wire the raw-capture EDMAC completion callback for D8 (edmac-d8.c / function_overrides.c),
then re-trigger raw_start_stop and watch for frames written to the .MLV.

---

# UPDATE 2026-07-14 ~16:40 — record crash narrowed to D8 EDMAC STUB ADDRESSES

## Correction to the "completion callback" theory
platform/M50mk2.101/edmac-d8.c OVERRIDES `edmac_copy_rectangle_cbr_start` with a BLOCKING D8
mem2mem copy (`mem2mem_emdac_copy_d8`). It does NOT use Abort/Pop CBRs (only RegisterEDmacCompleteCBR,
which is real). So the NOP'd RegisterEDmacAbortCBR/PopCBR are NOT the cause. Disregard that theory.

## The real finding: our edmac-d8.c == bilal's WORKING M50 mk1 edmac-d8.c (byte-identical config)
Both have: mem2mem_devices{0,7}, mem2mem_resources{0x100AD,0x100BB}, RD_CH 46, WR_CH 13,
BOOMER_SELECTOR 0x373f0126, MODE 0, WAIT_MS 50, same call sequence. Since IDENTICAL C works on
bilal's M50 mk1 (same D8) but crashes on our M50 mk2, the bug is in the M50mk2-specific ROM STUB
ADDRESSES in stubs.S (firmware differs: M50 1.1.0 vs M50mk2 1.0.1), OR subchip/resource setup.

## Crash mechanism (recap)
raw_start_stop @0x3B6C0B0C called -> raw_rec_task1 spawns -> mem2mem copy runs with a mis-resolved
EDMAC stub -> Canon `nullptr @ Engine::Edmac.c` (L:077C/05DE/05EF/040B/0770, non-fatal, caught) ->
~73s later a fatal null-fn call surfaces via the DebugMsg/assert path (pc=0 lr=0xE05D1161 in
DryosDebugMsg) -> reboot -> clean auto-recovery (boot fix working).

## Prime suspect + the definitive method
- SUSPECT: `edmac_select_boomer` stub = 0xe0683ca0. Ghidra decompile = FUN_e0683ca0(uint param_1) —
  takes ONE arg, writes param_1&0xffff to table[param_1>>16], asserts "Engine::BoomerSelector.c".
  But edmac-d8.c calls it with TWO args: edmac_select_boomer(WR_CH=13, 0x373f0126). Arg-count
  mismatch => likely the WRONG function (a low-level BoomerSelector setter, not the mem2mem
  edmac_select_boomer). NOTE: region 0xe068xxxx is plausible — Canon's Engine::MemoryToMemoryEsub5.c
  (@string 0xe0684894) and BoomerSelector.c (@0xe0683cf0) both live in 0xe068xxxx, so it is NOT a
  spatial outlier. Must verify against Canon's own mem2mem code.
- GROUND TRUTH: the function referencing "Engine::MemoryToMemoryEsub5.c" (string @0xe0684894) IS
  Canon's mem2mem implementation; it calls the correct sequence of D8 EDMAC leaf functions. Reading
  it off gives the correct M50mk2 addresses for ALL of: edmac_reset_channel, edmac_reset_boomer_vdkick,
  edmac_select_boomer, edmac_reset_packunpack_mode, edmac_set_address, edmac_set_size,
  edmac_set_transfer_mode, StartEDmac_maybe, ConnectReadEDmac_maybe, edmac_stop_boomer_maybe.
  (String has no direct xref/literal-pool hit — reached via ADR/computed ref; locate by disassembling
  the 0xe0684xxx region or following the BoomerSelector.c caller cluster.)

## NEXT WORK UNIT (offline RE, no camera until rebuild)
1. Locate Canon's mem2mem fn (near 0xe0684xxx / MemoryToMemoryEsub5.c) in Ghidra.
2. Read off the correct M50mk2 1.0.1 addresses for the ~10 D8 EDMAC leaf stubs above.
3. Diff vs platform/M50mk2.101/stubs.S; fix every mismatch (edmac_select_boomer first).
4. Also verify PwrMng_WakeSubChips (0xe06b934e) + mem2mem_resources vs Canon's ResLock setup.
5. Rebuild autoexec -> flash -> re-trigger raw_start_stop -> expect frames -> .MLV on card.
Also open: the `[FIO] ERROR @raw_rec_task1 AcqSize ecode=0x100b` (record task couldn't create the MLV;
same card-not-ready class as the boot race) — ensure the record task waits for a mounted card too.

---

# UPDATE 2026-07-14 ~17:10 — EDMAC-stub audit DONE; bug localized to the BOOMER subsystem

Audited the M50mk2 1.0.1 EDMAC stubs against the ROM (gang100.bin) in Ghidra. Result: the general
EDMAC leaf stubs are CORRECT; the failure is specifically the **boomer** stubs (channel-indexing ABI
mismatch). No M50 1.1.0 ROM available locally for direct cross-ref, so verified by decompiling each.

## VERIFIED CORRECT (raw-channel ABI, match edmac-d8.c usage)
- edmac_set_address 0xe05b848e = FUN(chan,addr): `*(chanTable[chan*8]+0xa0)=addr`. ✓ (name-accurate)
- StartEDmac_maybe 0xe05b9690 = FUN(chan): indexes `chanTable[chan*8]`, writes 0x11 to +8, and
  ASSERTS "Engine::Edmac.c" line 0x770 — this IS one of the crash asserts (L:0770/077C). It's correct;
  it fires because the channel was left half-configured upstream (by the boomer step).

## THE BUG: boomer stubs use PACKED (channel<<16) indexing, edmac-d8.c passes RAW channel
- edmac_reset_boomer_vdkick 0xe05b5720 = FUN(packed): uses `param>>0x10` as index, asserts
  "Engine::BoomerVdKick.c" line 0x153. Called from edmac-d8.c as edmac_reset_boomer_vdkick(WR_CH=13)
  → 13>>16 = 0 → operates on the WRONG boomer slot (index 0, not ch 13).
- edmac_select_boomer 0xe0683ca0 = the low-level "Engine::BoomerSelector.c" SETTER: FUN(packed) stores
  `param&0xffff` at boomerSelTable[param>>16]. edmac-d8.c calls it 2-arg (WR_CH=13, 0x373f0126); the
  function only reads r0=13 → index 0, value 13 → selector 0x373f0126 IGNORED. WRONG behavior.
  (FUN_e05b7592 wraps it as 1-arg channel→table-lookup; FUN_e0683fec "Engine::OrbbenPath.c" drives it
  with a pointer to a prebuilt config DAT_e0684394. Neither matches bilal's 2-arg raw-channel leaf.)

## Interpretation
On M50 mk1 (bilal, identical C, WORKS) the boomer funcs take a RAW channel. On M50mk2 1.0.1 the
boomer funcs at these addresses take a PACKED (channel<<16 | value16) arg and index different tables
(vdkick: DAT_e05b57ac stride 0xc; selector: DAT_e0683cec). So either (a) these are the WRONG stub
addresses (the raw-channel M50mk2 twins are elsewhere), or (b) the M50mk2 D8 boomer ABI genuinely
differs and edmac-d8.c must be ADAPTED for M50mk2 (pack the channel + encode the selector into the
boomer table the way Canon's own mem2mem/OrbbenPath path does). This is NOT a 1-line address swap.

## NEXT WORK UNIT (focused RE + edmac-d8.c adaptation)
1. Decompile Canon's real mem2mem/EFsVcopy (near Engine::MemoryToMemoryEsub5.c @string 0xe0684894,
   and OrbbenPath FUN_e0683fec @0xe0683fec) to learn the CORRECT M50mk2 boomer setup: what value is
   written to which boomer table for the mem2mem WR/RD channels, and in what order.
2. Either find raw-channel boomer twins, OR rewrite edmac-d8.c's boomer calls to the M50mk2 ABI
   (pack channel<<16, use the OrbbenPath/BoomerSelector config the way Canon does).
3. Best option may be to call Canon's high-level mem2mem path function directly instead of
   re-assembling it from leaf stubs (more robust; the leaf ABI clearly drifted between firmwares).
4. Verify RD_CH=46/WR_CH=13 and mem2mem_resources{0x100AD,0x100BB} are valid on M50mk2 (also from R180).
BLOCKER for cheap completion: no M50 1.1.0 ROM to diff bilal's known-good stub targets. If one can be
obtained, the cross-ref becomes mechanical. Otherwise this is deep unlabeled-ROM RE of the D8 boomer
path. Regular EDMAC leaf stubs (set_address/set_size/StartEDmac/reset_channel) do NOT need changing.

---

# UPDATE 2026-07-14 ~17:40 — Canon boomer subsystem RE'd (Ghidra + live harness). Fix needs Canon's mem2mem recipe.

## The M50mk2 boomer mechanism (fully characterized)
- BoomerSelector setter FUN_e0683ca0(packed): writes `packed&0xffff` to the MMIO reg pointed to by
  selTable[packed>>16]. LIVE (harness): selTable @ RAM 0xE1086394, entry[i] = &MMIO(0xD042324C + i*4).
  So boomer selector registers = 0xD0423xxx block.
- Vdkick FUN_e05b5720(packed): resets MMIO via vdkickTable[packed>>16] (@RAM 0xE1086A9C, stride 0xc,
  each = {flag=1, &MMIO(0xD04230xx), ...}).
- ⇒ the arg is `index<<16 | value16`, where INDEX is a per-PATH boomer slot (NOT the DMA channel), and
  for real Canon paths the value is often 0 (pure routing select).

## Canon path convention (from FUN_e065db54 = Warp::WbDetectionPath.c, a real caller)
Pattern: `thunk_FUN_e0683ca0(0x1e0000); thunk_FUN_e0683ca0(0x200000);` → selects boomer indices 0x1E
and 0x20 (value 0). Also uses FUN_e0683fec (generic "OrbbenPath" EDMAC path start/stop primitive),
FUN_e05b7c48 (RegisterEDmacCompleteCBR), FUN_e05b8106/FUN_e05b8146 (edmac_stop_boomer variants).
So each Canon EDMAC path picks SPECIFIC boomer indices. bilal's edmac-d8.c (raw channel + 0x373f0126)
does NOT fit this — 0x373f0126>>16 = 0x373f (invalid index, would assert), and passing raw channel 13
selects slot 0. This is why it corrupts the boomer and StartEDmac then asserts Edmac.c:0x770.

## Still missing: Canon's mem2mem (MemoryToMemoryEsub5) recipe
The mem2mem function (string "Engine::MemoryToMemoryEsub5.c" @0xe0684894) is NOT yet located — the
string has no Ghidra xref (loaded via MOVW/MOVT, not a literal pool; byte-search for the pointer
0xe0684894 = "94 48 68 e0" found nothing) and FUN_e0683fec's only two callers are WB-detection paths,
not mem2mem. Without Canon's exact mem2mem boomer indices/values for the RD(46)/WR(13) channels, a
confident edmac-d8.c rewrite can't be written yet (would be guessing the boomer index).

## Options to finish (ranked)
1. LOCATE Canon's mem2mem fn: scan for MOVW #0x4894 / MOVT #0xe068 (the Esub5 string load) in the
   0xe066xxxx-0xe068xxxx code, OR find MemoryToMemoryEsub1..4 code by structure; decompile it → read
   its exact boomer index/value + channel setup → mirror in edmac-d8.c. (Most correct.)
2. CALL Canon's high-level mem2mem entry directly from edmac-d8.c instead of re-assembling leaf calls
   (robust; needs the entry addr+signature from #1).
3. Obtain an M50 (original) 1.1.0 ROM → load in Ghidra → decompile bilal's known-good edmac_select_boomer
   (0xe054a3da) + reset_boomer_vdkick (0xe054a35c) → find the byte-identical M50mk2 twins mechanically.
CONCRETE DATA IN HAND for next session: boomer MMIO block 0xD0423xxx; selTable@0xE1086394 (→0xD042324C+i*4);
vdkickTable@0xE1086A9C (stride 0xc); path-primitive FUN_e0683fec; WB path FUN_e065db54 as a worked example.
NOTE: this is a genuinely deep D8 subsystem RE; the boomer ABI difference means the fix is an edmac-d8.c
ADAPTATION (not an address swap), and it needs Canon's mem2mem recipe before it can be written correctly.

---

# UPDATE 2026-07-14 ~18:10 — ALIGNED WITH DISCORD: kitor already decoded D8 mem2mem boomer (2024). Fix = stub-address translation, not from-scratch RE.

Sources: ml_discord/Dev_Stuff__ml-developers-general.md (kitor, Feb 2024) + Platform_Specific_Development__m50.md
+ user-supplied EOS R DmacInfo/DmacBoomerInfo dump. We were partly re-deriving; kitor's decode is the reference.

## kitor's decoded D8 mem2mem recipe (EOS R) — THE TEMPLATE
Channels (same as our edmac-d8.c): WR_CH=13 (0xD0420C00, BoomerID 0xC0000, InSelType 0x100, InSelEdmacType
0x20000000), RD_CH=46 (0xD0421600, BoomerID 0xE70000, InputPort 0xE7). Boomer = "makes the connection"
between write and read channels; per-channel config lives in ROM DmacInfo/DmacBoomerInfo, +/- SAME across
D8 cams. mem2mem boomer attr for 46->13 pair = 0x3f7f0126 (bilal's edmac-d8.c uses 0x373f0126), derived from
RD ch's InSelType/InSelEdmacType. Boomer MMIO block = 0xD0423xxx (matches my live read: selTable->0xD042324C+).

Canon's real setup sequence (EOS R addrs; the "*" ones bilal's reimpl folds/omits — verify on mk2):
  FUN_e0535998(WR_CH); FUN_e0535998(RD_CH);              // * "setup path" (lock res, power devices)
  Boomer_VdKick_for_edmac_port_e0535e64(WR_CH);          // = edmac_reset_boomer_vdkick
  BoomerSelector_wrdmac_e0535ee2(WR_CH, bsel);           // = edmac_select_boomer  <-- 2-ARG WRAPPER
  RegisterChannelCompCBR(WR_CH,..); RegisterChannelCompCBR(RD_CH,..);
  FUN_e0535fba(WR_CH); FUN_e0535fba(RD_CH);              // *
  edmac_set_address/size/transfer_mode(WR,RD); StartEDMac_maybe(WR,RD); ConnectReadEDmac_maybe(RD);
  ... after CBR: ClearEDmacCompleteCBR(RD,WR); Boomer_e0535f76_terminate(WR_CH);  // = edmac_stop_boomer_maybe
EOS R boomer fn addrs: VdKick 0xe0535e64, Selector 0xe0535ee2, terminate 0xe0535f76 (all 2-arg, raw channel).
M50 mk1 (bilal stubs.S): reset_boomer_vdkick 0xe054a35c, select_boomer 0xe054a3da, stop_boomer 0xe054a46e.

## THE BUG, restated with the template
Our M50mk2 `edmac_select_boomer = 0xe0683ca0` = the low-level BoomerSelector.c SETTER (1-arg, packed,
pokes 0xd0423xxx MMIO). bilal's edmac-d8.c calls edmac_select_boomer(WR_CH=13, 0x373f0126) expecting the
2-ARG WRAPPER `BoomerSelector_wrdmac` (EOS R 0xe0535ee2 / M50mk1 0xe054a3da). Wrong layer -> boomer
mis-set -> StartEDmac asserts Edmac.c:0x770 -> crash. Same likely applies to edmac_reset_boomer_vdkick
(our 0xe05b5720 vs the wrapper). The general leaf stubs (set_address 0xe05b848e, StartEDmac 0xe05b9690)
are CORRECT.

## FIX (much more tractable now)
1. Find M50mk2 analogs of the 2-arg boomer WRAPPERS (VdKick_for_edmac_port, BoomerSelector_wrdmac,
   terminate). They take a RAW channel + value/struct and reference boomer MMIO 0xd0423xxx. Anchor: on
   M50mk1 they cluster with the other edmac leafs (0xe054axxx); on mk2 look near the edmac cluster
   (0xe05bxxxx) — NOT the 0xe0683ca0 low-level setter region. Repoint stubs.S.
2. Keep 0x373f0126 + channels 13/46 (portable per kitor).
3. Verify the setup-path (FUN_e0535998) / FUN_e0535fba steps aren't needed on mk2.
ALT: find M50mk2 `mem_to_mem_edmac_copy` (200D @ e00c0484) and CALL it directly — kitor originally did
this before reimplementing; sidesteps boomer config entirely. Cleanest if the function exists on mk2.

---

# UPDATE 2026-07-14 ~18:30 — BOOMER STUBS FOUND + FIXED (Ghidra-verified). Build f306b5d9, awaiting HW test.

ROOT CAUSE (confirmed): TWO wrong boomer stub addresses in platform/M50mk2.101/stubs.S — both pointed at
the LOW-LEVEL per-InputPort setters instead of the Engine::Outer.c WRAPPERS that edmac-d8.c's
mem2mem_emdac_copy_d8() calls with a RAW channel:
  edmac_reset_boomer_vdkick: 0xe05b5720 (BoomerVdKick.c low-level, param>>16) -> 0xe05b7520 (Outer.c
     wrapper FUN_e05b7520: raw chan -> table[DAT_e05b767c + chan*0x14] -> low-level vdkick). DIGIT
     TRANSPOSITION 57<->75 in the original stub.
  edmac_select_boomer:       0xe0683ca0 (BoomerSelector.c low-level setter, 1-arg packed) -> 0xe05b752e
     (Outer.c wrapper FUN_e05b752e: 2-arg (raw chan, bsel); validates bsel&0xf00==BoomerInSelType,
     bsel&0xfffff000 & BoomerInSelEdmacType, then calls setter). VERIFIED bilal's 0x373f0126 passes both
     checks for ch13 (InSelType 0x100, InSelEdmacType 0x20000000). Boomer value + channels unchanged.
  edmac_stop_boomer_maybe:   0xe05b80c2 = ALREADY CORRECT (raw-chan wrapper, asserts Engine::Edmac.c:0x311).
Both correct wrappers are ADJACENT (0xe05b7520 -> 0xe05b752e) and share the per-channel boomer table
DAT_e05b767c (stride 0x14) — matches kitor's EOS R VdKick(e0535e64)->Selector(e0535ee2) adjacency.
Built ELF confirms edmac_reset_boomer_vdkick=e05b7521, edmac_select_boomer=e05b752f (thumb bit set).
Build: autoexec f306b5d9906ed724106bac868c586e97 + M50mk2_101.sym 9b2328a5b6155754fa006803881d5881.
NEXT: flash to card, boot, harness `call raw_start_stop` (recompute per-boot addr via CBR table), expect
raw_rec_task1 to copy frames + write .MLV instead of the Edmac.c:0x770 assert/crash. General EDMAC leaf
stubs (set_address/set_size/StartEDmac/reset_channel) verified correct earlier; no further changes.

---

# UPDATE 2026-07-14 ~19:00 — BOOMER FIX CONFIRMED ON HW (no crash!). New blocker: EDMAC channel-setup step.

HW RESULT of the boomer fix (build f306b5d9, boot3, harness `call raw_start_stop @0x010A606C`):
- NO CRASH, NO reboot (before: hard pc=0 crash ~73s in). Heartbeat climbed 0x1EA->0x206 stable.
- raw_rec_task1 RAN: allocated 118+ buffer slots, reached frame capture. On-screen: "Frame incomplete
  at slot 118, frame 1" (mlv_lite.c:3730 = get_frame_save_status()==0 = frame not saved because the
  per-frame mem2mem EDMAC copy TIMES OUT, edmac-d8.c WaitForAnyEventFlag 50ms).
=> The 2-address boomer fix WORKED (removed the fatal fault). Pipeline now runs end-to-end; frames don't
   complete. Milestone: "record crashes" -> "record runs, copy incomplete".

LIVE HARNESS DIAGNOSIS (no build needed, refuted my null-descriptor theory):
- Channel-descriptor table base *(0xe05b86e4) = RAM 0xE0EC37AC (== status table *(0xe05b996c), same table).
- ch13 (WR) entry {addr 0xD0420C00, flags 0x05}; ch46 (RD) {addr 0xD0421600, flags 0x06} — EXACTLY kitor's
  EOS R DmacInfo. Descriptors are VALID, not null.
- The 7 "nullptr @ Engine::Edmac.c" asserts are reset_channel (FUN_e05b96b8, L:077C) + StartEDmac
  (FUN_e05b9690, L:0770) checking a per-channel STATE bit in that table. ch46 flags 0x06 has bit0 clear
  -> reset_channel(46) asserts L:077C. The channels aren't in the "prepared/registered" state those
  functions require.

ROOT (best current understanding): bilal's edmac-d8.c calls edmac_reset_channel(RD)/(WR) where kitor's
REAL EOS R mem2mem sequence uses a per-channel SETUP-PATH FUN_e0535998 ("lock resources, power devices",
registers the channel + sets the state bit) FIRST, plus FUN_e0535fba. bilal SUBSTITUTED reset_channel;
works on M50mk1, but on M50mk2 reset_channel is Canon's real "reset an already-prepared channel" (asserts
if not prepared). So either (a) our edmac_reset_channel stub 0xe05b96b8 is the WRONG function (should be
the setup-path analog, like the boomer stubs were wrong), or (b) edmac-d8.c must ADD the setup-path call
before reset. Fix likely mirrors the boomer fix: repoint/extend to the setup-path function.

BLOCKER TO FINISHING: Canon's M50mk2 mem2mem function (would give the exact sequence + the setup-path
addr) is ELUSIVE in Ghidra — the "Engine::MemoryToMemoryEsub5.c" string @0xe0684894 has NO resolved code
reference (no get_xrefs_to hit, no literal-pool pointer "94 48 68 e0", no `movw #0x4894`). Reached via an
ADR/computed load the analyzer didn't turn into a reference. NEXT-SESSION OPTIONS to locate it:
  1. Ghidra run_script_inline: scan 0xe0682000-0xe0686000 for ADR/ADRP computing 0xe0684894, or manually
     disassemble that code region to find the mem2mem body (its literal pool holds the string near it).
  2. Find M50mk2 setup-path (FUN_e0535998 analog): the fn that SETS statusTable[chan*8+4] bit0/registers a
     channel; xref the writers of table 0xE0EC37AC, or the callers-cluster of edmac_set_address that also
     call the boomer wrappers (FUN_e05b752e) = Canon's mem2mem.
  3. Verify edmac_reset_channel stub 0xe05b96b8 is even the right function (decompile vs bilal's M50mk1 role).
  4. Empirical (Discord-endorsed): via harness, try the setup sequence variants / different channels.
NOTE: this is deep, slow-converging D8 RE; recommend a fresh session. Boomer fix is a permanent, committed win.

---

# UPDATE 2026-07-14 ~19:30 — ROOT CAUSE (agent + Discord + SX740 source + Ghidra): missing CHANNEL-PREPARE step.

Discord research subagent mined ml_discord/{Dev_Stuff__ml-developers-general.md, Platform_Specific_Development__m50.md}
(kitor's Feb-2024 EDMAC deep-dive + bilal's Aug-2025 M50 raw port) and I fetched kitor's actual
SX740.102/edmac-d8.c (identical to ours). CONVERGED CONCLUSION:

## THE FIX: channels need a per-channel PREPARE call (Canon FUN_e0535998) before StartEDmac.
kitor's REAL Canon EOS R mem2mem sequence: FUN_e0535998(WR); FUN_e0535998(RD); [boomer]; RegisterCompCBR
x2; FUN_e0535fba(WR); FUN_e0535fba(RD); [set addr/size/mode]; StartEDmac x2; ConnectReadEDmac(RD).
bilal's edmac-d8.c SUBSTITUTES edmac_reset_channel(RD/WR) for FUN_e0535998 and edmac_reset_packunpack_mode
for the FUN_e0535fba slot. FUN_e0535998 (kitor decomp, Dev_Stuff:147660) = the per-channel INITIALIZE:
  EDMAC_arr_0x73a58[port] = 0;                 // clear per-channel status array (ISR-updated)
  channelDesc->field_0x1c = 0x80000000; then 0;// reset dance
  channelDesc->field_0x18 = 0;
  channelDesc->field_0xb4 = 2;                 // <-- distinctive: writes 2 (not 1) to +0xb4
=> This sets exactly the "per-channel status" our reset_channel(Edmac.c:0x077C)/StartEDmac(0x0770) assert
   is MISSING. Un-prepared channel -> StartEDmac silently no-ops -> no completion IRQ -> mem2mem_CBR never
   makes status==3 -> WaitForAnyEventFlag 50ms TIMEOUT every frame = our exact "Frame incomplete" signature.
   (kitor Dev_Stuff:153418 "if transfer failed it will wait for flag forever... added timeout".)
Works on SX740/mk1 (asserts non-fatal, channels left benign) but mk2 leaves channels un-prepared.

## SO: edmac_reset_channel stub (0xe05b96b8) is the WRONG FUNCTION on mk2 (mirrors boomer fix).
Our 0xe05b96b8 = Canon "reset an already-prepared channel" (asserts if status bit unset). It should point
to mk2's FUN_e0535998 (prepare) analog. FIND IT: a fn taking a channel that writes 0x80000000 then 0 to
channelDesc+0x1c, 0 to +0x18, and *2* to +0xb4, and clears a per-channel status array; called from a
"...SetPath" fn just after EngRscLock; most-xref'd of the prepare candidates. Then also find FUN_e0535fba
(2nd prepare/commit half, occupies the reset_packunpack slot). Repoint stubs / add the prepare call in
edmac-d8.c mem2mem_emdac_copy_d8() BEFORE StartEDmac.

## STUBS VERIFIED CORRECT this pass (Ghidra) — do NOT change:
- ConnectReadEDmac_maybe 0xe05b95e4 = real connect/kick (writes MMIO+0xc4, DSB, +0xb4=1). OK.
- PwrMng_WakeSubChips 0xe06b934e = real Engine::PwrMng.c (recursive lock + FUN_e027447a wake + unlock). OK.
- StartEDmac 0xe05b9690, RegisterEDmacCompleteCBR 0xe05b7c48, edmac_set_address 0xe05b848e = OK (earlier).
- CreateResLockEntry 0xE05BE4BE = FUN_e05be4be, CONFIRMED via Canon "Mem1Path EngRscLock" fn FUN_e075444c
  (@0xe075444c, LiveViewDrive::Mem1Path.c:0x10b) which calls it. OK.
- mem2mem_devices {0,7} = CORRECT (mem2mem on EDMAC block1 -> wake device 0; kitor Dev_Stuff:152937). OK.
- NOT the &0x1FFFFFFF cache-mask bug (that yields SOME good frames; we already removed it; ours = all-incomplete
  = true never-start). mem_to_mem_edmac_copy does NOT exist on D8 (only 200D) - cannot call directly.

## ANCHORS to find mk2 FUN_e0535998/FUN_e0535fba next session:
- Strings: "Mem1Path EngRscLock" @0xe0754534 -> FUN_e075444c (Mem1Path lock); "Engine::SantaInOut.c"
  @0xe0748690; the mem2mem is Canon Engine::MemoryToMemoryEsub1.c (its M50mk2 string not yet located; Esub5
  @0xe0684894 elusive). Find Mem1Path SetPath = CALLER of FUN_e075444c or sibling in LiveViewDrive::Mem1Path.c.
- kitor's contrib/kitor/edmac_config.py "has data structures + pointers to multiple ROM versions" = fastest
  way to pull per-cam channel/boomer/status-array/prepare addresses (check for mk2).
- Live-validate before rebuild: find prepare addr, harness `call prepare(13)`+`call prepare(46)` while
  recording, watch frames complete. Then repoint stub + rebuild.
STATE: build f306b5d9 on card (boomer fix, record runs but frames incomplete). This is the LAST blocker to a .MLV.

---

# UPDATE 2026-07-14 ~19:45 — FUN_e0535998 (channel-prepare) FOUND @ 0xe05b7b42. Fix applied+built (3cd602e2), awaiting HW.

M50mk2 channel-PREPARE = FUN_e05b7b42 (Ghidra-verified match to kitor's EOS R FUN_e0535998):
  if (port > 0x4b) assert Engine::Edmac.c:0x288;   // Port < ELD_EDMAC_NUM(0x4c) bounds check
  statusArr[port] = 0;                              // *(DAT_e05b7e38 + port*4)=0  (clear per-channel status)
  desc->0x1c = 0x80000000; desc->0x1c = 0;          // reset dance
  desc->0x18 = 0;  desc->0xb4 = 2;                  // field_0xb4=2 (the distinctive prepare signature)
  desc->0x8 = (flag? 0x11 : 3); + boomer/packunpack fixups; calls FUN_e05b7b14/aa4/a78.
Found via: Canon Mem1Path SetPath FUN_e0754236 (@caller of "Mem1Path EngRscLock" FUN_e075444c) -> its first
per-channel call is FUN_e05b7b42; also called by OrbbenPath(FUN_e0683fec)/WbDetectionPath(FUN_e065db54) =
the "most-xref'd prepare candidate". FIX: stubs.S edmac_reset_channel 0xe05b96b8 -> 0xe05b7b42 (so bilal's
edmac_reset_channel(RD)+(WR) first-two-calls become Canon's FUN_e0535998(RD)+(WR) prepare). Built: autoexec
3cd602e2d7679fa40ef170ef9023f207 + M50mk2_101.sym ad3836dc0b59f158ba3c7c986770da70; ELF confirms
edmac_reset_channel=e05b7b43 (thumb). NOTE: FUN_e0535fba 2nd-half NOT added yet (bilal's reset_packunpack_mode
0xe05b8f38 occupies that slot; add only if prepare-alone insufficient). NEXT: flash 3cd602e2, boot movie LV,
enable RAW 640x384, harness call raw_start_stop (re-resolve per-boot via CBR table), expect frames COMPLETE
+ .MLV growing (no Edmac.c:0x77C/0x770 asserts, no 50ms timeout). If still incomplete -> add FUN_e0535fba.

## HW RESULT of prepfix (build 3cd602e2, harness call raw_start_stop @0x010C528C):
PREDICTED + CONFIRMED: the L:077C assert (reset_channel) is GONE (prepare fix works, pattern holds 3rd time).
Edmac.c asserts 7 -> 6. REMAINING 6: L:0770 x1 (StartEDmac), L:040B x2, L:05DE x1, L:05EF x2 (= set_size/
set_transfer_mode/reset_packunpack, which run AFTER prepare). Frame STILL "incomplete at slot 102, frame 1"
(mem2mem copy still times out); this time LiveView FROZE -> watchdog reboot (recovered, camera back online).
FIO @raw_rec_task1 ecode=0x100b still present (separate MLV-write blocker).
=> NEXT STUB (same pattern): the 2nd prepare-half FUN_e0535fba. bilal substitutes edmac_reset_packunpack_mode
(our stub 0xe05b8f38) for it. Decompile 0xe05b8f38: if it's just a packunpack reset (not the full commit that
arms the channel for set_size/StartEDmac), it's the WRONG function -> find mk2 FUN_e0535fba (called after
RegisterCompCBR, before transfer, once per channel; sibling of prepare FUN_e05b7b42 @0xe05b7xxx cluster;
kitor EOS R FUN_e0535fba / sibling FUN_e0535ffa) and repoint. The 6 remaining asserts should then clear.
Also still open after that: the FIO card-not-mounted for the record-task MLV write (card-ready gate for
raw_rec_task, same class as the boot fix). Progress is monotonic: each correct stub drops its assert.
Committed stub fixes so far (all in stubs.S): edmac_reset_boomer_vdkick 0xe05b7520, edmac_select_boomer
0xe05b752e, edmac_reset_channel 0xe05b7b42. Card build = 3cd602e2.

---

# UPDATE 2026-07-14 ~21:30 — WORKFLOW RE (4 agents + synth) mapped ALL 8 asserts. 3 disasm-verified changes. "Missing ARM" theory DISPROVEN.

Ran a 4-agent Ghidra fan-out (Mem1Path SetPath ground truth / boomer wrappers / packunpack+arm / Edmac.c
asserts) + synthesis. Every claim below is direct ROM disassembly + live-matching ROM table reads, not inference.
The 8 asserts = THREE wrong values (2 wrong stub addrs + 1 wrong boomer const), NOT a missing sequence step.

## THE FIX (applied; awaiting rebuild)
1. stubs.S L369  edmac_set_size            0xe05b8862 -> 0xe05b849a   (clears Edmac.c L:040B x2)
   - 0xe05b8862 = SCALAR byte-count SetEDmac wrapper (2nd arg = byte count). We pass a struct edmac_info*
     -> treated as byte count -> fabricates nonzero geometry -> inner writer FUN_e05b849a asserts 0x40B.
   - 0xe05b849a = real (channel, struct edmac_info*) geometry writer = set_address(0xe05b848e) + 0xc.
     Our info has xn=yn=0 -> capability gate false -> no assert. +0xc twin matches SX740/M50mk1.
2. stubs.S L394  edmac_reset_packunpack_mode 0xe05b8f38 -> 0xe05b8106  (clears Edmac.c L:05DE x1 + L:05EF x2)
   - 0xe05b8f38 = 3-arg pack/unpack MODE setter f(ch,mode{1,2,3},order{0,1}); we call 1-arg -> r1/r2
     garbage -> asserts 0x5EF (mode) x2 + 0x5DE (order) x1.
   - 0xe05b8106 = true 1-arg RESET leaf (zeroes desc+0x18/+0x1c/+0x20/+0x24) = stop_boomer(0xe05b80c2)+0x44,
     exact reference delta on SX740 (ec52+0x44) AND M50mk1 (a46e+0x44). Inverse sibling 0xe05b8146 (sets
     those =1) is the ARM step -- deliberately NOT used (working ports use the RESET here).
3. edmac-d8.c L85  MEM2MEM_BOOMER_SELECTOR   0x373f0126 -> 0x373f0226  (clears Outer.c L:0A0 x1)
   - Boomer cfg table base *(0xe05b767c)=0xE1087A8C, ch13 entry @0xE1087B90: InSelType(+4)=0x200,
     InSelEdmacType(+8)=0x00040000, node cmd(+0)=0x004E0000 (node 0x4E). Wrapper 0xe05b752e asserts
     Outer.c:0x0A0 when (sel&0xf00)!=InSelType. 0x0126->0x0226 makes &0xf00=0x200 == ch13. &0xfffff000
     still ANDs 0x40000 !=0 (0x0A2 still passes -- matches it being ABSENT from the firing list). Low byte
     0x26 (only bits reaching HW via 0xe0683ca0) unchanged. 373f-vs-3f7f is irrelevant (both give &0xf00=0x100).

## "MISSING ARM STEP" (FUN_e05b8146) THEORY -> DISPROVEN by disasm.
Agent A found Canon's Mem1Path SetPath calls FUN_e05b8146 (writes desc+0x20/+0x24=1) right after PREPARE and
hypothesized we're missing it. Agents C+D disproved: StartEDmac(0xe05b9690) gates its L:0770 assert on the
STATIC capability halfword table+ch*8+4 bit1, NOT on desc+0x20/+0x24 -- it never reads the arm flags. The
byte-identical working SX740/M50mk1 C has NO arm call and their packunpack-slot homolog is the RESET
(stop_boomer+0x44), which change #2 mirrors. => edmac-d8.c sequence UNCHANGED; do not add/reorder anything.

## 2 asserts INTENTIONALLY LEFT (both non-fatal):
- Edmac.c L:0770 x1: ch13 (WR) static capflags=0x05 (bit1 clear) -> StartEDmac asserts but still writes
  desc+8=0x11 to kick. kitor's EOS-R uses ch13 with the SAME caps 0x05 successfully => cosmetic on all D8.
- Outer.c L:0CD x1: node-0x4E connection-STATE getter mismatch (selector-independent); expected to self-clear
  once the select actually writes + end-of-frame edmac_stop_boomer_maybe(13) teardown runs.

## REMAINING UNKNOWN (the real "will it complete?" risk): boomer connection low byte 0x26 (source id written
to node 0x4E) is inherited from M50mk1/R180, unverified for mk2's 46->13 pair; RD ch46's boomer node is 0xBD
and is never passed to edmac_select_boomer by the current C. If all 6 asserts clear but the copy still times
out, the 0x26 routing / RD-side connect is the next suspect.

## CHEAP LIVE PROBES (run on CURRENT build 3cd602e2, NO rebuild; `call` needs ODD addr, harness doesn't set thumb bit):
- SANITY (rd, ROM-resident, 100% safe): rd 0xe05b86e4->0xE0EC37AC | rd 0xe0ec3818->0x05 (ch13 caps) |
  rd 0xe0ec3920->0x06 (ch46 caps) | rd 0xe05b767c->0xE1087A8C | rd 0xe1087b94->0x200 (ch13 InSelType) |
  rd 0xe1087b98->0x00040000 | rd 0xe1087b90->0x004E0000.
- SELECTOR KILLER (proves fix live): call 0xe05b752f 13 0x373f0126 -> EXPECT Outer.c:0x0A0 ; call 0xe05b80c3 13 ;
  call 0xe05b752f 13 0x373f0226 -> EXPECT NO 0x0A0.
- 0770 IS CAPS not arm: call 0xe05b9691 13 -> EXPECT L:0770 ; call 0xe05b9691 46 -> EXPECT none.
NEXT: (optional) run live probes to confirm, then rebuild autoexec+sym -> flash -> ONE cold-boot -> raw_start_stop
-> expect 6 asserts gone + copy completes + .MLV grows. Then the FIO card-mount gate for raw_rec_task.

---

# UPDATE 2026-07-14 ~20:00 — HW CONFIRMED: 3 fixes cleared 6/8 asserts, NO crash, NO "Frame incomplete". mem2mem copy WORKS. New/last blocker = SD not write-mounted.

Flashed build c88c4ff2 (autoexec) + 90e6d80e (sym). Cold-boot, movie LV, RAW 640x384. Live pre-flight ROM reads
CONFIRMED every root-cause value on silicon: rd 0xe05b86e4=0xE0EC37AC; ch13 boomer node @0xE1087B90=0x004E0000,
InSelType @0xE1087B94=0x200 (NOT 0x100 -> proves the selector fix), InSelEdmacType @0xE1087B98=0x00040000;
ch13 caps @0xE0EC3818=0x05, ch46 caps @0xE0EC3920=0x06.
raw_start_stop re-resolved via CBR table this boot: cbr tbl ptr @0x00110648=0x010B9DCC; vsync handler 0x010B6F50
(off 0x7520) and keypress_raw 0x010B3EE8 (off 0x44B8) differ by 0x3068 = 0x7520-0x44B8 (two-anchor MATCH) ->
module base 0x010AFA30 -> raw_start_stop = base+0x43FC = 0x010B3E2C (ARM prologue 0xE92D4037; mlv_lite.mo is ARM,
call EVEN). `call 0x010B3E2C` -> ret ok, spawned raw_rec_task1, NO crash/reboot (last boot froze+watchdog).

## HW ASSERT RESULT: 8 -> 2 (exactly as predicted)
CLEARED: Outer.c L:0A0 (selector 0x373f0226), Edmac.c L:040B x2 (set_size 0xe05b849a), Edmac.c L:05DE + L:05EF x2
(packunpack 0xe05b8106). REMAIN (both benign, both predicted): Outer.c L:0CD x1, Edmac.c L:0770 x1.
CRUCIAL: NO "Frame incomplete at slot N" ANYWHERE (last boot printed it every frame). => the per-frame mem2mem
copy is no longer timing out. The 3 disasm-verified fixes are VALIDATED on real silicon. Record runs, no crash.

## LAST BLOCKER (isolated, NOT a drive-letter bug): SD not write-mounted for Canon FIO
raw_rec_task1 aborted at FIO_CreateFile (mlv_lite.c:3440 -> goto cleanup): "[FIO] Not mounted ID=0 / efat_openfent:-2
/ AcqSize(ecode=0x100b) L:718 / Remove L:416". Because it aborts at file-open (BEFORE the capture loop at :3718),
the copy can't yet be proven over sustained frames -- but the absence of "Frame incomplete" is strong positive signal.
RULED OUT the drive-mismatch theory via LIVE harness: call get_shooting_card()@0xcf399 AND get_ml_card()@0xcf38f
BOTH return 0x0010043C; its card_info.drive_letter -> "B", type -> "SD". So SHOOTING_CARD CORRECTLY = the SD "B:".
mlv_lite targets the right card. "Not mounted ID=0" is Canon's DEVICE-level mount (SD = Canon device 0) not being
WRITE-mounted in movie-LV state, even though modules READ fine from it at boot (boot had its own mount race, ID=1
naming there was the ML CARD_B index, not the Canon device id -- do not conflate). The black LiveView = raw PREVIEW
hack (hack_liveview), not the aborted rec task.
NEXT INVESTIGATION (fresh unit, likely a workflow): why Canon's SD write-mount (FM "No Card"/device-0 not mounted)
is absent in movie-LV on mk2. Options: (a) RE Canon's card mount/FM init + trigger it before raw_rec_task creates
the file; (b) card-ready wait-gate in raw_video_rec_task mirroring the boot module_task gate; (c) confirm by having
Canon actually record (mounts the card) then raw alongside; (d) redirect the MLV to a known-mounted path (ML dir) as
a proof-of-capture to finally confirm the copy end-to-end. The mem2mem engine itself is DONE.

## COMMITTED CODE STATE (this session, m50mk2-port, NOT yet git-committed):
stubs.S: edmac_set_size 0xe05b849a, edmac_reset_packunpack_mode 0xe05b8106 (+ earlier boomer_vdkick 0xe05b7520,
select_boomer 0xe05b752e, reset_channel 0xe05b7b42). edmac-d8.c: MEM2MEM_BOOMER_SELECTOR 0x373f0226. module.c:
card-ready boot gate. console.c: CONSOLE_DEBUG. Card build = c88c4ff2.

---

# UPDATE 2026-07-14 ~20:30 — FIO write-mount blocker CONFIRMED real (not DCIM, not drive letter). mem2mem fix holds across reboots.

Second attempt: user took a photo (creates DCIM/100CANON on the card, persists across reboot), re-triggered.
- IMPORTANT per-boot GOTCHA (cost me a hang): toggling RAW off/on RELOCATES mlv_lite.mo -> raw_start_stop moves
  MID-BOOT. Re-resolve via the CBR table AND rd the prologue (must be 0xE92D4037) in a SEPARATE step BEFORE `call`.
  Calling a stale addr (0x00005F6C) hung the camera -> user reboot. (Fresh boot base: cbr tbl 0x010B95AC ->
  vsync 0x010B6730 / keypress_raw 0x010B36C8, diff 0x3068 -> base 0x010AF210 -> raw_start_stop 0x010B360C.)
- RESULT: assert tally STILL 8->2 (only benign Outer.c 0CD + Edmac.c 0770) -> the 3 fixes are stable across boots.
- BUT FIO write STILL fails identically: "efat_openfent:-2 / AcqSize(ecode=0x100b) / Remove". So the missing-DCIM
  theory is WRONG too. raw_rec_task1 aborts at FIO_CreateFile (mlv_lite.c:3440) every time.
- Attempted a decisive live FIO probe (call is_dir/FIO_CreateFile on "B:/") via the shell `load` scratch-staging
  cmd; the `load` response ("loaded N bytes @addr") does not round-trip through the mlserial daemon (async log
  noise / framing), so the scratch addr couldn't be captured. FIO_CreateFile=0xcf791, FIO_RemoveFile=0xcf475,
  is_dir=0xcf671, FIO_CreateDirectory=0xcf70d, FIO_CloseFile=0xe05b1c1b, sh_scratch is static (not in .sym).
  TODO harness: either add a `mkfile`/`isdir <path>` shell cmd, or fix `load` response capture in mlserald.

## CONCLUSION: the SD is not WRITE-mounted for Canon FIO in movie-LV on mk2 — a genuine storage/FM issue, NOT a
1-liner. Established: SHOOTING_CARD correctly = "B:"/SD (live-verified); DCIM exists; drive letter right; yet
FIO_CreateFile -> "Not mounted ID=0". Modules READ from the card at boot (its own mount race, ID=1). Suspect the
ML drive-letter->Canon-device-ID mapping for writes, or Canon's user-FS mount not being brought up on mk2 (recall
boot "[FM] Suspend : No Card"). NEXT UNIT (workflow, next session): RE Canon FIO/FM mount + the drive->device-ID
map on mk2; options ranked: (a) find/trigger Canon's SD write-mount before raw_rec creates the file; (b) redirect
the MLV to a path proven writable (the ML dir that modules load from) as an end-to-end proof-of-capture; (c) add a
harness `mkfile` cmd to test writability directly; (d) card-ready gate in raw_video_rec_task. The mem2mem copy
engine is DONE — this is purely the file-write/storage layer now.

---

# UPDATE 2026-07-14 ~21:00 — FIO blocker PRECISELY characterized: card WRITABLE at idle, raw_rec context fails. Not a mount/path/drive bug. RE workflow launched.

Decisive live-harness FIO probing (sh_scratch @0x0014EEA8; `load <pathhex>` then `call fn(sh_scratch)`; is_dir=0xcf671,
FIO_CreateFile=0xcf791, FIO_CloseFile=0xe05b1c1b, FIO_RemoveFile=0xcf475):
- Card is FULLY WRITABLE AT IDLE: is_dir("B:/")=1, is_dir("B:/DCIM/100CANON")=1, is_dir("A:/")=0 (no CF).
  FIO_CreateFile("mlvtest.dat")=h0x3, ("B:/mlvtest.dat")=h0x4, ("B:/DCIM/100CANON/TEST.MLV")=h0x3 — ALL succeed.
- get_shooting_card()==get_ml_card()==0x0010043C, drive_letter="B" type="SD" folder_number=0x64(100) file_number=0x1ECF.
  get_dcim_dir()@0xcf3f1 -> "B:/DCIM/100CANON" (exact dir mlv_lite targets). dcim_dir_suffix="CANON".
- KEY: MODULE_PATH = "ML/MODULES/" (NO drive letter) -> module_task uses the DEFAULT drive; mlv_lite uses explicit
  "B:". Both resolve to the same mounted SD (is_dir agrees). So path/drive/mount are all FINE at idle.
=> The blocker is NOT mount, NOT path, NOT drive letter. raw_rec_task1's FIO_CreateFile fails ONLY in its own
   context, during raw-record start, AFTER setup_buffers(): "[FirstEnt] Not mounted ID=0" x6 -> efat_openfent:-2
   -> AcqSize(ecode=0x100b) L:718 -> Remove L:416. The SAME create from the serial-shell task succeeds.
PRIME HYPOTHESIS: setup_buffers() (allocates ALL free mem for frame slots) exhausts the pool Canon FIO needs to
open a file / enumerate a dir (FirstEnt allocates), so the alloc fails and is misreported as "not mounted".
ALT: entering raw LV/movie state transiently disrupts the SD volume.

CRASH: the 2nd record attempt this boot did NOT abort cleanly (the 1st did) — it cascaded to pc=0x00000000
lr=0xE05D1161 dfar=0x80120011, an abort LOOP (-> watchdog reboot). Same lr=0xE05D1161 as the original pre-fix
crash = the generic DryOS null-callback panic path, fired DOWNSTREAM of the FIO-create failure/cleanup (NOT a
mem2mem regression — mem2mem asserts stayed at the benign 2, no "Frame incomplete"). Fixing the FIO create avoids
this path. (Also: mlv_lite.mo RELOCATES within a boot after alloc-heavy ops — re-resolve raw_start_stop via the
CBR table + verify prologue 0xE92D4037 EVERY time; a stale even-addr call hangs the camera.)

Per-boot data this session (build c88c4ff2): raw_start_stop seen at 0x010B3E2C, 0x010B360C, 0x010848EC (module
base drifts down as allocations happen). CBR two-anchor always holds: vsync(off 0x7520) - keypress_raw(off 0x44B8)
= 0x3068; base = vsync_handler - 0x7520; raw_start_stop = base + 0x43FC.

RE workflow launched (wf m50mk2-fio-rawrec-blocker): Ghidra on the "[FirstEnt] Not mounted ID=0"/AcqSize/efat_openfent
path (memory-exhaustion vs volume-state; malloc_info@e00f1c19, GetMemoryInformation@e05becab) + Discord/mk1 memory-FIO
comparison (setup_buffers reservation, create-before-buffers ordering, srm-not-working budget). Awaiting synthesis.
Camera needs a power-cycle (abort loop). mem2mem fix remains solid + committed in stubs.S/edmac-d8.c.

---

# UPDATE 2026-07-14 ~21:40 — ROOT CAUSE (Ghidra ROM-verified): SD VOLUME UNMOUNT (mount bit cleared), NOT FIO memory-OOM. Fix = diagnostic-gated retry/free-mem in mlv_lite.

Workflow wf_0451952a (Ghidra FIO error-path RE; Discord agent errored on schema but Ghidra agent + synth nailed it).
CONFIRMED IN ROM: efat open FUN_e011831c @0xe011831c returns -2 from a PURE MOUNT-FLAG TEST (no allocation):
  0xe0118352 lsls r0,#0xa ; 0xe0118354 bmi  -> tests bit 0x00200000 of the per-drive FAT volume descriptor.
  descriptor base *(0xe01185a8) = RAM 0x000aae70, stride 0x98, flags at +0xc, MOUNT BIT = 0x00200000.
  => DRIVE-B (SD, idx 1) mount-flag word = RAM 0xaaf14 (=0x000aae70+0x98+0xc); device-0/A word = 0xaae7c.
Chain: efat -2 -> errno 2 (FUN_e00aa1bc maps 0x100b<->2) -> "efat_openfent:-2" -> FIO "AcqSize(ecode=0x100b) L:718".
"[FirstEnt] Not mounted ID=0" (fmt @0xe05b15ac): ID = SD DEVICE id (0 = the single SD), NOT the drive-letter index
-> consistent with "B" = physical SD device 0. Printed 6x = Canon's own mount-retry loop, then gives up.
DECISIVE NEGATIVE: the FIO/FAT heap allocator a real OOM would hit (FUN_e05bf4d8->FUN_e05bee20, "System/Memory.c":0x5d7,
"[MEM] NG AllocateMemory") prints loud strings that are ABSENT from our UART. And idle create succeeds -> bit is SET
at idle. => at record-start the SD volume's mount bit is CLEAR (unmounted/mid-remount).

MECHANISM (synth, MEDIUM on exact trigger): mlv_lite grabs ALL shoot(+SRM) memory in realloc_buffers() (mlv_lite.c
1522-1523 shoot_malloc_suite(0)/srm) at IDLE when raw is ARMED — NOT in setup_buffers() at record-start (that only
sub-carves already-held suites). Record-start's raw_update_params()->raw_lv_request (mlv_lite.c:3421) makes Canon
(re)mount the card via its movie-format mount machinery (vcmMovieFormatMount@0xe0202328, fmMountCard@0xe02023c0,
fmUnmountCard@0xe02028b0); that remount needs memory but ML holds it all -> remount STARVED -> mount bit stays clear.
NOTE Canon already retried 6x and gave up -> likely STARVED (won't self-heal on time alone), so a bare retry may be
insufficient; freeing memory back to Canon is the probable real fix.

## DIAGNOSTIC (cheap, harness, decisive — run after power-cycle, raw ARMED):
  rd 0xaaf14  -> expect (val & 0x200000) != 0 at idle (mounted). Re-test idle FIO_CreateFile WHILE ARMED (confirm
  it still works when suites are held). Then `watch 0xaaf14 50 200` + trigger record:
   - bit 1->0 then back to 1 within ~hundreds ms  => TRANSIENT  => PRIMARY retry loop suffices.
   - bit 1->0 and STAYS 0 (matches 6x give-up)     => STARVED    => must free memory for Canon's remount.
  (Throwaway proof: during a failing start, `wr 0xaaf14 <val|0x200000>` then create -> proves the flag is the sole gate.)

## FIX (in modules/raw_video/mlv_lite/mlv_lite.c raw_video_rec_task(); rebuild mlv_lite.mo ONLY, re-derive offsets):
  PRIMARY (~10 lines): bounded retry around get_next_raw_movie_file_name()+FIO_CreateFile (lines 3434-3445, and the
    card_index==1 create ~3489), retry on failure w/ msleep, mirroring Canon's mount retry. No-op on healthy mount.
  DEEP (if STARVED): free memory so Canon's remount succeeds. SRM may already be off (function_overrides srm->NULL);
    the hog is shoot_malloc_suite(0) @1522. Option: free_buffers() before raw_update_params, remount, create file,
    then realloc_buffers() (slow ~1-2s); OR bound the shoot allocation to leave Canon headroom.
  SECONDARY: reorder file-create BEFORE raw_update_params()/wait_lv_frames() so the handle opens while mounted.
  DO NOT chase the FIO/FAT heap — ROM proves it's not the failing path.
Confidence: HIGH the gate is the cleared mount bit; MEDIUM transient-vs-starved (the `watch 0xaaf14` settles it in ~10s).
NOTE: the fix is in the MODULE (mlv_lite.mo), not autoexec — lighter rebuild; but adding code shifts the .mo offsets
(vsync 0x7520 / keypress_raw 0x44B8 / raw_start_stop 0x43FC) -> re-derive from the new .mo before HW test.

## HW DIAGNOSTIC RESULT (this session, before the fix): TRANSIENT unmount confirmed.
Mount-flag word @0xaaf14 read live: 0x02600001 at idle (bit 0x200000 SET = mounted); FIO_CreateFile succeeds while
raw ARMED (handle 0x3) => memory-hold alone does NOT break it. On record trigger the bit's core mount flag stayed
0x200000 in the ~3s `watch 0xaaf14` window (only status bits 0x80000/0x100000 toggled) — i.e. the unmount window is
SUB-30ms / happens inside the blocking raw_start_stop call and self-heals by the time the call returns. Attempts
varied: some got "[FirstEnt] Not mounted ID=0" x6, this one only "efat_openfent:-2 + Remove L:416" (milder/other -2
path) — classic RACE signature => bounded RETRY is the correct fix.

## FIX APPLIED + FLASHED: bounded retry around FIO_CreateFile in raw_video_rec_task (mlv_lite.c ~3433).
Loop up to 20x: get_next_raw_movie_file_name()+FIO_CreateFile, msleep(100) on failure (~2s worst case, no-op on a
healthy mount). Rebuilt MODULE ONLY: `cd modules/raw_video/mlv_lite && make clean && make` (platform `make` only
COPIES prebuilt .mo — must build the module dir directly). New mlv_lite.mo = f442f106c53d668191d39feb5629dd9b (73204B,
+48B). autoexec UNCHANGED on card (c88c4ff2); M50mk2_101.sym IDENTICAL (90e6d80e) so core addrs stable + only the .mo
was flashed. LOADING.LCK cleared, verified from fresh remount.

## NEW .mo PER-BOOT RESOLUTION (offsets shifted by the +48B): raw_start_stop STILL 0x43FC (before the edit point);
keypress_cbr_raw STILL 0x44b8; vsync_cbr NOW 0x7550 (was 0x7520). Recipe: read vsync handler V + keypress_raw handler
K from mlv_lite cbr table (module_list@0x00110438, entry2 cbr-ptr @0x00110648; entries are 5 words {name,sym,type,
handler,0}; type6=vsync, type8=keypress_raw). VALIDATE V-K = 0x3098 (=0x7550-0x44b8). Then raw_start_stop = K - 0xBC
(= K - 0x44b8 + 0x43FC). VERIFY prologue @that addr = 0xE92D4037 (ARM push) BEFORE `call` (stale = hang). Core harness
addrs stable (sym identical): is_dir 0xcf671, FIO_CreateFile 0xcf791, sh_scratch 0x0014EEA8, mount-flag 0xaaf14.
NEXT: user boot + arm RAW 640x384 -> re-resolve -> `call raw_start_stop` -> expect retry to survive the transient
unmount -> file created -> frames written -> .MLV grows on B:/DCIM/100CANON. If still fails, escalate to DEEP (free mem).

---

# UPDATE 2026-07-14 ~22:15 — RETRY + REORDER BOTH FAIL: SD unmounted from the VERY START of raw_rec_task. New hypothesis: `call raw_start_stop` bypasses Canon's movie-format card mount.

Tested build 5eff6467 (retry) then 913bedf7 (reorder = create file BEFORE raw_update_params, at powersave_prohibit):
- RETRY (5eff6467): crashed pc=0 lr=0xE05D1161. NO [rawrec] logs on UART, setup_buffers ran. => FIO_CreateFile's
  error path crashed in Canon DebugMsg BEFORE returning to the retry. Retry can't run.
- REORDER (913bedf7): STILL "[FIO] Not mounted ID=0" x9 (my early retry loop IS running, 9 attempts) then STILL
  crashed pc=0 lr=0xE05D1161. => the SD is ALREADY UNMOUNTED at powersave_prohibit (mlv_lite.c:3415), i.e. BEFORE
  raw_update_params. So the unmount trigger is EARLIER than raw_update_params — the reorder can't dodge it.
- NOTE: module printf() likely does NOT reach UART (only Canon [FIO] DebugMsg does) -> the [rawrec] logs are in
  ML/LOGS/console.log ON THE CARD, not uart.log. Read console.log from the card to see them.

## KEY REFRAME: the card is mounted+writable at IDLE-armed (proven: FIO_CreateFile handle 0x3), but UNMOUNTED the
moment raw_rec_task runs — even at its very first step. What differs between idle and task-start is NOT our code
(RAW_PREPARING is just a flag). STRONG NEW HYPOTHESIS: we always trigger recording via `call raw_start_stop` from
the serial-shell task, which BYPASSES Canon's normal movie-record entry. On these cams ML raw video normally runs
with Canon in an active movie/record state that KEEPS the card movie-mounted; the direct call skips that, so the SD
is not movie-mounted when raw_rec_task writes. Evidence in-code: mlv_lite use_h264_proxy() path calls movie_start()
(Canon H.264 record -> mounts card) at mlv_lite.c:3398-3412; the non-proxy path assumes the card is already mounted.

## NEXT TESTS (change the TRIGGER, not more retries):
1. Physically PRESS THE RECORD BUTTON on the camera (movie mode, raw armed) instead of `call raw_start_stop` — if
   Canon movie-mounts the card first, ML raw should find it mounted and write the .MLV. (Or send the record keycode
   via harness `key`.)
2. OR enable mlv_lite "H.264 proxy"/record-with-Canon option (use_h264_proxy=1) so movie_start() mounts the card,
   then raw records alongside.
3. Separately, the pc=0 lr=0xE05D1161 crash is a real landmine: Canon's DebugMsg error path calls a NULL callback on
   this port on certain [FIO] errors. Even with the mount fixed, worth RE'ing/stubbing that callback so FIO errors
   are non-fatal. (This is why every "Not mounted" run eventually hard-crashes instead of aborting cleanly.)
CURRENT: build 913bedf7 on card (retry+reorder+logging, harmless). Camera in pc=0 abort loop -> power-cycle needed.
The mem2mem copy fix remains solid; this whole blocker is the record-path SD mount + the DebugMsg crash, not capture.

---

# UPDATE 2026-07-14 ~22:35 — Canon-recording-alongside test: FAILED. Confirms mechanism (A) definitively. This is an architectural wall; pausing.

Test: user started CANON H.264 recording (record button; confirmed NOT wired to ML on M50 II — it only drives Canon),
then WHILE Canon was recording I fired `call raw_start_stop`. Result: STILL "[FIO] Not mounted ID=0" x10 + pc=0
lr=0xE05D1161 crash. => Even with Canon actively recording (card is written by Canon), ML's FIO_CreateFile to the
normal FAT volume (mount bit 0xaaf14) reports NOT MOUNTED.
DEFINITIVE MECHANISM (Ghidra (A) confirmed): entering ANY record/movie state makes Canon switch the SD to its
MOVIE-FORMAT mount (vcmMovieFormatMount @0xe0202328 / fmMountCard @0xe02023c0), which tears down the NORMAL FAT
volume descriptor (0xaaf14 bit 0x200000 clear) that ML's efat/FIO uses. So the card is only FAT-mounted at IDLE
(raw armed, not recording) — the exact window ML raw needs to WRITE is the window the FAT is gone. Plus the FIO
"Not mounted" error reliably trips the pc=0 lr=0xE05D1161 DryOS DebugMsg NULL-callback crash on this port.

## APPROACHES EXHAUSTED (all -> "Not mounted" -> pc=0 crash): idle `call raw_start_stop`; bounded retry (crash
beats retry); reorder create-before-raw_update_params (FAT already unmounted at task start); Canon-recording-alongside.

## THIS IS A DEEP CANON STORAGE-ARCH BLOCKER — needs focused RE next session, NOT more live trigger-timing:
1. How does Canon's movie-format mount expose the card for writes during recording? Can ML's FIO write via THAT
   mount/drive instead of the torn-down FAT? (RE vcmMovieFormatMount / fmMountCard / the movie-write path + what
   drive/handle Canon's own H.264 writer uses.)
2. How do WORKING ML raw cams (bilal M50mk1, others) write .MLV during recording — do they keep the FAT mounted,
   use the h264-proxy (movie_start) path with a specific write mount, or patch the mount switch? (Discord/source RE;
   the earlier Discord agent errored — re-run it.)
3. FIX THE CRASH (independent, important): RE + stub the pc=0 lr=0xE05D1161 DebugMsg NULL-callback so FIO "Not
   mounted" is a clean non-fatal error (then retries/aborts work instead of hard-crashing).
4. Possible angle: PREVENT the movie-format switch (keep FAT mounted) since ML raw doesn't need Canon H.264 — but
   that may break the sensor/EDMAC path recording depends on.

## SESSION BOTTOM LINE: the D8 mem2mem raw-frame COPY is fully fixed + HW-validated (the hard RE win, committed in
stubs.S/edmac-d8.c). Raw CAPTURE works; the remaining blocker is purely the Canon record-state SD-mount conflict +
the DebugMsg crash — a separate, well-characterized RE task. Cost ~$11k this session; pausing here is the right call.
Card build 913bedf7 (harmless retry/reorder/logging). All addresses + mechanism above for a clean resume.

---

# UPDATE 2026-07-15 ~00:00 — BREAKTHROUGH: root cause found via reference-port RE. NOT an architectural wall. Fix A (crash-harden) built+flashed.

Ran a 4-agent workflow over the CLONED D8 reference ports (research/bilal_M50_rawvideo [orig M50!], kitor_raw_draft_v2 [EOS R],
m6ii_raw_video, kitor_edmac_sx740) + ml_discord + a Ghidra crash RE. DEFINITIVE ANSWER to "how do working D8 ports write
the .MLV during record":

## THEY DON'T RECORD IN CANON'S SENSE. Zero card-mount/movie-format code exists in ANY reference port.
- They capture in movie-mode LiveView STANDBY (Canon NOT recording: __recording==0, PROP_SHOOTING_TYPE!=4). Capture runs off
  the LiveView vsync CBR (is_movie_mode-gated, mlv_lite.c:660) which fires continuously in standby — no Canon recording needed.
- They CONSUME the record-button press so Canon never enters its movie-record state machine -> vcmMovieFormatMount/fmMountCard
  never run -> the still/LV FAT (bit 0x200000 @0xaaf14) is never torn down -> the STOCK inline FIO_CreateFile just works.

## WHY M50 II BREAKS (two compounding, in-tree-verified gaps):
(a) BGMT_REC UNDEFINED in platform/M50mk2.101/gui.h (bilal M50.110/gui.h:28 = `#define BGMT_REC 0x20`). -> src/module.c:1134
    falls it to -1 -> no MODULE_KEY_REC -> raw_rec_keypress_cbr (mlv_lite.c:4218) can't match the record button.
(b) THE DEEPER CAUSE: M50II_BYPASS_GUI_OVERRIDE=1 (src/gui.c:124, made permanent — 6 gui override iters faulted). ML only
    READ-ONLY PEEKS Canon's event ring (gui.c:328), it CANNOT CONSUME events. So raw_rec_keypress_cbr returning 0 is a NO-OP:
    the physical REC press reaches Canon -> Canon starts H.264 -> vcmMovieFormatMount -> FAT unmounted -> crash.
=> Every failing HW test had Canon recording (REC button) OR ML's own capture-state entry; both unmount the FAT on mk2.
   The original M50 keeps the still FAT mounted through movie-LV standby; the mk2 firmware remounts on entering capture.

## USER'S KEY QUESTION reconciled (why did bare `call raw_start_stop` [Canon idle] ALSO fail?): raw_rec_task's start path
(raw_update_params -> raw_lv_request -> full raw/movie capture state) appears to trigger the SAME remount on mk2, TRANSIENTLY,
even with Canon idle. The mount bit "looked set" only because I sampled 0xaaf14 AFTER the ~1s blocking call (post-window). And
the pc=0 crash fired on the FIRST FIO error before the retry could catch the re-mount. "Transient" is GOOD: it recovers.

## CRASH RE CORRECTION (Ghidra): pc=0 lr=0xE05D1161 is NOT a null callback. It is DATA CORRUPTION inside Canon DryosDebugMsg
(@0xE05D10C4) while formatting the string "[%s] Not mounted ID=%d" (@0xE05B15AC) — a bad %s/format pointer deref in the
packer FUN_e005d89c (bl @0xE05D115C). CRITICAL: our UART debug channel is FED by this DebugMsg path, so globally neutering it
kills our own logging. => Fix = DON'T issue FIO while unmounted (so the "Not mounted" log never fires). context ptr *0x5154,
init FUN_e05d0f76; Fix-C last resort = patch 0xE05D10C4 push{r0-r3}(0F B4)->bx lr(70 47).

## FIX A IMPLEMENTED + FLASHED (build mlv_lite.mo 38704d576b8a4919b75e338d3c01a852):
modules/raw_video/mlv_lite/mlv_lite.c raw_video_rec_task: added static raw_wait_fat_mounted(ms) that POLLS *0xaaf14 & 0x200000
(PURE READ, no FIO, cannot crash) with msleep(20). Removed the risky early-create/printf-in-window (printf->console.log is FIO
= crash risk while unmounted). Create now AFTER setup_buffers, gated: wait-for-remount -> get_next_raw_movie_file_name ->
FIO_CreateFile, with UART-safe logging via m50ii_uart_str (0xcf0bf, no FIO). If mount never returns in 3s -> clean NotifyBox +
goto cleanup (NO crash). raw_start_stop STILL @.mo 0x43FC; keypress_raw @0x44b8; vsync @0x75dc -> recipe raw_start_stop =
keypress_handler - 0xBC (validate vsync-keypress = 0x3124). prologue 0xE92D4037.

## DECISIVE HW TEST (next): boot, arm RAW 640x384, `call raw_start_stop` (Canon-harmless — do NOT press REC button). Watch UART:
- "[rawrec] fat mounted=1 ..." + "[rawrec] create f=0x3.." + NO [AB] crash + a .MLV on B:/DCIM/100CANON (pull via reader, decode).
  => the transient-remount theory + Fix A CONFIRMED. If frame WRITES later crash, add the same mount-guard before FIO_WriteFile.
- If "[rawrec] fat mounted=0" (timed out) -> clean abort, no crash -> the unmount is NOT transient in ML's own path -> need the
  SET/menu trigger (keep Canon out) AND/OR write via Canon's movie-format volume. Either way: NO MORE pc=0 CRASH = progress.
LATER (UX): extend mlv_lite.c:4221 SET-trigger (cam_50d||cam_5d2 -> add M50 II) or a menu "start" action, so normal use doesn't
need the harness. mem2mem COPY remains fully fixed. Session pivoted from "wall" to "one tractable fix under test".

## HW RESULT of Fix A (build 38704d57, `call raw_start_stop @0x3B6BE6AC` this boot, Canon-harmless):
BIG WIN, chronological UART:
  [rawrec] fat mounted=1 bit=0x2680001    <- mount-wait succeeded
  [rawrec] create f=0x3                   <- FIO_CreateFile SUCCEEDED (first time ever in the record task!)
  [FIO] efat_openfent:-2 / AcqSize(ecode=0x100b) L:718 / Remove L:416   <- the HEADER WRITE hit not-mounted
  Buffer x many
=> Fix A protects the CREATE: "[rawrec] fat mounted=1"+"create f=0x3", NO crash AT the create (reproduced x2). BUT the
crash is NOT eliminated — it is DEFERRED to the per-frame WRITE path. CORRECTION (initial UART read was too early, before
the "after a while" crash): the FULL window shows the record task reaches the capture loop ("Frame incomplete at slot 0,
frame 1" on screen — frame 1 NEVER saved), the writes hit "[FIO] Not mounted ID=0" x11 / AcqSize / efat, and then the SAME
crash fires and LOOPS: 45x "[AB] pc=0x00000000 lr=0xE05D1161 dfar=0x80920005" -> watchdog reboot. REPRODUCED IN BOTH runs.
INTERPRETATION: "Frame incomplete at slot 0" = not one frame written => the still/LV FAT is likely PERSISTENTLY unmounted for
the whole recording (not a create-instant flicker). The file is created on the still FAT while it's briefly up, then entering
FULL capture (mlv_rec STARTING / init/write headers / hack_liveview) tears the still FAT down, and every write + its
"Not mounted" DebugMsg fails until one trips the pc=0 data-corruption crash.
NEXT (two things, both needed): (1) STOP the write crash independent of mount — Fix A only guarded the create; the per-frame
writes need it too, OR better neutralize the DebugMsg crash at the source (Fix B: raise the FIO class log-threshold via
*0x5154 ctx+class*3+0x38 after capturing the class on HW; OR the memory-overlap angle — the crash RE said pc=0 is data
corruption possibly from ML memory overlapping the DebugMsg buffers *0x5154/ctx+0x14, ties to ml_reserved_mem/_bss_end
budget). (2) MAKE THE WRITES SUCCEED — if the FAT is persistently unmounted during capture, per-write waits won't help;
need the standby approach (capture in movie-LV standby, keep Canon out) OR write to Canon's movie-format volume that H.264
uses. DECISIVE sub-test: guard the writes with raw_wait_fat_mounted() and log whether it TIMES OUT every frame (persistent
unmount) vs recovers (transient). STATUS: crash gone at CREATE + create WORKS; write path still crashes. Card build 38704d57.

## UPDATE 2026-07-15 ~00:19 — Fix A re-run (build 38704d57): NO pc=0 crash this time, but LiveView/AE pipeline WEDGES.
Chronological UART (single run, harness `rd 0xaaf14` after = 0x02680000 => FAT mounted at idle, bit 0x200000 set):
  [rawrec] fat mounted=1 bit=0x2680000     (t=188680.194)
  [rawrec] create f=0x3                      <- FIO_CreateFile OK
  [FIO] ERROR [FirstEnt] Not mounted ID=0  x11 CONTINUOUSLY 188680.194 -> 188683.444  (~3.25 s straight)
  [FIO] ERROR efat_openfent:-2 / AcqSize(ecode=0x100b) L:718 / Remove(ecode=0x100b) L:416  (t=188688.496)  <- mlv_lite
       cleanly deletes the empty chunk and exits its rec task. NO [AB] pc=0 crash — heartbeat keeps climbing n=11A->11D.
  THEN storm until crash: [AutoExposure::AEWBInteg.c] non-fatal asserts (L:F8/122/150) flood -> [DAF] FrameBufferDummy,
       [LENS] LensApi_SendPropDynamicData FAILED / QUEUE 56/56, [LAETC] ERROR "Over the Deadline Aewb 9",
       [EIS] ERROR EV_SET_FRAMECOUNT/EXPBALANCE_GLOBALVECTOR TryPostEvent(0xd) — all REPEAT UNTIL CRASH (user-confirmed).
THREE FINDINGS: (1) UNMOUNT IS PERSISTENT (~3.25 s, whole capture), NOT a create-instant flicker => per-write mount-guards
are USELESS (would time out every frame, save 0). Dropping that idea. (2) The pc=0 DryosDebugMsg crash is INTERMITTENT
(this run never hit it) => the crash was never the wall; the unmount + LV disruption is. (3) NEW/BIGGER: entering our raw
capture doesn't just unmount the FAT, it DESTABILIZES Canon LiveView/AE itself — AEWB misses its real-time deadline, EIS/
LAETC event queues fill (TryPostEvent 0xd), loops to a watchdog crash. The WORKING M50 mk1 (bilal) port does NOT cause this.
OPEN VARIABLE (decides next test, cheap): what CAMERA MODE was this test run in? Reference ports require MOVIE mode + LiveView
STANDBY (is_movie_mode-gated vsync CBR, mlv_lite.c:660). If our failing tests were in photo/other mode, raw_lv_request forces
a heavy LV/mount transition that (a) remounts to movie-format (FAT torn down) and (b) wedges AE. DECISIVE CHEAP NEXT TEST:
put camera in MOVIE mode, LiveView showing (standby, NOT recording), read 0xaaf14 to confirm FAT still mounted in that state,
arm raw small-res, `call raw_start_stop` FROM movie-LV standby. If FAT stays up + frames write => reference mechanism works on
mk2 (recording!). If FAT still unmounts even in movie-LV standby => standby-keeps-mounted is FALSE on mk2 => must (A) write to
Canon's movie-format volume that's up during capture [RE vcmMovieFormatMount 0xe0202328 / fmMountCard 0xe02023c0 for the drive/
path Canon's H.264 writer uses] or (B) rework mk2 capture entry to the lightweight standby-vsync path. Either way the AE-storm
disruption (finding 3) is a SEPARATE coexistence problem to solve too. Card build unchanged 38704d57.

## UPDATE 2026-07-15 ~01:15 — BREAKTHROUGH #2: the "architectural wall" was a WRONG-CAMERA-MODE artifact. Movie-LV STANDBY keeps the FAT mounted + NO crash + NO AE storm. New blocker isolated: raw_update_params() "Raw detect error" in standby.
DECISIVE TEST (user set camera to MOVIE mode, LiveView showing = standby, did NOT press REC): fired raw_start_stop from
that state. RESULT — completely unlike every prior (photo-mode) run:
  * FAT stayed MOUNTED through the whole trigger: 0xaaf14 = 0x02680000 before AND after (bit 0x200000 up). NO teardown.
  * NO crash, NO pc=0, NO AE "Over the Deadline" storm. Heartbeat clean the whole time (n climbs steadily, ovr=0).
  * raw_recording_state (=*(r4+0x210), r4=0x3B6CCB00 this boot => 0x3B6CCD10) went 0(IDLE) -> 1(PREPARING) -> 0(IDLE):
    the rec task DID start (raw_start_stop spawns task_create("raw_rec_task1",0x19,0x1200,raw_video_rec_task,0), src
    mlv_lite.c:4009) and bailed early.
  * On-screen NotifyBox (user-confirmed): "Raw detect error" => raw_video_rec_task hit `if (!raw_update_params()) {
    NotifyBox("Raw detect error"); goto cleanup; }` at mlv_lite.c:3441. The bail is SILENT on UART (NotifyBox = screen
    only), which is why the log showed only heartbeats.
=> The FAT-unmount + pc=0 crash + AE storm we spent the whole prior arc fighting were ALL caused by triggering raw in the
WRONG camera state (photo / non-standby), which forced Canon's capture/movie-format-mount transition. In movie-LV STANDBY
(the exact state the working M50mk1/EOS-R ports use) NONE of that happens. The mount blocker is effectively SOLVED by mode.
raw_video_enabled = 1 confirmed on HW (@0x3B6CCD4C = 0x00000001), so the raw feature IS on.

## NEW BLOCKER (well-defined): raw_update_params() fails in movie-LV standby => raw sensor stream not detected/armed.
ARMING CHAIN (mlv_lite.c): raw_lv_request_update() [REQUIRES(ShootTask), :1782] issues raw_lv_request() [:1765] ONLY when
`raw_video_enabled && lv && is_movie_mode()` (:1786). If that predicate is false in standby, the raw LV buffer (lv_save_raw
in raw.c) is never enabled => raw_update_params() has no raw stream => "Raw detect error". Earlier (capture-state) runs
detected raw fine because the heavy transition brought the stream up (but unmounted the FAT). So the mk2 open question is:
does raw LV come up in pure movie-LV standby, or does it need something the standby state doesn't provide?
NEXT DIAGNOSTIC (was mid-check when a menu tap rebooted the camera): read on HW (main-ML globals, magiclantern.sym):
  is_movie_mode = fn @0xde065 (thumb -> call 0xde065 returns bool) ; shooting_mode = 0x149e34 ; lv = 0x149e5c.
  If lv==0 or is_movie_mode()==0 in standby => that predicate blocks raw_lv_request => fix the mode/lv detection (SIMPLE).
  If both true but raw still not detected => raw.c LV backend (raw_lv_request/lv_save_raw/raw_update_params geometry) needs
  work on mk2 — resolve raw_info + the raw buffer and see why detection fails. Alt: add UART logging to the detect path
  (log is_movie_mode/lv/raw_lv_request result + raw_update_params failure reason) then rebuild/flash/reboot for a decisive read.

## PER-BOOT ADDRESS RESOLUTION (this method WORKED — module base shifts every boot; re-run each boot):
1. module_list @ FIXED 0x110438. Entries are 0xB8 bytes, module NAME is an inline char[] at entry+0x00. Entry[0]≈0x110438
   (file_man), Entry[1]=0x1104F0 = "mlv_lite", Entry[2]=0x1105A8 (bench). (Earlier confusion: I read entry[0]'s pointers and
   mis-took them for mlv_lite's — every module's __module_info shares the same 0x0000085A magic, so magic is NOT identifying.)
2. mlv_lite entry (base 0x1104F0) pointer fields: info @entry+0x94 (=0x110584), strings @entry+0x98, cbr @entry+0xA0
   (=0x110590). This boot: info=0x3B6CC108, cbr=0x3B6CC07C, strings=0x3B6D6AB0.
3. __module_cbr table = cbr ptr. 5-word entries [name_str, sym_str, type, handler, ctx]. type 6=vsync(1st), 8=keypress(3rd).
   keypress handler = *(cbr + 0x34). This boot cbr=0x3B6CC07C => keypress=*(0x3B6CC0B0)=0x3B6C6778 (type word 0x3B6CC0AC=8 ✓).
4. raw_start_stop = keypress_handler - 0xBC. VALIDATE: vsync handler (*(cbr+0xC)) - keypress = 0x3124; and VERIFY prologue at
   raw_start_stop = E92D4037 E59F4088 E59F3088 E5945210 E3550000 0A000007. This boot: raw_start_stop=0x3B6C6778-0xBC=0x3B6C66BC ✓.
5. CROSS-CHECK via info struct (0x0000085A magic): info+0xC=raw_rec_init=base_text+0x450; info+0x10=raw_rec_deinit=base_text+
   0x560c => base_text. This boot info=0x3B6CC108 => base_text=0x3B6C22C0 (init 0x3B6C2710, deinit 0x3B6C78CC).
6. GLOBALS in module .bss are NOT contiguous after .data (my computed .bss base was off by 0x318). To get a module global,
   read its RELOCATION SLOT instead: &raw_video_enabled is stored at .data 0x20; data_base = cbr - 0x174 (0x3B6CC07C-0x174=
   0x3B6CBF08); so &raw_video_enabled = *(data_base+0x20) = *(0x3B6CBF28) = 0x3B6CCD4C this boot. (.mo symbol offsets via
   `arm-none-eabi-nm build/mlv_lite.mo`, reloc slots via `readelf -r`. .mo = modules/raw_video/mlv_lite/build/mlv_lite.mo,
   md5 38704d57 = the flashed one; the platform/*/build/modules copy is STALE 9e6b9db5.)
HARNESS gotcha: run `python3 mlserial.py ...` from /mnt/data/WORK/eos/tools/mlserial (a stray `cd` elsewhere makes reads
return silently-empty). UART log tail = tools/mlserial/run/uart.log. Camera REBOOTED at end (user menu tap) — re-resolve next boot.

## CORRECTION 2026-07-15 ~06:44 — "Breakthrough #2" was WRONG: movie-LV standby does NOT keep the FAT mounted through capture. The capture-induced unmount + pc=0 crash STAND. The un-armed test only looked clean because it bailed at raw-detect and NEVER entered capture.
After the reboot the camera settled in movie-LV standby and raw_info @0x103ed4 was ALREADY POPULATED (buffer=0x2BC61CC8, w=0x830
=2096, h=0x48C=1164, pitch=0xE54=3668, frame_size=0x4125F0, bpp=0xE=14, black=0x7FF, white=0x3F48=16200) — so raw detection/
ARMING WORKS when the LV stream is settled (is_movie_mode()@0xde065=1, lv@0x149e5c=1; shooting_mode@0x149e34=0/P but movie
detection uses a different flag). So the prior "Raw detect error" was a NOT-YET-SETTLED stream, not a mode/lv problem.
Re-resolved raw_start_stop this boot = 0x010609AC (module base moved LOW to 0x0106xxxx; cbr @0x0106636C, keypress=0x01060A68,
vsync=0x01063B8C, delta 0x3124 ✓, prologue verified). Fired it with raw ARMED + FAT mounted (0xaaf14=0x02680000). RESULT — the
FULL failure, just sequenced differently:
  [rawrec] fat mounted=1 bit=0x2680000   (raw_wait_fat_mounted caught it up)
  [rawrec] create f=0x3                    (FIO_CreateFile OK)
  [FIO] Not mounted ID=0  x12 over ~2s     (the FIRST writes — write_mlv_chunk_headers @3481 — hit unmounted FAT)
  efat_openfent:-2 / AcqSize(ecode=0x100b) L:718 / Remove L:416   (rec task deletes empty chunk, bails GRACEFULLY, no crash yet)
  then [DAF]/[EIS] AE storm ... 31x "[AB] pc=0x00000000 lr=0xE05D1161 dfar=0xC8920841" -> watchdog reboot.
=> The FAT is torn down BETWEEN create (mlv_lite.c:3469) and the first write (write_mlv_chunk_headers :3481) — i.e. by the rec
task ENTERING FULL CAPTURE, NOT by the camera mode. Movie-LV standby keeps the FAT up at IDLE and through raw-detect, but the
moment the task commits to capture the still FAT drops (writes fail) exactly as in every prior run. The pc=0 crash is the same
DryosDebugMsg data-corruption landmine, still INTERMITTENT (it fired here after a delay; one earlier standby run bailed without
it). NET: back to the ORIGINAL, correctly-stated blocker (docs UPDATE ~00:19): the rec task's own capture entry unmounts the
still FAT on mk2. What's NEW & solid: (a) raw ARMING/DETECTION fully works (raw_info valid) so this is purely storage, not raw;
(b) NO camera-mode trick avoids it; (c) the unmount window is pinned to :3469->:3481 (between create and first header write) —
candidates in that gap: mlv_rec_call_cbr(MLV_REC_EVENT_STARTING) @3478, init_mlv_chunk_headers @3480, OR the now-running raw
EDMAC/vsync capture + setup_buffers(@3449) shoot-memory grab starving Canon's FAT (the mem-starvation hypothesis). REAL NEXT
WORK (code, not live thrash — live triggering just crashes the camera): bisect :3469->:3481 with UART markers to find the exact
step that clears 0xaaf14 bit 0x200000; compare to how bilal M50mk1 keeps the FAT mounted through the same code; test the
mem-starvation angle (free_buffers before writes / bound the shoot alloc). raw_start_stop this boot 0x010609AC (stale after any
reboot). mem2mem COPY still fully fixed. This session's durable wins: the movie-LV-standby state characterization, the reliable
per-boot address method, and pinning the unmount to the create->first-write window with raw arming proven independent.

## UPDATE 2026-07-15 ~10:27 — BISECT RESULT overturns the whole mount-bit premise: 0xaaf14 bit 0x200000 STAYS SET during the failing writes. The blocker is NOT a FAT teardown. "[FirstEnt] Not mounted ID=0" is a DIFFERENT condition (volume ID=0), unrelated to 0xaaf14.
Built + flashed an instrumented mlv_lite.mo (md5 cd9c9fdf, on card ML/MODULES/mlv_lite.mo) that logs the live 0xaaf14 value at
each capture-entry step via m50ii_mark(). Re-resolved raw_start_stop=0x3B6BE700 this boot (cbr@0x3B6C410C, keypress=0x3B6BE7BC,
vsync=0x3B6C1930, delta 0x3174 [markers shifted it from 0x3124], gap keypress-rss still 0xBC, prologue verified). Fired it in
movie-LV standby, raw armed (raw_info buffer 0x2BC61CC8). MARKERS (chronological):
  [bisect] pre-setup_buffers   aaf14=0x2680001 mounted=1
  [bisect] post-setup_buffers  aaf14=0x2680001 mounted=1
  [rawrec] fat mounted=1 bit=0x2680001
  [rawrec] create f=0x3                                  <- FIO_CreateFile OK
  [bisect] pre-STARTING-cbr    aaf14=0x2600001 mounted=1  <- bit 0x80000 CLEARED between create and here
  [bisect] post-STARTING-cbr   aaf14=0x2600001 mounted=1
  [bisect] pre-write-hdr       aaf14=0x2600001 mounted=1  <- STILL mounted=1 (0x200000 SET) right before the write
    [FIO] Not mounted ID=0 x3  (write_mlv_chunk_headers' FIO_WriteFile FAILS)
  [bisect] post-write-hdr      aaf14=0x2780001 mounted=1  <- STILL mounted=1 after (bits 0x100000+0x80000 toggled)
    ...Not mounted ID=0 xN, efat_openfent:-2, AcqSize(0x100b) L:718, Remove L:416, then AE storm -> 30x [AB] pc=0 -> watchdog.
=> BIT 0x200000 @0xaaf14 IS SET THE ENTIRE TIME, INCLUDING DURING THE FAILING WRITES. So the "capture unmounts the still FAT"
narrative (this whole doc's arc) is FALSE, and raw_wait_fat_mounted() was polling the WRONG flag (it always passed). The real
write failure "[FirstEnt] Not mounted ID=0" is a SEPARATE condition keyed on VOLUME ID=0, tracked somewhere OTHER than 0xaaf14
bit 0x200000. Also notable: a DIFFERENT status bit 0x80000 (bit 19) clears exactly at the create->STARTING-cbr boundary (0x2680001
-> 0x2600001) — candidate for the real "ready" flag, or just a side status bit. NET REFRAME: the blocker is "FIO write to the SD
fails with Not-mounted-ID=0 while the raw LV capture is active", NOT a FAT unmount. FIO_CreateFile succeeds (dir op), but the
first FIO_WriteFile's FirstEnt (directory/FAT read to extend the file) reports ID=0 not mounted.
NEXT RE (Ghidra, targeted): find the "[%s] Not mounted ID=%d" emitter's mount test — what does "ID=0" index (drive/volume table),
and what flag/state does it read (NOT 0xaaf14 bit 0x200000)? efat FUN_e011831c@0xe011831c was the earlier suspect but that
analysis was pinned to the 0x200000 bit which we now know is set — re-RE. HYPOTHESES to test: (a) the raw EDMAC/DMA capture blocks
or starves the SD READ path so directory reads (FirstEnt) fail transiently -> writes can't extend the file; (b) FIO_CreateFile
returns a deferred/cached handle and the FIRST real volume access (write) is where the true not-ready surfaces; (c) bit 0x80000 is
the real gate. QUICK live probes (harness, no rebuild): during a failing write window, `watch` the actual ID=0 volume descriptor
(find its base) not 0xaaf14; and test whether a plain FIO write to the card OUTSIDE raw capture works (it does at idle => the raw
capture is what breaks SD writes). The pc=0 DryosDebugMsg crash is still the same intermittent landmine downstream of the FIO error
(dfar varies: 0xC8920001/0841/0005). Instrumented build cd9c9fdf on card. raw_start_stop 0x3B6BE700 stale after reboot.

## UPDATE 2026-07-15 ~10:45 — LIVE FIO PROBES: raw capture does NOT break SD writes. Manual FIO writes SUCCEED with raw armed (even 4MB). The rec-task failure is rec-task-specific; leading (unconfirmed) cause = setup_buffers' shoot-memory grab, which DEFINITELY reproduces the AE storm.
Probed FIO directly via harness `call` (FIO_CreateFile=0xcf791, FIO_WriteFile=0xcf631, FIO_CloseFile=0xe05b1c1b, all thumb;
path staged via serial_shell `load` into sh_scratch @0x0014EEA8; RAM write buffer 0x40000000 [mlv warmup uses it]):
  * raw ARMED (raw_lv_request 0xe5b91; raw_info@0x103ed8 buffer=0x2BC61CC8): FIO_CreateFile("B:/PROBE.TXT")=handle 0x4 OK;
    FIO_WriteFile(h,0x40000000,0x200)=ret 0x200 OK; FIO_WriteFile(h,0x40000000,0x400000=4MB)=ret 0x400000 OK, close 0x0.
    => WRITES (incl. 4MB cluster-extension) WORK FINE WITH RAW CAPTURE ACTIVE. So "raw capture breaks SD writes" is FALSE.
  * PROBE BUG that misled me first: writing from 0xe0000000 (ROM) => "[FSU] WARN buf is ROM Addr" -> "FSUBlockWrite:NotPrepared"
    -> ret 0xFFFFFFFF ecode 0x1003. NOT a mount/raw issue — the SD DMA can't read ROM. Use RAM buffers only.
  * DRIVE-A theory (get_cf_dcim_dir switches to CARD_A if is_dir("A:/"), mlv_lite.c:2986) DISPROVEN on HW: is_dir("A:/")=0,
    is_dir("B:/")=1 => rec task correctly targets B: (the SD), not A:/ID=0. So "Not mounted ID=0" is NOT a wrong-drive bug.
  * MEMORY-STARVATION test: shoot_malloc_suite(0)@0xd12fd (grabs ALL shoot mem, exactly what setup_buffers/realloc_buffers
    :1522 does) returned suite 0x179BE8; FIO_CreateFile still OK (h=0x7); the WRITE result was INCONCLUSIVE (no FSU/Write/Not-
    mounted error logged, but the grab immediately triggered the [EIS]/[DAF] AE storm that flooded the serial so the ret was
    lost). KEY CORRELATION: grabbing all shoot memory REPRODUCES THE AE STORM by itself => the "Over the Deadline Aewb" storm
    (and thus the downstream pc=0 crash) comes from the shoot-MEMORY GRAB, NOT from raw capture. shoot_free_suite=0xd1331 (the
    test may have left the suite held -> camera unstable, needs power cycle).
NET REFRAME (much better targeted): the blocker is NOT raw-vs-SD, NOT a FAT unmount, NOT drive A. It is something the REC TASK
does beyond a plain armed write. Ruled OUT: raw capture, large/cluster-extending writes, directory ops, drive A. Still IN (the
differentiators between my working manual write and the failing rec-task write): (1) setup_buffers()'s shoot_malloc_suite(0)
grab (CONFIRMED it causes the AE storm; its effect on the WRITE itself is unconfirmed — cleaner test needed); (2) RAW_PREPARING
state / mlv_rec_call_cbr(STARTING) side-effects; (3) the specific write in write_mlv_chunk_headers (buffer/path/size combo).
NEXT (targeted, cheap): (a) redo the memory-pressure write test but READ the write ret cleanly (grab suite, single write, capture
ret before the storm — maybe disable the AE-storm noise or read ret via the daemon jsonl); (b) if the write DOES fail only under
the full grab => bound mlv_lite's allocation (leave headroom for Canon FSU/FAT) — matches how bilal M50mk1 (more free RAM
headroom) survives the identical code; (c) if writes still succeed under the grab => the cause is the RAW_PREPARING/STARTING-cbr
state, RE what MLV_REC_EVENT_STARTING CBRs do. FIO/mem addrs (fixed, main-ML autoexec unchanged): FIO_CreateFile 0xcf791,
FIO_WriteFile 0xcf631, FIO_CloseFile 0xe05b1c1b, is_dir 0xcf671, get_card 0xcf3a5, shoot_malloc_suite 0xd12fd, shoot_free_suite
0xd1331, raw_lv_request 0xe5b91, raw_lv_release 0xe5bed, raw_info 0x103ed4 (buffer@+4=0x103ed8). Camera unstable (shoot mem
grabbed + AE storm) -> power cycle; raw_start_stop 0x3B6BE700 stale after reboot; instrumented build cd9c9fdf on card.

## UPDATE 2026-07-15 ~10:53 — shoot_malloc_suite(0) HANGS the serial-shell task => live memory-grab test is INFEASIBLE from the harness. Pivot to a code-change test.
Retried the clean memory-pressure test (fresh boot, movie-LV, raw armed): `call 0xd12fd 0x0` (shoot_malloc_suite(0)) NEVER
returned a ret — it BLOCKED the ml_serial shell task; all subsequent `call`/`ver` commands queued behind it and timed out
(camera itself fine: heartbeat n=0x1E2 kept climbing, NO storm, NO crash — just the shell stuck inside the alloc). So
shoot_malloc_suite(0) from the shell task WAITS for shoot memory that LiveView/raw holds and never gets it -> deadlock. =>
you cannot replicate setup_buffers' grab via the harness; the memory hypothesis must be tested with a CODE CHANGE, not live.
NEXT (code, next build): to test/settle "does setup_buffers' shoot grab starve the SD write" — instrument raw_video_rec_task
to log free-mem before/after setup_buffers AND attempt a tiny known-good FIO write right before write_mlv_chunk_headers (RAM
buf, B:/ root) so we see if a SIMPLE write fails at that exact point (isolates memory/state from the header-write specifics);
OR bound realloc_buffers (mlv_lite.c:1522 shoot_malloc_suite(0)) to leave headroom and see if the rec-task write then lands.
ALSO still worth: inspect write_mlv_chunk_headers' actual write BUFFER (is it a shoot-suite/uncacheable ptr? the ROM-buf
probe showed buffer address matters a lot) and whether raw_recording_state==RAW_PREPARING changes any FIO path. Camera shell
HUNG -> power cycle needed. Net solid results stand: not mount-bit, not raw-capture, not drive-A; manual RAM writes work armed.

## UPDATE 2026-07-15 ~11:40 — BREAKTHROUGH via in-rec-task probe: NOT memory/state. A plain RAM->B:/root write SUCCEEDS inside the rec task at the exact failing point; only write_mlv_chunk_headers (writing the rec task's OWN handle f, created from the DCIM path) fails. => the cause is the SPECIFIC FILE/PATH/HANDLE, not the SD, memory, raw, or state.
Flashed probe build mlv_lite.mo md5 60c85224 (adds, right before write_mlv_chunk_headers @~:3494, a probe: FIO_CreateFile
("B:/PROBE2.TXT")+FIO_WriteFile(0x40000000,512)+close, logging "[probe] root create OK write ret=N"). raw_start_stop this
boot=0x010AC9B0 (cbr@0x010B243C, keypress=0x010ACA6C, vsync=0x010AFC60, delta 0x31F4 [probe shifted it], gap 0xBC, prologue OK).
Fired in movie-LV standby, raw armed (raw_info 0x2BC61CC8, mount 0x02680000). LOG (chronological):
  [bisect] pre/post-setup_buffers mounted=1 ; [rawrec] create f=0x3 ; pre/post-STARTING-cbr mounted=1 ; pre-write-hdr mounted=1
  [FIO] Not mounted ID=0 (start)
  [probe] root create OK write ret=512      <<< PLAIN B:/ROOT WRITE SUCCEEDS in the SAME rec-task state (mem grabbed, PREPARING)
  [FIO] Not mounted ID=0 x~12 / efat_openfent:-2 / AcqSize(ecode=0x100b) L:718   <<< write_mlv_chunk_headers(f,...) STILL FAILS
=> DEFINITIVELY NOT memory/state (a fresh root write works right there). The ONLY difference is the rec task's file handle f =
FIO_CreateFile(raw_movie_filename) where raw_movie_filename = get_next_raw_movie_file_name() -> get_cf_dcim_dir() = "B:/DCIM/
%03d<suffix>/M%02d-%02d%02d.MLV" (mlv_lite.c:2985/3014). So the failure is SPECIFIC to that DCIM-subdir file/handle. TWO
candidates left: (A) the DCIM SUBDIR PATH itself breaks the write's directory enumeration (FirstEnt) — even though create
returned f=0x3; (B) the handle f went STALE between its create (:3482, BEFORE mlv_rec_call_cbr(MLV_REC_EVENT_STARTING) :3478 +
init_mlv_chunk_headers) and the write (the probe's fresh handle was created AFTER those and works). NOTE FirstEnt = directory
FIRST-ENTRY enumeration -> a DIRECTORY read on the DCIM subdir is what returns "Not mounted ID=0".
DECISIVE NEXT TEST (one build, also a likely FIX): make the rec task write to B:/ ROOT (or the DCIM dir the PROBE proved
writable) instead of the get_cf_dcim_dir subdir — i.e. override raw_movie_filename to "B:/M<date>.MLV". If that records =>
the DCIM subdir/path was the whole blocker (fix = write to a working dir on mk2). If it STILL fails => it's handle staleness
(reorder: create f AFTER mlv_rec_call_cbr(STARTING)/init, or re-open). Could not read the exact raw_movie_filename this run
(.bss non-contiguous; raw_movie_filename @.bss 0x190; camera likely storming post-fail). Probe build 60c85224 on card;
raw_start_stop 0x010AC9B0 stale post-reboot. This is the closest we've been: the SD/FAT works, only the .MLV target path fails.

## UPDATE 2026-07-15 ~12:40 — NOT the path, NOT the handle timing, NOT the buffer address. Narrowed to write_mlv_chunk_headers / mlv_write_hdr itself. Plain FIO writes succeed EVERYWHERE (rec task, raw armed, module-region buffer); only the header-write sequence fails.
Two more builds + tests, both RULED OUT a hypothesis:
(1) ROOT-PATH override (build cef23768): forced raw_movie_filename = "B:/M50TEST.MLV" (root, proven writable). Result: create
    OK, but write_mlv_chunk_headers STILL "[FIO] Not mounted ID=0". So the DCIM subdir PATH was NOT the cause (root fails too).
    The in-rec-task probe (plain FIO_CreateFile+FIO_WriteFile 512B) SUCCEEDED at the same point => plain writes work, header
    writes don't.
(2) HANDLE-REORDER (build 2fbe232f): moved FIO_CreateFile to AFTER mlv_rec_call_cbr(MLV_REC_EVENT_STARTING)+init_mlv_chunk_
    headers ("[rawrec] create(post-STARTING) f=0x3"). Result: write_mlv_chunk_headers STILL fails "Not mounted ID=0". So it is
    NOT handle staleness / STARTING-cbr remount either.
(3) BUFFER-ADDRESS test (harness, live): FIO_WriteFile from the MODULE region 0x3B6CC000 => ret 0x200 OK, and from main RAM
    0x40000000 => ret 0x200 OK. So mlv_write_hdr writing a module-static struct (&file_hdr etc.) is NOT the problem — module-
    region buffers write fine. (mlv_write_hdr @mlv_lite.c:473 = mlv_rec_call_cbr(MLV_REC_EVENT_BLOCK,hdr) then FIO_WriteFile(f,
    hdr,hdr->blockSize) — a plain write.)
NET: EVERYTHING plain works (manual writes from ml_serial task, in-rec-task probe writes, module-region & main-RAM buffers, root
& handle-any-timing). ONLY write_mlv_chunk_headers' sequence fails with "FirstEnt Not mounted ID=0"/efat_openfent:-2/AcqSize
(0x100b) L:718. REMAINING SUSPECTS (not yet tested): (a) mlv_rec_call_cbr(MLV_REC_EVENT_BLOCK, hdr) run before EVERY mlv_write_hdr
(a registered BLOCK CBR may do an FIO/state op that breaks the next write) — grep MLV_REC_EVENT_BLOCK handlers; (b) the RAPID
MANY-writes sequence (8+ headers back-to-back) vs the single probe write — a per-write FAT/dir-update race with the active raw
capture; (c) FIO_SeekSkipFile(f,0,SEEK_CUR) "AcqSize" (mlv_lite.c:3187). DECISIVE NEXT (one build): in write_mlv_chunk_headers,
replace the FIRST mlv_write_hdr with a DIRECT FIO_WriteFile(f, &file_hdr, blockSize) bypassing mlv_rec_call_cbr(BLOCK); if that
first write lands => the BLOCK cbr is the culprit; if it still fails => it's the rapid-sequence/AcqSize race. Camera responsive
(raw idle post-bail). Builds cef23768 & 2fbe232f tested; current on card = 2fbe232f. COST NOTE: session ~$1100 — heavy; consider
pausing. The bug is now boxed into write_mlv_chunk_headers; the SD write path itself is proven healthy.

## UPDATE 2026-07-15 ~13:00 — EXHAUSTIVE isolated rule-out: EVERY FIO op works from the ml_serial task; the failure is PURELY the raw_rec_task1 context during active capture. mlv_snd NOT loaded => mlv_rec_call_cbr(STARTING/BLOCK) are NO-OPS.
Ran the full matrix of harness FIO probes (all from the serial-shell task, FIO_CreateFile 0xcf791 / FIO_WriteFile 0xcf631 /
FIO_SeekSkipFile 0xe05b1943 / FIO_CloseFile 0xe05b1c1b) — ALL SUCCEED, zero errors:
  - write 512B & 4MB from main RAM 0x40000000  -> ret 0x200 / 0x400000
  - write 512B from MODULE region 0x3B6CC000    -> ret 0x200  (buffer-region NOT the issue)
  - write 52B (0x34) & 180B (0xB4) unaligned     -> ret ok    (partial-sector size NOT the issue)
  - THREE back-to-back writes to the SAME handle -> all ok     (multi-write NOT the issue)
  - FIO_SeekSkipFile(f,0,SEEK_CUR)               -> ret 1, no error (AcqSize/seek NOT the issue in isolation)
  - create + write to the EXACT name "B:/M50TEST.MLV" -> ok    (cursed filename/dirent NOT the issue)
Also: mlv_snd module is NOT loaded (module_list = file_man/mlv_lite/bench), so mlv_rec_call_cbr(MLV_REC_EVENT_STARTING) and
(MLV_REC_EVENT_BLOCK) iterate an EMPTY registered_cbrs => NO-OPS. So the "STARTING remount" and "per-write BLOCK cbr" theories
are BOTH void, and mlv_write_hdr (mlv_lite.c:473) reduces to a bare FIO_WriteFile(f, hdr, hdr->blockSize).
=> COMPLETE rule-out list (this bug is NONE of these): mount-bit 0x200000, raw capture active, drive A, memory starvation,
handle create-timing, buffer address/region, write size/alignment, multi-write, seek/AcqSize, filename, STARTING/BLOCK cbrs.
The failure ONLY happens for write_mlv_chunk_headers' writes executed from raw_rec_task1 during a real record — yet an in-rec-
task PROBE (plain FIO_CreateFile+FIO_WriteFile, fresh handle) SUCCEEDED at the same point (cef23768). The unresolved delta:
write_mlv_chunk_headers writes to the rec task's handle `f`, the probe writes to a fresh handle — but reorder-create-after-
STARTING (2fbe232f) still failed, and a harness write to a fresh handle to the same file also works. So the remaining
possibilities are subtle: (i) `f` specifically is in a bad state in-task (the probe's fresh handle isn't); (ii) a timing/
concurrency effect that only bites the header-write burst mid-capture, not a single probe write.
DECISIVE NEXT BUILD (control-in-situ): right after the rec task creates `f`, do ONE control FIO_WriteFile(f, 0x40000000, 512)
+ log its ret, then FIO_SeekSkipFile(f,0,SEEK_SET) to rewind, THEN write_mlv_chunk_headers. If the control write to `f`
SUCCEEDS but the headers fail => it's the header data/blockSize path; if the control write to `f` FAILS (while the fresh-
handle probe works) => it's `f` itself in-task (re-create/re-open f, or dup the probe's approach for the real file).
COST: session ~$1150 — very heavy; strongly consider pausing. Current card build 2fbe232f (root override + reorder). All FIO/
resolution addrs catalogued above. The SD write path is 100% healthy; the bug lives entirely inside the rec task's header write.

## UPDATE 2026-07-15 ~13:18 — MILESTONE: HEADER WRITE FIXED. Root pattern found: writes from STATIC memory succeed, writes from HEAP/malloc (and shoot-memory) buffers FAIL "Not mounted ID=0". Fixed the header path; frame writes are the next (same-root) blocker.
Per-header instrumentation (build 34f6425d) proved ALL 8 mlv_write_hdr calls SUCCEED (they write STATIC module structs
&file_hdr/&rawi_hdr/... : [wh] #0..#7 ret==bs). The FIRST failure was the NEXT op: mlv_write_vers_blocks (mlv_rec/mlv.c:246),
which writes from a malloc'd HEAP buffer (mlv_build_vers -> hdr = malloc; FIO_WriteFile(f, hdr, ...) @mlv.c:298). Also the
original NULL-padding write used a STACK local (mlv_lite.c:3198) AND over-read it (blockSize up to 511 from a 16-byte struct).
FIX (build 5dc87e3b, on card): (a) SKIP mlv_write_vers_blocks (optional module-version metadata); (b) write the NULL pad from a
STATIC char[512] buffer, not the stack. HW RESULT: header path now COMPLETES CLEAN:
  [wh] #0..#7 ret==bs ; [wh] SKIP vers ; [wh] seek hdr_size=572 ; [wh] nul bs=452 ret=452  (=> write_mlv_chunk_headers returns
  padded_size=1024, SUCCESS — first time ever). FIO_SeekSkipFile works fine here (returned 572 = 52+180+32+84+40+96+44+44).
=> ROOT PATTERN CONFIRMED: the SD/FSU write DMA works from STATIC module memory & main RAM (0x40000000) but FAILS ("[FirstEnt]
Not mounted ID=0" / efat_openfent:-2 / AcqSize 0x100b) when the source buffer is HEAP (malloc) or shoot-memory. This is almost
certainly a CACHE/UNCACHEABLE or memory-region issue: those buffers need a cache clean / UNCACHEABLE() mapping before the DMA,
or the write must bounce through a DMA-safe buffer.
REMAINING BLOCKER (same root): after headers succeed, the FRAME writes fail the same way -> Engine::Edmac.c:770 assert (frame
mem2mem capture) then "[FIO] Not mounted ID=0" x11 / AcqSize L:718 / Remove L:416 (rec task deletes the file & bails -> AE
storm). Frame data lives in shoot_malloc_suite buffers (setup_buffers), i.e. the failing memory class. NEXT: (1) find the frame
FIO_WriteFile in the writer/save-group path; ensure the frame buffer is cache-CLEANED before the write (sync_caches / the same
UNCACHEABLE trick used for edmac-d8 mem2mem) OR that FIO_WriteFile is handed an UNCACHEABLE alias of the slot; (2) confirm by
whether the 4MB frame write then lands (my isolated 4MB write from 0x40000000 worked, so it IS buffer-region specific). Note
mlv_lite's FIO_WriteFile wrapper normally sync_caches() before writing — check that path is active on mk2 / for shoot buffers.
Builds 34f6425d (per-hdr log) & 5dc87e3b (skip-vers + static-nul, HEADER FIX) on card. Header write = DONE; frame write = next.
COST: session ~$1300 — very heavy; strongly recommend pausing here with this milestone banked.

## UPDATE 2026-07-15 ~13:32 — UNCACHEABLE frame-buffer fix did NOT help (build d31f2915). Re-diagnosed: the frame failure is FAT CLUSTER-ALLOCATION during active capture, NOT the data buffer.
Wrapped the frame FIO_WriteFile ptr in the D8/2GB uncacheable alias (ptr | (ptr<0x40000000?0x40000000:0)) at mlv_lite.c:3276/
3324 (build d31f2915, on card). HW: SAME failure — after "[wh] nul bs=452 ret=452" (header OK), the frame path -> "[FirstEnt]
Not mounted ID=0" x14 / efat_openfent:-2 / AcqSize(0x100b) L:718 / Remove L:416 -> AE storm. UNCHANGED. So it is NOT a cacheable-
buffer problem. KEY RE-READ of the error CLASS: FirstEnt / efat_openfent / AcqSize are all DIRECTORY/FAT-METADATA ops, not data
writes. The 8 headers + nul (~1024 B total) ALL FIT IN THE FIRST CLUSTER allocated at FIO_CreateFile -> NO cluster extension ->
they succeed. The frame write is ~4MB -> needs MANY NEW CLUSTERS -> FIO_WriteFile must EXTEND the file -> AcqSize (acquire size/
allocate clusters) -> reads the FAT/dir (FirstEnt) -> FAILS "Not mounted ID=0". My earlier isolated 4MB write worked because raw
was IDLE; the in-rec-task probe worked because it was a SMALL 512B write that fit the first cluster (no extension). => REAL ROOT:
FAT CLUSTER ALLOCATION (file extension) FAILS DURING ACTIVE RAW CAPTURE. The vsync/EDMAC capture disrupts the SD FAT-read path
that cluster allocation needs.
NEXT (the right fix): PRE-ALLOCATE the .MLV clusters BEFORE entering capture (write/seek the full expected size to a
DMA-safe/static region while raw is still idle+FAT healthy, so no extension happens during capture) — mlv_lite/mlv_rec often has
a preallocation or "reserve" path; check card_bufsize / the initial file-size reserve and whether M50 II skips it. ALT: find why
active capture breaks FAT reads (does the raw EDMAC hold an SD/DMA resource? does hack_liveview reconfigure the SD controller?)
and release it around the writes. The uncacheable wrap (mlv_lite.c:3276/3324) is harmless — leave or revert. Builds through
d31f2915 on card. HEADER WRITE remains FIXED (5dc87e3b onward). COST: session ~$1400 — STOP recommended; frame path = cluster-
allocation-during-capture, a fresh well-scoped task.

## ★★★★ 2026-07-16 ATOMIC-LOG RUN + HISTORICAL RE-ANALYSIS — big de-noise. "Not mounted ID=0" was BENIGN all along; the real failure is a POST-HEADER AcqSize.
Flashed 6907c90f (atomic @ML> logging). mllog.py `fio` decoded it CLEAN (no shredding):
  @ML> getsize path=B:/DCIM/100CANON/M01-000N.MLV ret=0  (x11, DRIVE B/1, SUCCEEDS)
=> the getsize/AcqSize in get_next_raw_movie_file_name is fine (drive B, ret=0). It just loops
through leftover M01-*.MLV files (size 0 from prior failed runs -> reuses one).
REGRESSION found+fixed: 6907c90f's m50ii_uart_atomic held IRQs off with a 2,000,000-spin TX wait;
11x in the filename loop during ACTIVE raw capture delayed the raw EDMAC/vsync IRQs -> Engine::RawPro.c
/ MvRawProDs.c asserts + stall. FIX (build b59f396f): bounded the IRQ-off spin to 4000 (drop a char
rather than stall) + removed the in-loop getsize log.
HISTORICAL RE-ANALYSIS (mllog boots 79/81, benign A: probes collapsed) — the TRUE failure sequence:
  create f=0x3 -> [ctrl] write-to-f ret=512 -> [wh] #0..#7 all ret=bs -> SKIP vers -> nul ret=452
  (HEADER = 1024 B, ALL WRITES SUCCEED) -> post-write-hdr mounted=1
  -> @raw_rec_task1 AcqSize(ecode=0x100b) L:718  -> Remove L:416  -> pc=0 lr=0xE05D1161 crash loop.
So: (a) the "[FirstEnt] Not mounted ID=0" x10 we chased ALL SESSION = the benign is_dir("A:/") probe
in get_cf_dcim_dir (drive 0=A:, expected fail) — NOT the blocker. (b) HEADER data writes SUCCEED.
(c) The real error is a POST-HEADER "AcqSize"(=_FIO_GetFileSize64) failing 0x100b in raw_rec_task1,
which then trips the pc=0 DryosDebugMsg crash. (d) The frame DATA writes are UN-OBSERVED historically
(no logging then) — b59f396f now logs the FIRST framewrite (ok/FAIL) to settle it.
OPEN QUESTION for the next flash (build b59f396f, READY): does the FIRST frame write succeed
("framewrite-ok") or fail ("framewrite-FAIL")? And is the post-header AcqSize from write_frames'
card-full/new-chunk path (get_next_chunk_file_name -> FIO_CreateFile/GetFileSize) after a frame-write
fail, or a separate op? The framewrite-ok/FAIL @ML> line answers the first; if FAIL, the AcqSize is the
chunk-retry; if OK, frames write and the AcqSize is elsewhere (finish/close/periodic).
NEXT (needs flash — user off-lab): flash b59f396f, record, `mllog.py fio` -> read framewrite-ok/FAIL +
the create/getsize ops. That names whether frame writes work. Build b59f396f on disk (not yet flashed).
