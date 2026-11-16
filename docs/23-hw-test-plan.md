# 23 — On-camera HW test plan: clean wrap-and-swap + .data-copy fix

**Date:** 2026-07-07 · **Target:** Canon EOS M50 Mark II, firmware **1.0.1** (do NOT update)
**Binary under test:** `magiclantern_simplified/platform/M50mk2.101/build/autoexec.bin`

## Objective / hypothesis

QEMU root-caused and let us fix a real bug in our boot code: the branch-patching in
`cstart()` clobbered the DryOS kernel-`.data` copy source pointer, poisoning the kernel
globals (docs/22). The fix (`*(volatile uint32_t*)(RELOCADDR + 0xC8) = 0xE1394358;`) is
verified in emulation — kernel globals come up clean and `my_init_task` is installed in
the init-slot `0x417C`.

**Hypothesis to test on HW:** with this fix, the **clean** wrap-and-swap (normal Canon
`create_init_task` → trampoline → `my_init_task`) now boots — i.e. matescb should no
longer *need* the hacky direct-entry path (patched saved-PC + `boot_pre_init_task` skip +
IRQ masking, docs/08). This is a **differential** test: compare the LED behavior of this
build against your previous clean-path attempts.

Why HW and not more QEMU: after the fix, emulation advances all the way into the EFM /
serial-flash subsystem, which the `M50mk2` QEMU model does not emulate (docs/22). Real
hardware has real flash, so on-camera the boot goes past where QEMU stalls.

## What's in this binary

- The clean wrap-and-swap (no direct-entry hack, no IRQ masking).
- The `.data`-copy fix in `cstart()`.
- **LED-ladder tracing** enabled (`M50II_TRACE_CREATE_LADDER` at the top of
  `src/boot-d678.c`) so the card LED reports wrap-and-swap progress. *(Remove that one
  `#define` for a clean/production build. Do NOT add `M50II_TRACE_LADDER` — it halts early
  at boot-d678.c:909, before the wrap-and-swap.)*

## Card prep

Use a **spare** SD card. Keep a **normal (non-boot) card** next to you for recovery.

1. FAT32-format the card, copy `build/autoexec.bin` to the card root as `AUTOEXEC.BIN`.
2. Set the Canon boot flags (same tool the dumper uses):
   `sudo python3 magiclantern_simplified/tools/card-flags/edit_card_flags.py <partition> --eos-develop --bootdisk`
   (or your existing bootable-card workflow — this is the same card that already runs your
   AUTOEXEC).

## Boot + read the LED

Insert the boot card, power on, watch the card LED. Blinks come in groups separated by
pauses; count the pulses per group.

| LED behavior | Meaning |
|---|---|
| single-blink groups counting **1,2,3,…up to 10** | `my_create_init_task` progressing: memory-bump + region/RESTARTSTART/alignment checks passing. **Reaching 10 = the clean wrap-and-swap setup fully succeeded.** |
| a group that **repeats forever** = **11 / 12 / 13 / 14** | a *setup* failure: 11 region-order, 12 RESTARTSTART mismatch, 13 not-enough-user-mem, 14 user-mem math. (Would indicate a per-cam constant issue, not the `.data` bug.) |
| **15 repeating forever** | `create_init_task` **returned** — Canon's kernel init completed the call. Strong success signal. |
| LED **goes dark / stuck after ~10** (no repeating group) | boot hung *inside* `create_init_task` — this is the old pre-fix failure mode (the `.data` corruption fault). If you see this, the fix didn't take on HW. |
| camera proceeds past the ladder (screen/behavior changes, or ML-ish activity) | `create_init_task` handed off to the scheduler and **`my_init_task` (ML) is running** — the clean path reached ML init. |

**The key result:** does this build get *further* than your previous clean-path attempts?
- Before the fix, the clean path died inside `create_init_task` (kernel-globals corruption)
  — which is why the direct-entry hack was needed.
- If the LED now reaches 10 and then **15-repeating** (or the camera moves on to ML), the
  fix works on HW and the hack is no longer required.

## Safety / recovery (read first)

- **NEVER update the camera firmware. Stay on 1.0.1.**
- The boot flag lives **on the card**, not in the camera. A normal card boots Canon
  normally — this is the same recoverable setup you've already been using.
- If the camera hangs (LED stuck or dark, no response): pull the battery, insert a **normal
  card**, power on → Canon boots. Nothing is written to the camera.
- This build does not flash, does not touch ROM, and does not run any firmware updater.

## What to report back

1. The exact LED sequence you observe (e.g. "1,2,…,10 then dark" or "…,10 then 15-repeating"
   or "…,10 then the screen came on").
2. How that compares to your previous clean-wrap-and-swap runs (further? same? less far?).

That tells us whether the `.data` fix unblocks the clean boot on real silicon. If it reaches
`my_init_task`/ML, the next step is a minimal `my_init_task` (LED-only, no full ML) for a
clean confirmation, then UART for real logs.

## Revert to clean build

Delete the `#define M50II_TRACE_CREATE_LADDER` line at the top of `src/boot-d678.c` and rebuild.
The `.data` fix stays (it's guarded by `BR_BR_CSTART`, unconditional for this cam).
Backup of the pre-fix file: `src/boot-d678.c.qemu-datacopy-bak`.
