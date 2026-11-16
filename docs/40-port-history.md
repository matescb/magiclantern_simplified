# 40 - Port history: what was changed to run Magic Lantern on the EOS M50 Mark II, and why

Written by agent D6 during the offline sprint of 2026-11-16. No camera was attached. Every statement
about behaviour on the camera is quoted from earlier project documents, commit messages or memory notes
(July 2026); none of it was re-run. The only things executed for this document were `git` queries,
static reading of the source at the named commits, one ROM disassembly (`tools/romdis.py`), and three
host builds of other camera platforms from exported copies of the tree (section 6).

## 0. Conventions and scope

- "ML" is Magic Lantern. "The port" is the set of 15 commits `b9d9a9557..a10b16c23` in
  `/mnt/data/WORK/eos/magiclantern_simplified`. `b9d9a9557` is the upstream base (`origin/dev`,
  "raw_vidx: define some standard modes for testing"). `a10b16c23` is the tip of the local branch
  `walkback-milestone`.
- A reference such as `7ddf3b400 src/init.c +612..+648` means: in the version of the file as it exists
  in commit `7ddf3b400`, lines 612 to 648. For the two squashed commits these line ranges are the added
  hunks reported by `git show --unified=0`. A reference such as `HEAD src/reboot.c:403` means the file at
  `walkback-milestone`.
- ROM addresses refer to `m50ii_dumper/dump_1.0.1/gang100.bin`, loaded at 0xE0000000, Thumb code.
- **Observed** = read directly in a commit, a file or a tool output during this sprint.
  **Inferred** = a conclusion drawn from observed facts; it can be wrong.
  **Unknown** = not determinable from the material.
  **HW (historic)** = a hardware result claimed by a July 2026 document; not re-checked.
- Terms. *DryOS* is Canon's real-time operating system. *cstart* is Canon's early C start-up function
  (ROM 0xE0040104, docs/13). *init task* is the first DryOS task. *Stub* is a line in
  `platform/M50mk2.101/stubs.S` that binds an ML function name to a ROM address. *OSD* is ML's on-screen
  drawing. *EDMAC* is Canon's DMA engine used to move image data. *MLV* is ML's raw video file format.
  *UART* is the camera's serial debug port.

## 1. The 15 commits at a glance

All commits are authored 2026-07-12 to 2026-07-21 (Observed, `git log --date=short`).

| # | Commit | Date | Title (shortened) | Size | Pushed to fork? |
|---|---|---|---|---|---|
| 1 | `b11c53abc` | 07-12 | add M50 Mark II platform (K468, DIGIC 8), fw 1.0.1 | 20 files, +1353 | yes |
| 2 | `7ddf3b400` | 07-12 | boot, display/OSD, menu-input support in shared ML code | 15 files, +3440 | yes |
| 3 | `1d1cc7e54` | 07-12 | keeper build: gate menu-input diagnostics off | 3 files | yes |
| 4 | `8f25664d6` | 07-12 | rbf_font: round FIO read buffer up to sector size | 1 file | yes |
| 5 | `730923693` | 07-12 | Revert of #4 | 1 file | yes |
| 6 | `bb10c937b` | 07-12 | mem: round DMA allocations up to sector size | 1 file, +9 | yes (fork tip) |
| 7 | `ecd2f853c` | 07-12 | raw-video R0+R1: modules, D8 EDMAC engine, VIEW toggle | 9 files, +366 | **no** |
| 8 | `d9614bca4` | 07-14 | font-DMA overflow, picstyle prop spam, raw-LV groundwork | 10 files | **no** |
| 9 | `49b89e783` | 07-14 | duplex UART serial debug harness | 4 files, +589 | **no** |
| 10 | `d9936e077` | 07-18 | raw DNG capture, FIO uncacheable write fix, menu input | 20 files, +787 | **no** |
| 11 | `6ff442ba3` | 07-18 | mlv_lite frame-write reconciliation, REC button binding | 3 files | **no** |
| 12 | `31442ed9a` | 07-18 | runtime flag to skip the EDMAC engine resource lock | 1 file | **no** |
| 13 | `298749a72` | 07-18 | CPU rectangle copy for raw frames | 1 file | **no** |
| 14 | `3ccf499dc` | 07-18 | async worker for the CPU frame copy | 1 file | **no** |
| 15 | `a10b16c23` | 07-21 | walkback: unchanged core + module-only raw fixes | 5 files | **no** |

### 1.1 What exists only on this machine (Observed)

- The remote `fork` (`https://github.com/matescb/magiclantern_simplified.git`) has one port branch,
  `fork/m50mk2-port`, and it stops at `bb10c937b` (commit 6). `git branch -r --contains ecd2f853c`
  returns nothing.
- Commits 7 to 14 (`ecd2f853c` .. `3ccf499dc`) exist on the local branches `m50mk2-port` and
  `walkback-milestone` only.
- Commit 15 (`a10b16c23`) exists on `walkback-milestone` only. That branch has no upstream.
- `fa0834b40` ("checkpoint boot-stability + raw_vidx work (WIP, does not boot)", 2026-07-21) exists on
  local `m50mk2-port` only; `git branch -vv` shows that branch as "ahead 9" of the fork. It is *not* an
  ancestor of `walkback-milestone` (merge base `3ccf499dc`). It is the only commit that contains the
  WiFi deploy module `modules/wifidep/` (docs/36, docs/37); `git ls-tree -r walkback-milestone` has no
  such path. So the WiFi lane described in docs/37 is not part of the 15 commits.
- Everything raw-video related, the serial harness and the FIO write fix are therefore unpublished and
  have no off-machine copy. This is a data-loss risk, independent of any technical finding.

## 2. Why the first two commits need reconstruction

Commits 1 and 2 were both authored on 2026-07-12 at 14:49 and 14:50. Together they add 4,793 lines.
They are a squash of the bring-up work that docs/01 to docs/28 date between 2026-07-05 and 2026-07-12.
The squash has three consequences a reader must know (all Observed):

1. **Fixes are visible only as final state.** For example the wrong `memcpy_dryos` stub never appears as
   a changed line; `b11c53abc platform/M50mk2.101/stubs.S:127` already carries the corrected address with
   a comment naming the old one.
2. **Most of commit 2 is diagnostic scaffolding, not port logic.** `src/boot-d678.c` grows by 1,276
   lines, but the final boot design needs none of them except one statement and one UART writer
   (section 3.1, 3.2). About 1,100 of those lines sit under `M50II_TRACE_*` macros that are never
   defined (`7ddf3b400 src/boot-d678.c +132..+605`, `+620..+869`, `+871..+992`, `+1325..+1651`); the only
   one defined is `M50II_TRACE_CREATE_LADDER` at line 14.
3. **The commit message claim "All changes gated behind CONFIG_M50mk2 / M50II_* flags" is false.**
   Section 6 lists the ungated code and shows that two other cameras no longer compile.

## 3. The actual sequence of fixes inside commits 1 and 2

Order and dates are taken from the documents; where two sources disagree it is stated. For each fix:
symptom, cause, change, where it lives, and the document that holds the reasoning.

### 3.0 Preliminaries: ROM dump, skeleton, stub campaign (2026-07-05 to 07-07)

The ROM of firmware 1.0.1 was dumped with a Canon Basic script (docs/02). `platform/M50mk2.101` started
as a copy of `platform/M50.110`, the original M50 (docs/03). Because the two cameras run different
firmware, every address had to be found again. docs/06 (81 targets), docs/11, docs/12 and docs/16
(`tools/verify_stubs.py`) record that work.

- Implemented by: all of `b11c53abc` under `platform/M50mk2.101/` (stubs.S 384 lines, consts.h 199 lines,
  Makefile with `FIRMWARE_ID = 0x80000468` and `RESTARTSTART = 0xC78E0`, gui.h, internals.h, features.h,
  platform headers), plus `7ddf3b400 src/fw-signature.h +45` (firmware signature constant).
- Caveat (Observed in this sprint's reports): the audit reports A14a/b/c found stubs that are still blind
  copies from the M50, for example `task_max` and `_request_RPC` (docs/offline/A14a.md, Summary). The
  header comment of `consts.h` at line 2 still reads "EOS M50 1.1.0 consts".

### 3.1 Boot handoff: "wrap-and-swap" of create_init_task (2026-07-07, hardware 2026-07-10)

- Symptom: `AUTOEXEC.BIN` was loaded but ML's init task never ran (docs/08).
- First approach, abandoned: a hand-written mirror of Canon's scheduler hand-off inside
  `src/boot-d678.c`, probed with LED blink ladders (docs/08). docs/10 calls it fragile and replaces it.
- Final design: the stock upstream mechanism. ML copies the first 0x800 bytes of Canon's start-up code
  to RAM, patches two calls in the copy, and runs the copy. The patched call to Canon's
  `create_init_task` lands in `my_create_init_task`, which shrinks Canon's user memory pool to reserve
  room for ML and replaces Canon's init task pointer with ML's `my_init_task` ("wrap" the call, "swap"
  the pointer). docs/20 confirmed with a QEMU watchpoint that Canon stores the init task pointer at
  RAM 0x417C from PC 0xE0166A6C.
- Change that matters: constants only. `b11c53abc platform/M50mk2.101/consts.h:9-21`
  (`BR_DCACHE_CLN_1/2`, `BR_ICACHE_INV_1/2`, `BR_BZERO32 0xE0040152`, `BR_CREATE_ITASK 0xE00401B4`),
  `consts.h:26` (`FIRMWARE_ENTRY_LEN 0x800`), and `Makefile` (`RESTARTSTART`, `ML_BOOT_OBJ = boot-d678.o`).
- In commit 2: `7ddf3b400 src/boot-d678.c +995..+1171` only inserts LED stage markers
  (`M50II_CREATE_STAGE(n)`) into the existing function and adds a switch `M50II_MINIMAL_INIT_TASK`
  (line 1080) for a beacon-only init task; the swap itself (line 1083) is upstream code.
  `7ddf3b400 src/boot.h +34..+136` and the single-line hunks up to `+281` add the same kind of stage
  markers to `my_init_task`.
- Left over: `#define M50II_TRACE_CREATE_LADDER` at `HEAD src/boot-d678.c:14` is still on and is not
  inside any camera guard. Its own comment says "REMOVE this line for a clean/production build".
- Documents: docs/08, docs/10, docs/13, docs/20, docs/23.

### 3.2 BR_BR_CSTART and the kernel data-copy literal (2026-07-07)

- Symptom (QEMU, docs/21, docs/22): boot hung in Canon's data abort handler, fault address 0x40. The
  DryOS kernel globals between 0x4000 and 0x2AD1C held garbage; the lock word at 0x4004 read 0xD3E06F98.
- Cause: Canon's start-up code jumps to cstart through `ldr r0,[pc,#0x3c]` at 0xE00400C0,
  `orr r0,r0,#1` at 0xE00400C2 and `bx r0` at 0xE00400C6 (Observed, `romdis.py E00400BC`). The word
  directly after, at 0xE00400C8, is a literal: 0xE1394358, the ROM source address of the kernel `.data`
  image (Observed, `romdis.py word E00400C8`). ML replaces the jump with a 4-byte branch into the
  relocated cstart. The constant `BR_BR_CSTART` says where. It was first set to 0xE00400C6 (docs/03
  line 26). A 4-byte write at 0xC6 covers 0xC6..0xC9 and therefore the low half of the literal.
- Changes: (a) `BR_BR_CSTART` moved to 0xE00400C0, `b11c53abc platform/M50mk2.101/consts.h:13-19`
  (docs/13 section 5.4). (b) one statement that rewrites the literal in the RAM copy,
  `*(volatile uint32_t *)(RELOCADDR + 0xC8) = 0xE1394358;`, in the hunk
  `7ddf3b400 src/boot-d678.c +1283..+1297` (now `HEAD src/boot-d678.c:1304`), docs/22.
- Inferred (new in this document): fix (b) is redundant once fix (a) is in. docs/22 reports the corrupted
  copy source as 0xE139F821 at the moment 0x4004 is written, so the source started at 0xE139F81D. The
  low half 0xF81D is exactly the second half-word of a Thumb `BL` with offset 0x3A, and 0x3A is the
  distance from 0xC6+4 to cstart's offset 0x104 in the copy. So the build measured in docs/22 was still
  patching at 0xC6. With the patch at 0xC0 the write covers 0xC0..0xC3 and never reaches 0xC8. The
  statement then writes back the value that is already there. This was not tested by removing the line.
- Risk (Observed): the statement is guarded by `#if defined(BR_BR_CSTART)`, not by the camera. Twelve
  other platforms define `BR_BR_CSTART` (for example `platform/RP.160/consts.h:16`,
  `platform/R.180/consts.h:13`). On those cameras the line would write an M50 II ROM address into their
  start-up copy. See section 6.

### 3.3 Verifying the foundations (2026-07-07)

Not a code fix, but it produced corrected constants that are in commit 1: the interrupt handler table
pointer `DRYOS_SGI_HANDLERS_PTR 0x4008` (`consts.h:33`, docs/14), `MALLOC_STRUCT_ADDR 0x2d160`
(`consts.h:36`), the memory map `user_mem 0xC78D8, length 0x112E80` (docs/15), task and semaphore stubs
(docs/17), the UART registers TX 0xC0800000 and status 0xC0800014 (docs/18). The direct UART writer
`m50ii_uart_str` that all later boot markers use is `7ddf3b400 src/boot-d678.c` inside the hunk
`+132..+605` (now `HEAD src/boot-d678.c:204`).

### 3.4 Coprocessor memory re-initialisation, "CamIF reinit" (2026-07-10)

This fix is not in the task list of the sprint but it is one of the largest single enablers.

- Symptom (HW, historic): about one second into Canon's boot the log showed `[MECHA] SHIRAHAMA DIED`,
  an assert at `Mecha::MechaControl.c:264`, the SD card never mounted and the screen stayed blank.
- Cause (as written in the code comment): the first-stage boot loader of this camera family erases the
  buffer it loads `AUTOEXEC.BIN` into, and that buffer overlaps RAM that the loader had already filled
  for the lens/stabiliser coprocessor. Canon's own boot does not have this problem; ML's does.
- Change: call Canon's re-initialisation routine at ROM 0xE000690C before jumping to the firmware.
  `7ddf3b400 src/reboot.c +344..+371`, switch `M50II_CAMIF_REINIT 1` (now `HEAD src/reboot.c:362`).
  Correctly guarded by `CONFIG_M50mk2`.
- Related: a no-op handler for software interrupt 0x9A raised by the dying coprocessor,
  `7ddf3b400 src/init.c` in the hunk `+1620..+1777` (now `HEAD src/init.c:1817`).
- Documents: the comment itself; memory note `ml-m50ii-port-status.md`. No numbered doc covers it alone.

### 3.5 PropertyMgr readiness gate (2026-07-10)

- Symptom: assert `Property/PropertyMgr.c:484` when ML registered its property handlers.
- Cause: Canon's property manager object pointer at RAM 0x4FF4 is still NULL when ML's init task runs.
  In QEMU it never becomes non-NULL; on hardware it does, slightly later.
- Change, three parts:
  - a weak hook `ml_canon_propmgr_ready()` that defaults to "ready", and a check before
    `prop_register_slave`: `7ddf3b400 src/property.c +162..+168` and `+174..+178`;
  - the camera's override that reads 0x4FF4: `b11c53abc platform/M50mk2.101/function_overrides.c:173-183`;
  - a bounded wait (10 ms steps, 5 s limit) at the start of ML's main init:
    `7ddf3b400 src/init.c +612..+648` (`m50ii_wait_canon_subsystems`) and its call in `+942..+1004`.
- Open issue (Observed in docs/offline/A4.md): the later boot crash at pc=0xE021B8FC fires *while this
  wait loop is running* in 21 of 29 logged dumps. The gate prevents the assert; it does not make the
  early property phase safe. The wait function is not inside a camera guard (section 6).

### 3.6 Second CPU start: the one-line mailbox fix in reboot.c

- Symptom (HW, historic): the second CPU core never started; `cpu1rdy=0`.
- Cause: the first-stage loader of the M50 II reads RAM 0xBFE01FC4 as the *entry pointer* for the second
  core (`ldr r0,[0xBFE01FC4]; ldr r0,[r0]; bx r0`, per the code comment). ML used the convention of the
  original M50, which writes the constant 0x10 there and the entry to 0xBFE01FC8, so the second core
  branched to address 0x10.
- Change: add the M50 II to the "new style" branch, `MEM(0xBFE01FC4) = MAIN_FIRMWARE_ADDR | 0x1;`.
  `7ddf3b400 src/reboot.c +393` (the changed `#if` line) and the comment hunk `+397..+402`
  (now `HEAD src/reboot.c:393-403`).
- Date conflict (Observed): the code comment is dated 2026-07-12; the memory index dates the hardware
  confirmation 2026-07-10 (build 7dd1b5e2). Which is right is Unknown.
- The memory index also records that an earlier note calling `reboot.c` "dead code" was wrong.

### 3.7 Wrong memcpy stub, and a CPU0 abort handler to find it (2026-07-11)

- Symptom (HW, historic): `mmu_init()` ended in a prefetch abort on the camera but not in QEMU.
- Tool built to find it: ML's own abort handler. It copies Canon's exception vector table to RAM,
  redirects the prefetch and data abort entries and prints PC, fault address and registers over UART.
  `7ddf3b400 src/init.c +1306..+1545` (now `HEAD src/init.c:1462` for the installer) and the install
  call in `boot_pre_init_task`, hunk `+1568..+1572`.
- Cause: stub `memcpy_dryos` pointed at 0xE065E998, the address pattern of the original M50, which on
  this ROM is unrelated code. The real function is 0xE0329D10, found through the DryOS export table at
  0xE004ECA0.
- Change: `b11c53abc platform/M50mk2.101/stubs.S:127`.
- Left over (Observed): the handler loops forever after printing. The memory note
  `m50ii-boot-stability.md` identifies this "print and hang" as the reason one transient fault becomes a
  dead camera. The handler is also not inside a camera guard.

### 3.8 MMU remap pool size and the memory budget (2026-07-10 to 07-12)

*MMU remap* is ML's way to patch ROM: a 64 KB ROM page is copied to RAM, edited, and the page tables are
changed to show the copy. Each such page costs 64 KB of ML's own memory.

- Symptom: after 3.4 the display came alive and Canon asserted at `GraphicsConvertPosType.c:9`. A patch
  that turns the assert call into a no-op was written, yet the assert still fired on hardware.
- Cause: the pool held one page, and the second-CPU hijack (`CONFIG_INIT1_HIJACK`) had taken it. The
  patch silently failed to get a page.
- Changes in commit 2: pool size macro `MMU_MAX_REMAP_PAGES` (default 2) and its sizing arithmetic,
  `7ddf3b400 src/patch_mmu.c +34..+69`; a UART logger for return codes, `+77..+126`; the patch itself, one
  region covering all seven assert call sites from 0xE01B8CAC, in the hunk `+861..+928`.
- The budget lesson: ML's memory is taken from Canon's pool
  (`ml_reserved_mem = _bss_end - user_mem_start`). Four pages left Canon about 368 KB and DryOS panicked
  before ML's first marker. To afford two pages, commit 1 turned off the module system and the on-camera
  compiler: `b11c53abc platform/M50mk2.101/Makefile:26-34` (`CONFIG_TCC := n`, `CONFIG_MODULES := n`),
  with `b11c53abc platform/Makefile` changing the two defaults from `=` to `?=`, and empty module
  functions in `function_overrides.c:185-197`.
- Later reversal: commit 7 (`ecd2f853c`) turned modules back on and set the pool back to one page.
  `HEAD src/patch_mmu.c:78` reads `MMU_MAX_REMAP_PAGES 1`. Inferred: with one page the assert patch
  competes with the hijack again; commit 10 added a "reclaim" of the hijack page
  (`HEAD src/patch_mmu.c:818`) for a different patch. Whether the assert patch is live at `HEAD` is
  Unknown without a boot log line `mmu.gcpt: apply @0xE0 rc`.

### 3.9 Wrong DMA allocator stubs and the Tier-A audit (2026-07-11)

- Symptom (HW, historic): crash at pc=0x2 in `_load_fonts`, with r0=0x8000000D.
- Cause: the stubs `_alloc_dma_memory` (0xE055AAF4) and `_free_dma_memory` (0xE055AB28) pointed into a
  Canon graphics function. Every `fio_malloc()` jumped there.
- Change: the stubs are commented out (`b11c53abc platform/M50mk2.101/stubs.S:141-142`) and replaced by C
  wrappers over the verified `_AllocateMemory` / `_FreeMemory` that return the uncacheable alias of the
  block: `b11c53abc platform/M50mk2.101/function_overrides.c:200-218`.
- Same audit removed `_LoadCalendarFromRTC` (`stubs.S:274-277`; replacement
  `function_overrides.c:44-65`) and rewrote `get_task_info_by_id` (`function_overrides.c:67-80`).
- Document: docs/25 (40 crash-class stubs checked by decompilation).

### 3.10 Font loading and early file I/O (2026-07-10 to 07-11)

- Symptoms: ML ran without fonts; a null font was dereferenced; Canon's bitmap font path was reached.
- Causes and changes, all in `7ddf3b400`:
  - `_load_fonts()` marked itself done before any font had loaded, so an early failed call disabled the
    real one. The flag is now set only on success: `src/rbf_font.c +665..+688`, `+715..+780`.
  - `startup_warning()` drew text before fonts existed. It now logs to UART and returns:
    `src/fio-ml.c +202..+218`; `_find_ml_card` keeps the default card: `+261..+266`.
  - Font load errors are sent to UART instead of being drawn with Canon's font:
    `src/rbf_font.c +57..+70` and the one-line replacements `+85..+112`.
  - Null-font guards in the draw functions: `src/rbf_font.c +243`, `+248`, `+372..+376`, `+402`, `+428`,
    `+518..+519`, `+570`.
- Document: memory index entry "font cascade root-fixed"; docs/26 section 8 for the related removal of
  three wait loops that always timed out (boot time 91 s to about 30 s; `src/init.c` hunks around
  `+1877..+1921`).

### 3.11 The sector-rounding overflow (2026-07-12, commits 4, 5, 6, and 8)

This is outside the two squashed commits but belongs to the same story.

- Symptom: "memory overflow @ rbf_font.c:178" in the Debug menu, a truncated config key.
- Cause: Canon's `_FIO_ReadFile` transfers whole 512-byte sectors and writes past a buffer whose size
  is not a multiple of 512.
- Changes: commit 4 fixed one call site; commit 5 reverted it; commit 6 rounds every DMA allocation up
  to 512 bytes in `memcheck_malloc` (`bb10c937b src/mem.c`, now `HEAD src/mem.c:669`). Commit 8 then
  found the transfer unit is a whole card cluster and gave `font_read` a scratch buffer of at least
  64 KB (`d9614bca4 src/rbf_font.c`).
- Note: commits 4 and 5 cancel out exactly and can be dropped in any rewrite.

### 3.12 Display: an OSD layer of ML's own (2026-07-11)

- Problem: on this camera Canon's display compositor (the firmware component that mixes image layers
  for the screen; Canon name "XCM", XimrContextMaker) does not use the single-context call
  `XimrExe(XIMR_CONTEXT)` that ML uses on other DIGIC 8 cameras (docs/26 section 0).
- Change: ML allocates its own 960x540 RGBA buffer, registers it as compositor layer 2 or 3 (free on
  this camera; layers 0 and 1 are Canon's), points ML's `rgb_vram_info` at it, and asks for a redraw by
  setting a dirty flag at RAM 0x11294+0x18.
  - Probe and layer set-up: `7ddf3b400 src/init.c` hunk `+612..+932`, functions `m50ii_display_probe`
    (line 784) and `m50ii_osd_try` (line 854), ROM entries 0xE00BE348, 0xE035D590, 0xE00BE37C.
  - Redraw trigger: `7ddf3b400 src/bmp.c +264..+269`.
  - Screen geometry reused from the M50: `7ddf3b400 src/vram.c +363`.
- Naming note: the function that creates the permanent OSD is still called `m50ii_osd_try`.
- Document: docs/26.

### 3.13 Menu input: a read-only peek at Canon's GUI event ring (2026-07-11 to 07-12)

- Standard ML method: replace Canon's `gui_main_task` with ML's and filter button events. It needs a
  callback on every task switch. On this camera the usual slot (RAM 0x4144) holds Canon's own
  callback-list walker; overwriting it caused DryOS panics (docs/26 section 9, docs/27).
- Step 1, kept: register ML's callback through Canon's own list-insert function at 0xE0164044.
  `7ddf3b400 src/init.c +285..+368` (`m50ii_cb`), registration in the hunk `+1587..+1611`;
  stubs at `b11c53abc platform/M50mk2.101/stubs.S:167-168`.
- Step 2, abandoned: the task replacement itself still faulted. It is switched off by
  `M50II_BYPASS_GUI_OVERRIDE 1` (`7ddf3b400 src/gui.c +107..+195`, now `HEAD src/gui.c:124`). The
  message-queue offset correction recorded in the memory index (read `gui_main_struct+0x1c`, not `+0x18`) is in
  `src/gui.c +368..+375`.
- Step 3, final: ML does not take events away from Canon. A polling task reads Canon's GUI event ring
  (base RAM 0xBAE08, 0x401 slots of 16 bytes, write index at RAM 0x543C) every 20 ms with its own read
  cursor, and passes each event to a bridge function. `7ddf3b400 src/init.c` hunk `+1620..+1777`
  (`m50ii_input_task`, now `HEAD src/init.c:1726`); bridge `m50ii_handle_canon_gui_event` in
  `7ddf3b400 src/gui.c +197..+334` (now `HEAD src/gui.c:251`). The FN key (code 0x22) opens the menu,
  the four-way pad (0x40, 0x42, 0x3E, 0x3C) navigates, the centre key (0x0A) enters submenus.
- Two supporting changes: the bridge ignores everything until `ml_started` is set, because a key
  pressed during boot stalled the boot (`src/gui.c +236..+245` region); and ML's menu no longer asks
  Canon to change GUI mode, because that caused an assert flood from `LiveViewDrive::EvfDevState`
  (`7ddf3b400 src/menu.c +5624..+5631`, `+5650..+5652`, `+5836..+5861`).
- Known limits (docs/33, docs/34): Canon still receives every key, so keys act in both systems; key
  codes differ by shooting mode. Commit 10 added a ROM hook to drop events while the ML menu is open
  (`HEAD platform/M50mk2.101/include/platform/mmu_patches.h`, patch address 0xE021E460), off by default.
- Document: docs/27, docs/28.

### 3.14 Small items in commit 2

- Lens data length check relaxed (`7ddf3b400 src/lens.c +1363..+1373`); the structure size is copied
  from the M50 and does not match. The lens metadata is therefore incomplete (Inferred from the TODO).
- Raw colour matrix and dynamic range table reuse the M50's (`7ddf3b400 src/raw.c +453`, `+621`).
- Non-fatal assert handler, on by default (`7ddf3b400 src/init.c +1222..+1272`). It lets Canon code
  continue after an assert that Canon intended to be fatal. That is a bring-up aid, not a safe default.
- A boot heartbeat task and LED beacons (`src/init.c` hunk `+1620..+1777`).

## 4. Commits 3 to 15 in brief

- **3 `1d1cc7e54`**: turns the input traces off. No functional change.
- **4, 5, 6**: section 3.11.
- **7 `ecd2f853c`** (07-12): modules and compiler back on; `platform/M50mk2.101/edmac-d8.c` (233 lines)
  copied from the working M50 raw video port (docs/29); 18 stubs. The sprint report A5 concludes that
  two of these stubs are not EDMAC functions (docs/offline/A5.md, Summary 1-3), which would explain why
  the engine copy never produced valid frames.
- **8 `d9614bca4`** (07-14): font scratch buffer; `FEATURE_PICSTYLE` off; raw LiveView constants;
  DIGIC 8 raw unpack offsets in `src/raw.c`.
- **9 `49b89e783`** (07-14): `src/serial_shell.c` (545 lines), a command shell over the UART. It reads
  input by following the write pointer of Canon's console ring instead of the UART register, because
  Canon's own interrupt handler consumes the bytes first. It made live memory reads and function calls
  possible without rebuilding (memory note `duplex-uart-harness.md`).
- **10 `d9936e077`** (07-18): the central fix is one line. `FIO_WriteFile` must be given the
  uncacheable alias of the buffer on this camera; the conversion existed but was compiled only for
  `CONFIG_MEM_2GB` (`HEAD src/fio-ml.c:695`). With it the first raw frame, 2096x1164, 14 bit, reached
  the card (docs/35). The commit also bundles the vsync hook on the state object at RAM 0x10108, eight
  compatibility stubs for `mlv_lite`, the GUI exclusivity hook and loader instrumentation. The sprint
  report A2 finds that 0x10108 is `EvfDevState`, not the object the generic code expects
  (docs/offline/A2.md, Summary 1-2).
- **11 `6ff442ba3`**: removes a second, conflicting uncacheable conversion in `mlv_lite`; defines
  `BGMT_REC`; routes a runtime-set REC key code to the module key handler.
- **12, 13, 14** (07-18, all in `edmac-d8.c`): a flag to skip the engine resource lock; then a plain
  CPU copy instead of the engine copy, because decoded frames contained heap strings; then the copy
  moved to a worker task, because copying inside the vsync callback crashed the camera (docs/32).
- **15 `a10b16c23`** (07-21): raw_vidx module fixes only (EDMAC redirect off, writer priority 0x1A,
  close before release, uncacheable source) and `raw_vidx` added to `modules.included`. Core files are
  those of commit 14. The message states the reason: core changes for boot stability (in `fa0834b40`)
  coincided with a crash at 0xE021B8FC. The sprint report C6 finds the logs do not show that core
  layout caused that crash (docs/offline/C6.md, "Short answer"); the cause is not identified.

## 5. What a reader should take from the history

1. The decisive fixes were small: two constants and one loader convention for boot (3.1, 3.2, 3.6), one
   call for the coprocessor (3.4), three stub addresses (3.7, 3.9), one conversion for file writes
   (commit 10). Most added lines are instrumentation that found them.
2. Three times the cause was an address copied from the original M50 that points at unrelated code on
   this ROM (3.7, 3.9, and per A5 the EDMAC pair). The audits in A14a/b/c list more of the same class.
3. Memory is the hard limit: ML's size is taken from Canon's pool, and the working window between
   "Canon panics" and "feature fits" was about 64 KB (3.8).
4. Wherever ML tried to take something from Canon (the task switch slot, the GUI task, the GUI mode, an
   EDMAC channel, the engine resource lock), Canon's firmware failed. Every approach that survived only
   observes or adds (3.12, 3.13, commits 13 and 14).

## 6. State of the shared code: ungated changes and broken builds (Observed)

A preprocessor nesting check of the files in `7ddf3b400` shows these additions are outside any
`CONFIG_M50mk2` guard:

| File (lines in `7ddf3b400`) | What | Effect on other cameras |
|---|---|---|
| `src/boot-d678.c:14` | `#define M50II_TRACE_CREATE_LADDER` | LED blink delays in boot for every DIGIC 6/7/8/X camera using this file |
| `src/boot-d678.c:1283-1297` | data-copy literal restore | writes 0xE1394358 into the start-up copy of every camera that defines `BR_BR_CSTART` |
| `src/boot-d678.c:197` | `m50ii_uart_str`, fixed MMIO 0xC0800000 | compiled for all cameras using this file |
| `src/init.c:939` onward | `big.N` UART markers, `m50ii_wait_canon_subsystems()` reading 0x4FF4, tier-1 diagnostic | wrong addresses on other cameras |
| `src/init.c:1462`, `1550..1620` | abort handler install, `pre.N` markers, callback registration | references symbols defined only for the M50 II |
| `src/fio-ml.c:205`, `:262` | UART fallback in `startup_warning` | needs `m50ii_uart_str` |
| `src/rbf_font.c:670` onward | `big.5.N` markers, null-font guards | needs `m50ii_uart_str` |
| `src/property.c:165` | weak readiness hook | harmless, generic |
| `src/mem.c` (commit 6) | 512-byte rounding of DMA allocations | harmless, generic |

Build test, run from `git archive` exports in the scratch directory so the checkout was not touched:

- `walkback-milestone`, `platform/200D.101`: **fails to compile** `src/init.c` with
  `'m50ii_cbnode' undeclared` (line 1602), `'m50ii_cb' undeclared`, `'m50ii_override_count' undeclared`
  (line 1679) and two unused-variable errors.
- `walkback-milestone`, `platform/50D.109` (DIGIC 4): fails at the same lines, plus
  `'is_cpu1_ready' undeclared` (line 1677).
- `b9d9a9557` (upstream base), `platform/200D.101`: `src/init.c` compiles. The build then stops at link
  with `undefined reference to module_exec_cbr`, which is caused by my `CONFIG_MODULES=n` override, not
  by the tree. So the comparison is valid for compilation of `init.c` only.

Consequence: the branch cannot be offered upstream as it is, and the split in section 7 must add
guards, not only reorder lines.

## 7. Proposed split of commits 1 and 2 (proposal only; history was not rewritten)

Goal: 16 topical commits that each build for the M50 II and do not change other cameras. Order follows
dependency. Each line gives title, files, rationale. "Guard" means: wrap the new code in
`#if defined(CONFIG_M50mk2)`.

| # | Title | Files (from commits 1 and 2) | Rationale |
|---|---|---|---|
| S1 | `chore: ignore M50mk2 demo build dir and backups` | `.gitignore` | Unrelated housekeeping. |
| S2 | `build: allow platforms to override CONFIG_TCC and CONFIG_MODULES` | `platform/Makefile` (2 lines) | Generic, upstreamable alone. |
| S3 | `feat(m50mk2): platform skeleton, fw 1.0.1 signature` | `platform/M50mk2.101/*` except the function_overrides parts named below; `src/fw-signature.h` | Stubs, constants, headers. Fix the "EOS M50 1.1.0" header. Carries 3.0, 3.1, 3.2(a), 3.3, 3.7 stub, 3.9 stub removal. |
| S4 | `feat(m50mk2): direct UART writer for boot diagnostics` | `src/boot-d678.c` (`m50ii_uart_str` only), new small header | Everything later depends on it. Guard; give other cameras an empty inline. |
| S5 | `fix(m50mk2): new-style CPU1 entry mailbox` | `src/reboot.c` hunks `+393`, `+397..+402` | The one-line fix of 3.6, reviewable alone. |
| S6 | `fix(m50mk2): re-init CamIF coprocessor memory before firmware start` | `src/reboot.c +344..+371`; SGI 0x9A handler from `src/init.c` | 3.4. |
| S7 | `fix(boot-d678): keep kernel .data source literal intact on M50mk2` | `src/boot-d678.c +1283..+1297` | 3.2(b). Guard by camera. First test whether it can be dropped (see 3.2, Inferred). |
| S8 | `feat(m50mk2): CPU0/CPU1 abort handlers with UART register dump` | `src/init.c +1306..+1545`, install call; `src/patch_mmu.c +549..+570` | 3.7 tool. Guard. Candidate to make generic for DIGIC 7/8 later. |
| S9 | `feat(patch_mmu): configurable remap pool; M50mk2 GraphicsConvertPosType patch` | `src/patch_mmu.c +30..+126`, `+766..+960`; `platform/M50mk2.101/include/platform/mmu_patches.h` | 3.8. Move the seven-site patch out of the generic function into the platform header. |
| S10 | `fix(m50mk2): DMA allocator and RTC wrappers replace wrong stubs` | `function_overrides.c:44-80`, `:200-218`; `stubs.S:141-142`, `:274-277` | 3.9. |
| S11 | `fix(property): wait for Canon PropertyMgr before registering handlers` | `src/property.c`; `function_overrides.c:173-183`; `src/init.c +612..+648` and its call | 3.5. The weak hook is generic; the wait is guarded. |
| S12 | `fix(fonts): do not latch fonts_loaded on failure; no drawing before fonts exist` | `src/rbf_font.c`, `src/fio-ml.c` | 3.10. The latch fix and null guards are generic and upstreamable; the UART markers need the S4 header. |
| S13 | `feat(m50mk2): OSD on a dedicated compositor layer` | `src/init.c` (`m50ii_display_probe`, `m50ii_osd_try`), `src/bmp.c`, `src/vram.c` | 3.12. Rename `m50ii_osd_try` to `m50ii_osd_init`; move to a platform file. |
| S14 | `feat(m50mk2): task-switch callback via Canon's list-insert` | `src/init.c +285..+368`, registration hunk; `stubs.S:167-168` | 3.13 step 1. Guard the registration call (this is the line that breaks other builds). |
| S15 | `feat(m50mk2): menu input by read-only GUI ring peek` | `src/init.c` (`m50ii_input_task`), `src/gui.c`, `src/menu.c`, `gui.h` | 3.13 step 3. Drop the abandoned override experiment (`M50II_GUI_TRANSPARENT`, `M50II_GUI_DIAG`). |
| S16 | `chore(m50mk2): lens data length tolerance, raw tables, boot markers` | `src/lens.c`, `src/raw.c`, remaining `big.N`/`post.N`/`pre.N` markers in `src/init.c`, `src/boot.h` | Leftovers. Markers behind one macro `M50II_BOOT_TRACE`. |

Not carried over at all: the roughly 1,100 lines of undefined `M50II_TRACE_*` variants in
`src/boot-d678.c` and the stage markers in `src/boot.h`. They document the abandoned hand-mirror
approach, which docs/08 and docs/10 already describe. Commit 3 (`1d1cc7e54`) folds into S15; commits 4
and 5 vanish; commit 6 follows S12 unchanged.

How to execute later, on a new branch only:

1. `git -C magiclantern_simplified worktree add ../wt/<ID> -b <18-character-name> walkback-milestone`.
   The branch name must be exactly 18 characters, because the Makefile embeds it in the build id and its
   length moves 53 core symbols (tools/symgate/README).
2. `git rebase -i b9d9a9557`, mark commits 1 and 2 as `edit`; at each stop `git reset HEAD^` and stage
   by hunk (`git add -p`) into the commits above. Because interactive tools are not available to agents
   in this environment, an agent would instead build each S-commit with `git apply --cached` on prepared
   patch files.
3. After every S-commit: build `platform/M50mk2.101`; from S4 on also build one DIGIC 4/5 platform and
   one other DIGIC 8 platform (for example `50D.109` and `200D.101`).
4. Acceptance test for the whole rewrite: the tree of the last rewritten commit, with guards removed,
   must equal the tree of `7ddf3b400` for M50 II code paths. The practical check is that
   `platform/M50mk2.101/build/autoexec.bin` of the rewritten branch tip passes
   `tools/symgate/check_symbol_stability.py` against the `a10b16c23` baseline. Removing dead code and
   adding guards does not change M50 II object code if done correctly; renames in S13 do change symbol
   names and should be a separate, later commit if layout identity is wanted.
5. Do not push and do not flash from the rewritten branch until one boot on hardware confirms it.

## 8. Subsystem table

"State on hardware" is the last state claimed by the July 2026 documents and commit messages. It was
not re-verified. "works" means a documented successful hardware run; "partly" means a documented run
with known failures; "untested" means no hardware result is documented for the code at `HEAD`.

| Subsystem | Files changed | State on hardware (historic) | Doc |
|---|---|---|---|
| Boot handoff (relocate, wrap-and-swap) | `platform/M50mk2.101/consts.h`, `Makefile`; `src/boot-d678.c`, `src/boot.h` | works; boot as a whole is intermittent, about 50 % per `a10b16c23` message | 08, 10, 13, 20, 22, 23 |
| Kernel data-copy literal | `consts.h:13`; `src/boot-d678.c:1304` | works; redundancy of the restore line untested | 13, 22 |
| Coprocessor (CamIF) re-init | `src/reboot.c:362` | works | code comment; memory `ml-m50ii-port-status` |
| Second CPU start | `src/reboot.c:403` | works | memory index; code comment |
| MMU remap (ROM patching) | `src/patch_mmu.c`, `mmu_patches.h`, `stubs.S` | partly: one page; cross-CPU update call faults (comment at `src/patch_mmu.c:820`); A14a suspects stub `_request_RPC` | 15, 25; offline/A14a |
| Abort handler, non-fatal asserts | `src/init.c` | works as a diagnostic; hangs the camera after any fault | memory `m50ii-boot-stability` |
| Memory allocators (DMA wrappers) | `function_overrides.c`, `stubs.S` | works | 25 |
| PropertyMgr gate | `src/property.c`, `function_overrides.c`, `src/init.c` | partly: assert gone; crash 0xE021B8FC occurs during the wait in some boots | offline/A4 |
| Fonts, early file reads | `src/rbf_font.c`, `src/fio-ml.c`, `src/mem.c` | works; rare boot fault at 0xE005DE52 (a `%s` with NULL from ML, per A3) | 32; offline/A3 |
| Display / OSD layer | `src/init.c`, `src/bmp.c`, `src/vram.c` | works | 26 |
| Menu input (ring peek) | `src/init.c`, `src/gui.c`, `src/menu.c`, `gui.h` | partly: works in the photo mode it was captured in; key codes depend on mode; Canon also sees the keys | 27, 28, 33, 34 |
| GUI exclusivity ROM hook | `mmu_patches.h`, `src/patch_mmu.c` | untested as default (runtime switch, off) | 33, 34 |
| Modules and on-camera linker | `Makefile`, `src/module.c`, `tcc/tccrun.c`, `modules.included` | works (modules load); loading raw_vidx at boot hurt boot reliability | 31; memory `m50ii-boot-stability` |
| Serial debug shell | `src/serial_shell.c`, `src/boot-d678.c`, `platform/Makefile` | works | memory `duplex-uart-harness`; tools/mlserial/TOOLS.md |
| File write (uncacheable source) | `src/fio-ml.c:695` | works | 35 |
| Single raw frame capture | `src/debug.c`, `src/raw.c`, `consts.h`, `internals.h` | works (one 2096x1164 frame, 2026-07-18) | 35 |
| Vsync / state-object hook | `src/state-object.c`, `platform/.../state-object.h` | partly: fires, but on `EvfDevState` input 6, which A2 says is not the readout-done event | 31; offline/A2 |
| EDMAC engine copy | `platform/M50mk2.101/edmac-d8.c`, `stubs.S`, `function_overrides.c` | does not work: stale frames and a Canon error storm; A5 attributes it to two wrong stubs | 29, 30, 32; offline/A5 |
| CPU frame copy with worker task | `edmac-d8.c` | partly: frames copied; too slow to keep up (queue fills) | 32; offline/A13 |
| mlv_lite recording | `modules/raw_video/mlv_lite/mlv_lite.c` | partly: header and one 4 MB write; then error storm, no frames | 32 |
| raw_vidx recording | `modules/raw_video/raw_vidx/*` | partly: a 13.4 MB `tst00.mlv` written on 2026-07-19; storm in about half the runs; file not decoded on the host according to the memory index | memory `m50ii-symbol-shift-boot-crash`; offline/C9-recommended |
| Lens metadata | `src/lens.c`, `platform/.../lens.h` | untested (structure size known to be wrong) | none |
| FPS registers, picture style | `fps-engio_per_cam.*`, `features.h` | untested (picture style disabled) | 31 |
| WiFi deploy | `modules/wifidep/*` in `fa0834b40` only | works per docs/37, but not in the 15 commits | 36, 37 |

## 9. Unknowns

- Whether the literal-restore statement (3.2 b) can be removed. One QEMU boot without it would answer.
- Whether the GraphicsConvertPosType patch is applied at `HEAD` with a one-page pool (3.8).
- The correct date of the second-CPU fix (3.6).
- The exact July builds behind the squash. The documents name build ids (for example 7dd1b5e2,
  93782021, df8ea890) that correspond to no commit; the intermediate source states are lost.
- Whether any camera other than the M50 II still builds and boots from this branch after guards are
  added; only compilation of two platforms was tried here.
