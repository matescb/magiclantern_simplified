# 28 — M50 Mark II: ML Menu Input (WORKING on hardware)

**Status: DONE.** The Magic Lantern menu opens, persists, draws, and is fully navigable on a
real Canon EOS M50 Mark II (firmware 1.0.1). Verified on silicon 2026-07-12.

Final build: `autoexec.bin` md5 `df8ea890b3ec238bdb84ec249ef6690b`.

---

## 1. What works

| Action | Physical button | HW keycode | ML event |
|---|---|---|---|
| Open / close ML menu | **FN** (M.Fn) | `0x22` | `gui_open_menu()` (toggle) |
| Move selection up | 4-way pad **UP** | `0x40` | `BGMT_PRESS_UP` |
| Move selection down | 4-way pad **DOWN** | `0x42` | `BGMT_PRESS_DOWN` |
| Change value − | 4-way pad **LEFT** | `0x3E` | `BGMT_PRESS_LEFT` |
| Change value + | 4-way pad **RIGHT** | `0x3C` | `BGMT_PRESS_RIGHT` |
| Enter submenu / edit | center **Q/SET** | `0x0A` | `BGMT_Q` |
| Back / exit | **MENU** | `0x0C` | `BGMT_MENU` |
| Toggle help | **INFO/DISP** | `0x0E` | `BGMT_INFO` |

- **Release** code = press code + 1 (`0x23,0x0B,0x0D,0x0F,0x3D,0x3F,0x41,0x43`).
- Word layout on the ring for a button: `type=0`, `p1=keycode`, `p2` = state (`0`=press,
  `1`=repeat/hold, `2`=release/variant).

> **Keycodes are Canon-GUI-MODE-DEPENDENT.** The values above are for the mode the camera boots
> into. They differ from an earlier capture ("iter15") taken in a different GUI state (that set had
> FN=`0x4A`, SET=`0x12`, RIGHT=`0x0A`…). If ML is used in a very different Canon mode, re-capture.

### Background chatter (ignored by the bridge)
The camera continuously emits, on its own (~every 20 s), the cluster
`0x9F / 0xA0 / 0xA3 / 0xA4 / 0xA6` and dial `0xB7`, plus an idle tick `0xA7` (~3 s) and the boot
burst `0xB0/0xB2/0xB4/0xB5/0xB6/0x4F`. None are user input; the bridge only acts on the eight codes
above (and drops `0x9F`, which an earlier map wrongly assigned to PLAY).

---

## 2. Architecture

ML never overrides Canon's `gui_main_task` (six attempts, "iter9-14", all faulted — see doc 27).
Instead:

```
Canon gui_enqueue_message(0xE021DCB2)                     ML side (all C, no Canon GUI-task hooks)
  writes every event to a ring:                           +-------------------------------------+
    base  = 0x000BAE08 (DAT_e021e034)                     | m50ii_input_task  (src/init.c)      |
    widx  = *(0x0000543C) (gui_main_struct+0x14)  --poll-->|  read-only ring peek @20ms          |
    entry = {type,p1,p2,p3}, 0x10 bytes, wraps 0x401      |  skip idle 0xA7; forward each event  |
  ALSO posts to Canon's queue (Canon dispatches normally) |                 |                    |
                                                          |                 v                    |
                                                          | m50ii_handle_canon_gui_event (gui.c) |
                                                          |  if !ml_started: return              |
                                                          |  FN(0x22) -> gui_open_menu()         |
                                                          |  else if gui_menu_shown():           |
                                                          |     map keycode -> BGMT_x            |
                                                          |     handle_buttons(&{type=0,param})  |
                                                          +-------------------------------------+
                                                                            |
                       menu_task (menu.c, prio 0x1a, waits on gui_sem) <----+ (semaphore)
                       menu_redraw_task (menu.c, prio 0x1a) -- draws to ML's OSD layer (doc 26/iter4)
```

Key properties:
- **Read-only on Canon's ring.** Canon's `gui_main_task` still drains + dispatches every event, so
  the GUI/Edet subsystems stay alive (the property that made the "iter15" peek boot clean).
- **ML's menu is self-contained.** `menu_task` + `menu_redraw_task` are already created and drive
  open/close (via `gui_sem`) and drawing — they do **not** need Canon's `gui_main_task`.
- **Opening = one semaphore.** `gui_open_menu()` is just `give_semaphore(gui_sem)`; `menu_task`
  toggles.
- **Navigation = direct call.** The bridge builds `struct event{type=0,param=BGMT_x}` and calls
  `handle_buttons()` in place, running ML's full menu-key chain (`handle_ml_menu_keys`).
- **`fake_simple_button()` is unusable here** — it routes through `GUI_Control()`, which is an
  **unresolved ROM stub** on this body (`stubs.S:221`). We call `handle_buttons()` directly instead.
- **The ML menu is a pure OVERLAY.** It draws on ML's own composited OSD layer and leaves Canon's
  display/GUI mode untouched (see fix #4).

---

## 3. Files changed

| File | Change |
|---|---|
| `src/gui.c` | New `m50ii_handle_canon_gui_event(type,p1,p2)` bridge + `M50II_KEY_*` map + `ml_started` guard. Flags `M50II_MENU_INPUT` / `M50II_MENU_INPUT_TRACE`. |
| `src/init.c` | `m50ii_gui_peek_task` -> `m50ii_input_task` (label `ml_in`): ring poll, skip idle `0xA7`, forward to bridge; diagnostics `[ML] BTN` (every button, unbounded) + `[ML] in.hb` (liveness/counters). |
| `src/menu.c` | `piggyback_canon_menu()` + `close_canon_menu()` -> **no-op on `CONFIG_M50mk2`** (fix #4). `menu_task` auto-close guard -> suppressed+logged on M50mk2 (safety net; never actually tripped). |
| `platform/M50mk2.101/gui.h` | **Unchanged.** `BGMT_*` are internal ML tokens; the HW->BGMT mapping lives entirely in the gui.c bridge. |

---

## 4. The fixes that got here (lessons)

1. **Don't override `gui_main_task`.** Six iterations proved a replacement never dispatches and
   cascades Canon into faults. Read-only ring peek + direct `handle_buttons()` instead.
2. **`GUI_Control`/`fake_simple_button` are no-ops on M50 II** (unresolved ROM stub). Call
   `handle_buttons()` and `gui_open_menu()` directly.
3. **Keycodes are Canon-GUI-mode-dependent.** Re-captured with an unbounded per-button UART trace
   (`[ML] BTN`) that skips the boot flood, after an earlier map (captured in another mode) failed.
4. **Never make Canon change display state.** `menu_open()`->`piggyback_canon_menu()`->
   `SetGUIRequestMode(GUIMODE_MENU)` detonated the EVF/LiveView state
   (`LiveViewDrive::EvfDevState.c` nullptr **flood** the instant FN opened the menu) and dropped the
   display. Making piggyback a no-op -> the ML menu draws as an overlay, stays up, zero asserts. Same
   lesson as the old Edet cascade.
5. **Gate input on `ml_started`.** The input task runs from `post.0h`, long before `big.8`. Pressing
   FN *during boot* called `gui_open_menu()` before `gui_sem`/`menu_task` existed -> `KerSem.c:162`
   assert -> EVF cascade -> `my_big_init_task` **stalled** (never reached `big.8`). This looked like a
   "flaky boot". Fix: `if (!ml_started) return;` at the top of the bridge.
6. **Q/SET -> `BGMT_Q`, not `BGMT_PRESS_SET`.** Entering a submenu is `menu_entry_select(menu, 2)` =
   `menu_toggle_submenu()` (mode 2 = Q). `BGMT_PRESS_SET` (mode 3) only toggles/edits and shows the
   item hint — which read as "help". The M50 II's single center button maps to `BGMT_Q`; values
   change with LEFT/RIGHT.

---

## 5. Build / flash / test

### Build
```bash
cd magiclantern_simplified/platform/M50mk2.101 && make        # -> build/autoexec.bin
```

### Make a fresh bootable ML card (Canon "bootdisk")
A blank card needs FAT32 + the Canon boot signature + the ML payload. The camera's boot-flag is
already enabled (ML has been running), so only the card signature is needed. `/dev/sdX` = the card.
```bash
sudo parted -s /dev/sdX mklabel msdos
sudo parted -s /dev/sdX mkpart primary fat32 1MiB 100%
sudo parted -s /dev/sdX set 1 lba on
sudo mkfs.vfat -F 32 -n EOS_DEVELOP /dev/sdX1          # writes "EOS_DEVELOP" at VBR offset 0x47
printf 'BOOTDISK' | sudo dd of=/dev/sdX1 bs=1 seek=92 conv=notrunc  # "BOOTDISK" at offset 0x5C
# then: mount, unzip build/magiclantern.zip (autoexec.bin + ML/ tree), sync, umount
```
Verify: `dd if=/dev/sdX1 bs=1 skip=71 count=11` -> `EOS_DEVELOP`; `skip=92 count=8` -> `BOOTDISK`.
(FAT16 uses offsets `0x2B`/`0x40` instead.)

### Re-flash just autoexec (existing card)
`scratchpad/stage_menuinput.sh` auto-detects the `EOS_DEVELOP` card, copies `build/autoexec.bin`,
verifies md5.

### Test
Arm the UART logger (`/dev/ttyUSB0`, 115200 8N1) -> `hw_menuinput.log`, cold-boot, wait ~40 s for
`big.8 / ml_started`, then press FN. Grep the log for `menu-input:` and `[ML] BTN`.

---

## 6. Remaining / cleanup

- **Diagnostics still on** (UART-only, harmless, but a "keeper" build should gate them off):
  `M50II_GUI_TRACE`, `M50II_GUI_DIAG`, `M50II_MENU_INPUT_TRACE`; the `[ML] BTN` / `[ML] in.hb`
  traces in `m50ii_input_task`; the `0xB7` dial probe in the bridge; the `menu_task` auto-close
  suppression log.
- **Control-dial (`0xB7`) rotation** is NOT wired to menu scroll — its `*p2` struct is not cleanly
  decodable (varying heap objects, no stable direction field, + background noise). Up/down uses the
  discrete 4-way pad instead, which is sufficient. Dial scroll is a future nicety.
- **Exclusivity** (blocking Canon from also seeing FN/nav while the ML menu is open) is not needed in
  practice — the overlay works and Canon's reactions are benign. If ever required, the safe path is a
  ROM/cache patch of the leaf handler `funcs[0]=0xE021E461` via `apply_normal_patches` (documented in
  the gui.c bridge tail); do NOT override the task.
- **UP/DOWN direction** was assumed from the `RIGHT 0x3C / LEFT 0x3E / UP 0x40 / DOWN 0x42` ladder
  and confirmed correct on HW.

---

## 7. Key addresses (M50 II 1.0.1, local reference only — not for public posts)

| Symbol | Address |
|---|---|
| `gui_enqueue_message` | `0xE021DCB2` |
| GUI event ring base (`DAT_e021e034`) | RAM `0x000BAE08` |
| ring write index (`gui_main_struct+0x14`) | RAM `0x0000543C` |
| `GMT_FUNCTABLE` (funcs[0..6]) | `0xE099E958` -> `E021E461/E4B1/E50D/E541/E57F/E587/E58D` |
| `SetGUIRequestMode` (now avoided) | `0xE0599FBA` |
