# R-F7 - second-agent review of `offline-stub-fixes` (F7)

Reviewed: `/mnt/data/WORK/eos/wt/F7`, 19 commits `127630367..ea62b792e` on top of `walkback-milestone`.
The branch was not modified. Nothing here is proven on the camera; every ROM statement below is from
`gang100.bin` (romdis + aligned literal scan), every behaviour statement is from reading ML source.

## Verdict

**Ready for a hardware boot test, but only as `HEAD~1` minus `b1e413554`** (17 commits, i.e. without
`LV_OVERLAYS_MODE` and without `LiveViewApp_dialog`). With those two left out I found no call site
reachable during boot, menu use or `raw_vidx` recording whose behaviour changes. The two left-out
commits switch ML's LiveView overlay path from "never" to "on" and must be tested separately, after
recording works.

## Build and symgate (rebuilt in a private worktree)

- Worktree `/mnt/data/WORK/eos/wt/R-F7`, branch `offline-rf7-review` (= `ea62b792e`, no commits added).
  `make` in `modules/` and in `platform/M50mk2.101` both exit 0.
- `check_symbol_stability.py`: all section starts/ends and anchors OK (`_text_end` 0xF3328, `_bss_end`
  0x14FC00, `raw_info` 0x104434, `raw_lv_redirect_edmac` 0xE5319). `RESULT: FAIL - 13 pre-existing
  symbol(s) moved` - the 13 are exactly the re-pointed stubs (absolute addresses), no ML-defined symbol
  moved. This matches F7's claim. Note the tool prints FAIL but exited 0 in my run.
- `check_module_syms.py`: PASS, 4 modules (bench, file_man, mlv_lite, raw_vidx), all imports resolvable.
- Slip on my side: the first `worktree add` used a relative path and created the worktree inside the
  main checkout directory (`magiclantern_simplified/wt/R-F7`). I moved it with `git worktree move`
  and removed the empty directory; `git status` of the main checkout shows only the two untracked
  `edmac-d8.{d,o}` files that were there before.

## Findings by severity

### HIGH - behaviour changes, leave out of the first boot/recording test

1. **`ea62b792e` `LiveViewApp_dialog` 0x139B8 -> 0xE9F0** (stubs.S). ROM: literal 0x139B8 has 0 hits;
   0xE0445912 loads handler literal 0xE0444059, calls 0xE059F43C, `str r0,[r4,#4]` at 0xE0445922.
   I did not re-check that r4 = 0xE9EC at that point (0xE9EC has 11 literal hits in 0xE0440DEC..0xE04443B0)
   nor that the slot is zeroed on app close.
   Call sites: `src/zebra.c:3568` (`liveview_display_idle()` returns the pointer) and
   `src/zebra.c:3921` (`redraw_do()` -> `dialog_redraw`). With the old address the pointer was most
   likely always 0, so ML treated LiveView as never idle. With the new one every consumer of
   `liveview_display_idle()` becomes active in LiveView and during recording (`powersave.c:256` idle
   actions, cropmark/global-draw tasks). Extra display traffic on a recording path that already has
   a throughput limit and an intermittent EVF storm.
2. **`b1e413554` `LV_OVERLAYS_MODE` MEM(0x13CC8) -> MEM(0x15998)** (consts.h). ROM confirmed: 0xE0709C8C
   `ldr r5,=0x15998; ldr r6,[r5]`; 0x13CC8 has 0 literal hits. Call site `src/zebra.c:423`:
   `lv_disp_mode = LV_OVERLAYS_MODE != 3`, and `get_global_draw()` needs `lv_disp_mode == 0`. The
   meaning of the value (3 = clean page) is assumed from EOS R, not shown by the ROM. Together with
   finding 1 this is what turns ML overlay drawing on.
3. **`78d225b87` `dialog_redraw` -> bare `bx lr` 0xE065A4FC** is the safety net for finding 1 (old
   0xE054F3CA is not code: `movs r1,r0; lsls r5,r2,#1; strh r0,[r0]` is a literal pool). Keep it in
   the first set; it must be present before finding 1 is ever enabled. Side effect once 1 is on:
   `redraw_do()` no longer clears stale ML graphics (no `clrscr()` fallback when the pointer is non-zero).

### MEDIUM - correct value, new exposure only when the user triggers it

4. **`ec3363438` `sd_device` 0x6450 -> 0x1405C.** ROM confirmed: SD_DeviceCreate 0xE00E5E30
   `ldr r6,=0x14060; str r4,[r6]`. The only literal use of 0x1405C itself is 0xE06D3E74, the seed of a
   `rand()`-style LCG (0x41C64E6D / 0x3039), so `sd_device[0]` is an unrelated word - harmless because
   `bootflags.c:144` takes `sd_device[1]` for this body. Only importer: `src/bootflags.c`
   (`FEATURE_BOOTFLAG_MENU` is defined in features.h). Before: `dev` was whatever sat at 0x6454,
   probably failing the `if (!dev)` check or crashing. After: `dev` is the real storage device, so the
   bootflag menu gets further and then calls `fsuDecodePartitionTable` at the unresolved 0xDF00D365
   (F7 says the same). Do not open the bootflag menu entry; better, `#undef FEATURE_BOOTFLAG_MENU`
   before any hardware session. Not reachable at boot or while recording.
5. **`e4adf5aa0` `_request_RPC` 0xDF002C4A, `clear_RPC_request` 0xDF002C6A.** Blob mapping confirmed:
   [0xE00400EC..F4] = src 0xE14995A4, dst 0xDF002800..0xDF0033A0, so the entries are ROM 0xE14999EE
   (`push {r4,r5,r6,lr}`, irq-save, `bl 0xE1499876`) and 0xE1499A0E (`push {r4,lr}`, `bl 0xE149999A`).
   Both inner functions take the same `ldrex` spinlock at 0xDF003370. The old 0xDF002C32 is ROM
   0xE14999D6, the second half of the `dmb sy` at 0xE14999D4 - confirmed wrong. Identity is by shape
   only, no string anchor.
   Reachability: `request_RPC()` is called from `cpu.c:94/116`, i.e. from `patch_memory_rom/ram`
   when `is_cpu1_ready`. At boot `apply_normal_patches` clears `is_cpu1_ready` around the one
   M50mk2 patch (`patch_mmu.c:828-848`), so boot does not reach it. `raw_vidx` does not call
   `apply_patches`. `mlv_lite.c:2220` and `mlv_rec.c:1835` do (dialog refresh timer patch), as do
   `gdb.c` and `movtweaks.c`. So: no change for boot or `raw_vidx` recording; a real change for any
   run-time patch. Worth noting: the "RPC/SGI-to-cpu1 handshake FAULTS on M50 II HW" comment at
   `patch_mmu.c:820` was observed with the bogus entry (which stores r1 through r0 = the `do_RPC`
   function pointer, i.e. into ML code). That fault may have been the stub, not the handshake - an
   inference, to be tested on hardware by a deliberate single RAM patch, not during a recording test.
6. **`127630367` `task_max` 0x40CC -> 0x4108.** ROM confirmed: `get_task_by_id` 0xE00A6BC6 does
   `cmp id,*(0x4108); bgt fail`, table base `*(0x40E8)`, element 0x5C; enumerator 0xE014A65E uses the
   same pair. Corroborated in-tree: `src/boot-d678.c:273-275` records that 0x4108 held 199 until a
   hook overwrote it. (romdis still labels 0x4108 `task_dispatch_hook`; that label is stale, the stub
   is at 0x4144.) 0x40CC is used at 0xE03EF4C8 as a list head, not a count; F7 cites a hardware read
   of 1. Importers: `src/tasks.c:231` only (`tskmon.c:14` declares it and never uses it; the
   `boot-d678*.c` hits are the unrelated struct field `dryos->task_max`). Change: the "Show tasks"
   menu loop goes from 1 iteration to about 199 calls of `get_task_info_by_id` per redraw, inside the
   menu redraw. Bounded and Canon-validated ids, but it is new code running on hardware; do not open
   that menu page during the first recording test.

### LOW - correct or neutral, no reachable call site in this build

7. `7520d3c2c` `dump_file` 0xE065FE58: prologue `push {r4,r5,r6,lr}; sub sp,#0x18`, args r0/r1/r2 saved
   - consistent. Callers `minimal-d678.c:132`, `io_trace.c:944`: not in this build's run path.
8. `33a389e79`/`7f68e0596`/`e2d2f3e54`/`08e99ab07` memory-info four: all four new addresses are
   function prologues (0xE039A3D8, 0xE013D480, 0xE03CAE30, 0xE03CAEC8); 0xE039A3D9 and 0xE013D481
   appear in pointer tables. I did not check the strings they print. Callers: `minimal-d678.c` only.
9. `56ae9083d` `LiveViewApp_handler` 0xE0444058: literal 0xE0444059 has one hit (0xE0445C1C), loaded at
   0xE0445912 as the handler argument. Only used in the non-D678 branch of `zebra.c:3582`: no importer.
10. `14d86bb21` `gui_task_list` 0x11144: 0xE07BD3E2 `ldr r6,=0x11144; AcquireRecursiveLock([r6]); ldr r0,[r6,#0xC]`.
    Base right; `current` is at +0xC here while `gui-common.h:116` has +8 (F7 says so too). Users are
    `debug.c:1535-1582` and the non-D678 branches of zebra.c. I did not establish which feature flag
    gates the `debug.c` users; treat any ML function that redraws via `gui_task_list.current` as
    still broken.
11. `ea6e7dbb3` `gui_init_end` -> `bx lr`: only call is `gui.c:410`, compiled out by
    `M50II_GUI_TRANSPARENT=1` (and the override is bypassed). No change. Old 0xE065A4F2 is confirmed
    to be `if (r0==0) 0xE06CF730(4)`.
12. `72098bed2` SRM pair 0xE057B7BA / 0xE057DA00: both in the pointer table at 0xE02D19C8/CC, both
    allocate 0x34. `CONFIG_MEMORY_SRM_NOT_WORKING` still defined, so not called. Argument order unverified.
13. `b20a6a89a` seven new stubs: prologues valid at all seven; `LoadCalendarFromRTC_canon` 0xE0608794
    is the thunk described. No ML caller. Low risk of a later accidental link through the generic
    names `debug_assert`, `_turn_on_display`.
14. `13e03ea1e` DISPLAY_IS_ON: default stays 1. 0xE0778D14 writes its argument to [0xABB0+0x80+0x34]
    = 0xAC64 and `prop_deliver`s - confirmed. Do NOT enable `M50II_DISPLAY_IS_ON_FROM_RAM` before a
    hardware read: about 40 call sites gate on it (`menu.c:4735/5009/5022/5186/5705`, `fio-ml.c:219`,
    `zebra.c:3645` `while (!DISPLAY_IS_ON) msleep(100)`); if the word is 0 with the panel on, the ML
    menu will not draw and that loop never ends.
15. `db78afb89` raw skips: table record at 0xE13593E8 = (2096,1164,1,92,38,2000,1126) confirmed; the
    neighbouring 838/714-line records have 88/34. Opt-in define, default unchanged. If enabled it
    changes `raw_info` active area (2008x1130 -> 2004x1126), hence MLV frame geometry.
16. `cf12c3135` comment only.

## Adoption order

1. First card: 17 commits = all except `b1e413554` and `ea62b792e`. Expected run-time difference
   against the known core: none at boot, none in `raw_vidx` recording. Do the `rd`/`md` checks from
   F7.md "Next hardware checks" 1, 2, 5 on that build.
2. Before that session: disable or avoid the bootflag menu (finding 4) and the task-list page (finding 6).
3. Later, one at a time and not in a recording session: `b1e413554`, then `ea62b792e`, after
   `rd 0x15998` and `rd 0xE9F0` / `rd 0x139B8` confirm the values.
4. Never without a hardware read first: `-DM50II_DISPLAY_IS_ON_FROM_RAM`, `-DM50II_RAW_SKIP_FROM_CANON_TABLE`.

## Not checked (time box)

- Strings printed by the four memory-info functions; SRM argument order; the gating of `debug.c:1535+`.
- That 0xE9F0 is cleared when LiveViewApp closes.
- Byte-level diff of `autoexec.bin` against the baseline build (F7 reports 135 differing bytes
  against a different build of the main checkout; I only ran the symbol gate).
- This branch has not been merged with G1 (`offline-plan-step2`); both touch `stubs.S`, conflicts not assessed.
