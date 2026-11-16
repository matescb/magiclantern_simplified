# 33 — M50 II menu/button fixes: input exclusivity + REC-to-record (analysis + plan)

Branch `menu`. Extends `docs/28` (working menu-input). Status: **analysis + RE done offline;
live-harness confirmation + implementation pending (camera was stalled at time of writing).**

## The five reported issues
1. Start ML **raw recording with the physical REC button**.
2. **Up/Down don't work at the top menu** until a submenu is opened.
3. **Second FN press doesn't close** the ML menu.
4. **MENU (to exit ML) also opens Canon's config** menu.
5. **Buttons trigger Canon functions while inside ML** (input leaks through).

## Root cause (grounded in code + RE)
Current input path (`docs/28`, `src/gui.c`, `src/init.c`) = **read-only ring peek**:
`m50ii_input_task` (init.c) polls Canon's event ring (RAM `0xBAE08`, widx `0x543C`) and calls
`m50ii_handle_canon_gui_event()` (gui.c) — but **Canon's own `gui_main_task` still dispatches every
event too.** There is **NO input exclusivity**: the piggyback `SetGUIRequestMode(GUIMODE_MENU)` that
normally provides it was made a **no-op** (docs/28 fix #4 — it detonated the EVF). The Phase-4 note in
`gui.c:329` is STALE: it claims piggyback still runs; it does not. So Canon reacts to every ML button.

=> **#4 and #5 are this leakage directly.** #2 and #3 are *most likely* downstream (Canon reacting to
ML nav — shifting its GUI mode, and keycodes are Canon-GUI-MODE-DEPENDENT per docs/28 §1, so a shift
breaks the UP/DOWN/FN map) — but this is a HYPOTHESIS to confirm with a trace before coding, because #2
could instead be an ML menu-state bug.

## RE findings (gang100.bin, M50 II 1.0.1)
- **Canon `gui_main_task` @0xE021DB7C**: copies the handler table to a **stack array ONCE at task
  entry** (`memcpy(stack, GMT_FUNCTABLE, 0x1c)` before its loop), then dispatches `funcs[event->type]()`.
  => Patching the ROM **table pointer** after boot does NOTHING (Canon holds a stack copy). The reliable
  interception is a **code patch of the handler `funcs[0]` at `0xE021E460` (Thumb)**.
- **`funcs[0]` @0xE021E460** is a clean Thumb function:
  `mov r1,r0 / ldr r0,[pc] / push {r3,lr} / ...` — standard prologue, **cleanly hookable**.
  Trampoline: patch entry -> ML wrapper; if `gui_menu_shown()` -> `bx lr` (drop the event); else replay
  `mov r1,r0` + absolute-load r0 + `b 0xe021e464`. Install via the already-wired `apply_normal_patches`.
- Key addrs (autoexec, Jul-14 build, matches flashed `ver` 2026-07-14): `m50ii_handle_canon_gui_event`
  `0xd7c81`, `gui_menu_shown` `0xd4ea9`, `gui_open_menu` `0xd4f9d`, `gui_sem` `0x1477a0`,
  `ml_started` `0x14efd4`. ROM: `gui_main_task` `0xE021DB7C`, `funcs[0]` `0xE021E460`,
  `GMT_FUNCTABLE` `0xE099E958`, `gui_enqueue_message` `0xE021DCB2`.

## In context of other ML cameras
Every other port — including the D8 siblings **R/RP/M6II/850D** (`gui.c:375`) — **overrides
`gui_main_task`** and drains `msg_queue_eosr`, which naturally **consumes** events => exclusive input
for free. The **M50 II is the outlier**: that override faulted (iter9-14, EVF cascade), so it uses the
passive peek => the leakage. We do NOT revive the override (high-risk, would regress the working menu);
we add the narrowest consumption (the `funcs[0]` gate) to reach parity.

## Plan (phased)
- **Phase 0 — Diagnostic (1 build):** re-enable `M50II_MENU_INPUT_TRACE` + `[ML] BTN`; capture
  keycodes/events in every state (menu closed / top-level open / submenu open / REC press). Decode with
  `mllog.py fio|markers`. Disambiguates #2/#3 (keycode-shift vs ML-state vs Canon-interference) and gets
  the REC keycode+path for #1. **Nothing else coded until this exists.**
- **Phase 1 — Exclusivity (core fix #4/#5, likely #2/#3):** hook `funcs[0]` @0xE021E460, drop button
  events when `gui_menu_shown()`. Confirmed hookable (above).
- **Phase 2 — REC -> ML raw (#1):** map the REC keycode to mlv_lite `raw_start_stop` (via module
  keypress CBR or a registered start/stop), suppress Canon h264 REC (same exclusivity gate / REC-specific).
- **Phase 3 — Residual nav fixes:** driven by Phase-0 data (alt keycodes per mode, or menu.c state fix).

## LIVE test READY (no flash needed — run when the camera responds)
The bridge is callable and the ring is injectable via the harness, so #2/#3 can be tested IN ISOLATION
(no physical buttons; bypasses Canon's queue so it tests ML's handling cleanly):
```
python3 mlserial.py cmd "call 0xd4ea9"                 # gui_menu_shown() -> expect 0 (closed)
python3 mlserial.py cmd "call 0xd7c81 0x0 0x22 0x0"    # inject FN press  -> bridge -> gui_open_menu
python3 mlserial.py cmd "call 0xd4ea9"                 # -> expect 1 (opened)
python3 mlserial.py cmd "call 0xd7c81 0x0 0x22 0x0"    # inject FN again  -> should TOGGLE closed (#3)
python3 mlserial.py cmd "call 0xd4ea9"                 # -> 0 if toggle works; 1 => #3 is a toggle bug
# with menu open: inject UP 0x40 / DOWN 0x42, observe menu selection / redraw (#2)
```
If injected FN toggles and injected UP/DOWN move the selection, the bridge logic is CORRECT and the
real-HW #2/#3 are Canon-leakage/keycode-shift => Phase 1 fixes them. If injected events DON'T work, it's
an ML-side bridge/menu-state bug independent of Canon. **This is the first thing to run when live.**

## Blocker at time of writing
Camera unresponsive (stalled by the mlv_lite record-test RawPro assert in the other branch); daemon
alive but no camera replies. Needs a power cycle for the live tests. Autoexec on card = Jul-14 build
(menu-input working, df8ea890 lineage); menu fixes are core-ML (autoexec), separate from mlv_lite.mo.

## ★ 2026-07-16 GHIDRA DEEP-DIVE — leakage engine RE'd; #4/#5 root-caused at instruction level; #1 designed
No HW needed for this — all from gang100.bin.

**The leakage path is fully traced:**
`Canon gui_main_task @0xE021DB7C` (stack-copies GMT_FUNCTABLE once, dispatches `funcs[type]()`)
-> `funcs[0] @0xE021E460` (keycode = event->[4]; per-keycode table @*0x33264; gui_main_struct @0x5428;
   tail-calls) -> **`gui_massive_event_loop @0xE03FD182`** = the keycode->GUICMD->WinSys translator
   (1376 B; created+decompiled). Big `switch(keycode)`: each case sets a GUICMD and calls
   `FUN_e0680bb0(cmd, ...)` = GuiEventPostToWinSys. THIS is what makes Canon's UI react.

**#4 CONFIRMED at instruction level:** `case 0xc` (MENU) -> `FUN_e03ffcde(0xc,..)`, GUICMD **0x83e**,
posted to WinSys -> Canon opens its menu. Leaked MENU => Canon config. Blocking funcs[0] kills it.
**#5 CONFIRMED:** nav keys 0x40/0x42/0x3c/0x3e -> GUICMD 0x80e/0x810/0x80a/0x80c -> WinSys (AF/UI move);
FN 0x22 -> `default:` -> "EVENT_CATEGORY_CMD" -> WinSys (M.Fn). Every leaked key drives WinSys.
**#2/#3 MECHANISM (RE-supported):** a leaked FN/nav makes WinSys change the Canon UI state; keycodes are
physical->keycode mapped per-UI-mode (upstream of gui_enqueue), so the shift makes the 2nd FN / the nav
keys stop matching ML's map. => Blocking funcs[0] freezes Canon's mode while the ML menu is open =>
keycodes stay constant => #2/#3 resolved IF they're leakage-driven (residual: a pure ML menu-state bug,
settled by the live isolation test). The keycode->GUICMD switch here is FIXED (not mode-dependent); the
mode-dependence lives in the physical->keycode driver (a caller of gui_enqueue @0xE021DCB2; the e041xxxx
cluster) — RE'able later to enumerate per-mode keycodes if the isolation test shows it's needed.

**EXCLUSIVITY FIX (Phase 1) — confirmed sound & surgical:** patch funcs[0] @0xE021E460 entry -> ML
wrapper: `if (gui_menu_shown()) return;` else replay `mov r1,r0` + absolute-load r0 + `b 0xe021e464`.
Stops the ENTIRE dispatcher/WinSys chain for buttons while the menu is open. Install via apply_normal_patches.

**#1 REC->ML raw — DESIGN COMPLETE (no property hook):** `raw_rec_keypress_cbr` (mlv_lite.c:4355) ALREADY
toggles raw on `key==MODULE_KEY_REC || MODULE_KEY_LV` -> `raw_start_stop()`. So: bridge detects the REC
keycode -> `module_send_keypress(MODULE_KEY_REC)` -> mlv_lite's existing CBR fires -> toggles raw.
Suppress Canon h264 REC via the same funcs[0] gate (REC-specific / movie-mode). NOTE: do NOT use
PROP_MVR_REC_START — M50mk2 consts.h:183 flags the MVR path as "junk... probably don't get used" on this
body. Only unknown left for #1 = the physical REC-button KEYCODE in movie mode (one HW capture, or RE the
e041xxxx physical->keycode driver). Movie GUICMDs exist in the dispatcher (0xd4 MOVIE_REQUEST_REC_STOP,
0xba/0xbb SHOOT_START/END) for reference.

**Net:** #4/#5 root-caused + fix proven purely by RE. #2/#3 mechanism RE-supported (one cheap isolation
test to rule out an ML bug). #1 fully designed (one keycode value to capture). Everything is implementable
now except the two small HW data points.

## ★★ 2026-07-16 IMPLEMENTED + QEMU-VALIDATED (btn injection + REC routing; #3 proven leakage-caused)
**Implemented (build 2026-07-16, autoexec builds clean):**
- `serial_shell.c`: `btn <keycode> [state]` = FULL press via gui_enqueue_message @0xE021DCB2 (ring +
  Canon queue -> both Canon AND ML process it); `key <keycode> [state]` = ML-BRIDGE-ONLY injection
  (bypasses Canon; for isolation tests). Button injection over UART -- works in QEMU where the MPU
  (hardware button codes) is NOT modeled.
- `gui.c` bridge (Phase 2 REC): global `m50ii_rec_keycode` (=0x102698, default 0xFF, runtime-settable
  via `wr`) -> `module_send_keypress(MODULE_KEY_REC)` -> mlv_lite raw_rec_keypress_cbr -> raw_start_stop.

**QEMU test rig (reusable):** `qemu-eos-build/arm-softmmu/qemu-system-arm -M M50mk2,firmware=boot=1`
+ SD card `m50ii_dumper/qemu_stage/sd_mltest.raw` (swap in build/autoexec.bin) + `-drive if=ide,...
cf.base.raw,snapshot=on` (CF required or "CF init failed") + `-serial tcp:127.0.0.1:4455,server,nowait`.
ML boots to **big.8 / ml_started=1 in QEMU** with the new build (no regression). serial_shell RESPONDS
over the TCP serial (`ver` -> "ML M50mk2 1.0.1 2026-07-16"). NOTE: QEMU UART RX drops chars on fast
bursts -> PACE TX ~20ms/char (client `qtest.py`); and QEMU floods `rgb_vram_info was NULL` (no display).

**RESULT -- #3 FN-toggle isolation test (the one I said needed HW): PASS in QEMU.**
Injecting FN via the bridge (`key 0x22 0`) and reading `gui_menu_shown` (call 0xd4ea9):
  start=0 -> FN=**1 (open)** -> FN=**0 (toggled CLOSED)** -> FN=**1 (reopened)**.
=> The FN toggle + menu open/close logic is CORRECT in isolation. So **#3 (2nd FN won't close) on HW is
caused by Canon LEAKAGE** (leaked FN shifts Canon's GUI mode -> the 2nd FN's keycode changes -> bridge
doesn't match), NOT an ML bug. #2 shares the mechanism => also leakage. **=> Phase 1 exclusivity fixes
both.** REC inject (`key 0x99` after `wr 0x102698 0x99`) ran with no crash (routing wired; real REC
keycode still needs one HW capture). 

**Still TODO:** Phase 1 exclusivity ROM patch. patch_hook_function (patch_cache.c) is DIGIC-45 ARM
(unusable on M50 II thumb) and it's a pre-hook (can't early-return anyway); the M50 II path is an
MMU-remap code patch (patch_mmu.c) of funcs[0] @0xE021E460 -> ML gate that returns when
gui_menu_shown(). Design + QEMU-test that next (QEMU can now test it: inject MENU via `btn` with menu
open, verify Canon does NOT open its menu).

## ★★★ 2026-07-17 HW SESSION — sym-mismatch fixed, #3 fixed+validated on HW, REC leakage shown
**FLASH-HYGIENE BUG (was the "freeze after boot"):** reflashing autoexec.bin ALONE left the STALE
ML/MODULES/M50mk2_101.sym on the card (34218 B) while the rebuilt autoexec had shifted data-symbol
addresses (my new `m50ii_rec_keycode` global sits before module_list). Modules relocated against the
old sym -> module_list/CBR pointers wrong -> corrupt handler call -> jumps into module_list data
(pc=0x1104A4, dfar=0x66) -> data-abort cascade after big.8 -> freeze. QEMU was fine (its card had a
matching sym). FIX: always flash the matching M50mk2_101.sym (build/M50mk2_101.sym, 34245 B) WITH the
autoexec. After doing so: clean boot, big.8/ml_started, HEALTHY heartbeat, zero crashes. **TODO: make
flash.sh/procedure always copy the sym so a partial autoexec-only flash can't recur.**

**#3 (2nd FN won't close) — ROOT-CAUSED + FIXED + HW-VALIDATED.** On HW, bridge-only FN injection
(`key 0x22`, no Canon) gave gui_menu_shown 0->1->1->1: the menu OPENS but won't toggle closed. Cause:
the bridge called `gui_open_menu()` (= give_semaphore(gui_sem)) on every FN, but that OPENS only --
menu_task does NOT reliably toggle-closed on HW (it stays in its menu loop). QEMU's timing masked this
(there it showed 1->0->1). FIX (implemented in gui.c): explicit toggle
`if (gui_menu_shown()) gui_stop_menu(); else gui_open_menu();`. VALIDATED LIVE without a rebuild by
calling gui_stop_menu (0xd4f35): 0->1(FN)->0(gui_stop_menu)->1(FN). So #3 was PARTLY an ML bug (the
FN-close path), not only leakage -- important, HW-only finding.

**MENU close works, REC leaks:** injecting BGMT_MENU (`key 0x0c`) closes the menu cleanly on HW
(1->0). Pressing the physical REC button twice (in movie mode, no exclusivity) made the camera
UNRESPONSIVE -> REC reached Canon and started h264 recording = live proof of leakage (#5) and why REC
must be intercepted (exclusivity) before it can drive ML raw. REC keycode capture incomplete (the
press triggered Canon before I could read the ring delta; widx baseline was 0x229).

**State:** build dcedf3f3 (autoexec) + matching sym on card; #3 fix now in gui.c source (NOT yet built/
flashed). Camera unresponsive after the REC press (likely recording/froze) -> needs power-cycle.
**NEXT (batch into one autoexec+sym reflash):** (1) build the #3 toggle fix; (2) Phase 1 exclusivity
(MMU-remap funcs[0] gate) -- also fixes REC leakage so REC capture+routing becomes clean; (3) then
capture REC keycode + test #2 nav in isolation on HW.

## ★★★★ 2026-07-17 DIAGNOSIS COMPLETE (all HW-confirmed) — one fix remains: exclusivity
Build f53df81b (autoexec) + sym a030c566 flashed (autoexec+sym together). Clean boot.
- **#3 FN toggle: FIXED + USER-CONFIRMED on HW** — physical FN now opens AND closes the ML menu
  (gui_stop_menu explicit-toggle fix). DONE.
- **#2 up/down: LEAKAGE (HW-confirmed).** Bridge-only injection (`key 0x42`/`0x40`) moves the ML menu
  highlight down/up correctly (user watched the screen). Pressing PHYSICAL up/down instead shows a
  Canon overlay -> Canon intercepts the nav keys. So ML nav is fine; the physical failure is leakage.
- **#4 MENU->config, #5 buttons->Canon: leakage** (same mechanism; #5 shown by the physical-nav Canon
  overlay + REC starting Canon h264).
=> **ALL of #2/#4/#5 reduce to ONE fix: input exclusivity** (block Canon's button handler while the ML
menu is shown). #1 (REC->raw) is routing-done and unblocked by the same exclusivity (so REC stops
starting Canon h264). 
**REMAINING WORK = Phase 1 exclusivity only:** MMU-remap code patch (patch_mmu.c) of funcs[0]
@0xE021E460 -> ML gate that returns when gui_menu_shown() (drops the button so gui_massive_event_loop/
WinSys never runs). Design fully worked out earlier in this doc; implement + build + flash(autoexec
+sym) + test on HW (inject MENU via `btn` with menu open -> Canon must NOT open its menu; physical
up/down must move the ML menu, not a Canon overlay). Then capture REC keycode (clean once REC no
longer leaks) + set m50ii_rec_keycode.

## ★ 2026-07-17 PHASE 1 EXCLUSIVITY — IMPLEMENTED + QEMU-VALIDATED (blocked on MMU budget, solved via page reclaim)

### The hook (correct + installed in ROM)
`platform/M50mk2.101/include/platform/mmu_patches.h`: `hook_gui_funcs0_exclusivity` (naked Thumb) +
a `normal_code_patches[]` entry patching funcs[0] @0xE021E460. Exact prologue (from gang100.bin):
```
e021e460 01 46     mov  r1, r0
e021e462 78 48     ldr  r0, [pc, #0x1e0]   ; literal @0xe021e644 = 0x5428 (gui_main_struct)
e021e464 08 b5     push {r3, lr}
e021e466 83 68     ldr  r3, [r0, #8]        ; continue at e021e468
```
Hook: `push {r0-r3,r12,lr}; bl gui_menu_shown; cmp r0,#0; pop {r0-r3,r12,lr}` (POP preserves flags);
if shown -> `movs r0,#0; bx lr` (DROP, lr = gui_main_task return); else replay prologue
(`mov r1,r0` / `movw r0,#0x5428` [relocated PC-rel load] / `push {r3,lr}` / `ldr r3,[r0,#8]`) +
`ldr pc,=0xe021e469` (funcs[0]+8, Thumb bit). The 8-byte hook is `df f8 00 f0 <target LE>` = ldr pc,[pc].

### THE BLOCKER: 1-page MMU remap budget (root-caused live)
First build: apply_normal_patches returned, but `dump 0xE021E460` = ORIGINAL bytes, `num_patches`=0.
Root cause read from live MMU state (global_mmu_conf @0x120004): `max_L2_tables=1`, `max_64k_pages_remapped=1`.
The single L2 (virt_page_mapped=**0xE0000000**, in_use=1) is consumed by **CONFIG_INIT1_HIJACK** patching
PTR_INIT1_TASK (0xe00403c0, section 0xE0000000) for cpu1 bring-up. funcs[0] is section **0xE0200000** (a
DIFFERENT 1MB region) -> `find_L2_for_patch` (matches on addr&0xfff00000) returns NULL -> apply_data_patch
returns E_PATCH_BAD_MMU_PAGE -> silent (qprintf) fail. `MMU_MAX_REMAP_PAGES=2` MEASURED `_bss_end 0x160000`
(> the 0x15fe00 Canon DryOS-pool starvation threshold) -> HW boot panic; and there is NO ML BSS to reclaim
(only large BSS is patch_mmu.o's generic_mmu_space itself). So bumping pages is out.

### THE FIX: post-boot reclaim of the init1-hijack L2 page (patch_mmu.c `mmu_reclaim_init1_page`)
By big.4a (apply_normal_patches, after the cpu1-ready wait) the init1-hijack has served its purpose (cpu1
already adopted ML tables; PTR_INIT1_TASK never read again). So we release its page/L2 and reuse it for
funcs[0], staying at 1 page. The init1-hijack's split_l1_supersection already broke the 16MB supersection
0xE0000000-0xE0FFFFFF into 16 ROM sections; only section 0xE0000000 (L1 idx 0xE00) was swapped to the L2.
Reclaim: restore L1[0xE00] to a plain ROM section by copying flags from its already-split sibling
L1[0xE01] (0xE0100000; `l1[0xE00]=(l1[0xE01]&0xfffff)|0xE0000000`), clear L2[0] bookkeeping, reset the
(now file-scope) `backing_page_index`, dcache_clean L1. The subsequent apply_patch(funcs[0]) reuses the
freed L2 for section 0xE0200000 and finalizes with change_mmu_tables (both cores) — one TLBIALL makes the
L1 restore + new mapping live together. Called from apply_normal_patches under `#if CONFIG_M50mk2`, only
when `COUNT(normal_code_patches)`; logs `[ML] big.4a2: init1 remap page reclaimed`.

### QEMU validation (build 22:11:59, autoexec md5 3e89ef2f, sym 9c792f14) — ALL GREEN
- Boot: `big.4a2: init1 remap page reclaimed` -> `big.8: ml_started=1` (reclaim's L1 surgery is safe; no crash).
- `dump 0xE021E460` = **`DF F8 00 F0 69 E6 0C 00`** = ldr pc,[pc] -> 0x000CE669 (hook installed! was original before reclaim).
- `patches_global[0]`: addr 0xE021E460, size 8, is_instruction 1, valid desc ptr => registered/applied.
- Menu-CLOSED `btn 0x0c` through full Canon dispatch -> shell ALIVE (replay+continue asm validated on real ROM).
- Drop-while-shown path: NOT testable in QEMU (menu won't open — needs display HW; `rgb_vram_info NULL`).
  It is the simplest hook path (`movs r0,#0; bx lr`) and is instruction-verified.

### Symbols (build 22:11:59): hook_gui_funcs0_exclusivity 0xce669, gui_menu_shown 0xd4f99, gui_open_menu
0xd508d, gui_stop_menu 0xd5025, m50ii_handle_canon_gui_event 0xd7d71, m50ii_rec_keycode 0x102830.

### HW TEST PLAN (needs the user to flash autoexec.bin + M50mk2_101.sym TOGETHER, then cold boot)
1. Boot log must show `big.4a2: init1 remap page reclaimed` then `big.8` (mllog.py phases). If instead
   `init1 reclaim FAILED`, the reclaim didn't find the page — abort, do not trust exclusivity.
2. `mlserial.py cmd "dump 0xe021e460 8"` must show `DFF800F069E60C00` (hook live in ROM).
3. Open ML menu (physical FN). With menu OPEN: press physical MENU -> Canon config must NOT appear;
   press physical UP/DOWN -> ML highlight moves, NO Canon overlay (fixes #2/#4/#5).
4. `mlserial.py cmd "btn 0xc 0"` with menu open -> Canon menu must NOT open (drop path).
5. Regression: with menu CLOSED, Canon buttons behave normally (replay path).
6. Then #1: capture the physical REC keycode (now clean — press REC with ML menu open so the gate drops it
   from Canon), `wr &m50ii_rec_keycode <code>`, and add REC-specific movie-mode suppression for menu-closed REC.

### Files changed (this session): mmu_patches.h (hook + patch entry), patch_mmu.c (mmu_reclaim_init1_page +
file-scope backing_page_index + call in apply_normal_patches), features.h (comment documenting the 1-page
constraint + reclaim). gui.c REC-routing + #3 FN-toggle from prior session retained.
