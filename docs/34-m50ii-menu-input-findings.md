# M50 II Menu & Button Input — Findings & Resume Plan (2026-07-18)

Authoritative summary of the menu/button investigation. Companion to the append-log in
`docs/33-m50ii-menu-input-fixes.md`; this file is the readable conclusion + how to resume.

---

## 1. The five requested issues — status

| # | Issue (user's words) | Status |
|---|----------------------|--------|
| 1 | "start recording by REC button on camera" | **Routing done, not wired live.** Bridge → `module_send_keypress(MODULE_KEY_REC)` → mlv_lite `raw_start_stop`. Blocked on the same keycode/mode problem as #2 (the REC keycode is mode-dependent & uncaptured). |
| 2 | "denoise menu control — up/down not working until submenu" | **Root-caused, not fully fixable cheaply.** It's Canon per-shooting-mode input routing (see §2). Works in Auto mode; not in Video/M. |
| 3 | "second press of FN will not close menu" | **FIXED (HW-confirmed).** `gui.c` FN handler toggles explicitly: `gui_menu_shown() ? gui_stop_menu() : gui_open_menu()`. `gui_open_menu()` is give-semaphore = open-only, so a second press never closed it. |
| 4 | "MENU to exit ML exits and opens Canon config" | Symptom of the "leakage"; the exclusivity hook addresses it but is **net-negative** (see §4) → shelved. |
| 5 | "buttons activate Canon functions inside ML" | Same leakage as #4; same verdict. |

---

## 2. ROOT CAUSE — menu input is Canon **shooting-mode**-dependent

The M50 II menu bridge is a **read-only peek** of Canon's GUI event ring (`m50ii_input_task`
in `init.c`, ring base `0xBAE08`, write index `0x543C`, 0x10-byte entries `{type,p1=keycode,
p2=state,p3}`). It calls `m50ii_handle_canon_gui_event` (`gui.c`) which maps keycodes → `BGMT_*`
→ `handle_buttons()`.

**The keycode Canon writes for the same physical control depends on the shooting mode / GUI
state.** HW-confirmed behaviour matrix (ML menu open, exclusivity off):

| Mode dial | Wheel | Arrows (4-way pad) |
|-----------|-------|--------------------|
| **Auto**  | works | works |
| **Manual photo (M)** | works | **dead** (even in a submenu) |
| **Video** | **dead** | **dead** |

- Video is the worst case — and it's the target for raw video.
- "Up/down only work after opening a submenu" = pressing SET/Q (which Canon processes) flips
  Canon's GUI state into one where the pad emits mappable codes. The submenu itself isn't magic;
  Canon's processing of the SET press is.
- Captured keycodes are therefore **not stable**: the same physical up-press was seen as `0xB7`
  (wheel-struct event, fresh top-level), `0x1D` (type-2, after returning from a submenu), and the
  "canonical" `0x40` only in the mode where it works. Chasing keycodes is the wrong layer.

**The remap itself:** physical button → `e041xxxx` driver cluster **[mode-dependent keycode
lookup]** → `gui_enqueue_message` (`0xE021DCB2`) → ring/queue → `funcs[0]` (`0xE021E460`) →
`gui_massive_event_loop` (`0xE03FD182`) **[FIXED keycode→GUICMD map]** → WinSys. The
mode-dependence lives in the `e041xxxx` physical→keycode driver, upstream of the ring.

---

## 3. What was built this session (kept in-tree, mostly shelved)

All of this is on the clean baseline build but **inert by default**:

- **Exclusivity hook** (`platform/M50mk2.101/include/platform/mmu_patches.h`): a naked-Thumb ROM
  code-patch of `funcs[0]` @`0xE021E460` (via `normal_code_patches`/`apply_normal_patches`) that
  drops the button when `gui_menu_shown()`. Instruction-verified; installs live on HW
  (`dump 0xe021e460` = `DFF800F069E60C00` = `ldr pc,[pc]` → hook `0xCE669`).
  **Runtime kill-switch `m50ii_exclusivity_on` @`0x1009F0`, DEFAULT 0** (`wr` it to 1 to enable).
  With it 0 the hook is transparent (replays the original prologue + continues = stock `funcs[0]`).
- **MMU page reclaim** (`patch_mmu.c` `mmu_reclaim_init1_page`): the M50 II has only
  `MMU_MAX_REMAP_PAGES=1` (2 pages ⇒ `_bss_end 0x160000` > the `0x15fe00` Canon-DryOS-pool
  starvation threshold ⇒ boot panic; no ML BSS to reclaim). The one page is held by
  `CONFIG_INIT1_HIJACK` (`PTR_INIT1_TASK 0xe00403c0`, L1 section `0xE0000000`). By `apply_normal_patches`
  the hijack has served its purpose, so we free it (restore `L1[0xE00]` from the already-split ROM
  sibling `L1[0xE01]`, clear the L2 + backing-page counter) and reuse it for `funcs[0]`'s section
  `0xE0200000`. **First-ever unmap in this MMU implementation** ("we never unmap"). HW-confirmed via
  the `big.4a2: init1 remap page reclaimed` marker.
- **CPU0-only patch apply** (`patch_mmu.c` `apply_normal_patches`, M50mk2): `patch_memory_rom`'s
  cpu1 coordination (`request_RPC` → `wait_for_cpu1_busy_wait_update_mmu`) **faults on M50 II HW**
  (`dfar` in the 0xC09xxxxx GIC region) — the same reason `CONFIG_INIT1_HIJACK` exists. Since
  `funcs[0]`/`gui_main_task` is cpu0, we clear `is_cpu1_ready` around the apply so the cpu1 handshake
  is skipped. **This crashed the first HW boot; QEMU missed it because cpu1 is down in QEMU.**
- **Dial keycode wiring** (`gui.c`): `0x08`/`0x09` (dial CW/CCW, observed) → `BGMT_PRESS_UP/DOWN`.
  Harmless but ineffective (dial is also mode-gated).
- **UART input trace** (`init.c` `M50II_INPUT_TRACE`, DEFAULT 0): logs each non-background ring
  event `[ML] EVT t=/p1=/p2=` tagged with menu state `[TOP]/[SUB]/[closed]`. Background chatter
  (type4 poll, type2 `0x44`/`0xEB`, ~25/s) is filtered. The live ring-read is unreliable (background
  overwrites presses before a read); the trace is the reliable capture.

---

## 4. Why the exclusivity hook is NET-NEGATIVE (shelved)

The `funcs[0]` drop stops Canon processing the button while the ML menu is open. But **Canon's
processing is what activates the menu-input mode** (§2). Blocking it → the mode never activates →
**all** menu nav dies (worse than stock). It's also **incomplete**: "up leaks" even with it on,
because up arrives via a handler that is not `funcs[0]` (a different event `type` → different
`funcs[N]`; the hook only covers type 0). Verdict: remove it. Defaulted off; kept in-tree behind
the kill-switch for the deep-fix work only.

---

## 5. Approaches evaluated for real Video-mode nav

### Approach A — force Canon's GUI mode on menu open (the M50 **mk1** way) — **BLOCKED**
M50 mk1 (`platform/M50.110`) calls `SetGUIRequestMode(GUIMODE_ML_MENU = lv?0x63:3)` on menu open,
forcing a fixed mode with stable keycodes (`gui.h`: UP=0x31, …). Live test on M50 II:
`call SetGUIRequestMode(0x3)` @`0xE0599FBA` **flooded the EVF and hung the camera** (shell echoes
commands, never replies; power-cycle required) — the documented flood, re-confirmed. The mode param
is a bitfield (`ubfx r4,r0,#0xc,#0x8` = bits[19:12] = mode index). Salvage = RE the
`SetGUIRequestMode` worker (past its DebugMsg tail `0xE05D10C4`) to separate the **input key-table**
switch from the **display/EVF reconfig** and set only the former.

### "Toggle to Auto" via RAM write — **DEAD END**
Idea: write a GUI variable to fake Auto on menu open. Disproved empirically: the GUI-state block
`0x7E5C…0x7EE0` is **byte-for-byte identical** in Auto+menu-open vs Video+menu-open
(`CURRENT_GUI_MODE 0x7EC0 = 0` in both; `0x7EC4=0x9083`, `0x7ECC/0x7ED0=1` in both), yet the pad
works in Auto and not Video. **The Auto/Video gating is not in any accessible GUI variable** — it's
the deep shooting-mode input routing. Poking the real shooting-mode property would hit the same
LV/EVF reconfiguration (flood) + exposure side effects. No cheap lever exists.

### Approach B — hook the physical-button layer below the remap — **RECOMMENDED for resume**
The only truly mode-independent layer is **below** the `e041xxxx` remap: the raw physical-button
events (the `e041xxxx` input, or the MPU/GPIO source) carry fixed IDs regardless of shooting mode.
Hook there and drive the ML menu directly — no keycode chase, no GUI-mode change, no flood. This is
the user's original instinct and the correct architecture. Cost: real RE (find the single stable
dispatch point, replicate press/release semantics), on the order of the rest of this port.

---

## 6. Current state (clean & usable)

- **Clean baseline flashed** to the card: `autoexec.bin` md5 `4766d400bc4fc869ad43a5c8c25fbcf8` +
  `M50mk2_101.sym` `93dbf49a7c91189bd04df51bddf6d7b7` (`platform/M50mk2.101/build/`). Exclusivity
  off, trace off, #3 FN fix kept, hook transparent. No regression vs stock ML behaviour.
- **Revert backup on card:** `AUTOEXEC.menu3fix-f53df81b.BIN` (rename to `autoexec.bin` — but its
  sym is not on the card, so prefer re-flashing a matched pair if reverting).
- **Flash rule (critical):** always flash `autoexec.bin` **and** the matching
  `ML/modules/M50mk2_101.sym` together; a stale sym mis-relocates modules → crash.
- **Working workflow:** configure ML/mlv_lite in **Auto** mode (menu fully works), flip the dial to
  **Video** to record raw.

---

## 7. Resume plan (when picking this back up)

**Preferred: Approach B (physical-layer hook).**
1. RE the `e041xxxx` cluster (callers of `gui_enqueue_message 0xE021DCB2`) to find the physical
   button dispatch and the mode-table lookup; identify the **stable physical ID** (table index) and
   a single hook point above the mode lookup.
2. Alternatively locate the MPU/GPIO button source (cf. 200D `hook_mpu_recv`).
3. MMU-code-hook that point (infra exists: `normal_code_patches` + reclaim + cpu0-only apply are
   proven). Feed the stable IDs into `m50ii_handle_canon_gui_event`/`handle_buttons` with a
   fixed ID→BGMT map. Replicate press vs release.
4. Capture keycodes with the trace build (`M50II_INPUT_TRACE=1`), not live ring-reads.

**Fallback: salvage Approach A.** RE `SetGUIRequestMode` @`0xE0599FBA` worker to set only the input
key-table without the display reconfig; test the specific Auto-menu mode value (not `0x3`).

**Then #1 REC:** once a stable layer exists, capture the REC physical ID and route to
`module_send_keypress(MODULE_KEY_REC)` (routing already in `gui.c`, keycode var
`m50ii_rec_keycode`).

---

## 8. Reference — addresses & symbols

| Symbol | Addr | Notes |
|--------|------|-------|
| `gui_enqueue_message` | `0xE021DCB2` | writes ring + posts queue (100+ callers) |
| `funcs[0]` (keypress handler) | `0xE021E460` | exclusivity hook target; prologue `01 46 78 48 08 b5 83 68`, continue `0xE021E469` |
| `gui_main_task` | `0xE021DB7C` | stack-copies GMT_FUNCTABLE once; dispatches `funcs[type]()` |
| `gui_massive_event_loop` | `0xE03FD182` | fixed keycode→GUICMD map |
| `SetGUIRequestMode` | `0xE0599FBA` | floods EVF; mode param bitfield bits[19:12] |
| `GUI_ChangeMode` | `0xE03FF1D8` | ctrlman (unused) |
| `CURRENT_GUI_MODE` | RAM `0x7EC0` | = 0 in Auto+menu, Video+menu, Video-idle (not the differentiator) |
| GUI event ring | RAM `0xBAE08`, widx `0x543C` | 0x10-byte entries, wraps 0x401; bg = type4/type2-0x44/0xEB |
| `e041xxxx` cluster | — | physical→keycode driver (mode-dependent); Approach B target |
| `PTR_INIT1_TASK` | `0xe00403c0` | init1-hijack page (L1 section 0xE0000000), reclaimed |
| ML: `m50ii_exclusivity_on` | RAM `0x1009F0` | kill-switch, default 0 |
| ML: `m50ii_handle_canon_gui_event` | — | the bridge (gui.c) |
| ML: `m50ii_rec_keycode` | — | REC routing var (gui.c) |

Build/tooling: `tools/mlserial/` (`mlserial.py cmd`, `mllog.py`, flash via mount `/dev/sdc1`).
Ghidra: `gang100.bin` (base 0xE0000000) loaded; `gang200.bin` also open.
