# 26 — Display / OSD / Raw-Video Frontier (M50 II 1.0.1)

Reverse-engineering + phased plan for the last big port goal: on-screen ML OSD, then
LiveView, then raw video. Grounded in `gang100.bin` (Canon 1.0.1) RE + the Magic Lantern
Discord findings (`docs/07-discord-findings.md`) + sibling ports.

## 0. Compositor path: XCM, settled by ROM evidence (not the Discord M50-mkI analogy)

Two sources disagreed; direct ROM evidence wins:

| Source | Claim |
|---|---|
| Discord (kitor, ~2020) | "M50 has NO XCM, pure Ximr, 200D-like" — but that is the **mk I** |
| **gang100.bin RE (this ROM)** | ROM contains the **full XCM API**: `XCM_SetSourceSurface`/`GetSourceSurface`/`SetSourceArea`, `XOC_SetLayerEnable`, `XCMStatus`, and **`VMIX_InitializeScreen xcmNum=%d`** |
| M6II.111 (contemporaneous D8 M-series) | uses `CONFIG_COMPOSITOR_XCM_V2` |

**Verdict: M50 II is XCM.** The mk II's "software-only differences vs mk I" (Clean HDMI,
25fps) plausibly include the upgraded compositor. Reference = **M6II.111**, not M50.110.

## 1. RE'd addresses (M50 II 1.0.1, gang100.bin) — Phase A

All confirmed by decompilation. Debug strings are the anchors (present in-ROM).

| Symbol | Address | Evidence |
|---|---|---|
| `XCM_GetSourceSurface` | `0xE00BE348` THUMB | `*(ctx + layer*0x24 + 0x974)`, asserts layer<4, handle!=0 |
| `XCM_SetSourceSurface` | body from `0xE00BE092` (Ghidra merges into `FUN_e035d590`) | `(screenIdx 0-2, layer 0-3, MARV)`; resolves ctx from table |
| `XCM_SetSourceArea` | `0xE00BE37C` | `(ctx*, layer, x,y,w,h)` -> writes ctx+layer*0x24+0x978.. |
| `XOC_SetLayerEnable` | ref site `0xE00EA884` (entry TBD) | string `XOC_SetLayerEnable Illegal handle` |
| `refreshVrmsSurface` / `VMIX_TransferRectangleToVram` | `0xE035D134` | prints both debug strings; the top-level OSD recomposite |
| `XimrExe` (mzrm submit -> Zico) | `0xE018B9F4` THUMB | allocs msg, copies **0x31c-byte** Ximr blob, sends+waits *(already in stubs.S)* |
| XCM screen-ctx ptr table | RAM `0x7F35C` | `*(0x7F35C + screen*4)` = ctx object ptr |
| VMIX primary ctx | RAM `0x11294` | refreshVrms main ctx: +4 sem, +8 rows, +C cols, +10 dbl-buf, +18 dirty |
| `winsys_sem` handle | RAM `0x11298` | = *(0x11294+4). **Corrected in stubs.S from the stale mk1-copy `0xFC10`** |
| RENDERER surf array (alt path) | RAM `0x7F1B4` | prior RE: `_rgb_vram_info` source; `CreateMARV`=`FUN_e0186278`, `RENDERER_InitializeScreen`=`FUN_e02c9112`; cache @`0x11264` |

`struct MARV` (non-DIGIC_X): +0 sig(`0x5652414D`) +4 bitmap_data +8 opacity +C flags
+10 width +14 height +18 pmem.

## 2. Two complications (why Phase A is a probe, not yet a working OSD)

1. **Two parallel display layers**: RENDERER (surf array `0x7F1B4`) and XCM (ctx table
   `0x7F35C`). Which one holds the MARV we can draw into must be settled empirically.
2. **XCM setter API is signature-inconsistent**: `GetSourceSurface(ctx*, layer)` takes a
   context **pointer**, but `SetSourceSurface(screenIdx, layer, MARV)` takes a screen
   **index** and resolves the ctx from `*(0x7F35C+idx*4)`. So the M6II `CONFIG_COMPOSITOR_XCM_V2`
   path in `src/compositor.c` (which passes `_pXCM` to all three) does **not** drop in
   unchanged, and a blind `XIMR_CONTEXT` would crash `XimrExe` (Discord finding #333:
   "a copied sibling XCM const crashes XimrExe").

Also: M50 II `refreshVrmsSurface` is **chunked/mzrm-based** (per-chunk `FUN_e018b9f4`
submit), not a single `XimrExe(ctx+0x10)` like M6II — so the OSD refresh should prefer
setting `display_refresh_needed` and letting Canon's ~30fps recomposite (Discord #331),
rather than a manual `XimrExe`.

## 3. Phase A — display ground-truth probe (BUILT, flashed)

`src/init.c :: m50ii_display_probe()` (guarded `CONFIG_M50mk2`), called after the
big.1b tier1-diag yield. **Safe by construction** (reads + Canon read-only evprocs only;
no buffer writes, no manual XimrExe -> cannot hang Zico). Captures over UART:
- RENDERER `*(0x7F1B4)` MARV (sig/bitmap/flags/w/h) + cache `0x11264`
- XCM `ctx_tbl[0..2]` @`0x7F35C`; for `ctx_tbl[0]`, `XCM_GetSourceSurface(ctx,0..3)` MARVs
- VMIX dirty flag, `winsys_sem`
- `call("XCMStatus")` + `call("VramState")` — Canon's own full layout dump

**HW test = capture UART** (established workflow, `/dev/ttyUSB0`). The dump tells us which
structure is live + real geometry -> iteration 2 binds `rgb_vram_info` correctly and adds a
dedicated ML layer.

## 4. Phase B — LiveView + un-NOP EDMAC (RE DONE; un-NOP deferred to post-A-HW)

`platform/M50mk2.101/function_overrides.c` NOPs the EDMAC/engio set for HELLO_WORLD.
RE'd ROM addresses (gang100.bin, THUMB — stub as `THUMB_FN(addr, name)`; runtime = `addr|1`):

| ML name | M50II addr | conf | note |
|---|---|---|---|
| `_engio_write` | `0xE06CAEB2` | **high** | batch reg writer, `{addr,val,…,0xFFFFFFFF}`; assert `EngineDS::DSEngIO.c` |
| `_EngDrvOut` | *(wrap over `_engio_write`)* | n/a | no distinct single-reg writer on D8 (as R.180) — emit a 2-entry list |
| `shamem_read` | *(absent on D8)* | n/a | no shadow memory; keep NOP/return 0 or read `0xD04xxxxx` MMIO directly |
| `SetEDmac` (geometry) | `0xE05B849A` | med | writes `edmac_info` geom to `mmio[ch]+0x48..0x70`; **omits DMA addr** |
| `SetEDmac` (addr+geom variant) | `0xE05B8AE4` | med/low | 4-arg incl. addr→`+0xa4`, size→`+0xa8`; arg order (size vs flags) differs from ML |
| `StartEDmac` | `0xE05B802E` | med | `(ch,flags)`; sets running `+0xb4=2`, BoomerSelector connect |
| `AbortEDmac` | `0xE05B7B42` | med | full abort; simpler stop variant `0xE05B80C2` |
| `RegisterEDmacCompleteCBR` | `0xE05B7F1E` | med-high | `(ch,cbr,ctx)`; sets `mmio[ch]+0x3c cbr_registered` |
| Unregister*CBR | `0xE05B7FB6` / `0xE05B7FDE` | low | Complete/Abort/Pop not disambiguated |
| `ConnectWriteEDmac` | `0xE05B93C4` | **low** | connect cluster (assert `Edmac.c`:0x6cb); sig doesn't match ML `(ch,where)` |
| `ConnectReadEDmac` | `0xE05B946E` | **low** | read-path sibling (0x6eb) |

Structural anchors: EDMAC channel→register table @ ROM `0xE0EC37AC` ({MMIO_base,flags}×N);
per-channel MMIO bases in **`0xD04xxxxx`** (ch0=`0xD0404000`, ch1=`0xD0420000`, …);
connection primitive `FUN_e0683ca0` (`Engine::BoomerSelector.c`); assert helper
`FUN_e0674574(0,file,line)`.

**Deferred deliberately:** the un-NOP is NOT staged into code yet — (a) `SetEDmac`/`Connect*`
are med/low confidence and need one disambiguation pass (trace a real raw/LV caller that
connects a known read+write channel pair) before committing; (b) un-NOPing EDMAC is
HW-risky and must not stack on an unvalidated Phase A. Do it as a deliberate step behind
`#define M50II_ENABLE_EDMAC` once Phase A is HW-confirmed. Discord D8 gotchas: cache
`x | 0x40000000` + 0x20-align, drop `& 0x1FFFFFFF` (M50 II = 1GB).

## 5. Phase C — Raw backend (scaffold + doc; core needs the fork)

`src/raw.c` wants: `CONFIG_RAW_LIVEVIEW`, `CONFIG_EDMAC_RAW_SLURP`,
`RAW_LV_EDMAC_CHANNEL_ADDR`, `RAW_TYPE_REGISTER`, `PREFERRED_RAW_TYPE`, sensor geometry.
Donor = bilal's working **original-M50 raw branch** = kitor `edmac-d8` (SX740 lineage) +
`realloc_buffers()`. That core is **not in this tree** — must be ported from the fork.
Discord: RAW sensor geometry `0xC0F06800/0804`; PACK32 bit-depth `D041xxxx` MMIO (14=0x20);
LV raw via `lv_save_raw`/`lv_set_mm`; find active EDMAC channel; EDMAC #9 purple fix.

## 6. Phase D — Raw recording (scope only)

Enable `mlv_lite`, port `realloc_buffers()` 2nd-clip freeze fix, card write-speed check.
Depends on C.

## 7. GUI input / interactive menus (RE DONE, fix staged — build iter5-gui-849b7eed)

**Goal:** get button events to ML's `handle_buttons` so the (already-rendering) OSD menu is navigable.

**Blocker was:** `src/gui.c` had `M50II_BYPASS_GUI_OVERRIDE=1`, disabling `TASK_OVERRIDE(gui_main_task, ml_gui_main_task)` — so ML never saw buttons. It was disabled during the big.5 hunt on the theory that ML's override mis-dispatched a "DIGIC-8 5-word / 0x14-byte GUI message needing a 2nd handler param."

**RE verdict (Canon `gui_main_task` @0xE021DB7C, gang100.bin):** created the function (Ghidra didn't auto-define it; xref from `GMT_FUNCTABLE` 0xE099E958 confirms it is the dispatch loop). Disassembly:
```
ldr r1,[0xe021df78]            ; r1 = GMT_FUNCTABLE (=0xE099E958, verified in literal pool)
movs r2,#0x1c ; blx memcpy     ; copy 0x1c = 7 funcs to stack   (GMT_NFUNCS=7)
ldr r4,[0xe021df7c]            ; r4 = gui_main_struct (=0x00005428, SAME global as ML)
loop:
  ldr r0,[r4,#0x1c] ; bl msg_queue_receive(&event,0)   ; QUEUE @ struct +0x1c  = msg_queue_eosr
  ldr r0,[r4,#0x4]  ; subs #1 ; str  [r4,#0x4]          ; counter @ +0x4        = counter_550d
  ... if event==NULL: re-receive
  ldr r0,[event,#0] ; cmp #7 ; (>=7 -> assert GUI.c:0x354)
  ldr r0,[sp,#0x1c](event); ldr r1,[r0,#0](type); ldr r1,funcs[type]; blx r1   ; funcs[type](event)
```
So dispatch is **`funcs[event->type](event)` with a SINGLE argument (event in r0)** — identical to `ml_gui_main_task`. **The 2nd-param theory was wrong.**

**Actual defect:** `ml_gui_main_task` grouped `CONFIG_M50mk2` with the DIGIC-7 `CONFIG_M50` branch, which receives from `gui_main_struct.msg_queue_m50` at **+0x18**. The D8 M50 II uses **+0x1c** (`msg_queue_eosr`), like R/RP/850D/R5/M6II. The override drained the wrong queue → real button events (queued at +0x1c) were never consumed and malformed dispatches drove the big.5 GraphicsConvertPosType crash (so disabling the override "fixed" big.5 as a side effect, alongside the separately-fixed DMA stub).

**Fix (this build):** move `CONFIG_M50mk2` to the +0x1c/`msg_queue_eosr` branch; set `M50II_BYPASS_GUI_OVERRIDE=0`; set `M50II_GUI_TRACE=1` (logs first 128 received events' type/param over UART, relocated to fire on every received event before ML consumes any, for keycode capture). Override-entry helpers verified real: `gui_init_end`=0xE065A4F2; `GMT_FUNCTABLE[0..6]` = 0xE021E461,0xE021E4B1,0xE021E50D,0xE021E541,0xE021E57F,0xE021E587,0xE021E58D (all valid THUMB).

**HW acceptance:** (a) reach big.8 with override ON, no abort ⇒ queue fix correct; (b) `[ML] gui.ev type/param` lines on button press ⇒ events flow to ML; (c) harvest real keycodes from the trace. **Next iteration (needs the trace):** fix `platform/M50mk2.101/gui.h` button placeholders (TRASH/joy/zoom/LV/wheel), pick the menu-open button, and correct `GUIMODE_ML_MENU` (0x63 is a guess) to actually open/navigate the menu. Fallback if big.5 returns with override ON: set `M50II_BYPASS_GUI_OVERRIDE=1` (one #define).

## 8. Boot-time fix (init.c) — ~91 s → ~30 s

HW UART decode: real boot `pre.0`→`big.8` = 91 s, of which **63 s was three blind timeout loops** in `boot_post_init_task` that never satisfy their condition on this silicon:
- `post.0c` `rgb_vram_preinit()` — 50 iters ≈ 16 s, always returns 0.
- `post.1` `bmp_vram_raw()` — 100 iters ≈ 36 s, always 0x0 (D8 never populates the mk-style `bmp_vram_raw`).
- `post.2→3` `ml_gui_initialized` — 50 iters ≈ 11 s, only set once a type-0 event flows through the override.

All three are bring-up nets the XCM/OSD path doesn't need (`osd_try` builds its own MARV after `ml_started`). Caps cut to 3/3/5 (each still "proceed on timeout"). `m50ii_busy_wait(N)` = N×0x40000 spins ≈ N×6 ms. The remaining ~26 s (`big.0`→`big.8`) is legitimate init (fonts, patch apply) — optional later follow-up.

## 9. The actual enabler: task-dispatch hook (§7's queue fix was inert without this)

HW test of §7 (build iter5) reached big.8 with no crash but produced **zero** `gui.ev` traces — `ml_gui_main_task` never ran. Root cause: `TASK_OVERRIDE(gui_main_task)` is driven by `my_task_dispatch_hook` (init.c), installed by writing the `task_dispatch_hook` global — but that install was **disabled** on the M50 II (init.c:1484) because the stub address `0x4108` was a blind R180 copy that is actually Canon's `task_max` global (writing it → the LED-13 DryOS panic). With the hook uninstalled, **no** `TASK_OVERRIDE` fires, so ML never replaces Canon's `gui_main_task` and never sees buttons (the OSD still draws via timer-driven global-draw, which is why the histogram/level appeared).

**RE (gang100.bin):** the real per-context-switch hook slot is **`0x4144`**. The scheduler `FUN_e01b1d82` does `if (*(fn*)0x4144) (*(fn*)0x4144)(1, old, next); … current_task(0x1028)=next` (literal `DAT_e01b2128 = 0x4144`). Every struct-task offset it touches — `context@0x54`, `entry@0x0c`, `cpu_assigned@0x51`, `cpu_requested@0x50`, `yieldRequest@0x4b` — matches ML's `CONFIG_TASK_STRUCT_V2_SMP struct task`. `CONFIG_NEW_DRYOS_TASK_HOOKS` is active for M50mk2 (config-defines.h → internals-common.h ← CONFIG_DIGIC_VIII), so the hook already reads `next` from the 3rd arg (this scheduler's convention).

**Complication:** unlike R180's free slot, `0x4144` on the M50 II is Canon's **dynamic task-switch list-walker** slot (`FUN_e012c6f4` installs a walker when a callback registers; `FUN_e01640e8` clears it when the list empties; slot 2 / `0x4148` is a separate hook). So a blind write would clobber Canon / be clobbered back.

**Fix (build iter6-taskhook):** `stubs.S` `0x4108→0x4144`; a dedicated minimal `m50ii_task_dispatch_hook` (init.c) that **chains** to any saved Canon walker (guarded to ROM ptrs) then does the `TASK_OVERRIDE` entry-swap on first dispatch (`context->pc==task_trampoline`); gated install (`M50II_INSTALL_TASK_HOOK`, default 1) at `pre.5` that logs `*0x4144`/`*0x4148` and installs with save+chain. Kept minimal (no tskmon) to isolate the GUI override for bring-up. **QEMU: big.8, no abort, `INSTALLED @0x4144 (chained)`, `*0x4144 prev=0` (QEMU registers no walker); no override fired because QEMU doesn't create `gui_main_task` — HW-only verifiable.** HW win = `[ML] task-hook: overrode entry for task GuiMainTask` → then `gui.ev` traces on button press. Fallback: `M50II_INSTALL_TASK_HOOK=0`.
