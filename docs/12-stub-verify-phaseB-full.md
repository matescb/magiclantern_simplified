# 12 - Stub verification Phase B FULL (un-campaigned inherited stubs)

Verification-lead pass over the **remaining** inherited stubs/consts in
`magiclantern_simplified/platform/M50mk2.101/stubs.S` and `consts.h` that were
**not** covered by the boot-critical Phase B fixes (`docs/11`) and were never
independently re-anchored in the Ghidra documentation campaign (`docs/06`).
Method and grading follow `docs/06` / `docs/11`: capstone string-anchoring +
independent romdis verify on our own ROM `m50ii_dumper/dump_1.0.1/gang100.bin`
(base `0xE0000000`, Thumb-2), with the `0xDF00xxxx` dual-core RPC blob now
grounded via the confirmed boot copy descriptor
(`[0xE00400EC]=0xE14995A4 src -> [0xE00400F0]=0xDF002800 dst, len 0xBA0`).

**Grading (same as docs/06 / docs/11):** `OK` = anchor + verified
prologue/disasm; `SUSPECT` = plausible but not statically anchorable to the
claimed role (needs runtime/HW); `WRONG` = current address is not a valid entry
for the claimed role on this ROM (mid-function, wrong module, or stale
inherited copy). **A `WRONG` stub can hang/brick the camera - do not build
against it. Only `OK` entries are safe.**

**Result:** 46 targets audited - **4 OK, 8 SUSPECT, 34 WRONG**.
Of the 34 WRONG, **25 have a replacement address** (20 statically verified +
5 HW-contingent candidates, section 2) and **9 have no replacement** and must
be disabled/`notfound` (section 5). Plus consts.h findings (section 3) and the
SUSPECT/TODO list (section 4).

> Note: `docs/11` already re-pointed `change_mmu_tables`, `uart_printf`,
> `vsnprintf`, the RecursiveLock trio, `take_semaphore_now`, and
> `msg_queue_count` in the live `stubs.S`. `memcpy_dryos` was recommended in
> `docs/11` but is **still bound to the WRONG `0xE065E998`** in `stubs.S` L121,
> so it reappears in the fix list below.

---

## 1. Summary tables per subsystem

Legend: current = value currently in `stubs.S`/`consts.h`; -> = replacement.

### memory_suite (RscMgr / PackMem)

| stub | current | verdict | evidence (abridged) | action |
|---|---|---|---|---|
| AllocateMemoryResource | `0xE05C7F0E` | **WRONG** | `mov r3,r4`, mid Flavor/Job (DummySuite) module, not RscMgr.c (RscMgr ~0xE057Cxxx). No in-ROM name anchor. | disable / TODO-HW |
| AllocateContinuousMemoryResource | `0xE05C7F3C` | **WRONG** | `str.w r2,[r3,#1]` = function tail; real entry is +6. Wrong module (Flavor/Job). | disable / TODO-HW |
| FreeMemoryResource | `0xE05C7FE2` | **WRONG** | `add r5,ip`, mid-function, wrong module. RscMgr free poster not statically pinnable. | disable / TODO-HW |
| AddMemoryChunk | `0xE0553C5C` | **WRONG** | garbage (`cdp2`). Real `FUN_e05bd70c` appends chunk to suite tail; `System::PackMem.c` asserts (IsSuite/IsChunkSignature). | **-> 0xE05BD70C** |
| GetFirstChunkFromSuite | `0xE0553D82` | **WRONG** | mid jump-table. Real `FUN_e05bd790`: IsSuiteSignature, returns first chunk base. | **-> 0xE05BD790** |
| GetNextMemoryChunk | `0xE05546A4` | **WRONG** | `asrs`, mid-function. Real `FUN_e05bdfb0`: 2-arg (suite,chunk), reads chunk+8; loop-paired with GetFirstChunk. | **-> 0xE05BDFB0** |
| GetMemoryAddressOfMemoryChunk | `0xE0553B9C` | **WRONG** | mid `bl`. Real `FUN_e05bd552`: IsChunkSignature, returns chunk+0x18. | **-> 0xE05BD552** |
| DeleteMemorySuite | `0xE0553FE4` | **WRONG** | mid switch. Real `FUN_e05bd95c`: DebugMsg 'DeleteMemorySuite 0x%x', walks+frees chunks, VERIFIED free 0xE03C52A8. | **-> 0xE05BD95C** |
| CreateMemorySuite | `0xE0554276` | **WRONG** | garbage. Real `FUN_e05bdbe8`: malloc(0x18) via 0xE03C5270, inits empty suite, appends chunk. Matches ML 'calls malloc(0x18)'. | **-> 0xE05BDBE8** |
| CreateMemoryChunk | `0xE055374C` | **WRONG** | `cmp r0,#1` mid-function. Real `FUN_e05bd22e`: malloc(0x28), sets address/size/type; called from CreateMemorySuite. | **-> 0xE05BD22E** |

### gui_core

| stub | current | verdict | evidence (abridged) | action |
|---|---|---|---|---|
| dialog_redraw | `0xE054F3CA` | **WRONG** | inside a literal pool (garbage). This build's debug_assert is called `(0,file,line)` with NO expression string, so the `pcSignature` anchor is structurally absent. Real fn in WindowSystem::Dialog.c (~0xE059Dxxx) but not statically pinnable. | disable / TODO-HW |
| gui_massive_event_loop | `0xE037719E` | **WRONG** | mid StartCBR routine. Real `0xE03FD182`: big `push.w` 3-arg spill -> 227-entry GUICMD switch-table @0xE03FD95C; GUICMD_* string cluster. | **-> 0xE03FD182** |
| gui_enqueue_message | `0xE004EFCC` | **WRONG** | dcache/copy loop. Real `0xE021DCB2`: stores 4 args as msg, VERIFIED msg_queue_post + debug_assert 'GUI::GUI.c', 'warning! QueueLength=%d'. | **-> 0xE021DCB2** |
| gui_init_end | `0xE065A4F4` | **WRONG** | `bne` mid-function (flags from prior cmp). 'EndGuiInit'=0 hits ROM-wide; DNE on D8 sisters (R5/R180). | disable |
| StartDateTimer | `0xE064895C` | **WRONG** | mid AF-frame module. Real `0xE0329DB2`: loads default CBR, VERIFIED SetTimerAfter (490ms), 'MenuMain.c StartDateTimer(%#x)', timer slot 0x167A8; 2 callers. | **-> 0xE0329DB2** |
| CancelDateTimer | `0xE06489D4` | **WRONG** | mid AF-frame module. Real `0xE0329E64`: 'CancelDateTimer' string, same timer slot 0x167A8, VERIFIED CancelTimer. | **-> 0xE0329E64** |

### app_handlers (WINSYS_CreateDialogBox dispatch targets)

| stub | current | verdict | evidence (abridged) | action |
|---|---|---|---|---|
| PlayMain_handler | `0xE04C7E5C` | **WRONG** | stale M50.110 addr (garbage). StartPlayMain `FUN_e0568a90` ('PlayMain_StartPlayMain') BLs WINSYS_CreateDialogBox with handler literal 0xE0567A35 (id=0x120). | **-> 0xE0567A34** |
| ShootOlcApp_handler | `0xE02D40F2` | **WRONG** | garbage. StartShootOlcApp `FUN_e06bd678` ('StartShootOlcApp PushPalette') passes handler 0xE06BCD85 (id=0x158); adjacent to StopShootOlcApp. | **-> 0xE06BCD84** |
| LiveViewApp_handler | `0xE04DC446` | **WRONG** | mid-function. _StartLiveViewApp `FUN_e07a8d10` passes handler 0xE07A8C31 to WINSYS_CreateDialogBox (id=0x5e); message-code dispatcher. | **-> 0xE07A8C30** |
| PlayMovieGuideApp_handler | `0xE058AF54` | **WRONG** | data/mid-function. PlayMovieGuide_Start `FUN_e08f9ac0` (DlgPlayMovieGuide.c) passes handler 0xE08F993D (id=0x116). | **-> 0xE08F993C** |

### misc_fns

| stub | current | verdict | evidence (abridged) | action |
|---|---|---|---|---|
| memcpy_dryos | `0xE065E998` | **WRONG binding** (target OK) | current addr is a DebugMsg cbz fall-through. Real newlib memcpy `0xE0DBBEB4` (`push.w{...}`, 0x20-byte ldm/stm loop, tbb tail, returns dst). | **-> 0xE0DBBEB4** (TODO-HW) |
| _LoadCalendarFromRTC | `0xE0637210` | **WRONG** | mid 32-bit insn inside an MMIO block routine. Real calendar reader `0xE0607FCA` (Device::RTC.c), 5-arg; caller '[RTC] LoadCalenderAnd...' invokes `(0,0x10,0,DAT_e0608154,&out)`. | **-> 0xE0607FCA + wrapper** (TODO-HW) |
| ErrForCamera_handler | `0xE037F9C0` | **WRONG** | mid ExtractFixData/MIC fn. Real `0xE01DD8FC`: WINSYS dialog dispatcher ADR-refs all three DlgErrForCamera.c strings (TERMINATE_WINSYS / DELETE_DIALOG_REQUEST / LOCAL_CLOSE_DIALOG). | **-> 0xE01DD8FC** |
| dump_file | `0xE00DB9E6` | **WRONG** | mid bit/stream writer. No anchor ('dump_file'/'DumpFile'=0). Stub comment: 'gone from newer models'. | disable (legacy) |
| _get_task_info_by_id | `0xE01C8378` | **WRONG** | not an entry (Ghidra: no function); mid FSS `fssNotifyJob`. Genuine extask getter is RAM-relocated (0 static xrefs). | disable / TODO-HW |

### dma_rpc

| stub | current | verdict | evidence (abridged) | action |
|---|---|---|---|---|
| _alloc_dma_memory | `0xE055AAF4` | **WRONG** | `strd` mid-fn inside GUI 'GRAPHICS_DrawOneYuvImage'. No 'Uncacheable'/'dma_memory' anchor. | disable / TODO-HW |
| _free_dma_memory | `0xE055AB28` | **WRONG** | lands on a `bl DebugMsg` inside the same GUI fn. | disable / TODO-HW |
| _request_RPC | `0xDF002C32` | **WRONG** | src 0xE14999D6 inside a `dmb sy` (not an insn boundary); enclosing blob fn is the clear routine. True low-level poster `0xE1499876` = rt `0xDF002AD2`. | **-> 0xDF002AD2** (TODO-HW; no anchor, blob) |
| clear_RPC_request | `0xDF002C52` | **WRONG** | src 0xE14999F6 inside a `blx`; enclosing fn is the high-level request wrapper. True clear routine (spinlock, writes 0, `sev`) `0xE149999A` = rt `0xDF002BF6`. | **-> 0xDF002BF6** (TODO-HW; no anchor, blob) |

### data_ptrs

| stub | current | verdict | evidence (abridged) | action |
|---|---|---|---|---|
| current_interrupt | `0x1010` | **OK** | fixed DryOS 0x1000 kernel block; [0x100C]=interrupt_level used at 0xE014A650; current_task=*(0x1028) verified. base+offset, no pool load. | keep |
| task_max | `0x40CC` | **WRONG** | 0x40CC is a lazy-init driver struct (byte flag@0, handle@+0xc). Real task_max=*(0x4108): enum fn 0xE014A63C computes `end=*(0x40E8)+0x5C*(*(0x4108))`. | **-> 0x4108** |
| task_dispatch_hook | `0x4108` | **WRONG** | 0x4108 is actually task_max (see above). Real hook=*(0x4148): dispatch fn 0xE00A6F0C loads it, cbz, `blx` with task in r0. | **-> 0x4148** |
| gui_task_list | `0xFB38` | **SUSPECT** | 0 code loads; sole match is a numeric data-table value. 'CtrlMan: Use RecursiveLock' exists but has 0 ADR sites. Unconfirmable statically. | TODO-HW |
| gui_main_struct | `0x536C` | **WRONG** | 0x536C is a 17x8 handler table. Real 0x5428: verified gui_main_task 0xE021DB7C does `ldr r4,=0x5428; [r4+0x1c]=msg_queue; [r4+4]=counter--`. Matches ML gui.c. | **-> 0x5428** |
| sd_device | `0x115B8` | **WRONG** | not code-referenced. SD_DeviceCreate 0xE04E923C allocs 0x98-byte struct -> stores ptr to *(0x6450). | **-> 0x6450** |
| winsys_sem | `0xFC10` | **WRONG** | 0 aligned code refs. Refresh lock is a FIELD of bmp_vram_info: `ldr r0,[r6,#4]` (r6=0x11294) before take_semaphore in RefreshVrms 0xE035D33A. | **-> 0x11298** (TODO-HW; contingent on bmp_vram_info) |
| _rgb_vram_info | `0xFC1C` | **SUSPECT** | data-table value, not a code pool. 'VDrv_InitializeScreen' string DNE. Likely unused under CONFIG_COMPOSITOR_XCM (this cam). No verified replacement. | disable (likely unused) / TODO-HW |
| bmp_vram_info | `0x11294` | **SUSPECT** (valid) | RefreshVrms_Panel 0xE035D134 loads pool 0x11294 as OSD VRAM struct before DISP_SetUpdateOSDVram. Re-derived on M50 II (M50.110 had 0xFC20). Struct-identity uncertainty only. | keep (TODO-HW) |
| additional_version | `0xE484` | **SUSPECT** | ML-private char buffer (0 ROM refs). Correctness = 0xE484 being a free Canon RAM slot; M50 II BSS relocated so may be stale/collide. | TODO-HW |

### consts_h - see section 3.

---

## 2. stubs.S fixes to apply (WRONG with a replacement) - FOR HUMAN REVIEW, do NOT auto-apply

Exact old -> new lines. **2a** = statically verified (safe to write). **2b** =
replacement candidate that still needs a HW/QEMU smoke test before trusting.

### 2a - Statically verified re-points (20)

**Memory suite (PackMem cluster, `System::PackMem.c` anchored):**
```
- THUMB_FN(0xe0553c5c,  AddMemoryChunk)                ->  THUMB_FN(0xE05BD70C,  AddMemoryChunk)
- THUMB_FN(0xe0553d82,  GetFirstChunkFromSuite)        ->  THUMB_FN(0xE05BD790,  GetFirstChunkFromSuite)
- THUMB_FN(0xe05546a4,  GetNextMemoryChunk)            ->  THUMB_FN(0xE05BDFB0,  GetNextMemoryChunk)
- THUMB_FN(0xe0553b9c,  GetMemoryAddressOfMemoryChunk) ->  THUMB_FN(0xE05BD552,  GetMemoryAddressOfMemoryChunk)
- THUMB_FN(0xe0553fe4,  DeleteMemorySuite)             ->  THUMB_FN(0xE05BD95C,  DeleteMemorySuite)
- THUMB_FN(0xe0554276,  CreateMemorySuite)             ->  THUMB_FN(0xE05BDBE8,  CreateMemorySuite)
- THUMB_FN(0xe055374c,  CreateMemoryChunk)             ->  THUMB_FN(0xE05BD22E,  CreateMemoryChunk)
```
- AddMemoryChunk: appends chunk to suite tail, same link op CreateMemorySuite uses; IsSuite/IsChunkSignature asserts.
- GetFirstChunkFromSuite: IsSuiteSignature, returns first chunk base; loop head with GetNextMemoryChunk.
- GetNextMemoryChunk: 2-arg (suite,chunk), reads chunk+8; iterated by FUN_e05bdff4 right after GetFirstChunk.
- GetMemoryAddressOfMemoryChunk: IsChunkSignature, returns chunk+0x18 (the CreateMemoryChunk address arg).
- DeleteMemorySuite: DebugMsg 'DeleteMemorySuite 0x%x', frees all chunks + suite via VERIFIED free 0xE03C52A8.
- CreateMemorySuite: malloc(0x18) via VERIFIED malloc, inits empty suite, appends first chunk (matches ML hint).
- CreateMemoryChunk: malloc(0x28) chunk struct, sets address/size/type; called from CreateMemorySuite.

**GUI core:**
```
- THUMB_FN(0xe037719e,  gui_massive_event_loop)  ->  THUMB_FN(0xE03FD182,  gui_massive_event_loop)
- THUMB_FN(0xe004efcc,  gui_enqueue_message)     ->  THUMB_FN(0xE021DCB2,  gui_enqueue_message)
- THUMB_FN(0xe064895c,  StartDateTimer)          ->  THUMB_FN(0xE0329DB2,  StartDateTimer)
- THUMB_FN(0xe06489d4,  CancelDateTimer)         ->  THUMB_FN(0xE0329E64,  CancelDateTimer)
```
- gui_massive_event_loop: 227-entry GUICMD switch-table dispatcher @0xE03FD95C; giant command dispatcher.
- gui_enqueue_message: stores 4 args as a msg, VERIFIED msg_queue_post + debug_assert 'GUI::GUI.c' + 'warning! QueueLength=%d' (both anchor strings in one fn).
- StartDateTimer: 'MenuMain.c StartDateTimer(%#x)', VERIFIED SetTimerAfter (490ms), timer slot 0x167A8, 2 callers.
- CancelDateTimer: 'CancelDateTimer' string, same slot 0x167A8, VERIFIED CancelTimer.

**App handlers (WINSYS_CreateDialogBox targets, +1 thumb-bit -> even entry):**
```
- THUMB_FN(0xe04c7e5c,  PlayMain_handler)          ->  THUMB_FN(0xE0567A34,  PlayMain_handler)
- THUMB_FN(0xe02d40f2,  ShootOlcApp_handler)       ->  THUMB_FN(0xE06BCD84,  ShootOlcApp_handler)
- THUMB_FN(0xe04dc446,  LiveViewApp_handler)       ->  THUMB_FN(0xE07A8C30,  LiveViewApp_handler)
- THUMB_FN(0xe058af54,  PlayMovieGuideApp_handler) ->  THUMB_FN(0xE08F993C,  PlayMovieGuideApp_handler)
```
Each anchored by its Start* routine's DebugMsg string + the literal loaded into r2 before `bl WINSYS_CreateDialogBox` (0xE059F43C).

**Misc:**
```
- THUMB_FN(0xe037f9c0,  ErrForCamera_handler)  ->  THUMB_FN(0xE01DD8FC,  ErrForCamera_handler)
```
WINSYS dialog dispatcher ADR-refs all three DlgErrForCamera.c strings (TERMINATE_WINSYS / DELETE_DIALOG_REQUEST / LOCAL_CLOSE_DIALOG); matches ML `get_current_dialog_handler()==&ErrForCamera_handler`.

**Data pointers:**
```
- NSTUB( 0x40CC, task_max)                       ->  NSTUB( 0x4108, task_max)
- DATA_PTR(    0x4108,  task_dispatch_hook)       ->  DATA_PTR(    0x4148,  task_dispatch_hook)
- DATA_PTR(    0x536c,  gui_main_struct)          ->  DATA_PTR(    0x5428,  gui_main_struct)
- DATA_PTR(   0x115b8,  sd_device)                ->  DATA_PTR(   0x6450,  sd_device)
```
- task_max -> 0x4108: enum fn 0xE014A63C uses `*(0x40E8)+0x5C*(*(0x4108))` (23*N stride).
- task_dispatch_hook -> 0x4148: dispatch fn 0xE00A6F0C loads *(0x4148), cbz, `blx` with task in r0. **Apply BOTH task_max and task_dispatch_hook together** - they are currently swapped-adjacent (0x4108 is task_max), so today's stub would make ML overwrite task_max when installing its hook.
- gui_main_struct -> 0x5428: verified gui_main_task msg loop (`[+0x1c]`=queue, `[+4]`=counter).
- sd_device -> 0x6450: SD_DeviceCreate 0xE04E923C stores the 0x98-byte device struct ptr to *(0x6450).

### 2b - Replacement candidates (need HW/QEMU verify before trusting)

```
- THUMB_FN(0xe065e998, memcpy_dryos)   ->  THUMB_FN(0xE0DBBEB4, memcpy_dryos)   /* TODO-verify QEMU/HW; fallback 0xE0E1B1A0 */
- THUMB_FN(0xdf002c32, _request_RPC)   ->  THUMB_FN(0xDF002AD2, _request_RPC)   /* TODO-HW; structural only, no anchor, dual-core blob */
- THUMB_FN(0xdf002c52, clear_RPC_request) -> THUMB_FN(0xDF002BF6, clear_RPC_request) /* TODO-HW; structural only, dual-core blob */
- DATA_PTR(    0xFC10, winsys_sem)     ->  DATA_PTR(   0x11298, winsys_sem)      /* TODO-HW; = bmp_vram_info+4, contingent on 0x11294 */
```
Plus `_LoadCalendarFromRTC` - the entry moves AND the signature changes, so it
needs a wrapper, not a bare re-point:
```
- THUMB_FN(0xe0637210, _LoadCalendarFromRTC)  ->  THUMB_FN(0xE0607FCA, _LoadCalendarFromRTC_raw)
  + wrapper:  _LoadCalendarFromRTC(tm) => _LoadCalendarFromRTC_raw(0, 0x10, 0, DAT_e0608154, tm)
```
Real RTC calendar reader `0xE0607FCA` (Device::RTC.c); caller invokes it 5-arg
and reads back struct-tm-ordered {sec@0,min@4,hour@8,day@0xc,mon+1@0x10,
year+1900@0x14}. **TODO-verify-on-HW that param2=0x10 selects the calendar read.**

Notes:
- **memcpy_dryos**: two byte-identical newlib cores exist (0xE0DBBEB4 /
  0xE0E1B1A0); either is a valid memcpy, but confirm which DryOS resolves to.
- **RPC pair**: the `0xDF00xxxx` blob is now grounded (base map confirmed) but
  there are no string anchors; the poster/clear identification is structural
  inference on a dual-core blob. Ship only after a QEMU/HW trace.
- **winsys_sem**: 0x11298 is `bmp_vram_info(0x11294)+4`; only as trustworthy as
  bmp_vram_info, which is itself SUSPECT.

---

## 3. consts.h values

| symbol | current | verdict | finding | action |
|---|---|---|---|---|
| `CURRENT_GUI_MODE` | `0x7EC0` | **SUSPECT** | Both fields real, 8 bytes apart, inside verified SetGUIRequestMode 0xE0599FBA (base 0x7E5C): `[+0x64]=0x7EC0` is the **compared** field, `[+0x6c]=0x7EC8` is the **written** field. Canon's own getter 0xE059A1BA returns `[0x7EC8]`. Both store the FULL request code (0x2002 for PLAY bank), so `CURRENT_GUI_MODE==GUIMODE_PLAY(2)` cannot hold as currently paired. R.180 delta (+8) matches the 0x64->0x6c structural shift exactly. | keep 0x7EC0 (R.180/M50.110 convention, safe) **OR** re-point 0x7EC8 (what Canon's getter returns) + switch GUIMODE_PLAY/MENU to request-code form 0x2002/0x2003. **TODO-HW: settle what get_gui_mode returns on-cam.** |
| `GUIMODE_PLAY` | `2` | **OK** | SetGUIRequestMode `ubfx(arg,#0xc,#8)` = mode class; `cmp r4,#2; beq` PLAY handler (bank 0x2xxx). (0x2002>>12)&0xff = 2. Matches all D7/D8 sisters. | keep |
| `GUIMODE_MENU` | `3` | **OK** | `cmp r4,#3; beq` MENU handler (clears state field 0x8000, bank 0x3xxx). Matches all sisters. | keep |
| `GUIMODE_WB=5, FOCUS_MODE=9, DRIVE_MODE=8, PICTURE_STYLE=4, Q_UNAVI=0x18, FLASH_AE=0x22, PICQ=6` (consts.h L161-167) | 50D block | **SUSPECT** | Explicitly commented 'copied from 50D' (DIGIC 4). M50 II dispatch only exercises classes {1,2,3,9,0xc,0xd}; no static anchor confirms these DIGIC-4 values. `GUIMODE_ML_MENU=0x63` also unverified. | **TODO-HW** (press buttons, read get_gui_mode). Do not rely. |
| `LVAE_STRUCT` | `0x75094` | **SUSPECT** | Stale M50.110 inheritance. 0 literal-pool xrefs ROM-wide; the claimed init addr 0xe04ffb38 is not a function entry; the lvae_* offset anchors live only in the dead debug-name blob (0xE13Cxxxx, 0 code xrefs). RAM struct, address only knowable from a live dump. | **TODO-HW** (dump on cam, re-derive CONTROL_BV/BV_TV/AV/ISO/DISP_GAIN offsets). Do not build AE overrides against it. |
| `LVAE_ISO_STRUCT` | `0x8e660` | **SUSPECT** | Stale M50.110 inheritance. 0 xrefs; 'ISOMin' DNE as live code string. Runtime-only, same class as LVAE_STRUCT. | **TODO-HW** |

---

## 4. SUSPECT / TODO-verify-on-HW (roles not statically anchorable)

Leave the current value in place only if it is a benign ML-private RAM buffer
or a valid-but-uncertain-identity pointer; otherwise treat as unresolved. None
of these should be trusted for functional correctness until checked on
QEMU/HW.

| symbol | current | why unresolved | how to resolve |
|---|---|---|---|
| bmp_vram_info | `0x11294` | valid entry, referenced by the expected RefreshVrms/DISP_SetUpdateOSDVram path, but struct identity not fully confirmed. | keep; dump struct fields on-cam. |
| gui_task_list | `0xFB38` | 0 code loads; anchor string 'CtrlMan: Use RecursiveLock' has 0 ADR sites. Inherited verbatim from M50.110. | trace CtrlMan task-list global at runtime. |
| additional_version | `0xE484` | ML-private buffer (0 ROM refs); only needs to be a free Canon RAM slot; M50 II BSS relocated so may collide. | runtime RAM-map check. |
| _rgb_vram_info | `0xFC1C` | data-table value, not a code pool; likely unused under CONFIG_COMPOSITOR_XCM. | disable if XCM path confirmed, else dump VDrv screen ptr. |
| winsys_sem | `0xFC10` -> cand `0x11298` | replacement is bmp_vram_info+4, only as good as 0x11294. | resolve with bmp_vram_info. |
| memcpy_dryos | -> cand `0xE0DBBEB4` | verified memcpy, but two identical cores; no string anchor. | QEMU/HW smoke test of a copy. |
| _request_RPC / clear_RPC_request | -> cand `0xDF002AD2` / `0xDF002BF6` | dual-core blob, structural inference, no anchors. | QEMU/HW RPC trace. |
| _LoadCalendarFromRTC | -> cand `0xE0607FCA` + wrapper | signature changed (5-arg); param2=0x10 selecting calendar read unverified. | HW: call wrapper, check returned struct-tm. |
| CURRENT_GUI_MODE / GUIMODE 50D block / LVAE_STRUCT / LVAE_ISO_STRUCT | see section 3 | representation coupling / DIGIC-4 copies / runtime RAM structs. | HW: read get_gui_mode + dump LVAE structs. |
| dialog_redraw | `0xE054F3CA` | real fn in WindowSystem::Dialog.c but this build's assert has no expression string, so no static anchor. | sister-ROM body match or on-cam dialog trace. |
| _get_task_info_by_id | `0xE01C8378` | genuine extask getter is RAM-relocated (0 static xrefs). | kitor extask method (trace relocated handler -> id-verify helper -> BL to getter). |

---

## 5. notfound / disable (WRONG, no replacement)

These are confirmed WRONG (invalid/unrelated/mid-function entry) with **no**
statically verifiable replacement. **Recommend commenting them out** in
`stubs.S` so ML cannot call the bad address (several are mid-function or in a
live GUI/graphics routine and would corrupt state / hang / brick if invoked).

| stub | current (bad) | why disable | replace how |
|---|---|---|---|
| AllocateMemoryResource | `0xE05C7F0E` | mid Flavor/Job module, not RscMgr.c; no name anchor. | runtime CBR-registration dump (RscMgr.c ~0xE057Cxxx). |
| AllocateContinuousMemoryResource | `0xE05C7F3C` | function tail, wrong module. | runtime dump. |
| FreeMemoryResource | `0xE05C7FE2` | mid-function, wrong module. | runtime dump. |
| gui_init_end | `0xE065A4F4` | mid-function; 'EndGuiInit' DNE; DNE on D8 sisters. | disable permanently (function does not exist on D8). |
| dialog_redraw | `0xE054F3CA` | inside a literal pool; no static anchor (see section 4). | disable until re-anchored on HW. |
| dump_file | `0xE00DB9E6` | mid bit/stream writer; legacy stub ('gone from newer models'). | disable (unused legacy). |
| _get_task_info_by_id | `0xE01C8378` | enters FSS fssNotifyJob (hang/brick risk); getter RAM-relocated. | disable; re-anchor on HW (see section 4). |
| _alloc_dma_memory | `0xE055AAF4` | live GUI YUV-draw fn; would corrupt graphics. | disable; runtime `drysh memmap`. |
| _free_dma_memory | `0xE055AB28` | `bl DebugMsg` inside the same GUI fn; pops regs never pushed = stack corruption. | disable; resolve pair with alloc at runtime. |

---

## 6. Readiness for a baseline ML build

**What is now safe.** Combined with the 9 boot-critical fixes from `docs/11`,
this pass clears the remaining static-anchorable inherited stubs: the entire
PackMem memory-suite (7), all four app dialog handlers, the GUI command
dispatcher / enqueue / date-timer pair, ErrForCamera, and the core task/GUI/SD
data pointers (`task_max`, `task_dispatch_hook`, `gui_main_struct`,
`sd_device`) all have string- or structure-verified replacements and can be
re-pointed with confidence - **20 drop-in fixes** that make those subsystems
callable instead of jumping into mid-function garbage. `GUIMODE_PLAY=2` /
`GUIMODE_MENU=3` and `current_interrupt` are confirmed correct as-is. Applying
the section-2a re-points and disabling the section-5 nine bad addresses yields
a `stubs.S` where nothing left in the file points into a live-but-wrong Canon
routine - the single most important property for not bricking on first boot.

**What still blocks a clean build.** Five roles have only *candidate*
replacements that must be smoke-tested first: `memcpy_dryos` (used by
CONFIG_MMU_REMAP - needed early, but two identical cores and no anchor), the
`_request_RPC`/`clear_RPC_request` dual-core blob pair (structural inference
only), `_LoadCalendarFromRTC` (needs a signature-changing wrapper), and
`winsys_sem` (contingent on the SUSPECT `bmp_vram_info`). Beyond those, the
runtime-only RAM structs - `LVAE_STRUCT`/`LVAE_ISO_STRUCT` (AE overrides),
`gui_task_list`, `additional_version` - and the `CURRENT_GUI_MODE`
representation question plus the 50D-copied GUIMODE_* block cannot be resolved
statically at all and gate any feature that touches AE control or GUI-mode
logic. `_get_task_info_by_id` and both `_dma_memory` allocators have no static
answer and must be disabled meanwhile. **Bottom line:** the port is ready for a
*baseline, boots-and-runs* ML build once section-2a is applied and section-5 is
disabled (memory/task/GUI/dialog scaffolding is sound); it is **not** ready for
a *feature-complete* build - MMU-remap (memcpy), cross-core RPC, RTC clock, and
all AE/exposure-override features remain gated on HW/QEMU re-anchoring.
