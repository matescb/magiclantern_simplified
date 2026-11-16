# 21 — Booting our ML AUTOEXEC in QEMU: the boot blocker is a data abort on 0x40 (WS-QEMU Phase 3)

**Date:** 2026-07-07 · **Model:** `qemu-eos -M M50mk2,firmware=boot=1`
**Card:** freshly staged image — `sd.base.raw` copy + our current
`platform/M50mk2.101/build/autoexec.bin` (237472 B, the wrap-and-swap build,
RESTARTSTART=0xC78E0) installed as `AUTOEXEC.BIN`, `EOS_DEVELOP`+`BOOTDISK` flags set
via `tools/card-flags/edit_card_flags.py` on the loop-mounted FAT16 partition.

## Goal

Boot **our** ML (not Canon) in emulation and watch the wrap-and-swap put `my_init_task`
into the `0x417C` init-task slot (docs/20 proved Canon puts its own `init_task` there,
via a store at PC `0xE0166A6C`; the swap should make that value `my_init_task`).

## What happened

Canon bootloader loads and launches our build:

```
BootLoad
Read File AUTOEXEC.bin (237472 bytes)     <- our full current build
Now jump to 0x00800000: AUTOEXEC.bin!!!
```

Then **no ML DebugMsg, no STARTUP** — the boot hangs. Two hard datapoints:

**(1) Watchpoint on `0x417C`** fired once, *before* Canon's `create_init_task`:

```
Old value = 0  ->  New value = 0xFFFFFFFF     writer PC = 0x0014509E
```

Not `my_init_task`, not Canon's `0xE0040237` — our early code at `0x14509E`
(in the `user_mem` region, ~the historical `0x145150` blocker) writes a `-1`
sentinel and the swap never gets a chance to run.

**(2) The hang is a data abort — exec log spins at a single PC:**

```
[CPU0] E02E9D18: MRC ... DFSR_S -> 0x7      (data fault status: translation fault)
[CPU0] E02E9D18: MRC ... DFAR_S -> 0x40     (faulting data address = 0x40)
[CPU0] E02E9D18: MRC ... IFSR_S -> 0x7
[CPU0] E02E9D18: MRC ... SCTLR/DACR/IFAR ...
   … same block ×399 (tight loop) …
```

`0xE02E9D18` reads DFSR/DFAR/IFSR/IFAR — it is **Canon's data-abort/fault handler**,
stuck in a diagnostic spin. `DFAR = 0x40` + `DFSR = 0x7` (translation fault) ⇒ our ML
boot path **dereferenced a null/uninitialised pointer at offset `0x40`** (`[reg+0x40]`
with `reg≈0`), took an unhandled data abort, and Canon's handler hung.

## Interpretation

- **This is the real "0x145150 boot-handoff" blocker**, now root-caused in emulation:
  not an incomplete scheduler/GIC restore per se, but an early **`[0]+0x40` data abort**
  in our boot/wrap-and-swap path that occurs *before* `create_init_task` runs.
- The asymmetry from docs/19 is explained: Canon `boot=0` sails to STARTUP; **our**
  `boot=1` faults on `0x40` and dies in the abort handler. It's our early code, not the
  model.
- Emulator win: an LED could never show `DFAR=0x40`. We now have the fault address and
  a bounded suspect region (`~0x14509E` / the wrap-and-swap init path).

## Next debugging step (next session)

1. **Pin the faulting instruction.** Re-run `boot=1` with `hbreak` on the abort *vector*
   (VBAR-relative data-abort entry), read the abort-mode banked `LR` (= faulting PC + 8)
   → the exact `ldr/str [reg,#0x40]` that dies. Then map that PC:
   - if in the relocated ML image (`0xC78E0…0x101800`) → an ML bug / bad stub return;
   - if in copied Canon code → a stub/const we feed it is null (a `[base+0x40]` where
     `base` came from a stub that returns 0 in qemu).
2. **Prime suspects for a null `[+0x40]`:** a task/struct/hook pointer read from a global
   that Canon would have set but our hijack skipped, or one of the 12 REVIEW/disabled
   stubs (docs/16) returning 0 and then being used as a base. `0x40` is small enough that
   `base` is almost certainly `0x0`.
3. Cross-check the `0x14509E` writer and the fault site against Ghidra once the PC is
   known.

## Reusable harness notes (confirmed this phase)

- Stage a bootable qemu SD: `cp sd.base.raw x.raw`; `losetup -f --show -P x.raw`;
  `mount ${loop}p1`; copy `AUTOEXEC.BIN`; `edit_card_flags.py ${loop}p1 --eos-develop
  --bootdisk`; `losetup -d`. (sudo works non-interactively here.)
- `-M M50mk2,firmware=boot=1` runs the card's AUTOEXEC; `boot=0` runs Canon (docs/19).
- Spin detection: `tail -c` the exec stderr and `uniq -c` the PCs — a single PC ×N = hang.
