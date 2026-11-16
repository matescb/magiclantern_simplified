# 22 — ML boot hang root-caused: corrupted cstart .data-copy poisons kernel globals (WS-QEMU Phase 3, cont.)

**Date:** 2026-07-07 · **Model:** `qemu-eos -M M50mk2` · gdb hbreak/watch · our `autoexec.bin`

## The full causal chain (6 levels, all confirmed in-emulator)

1. **Hang** = ML boot spins forever in Canon's data-abort/fault handler `0xE02E9D18`
   (reads DFSR/DFAR in a loop). `DFSR=0x7` (translation fault), `DFAR=0x40`.
2. **Fault** = a `blx` to a **null** function pointer. The contended-spinlock path in the
   Canon lock helper (`~0xE021B8F0`) does `blx MEM[0x40C8]`; `MEM[0x40C8] = 0` →
   `blx 0` → the null routine dereferences `[0]+0x40` → data abort on `0x40`.
3. **Why the contended path** = `register_interrupt` (`0xE018E384`, called by Canon's
   kernel init at `0xE00A676D` while building `dryos_init_info`) takes the lock at
   `0x4004`; the lock reads **`0xD3E06F98`** (nonzero ⇒ "held") → spins → calls the
   null callback.
4. **boot=0 vs boot=1 diff** (same call, same caller):
   | | `MEM[0x4004]` lock | `MEM[0x40C8]` cb |
   |---|---|---|
   | Canon `boot=0` (reaches STARTUP) | **`0` (free)** → fast path, callback never used | `0` |
   | our ML `boot=1` (hangs) | **`0xD3E06F98` (garbage)** → contended path | `0` |
   The null callback is a red herring (null in both). The bug is the **garbage lock**.
5. **Who writes the garbage** = a `.data`-copy loop at RAM PC **`0x14509E`**:
   ```
   0x145096: ldrcc r2,[r0],#4     ; r2 = *src++
   0x14509a: strcc r2,[r1],#4     ; *dst++ = r2
   0x14509e: bcc  0x145092        ; loop while dst < end
   ```
   At the write to `0x4004`: `src r0 = 0xE139F821`, `dst r1 = 0x4004`, `dst_end r3 =
   0x2AD1C`. It copies a ROM `.data` image (`0xE139xxxx`) into the kernel-globals region
   (`…0x2AD1C`), which includes the lock `0x4004` and the init-slot `0x417C` (also seen
   getting `0xFFFFFFFF` from this same PC).
6. **Root anomaly** = the copy **source is misaligned**: `0xE139F821 & 3 == 1`. A
   word-copy (`ldr [r0],#4`) with a byte-misaligned source copies **shifted garbage**.
   In `boot=0` the equivalent copy leaves `0x4004 = 0` (correct); in `boot=1` it writes
   `0xD3E06F98`. **Our boot corrupts Canon's `.data`-copy source pointer**, poisoning the
   whole kernel-globals region — the `0x4004` lock is just the first symptom to bite.

## What this means

- The boot blocker is **not** a missing stub or a scheduler/GIC gap. It is a
  **relocation/patch corruption**: our cstart handling makes Canon's `.data` init copy
  from a misaligned source, so DryOS kernel globals come up as garbage.
- Strongly implicates the cstart reloc/patch path in `src/boot-d678.c`:
  `patch_thumb_branch(BR_BR_CSTART=0xE00400C0, reloc_addr(cstart))`, the 2 KB `_reloc`
  buffer, and `patch_thumb_branch(BR_BZERO32/BR_CREATE_ITASK,…)`. The historical note
  that "a 4-byte patch near `0xE00400C6` corrupts a copy-loop source literal at
  `0xE00400C8`" is the same failure family — `BR_BR_CSTART` was moved `0xC6→0xC0` to
  avoid it, but a `.data`-copy source is **still** coming out misaligned.

## Open question (next diagnostic)

`0x145xxx` is **not** our `_reloc` buffer (that's a 2 KB static in ML BSS ~`0xC78E0`).
It appears to be Canon's own copied loader/second-stage that runs in both boots. Need
to determine:
1. Does the **same** copy loop run at `0x14509E` in `boot=0`? (hbreak `0x14509E` in
   `boot=0`.) If yes, compare the source-pointer literal `MEM[0x145118]` between boots —
   a difference pinpoints exactly which byte our patch corrupts.
2. If the literal is identical but the *source data* differs, the corruption is upstream
   (the ROM→`0x145xxx` loader copy itself), i.e. our patch damaged the loader image
   before it was copied.

## Fix directions (once the corrupt literal is pinned)

- **Preferred:** correct whatever reloc/patch misaligns the `.data`-copy source so the
  kernel globals init cleanly (matches Canon `boot=0`). This fixes `0x4004`, `0x417C`,
  and every other poisoned global at once.
- **Fallback (masks symptom, not root):** after the copy / before `create_init_task`
  (boot-d678.c:1060), re-zero the kernel lock/globals. Risky — other poisoned globals in
  `0x4000…0x2AD1C` would still be wrong downstream; only viable if the poison turns out
  to be limited.

## FIX APPLIED & VERIFIED (2026-07-07)

`src/boot-d678.c` `cstart()`, right before `sync_caches()`, guarded by
`#if defined(BR_BR_CSTART)`:

```c
    // restore the .data-copy source literal our patching clobbered
    *(volatile uint32_t *)(RELOCADDR + 0xC8) = 0xE1394358;
```

Rebuilt `autoexec.bin`, re-staged the SD, re-ran `boot=1`. **Verified in qemu:**

| global | before fix | after fix |
|---|---|---|
| `MEM[0x4004]` lock | `0xD3E06F98` (garbage) | **`0x00000000`** (free) |
| `MEM[0x40C8]` callback | `0x0` (null) | **`0xE00D4375`** (valid handler) |
| `MEM[0x417C]` init-slot | `0xFFFFFFFF` | **`0xCEBF1`** (ML region = `my_init_task` — wrap-and-swap works) |

The original `DFAR=0x40` data abort is **gone**; boot runs through `register_interrupt`
and deep into DryOS init. Backup: `src/boot-d678.c.qemu-datacopy-bak`.

## NEXT FRONTIER — a new, later fault (different class)

After the fix, boot reaches a **new** fault at handler `0xE02E9D18` with `DFSR=0x7`,
**`DFAR=0`** (null deref, not `0x40`), faulting near `0xE05BE74B`:

```
0xe05be743: blx lr
0xe05be747: bl 0xe05d0b42      ; sync/semaphore area (create_counting_semaphore ~0xE05D0CFA)
0xe05be74b: cbz r0, ...        ; lr here
```
Pinned via the data-abort vector (`0xE03C50D0`): **faulting insn `0xE05BE752: ldr r1,[r5]`
with `r5=0`** (DFAR=0). `r5` = `r0` from a `blx lr` allocator/create call at `0xE05BE743`
that **returned NULL**; the code then dereferences it. This is DryOS sem/sync-object
creation, and it sits **before** boot=0's STARTUP ceiling (Canon `boot=0` gets through it
to `[STARTUP]`), so it's an **our-boot** difference, not a qemu gap. Leading hypothesis:
`my_create_init_task`'s `user_mem`/`sys_objs` shrink (boot-d678.c:1013-1054, incl.
`CONFIG_INCREASE_MAX_TASKS`) squeezes the object pool so a Create*Semaphore alloc returns
NULL. Next: compare the `dryos_init_info` pool counts/sizes we pass to `create_init_task`
vs Canon `boot=0` (the sys_objs region + the 0x1CF/0x8E/0xE6 counts), and check whether
the failing allocator's pool base/free-list is null. Kernel globals (`0x4004`/`0x40C8`/
`0x417C`) are healthy, so the primary `.data` copy is fully fixed — this is a new class.

**Identified (2026-07-07):** the faulting validate-object routine `0xE05BE740` is called
with a NULL handle from `0xE065E9A5`, inside the **EFM (Electronic File Manager /
serial-flash) subsystem** — strings `EfmErscUnLockResources`, `FMID_SYS_UnlockSeqProc
start/Call`. So after the `.data` fix, boot advances all the way into DryOS file-manager
init and there a flash/EFM resource handle is NULL. Canon `boot=0` doesn't hit this, but
that's **confounded**: boot=0 stalls earlier at `eep_i2c` (EEPROM), and EFM runs after
EEPROM, so boot=0 never reaches it. Our boot=1 reaches EFM via the hijacked `my_init_task`
path (no STARTUP UART — it's off Canon's normal STARTUP task). **Assessment:** this is the
**long tail of new-model qemu bring-up** — EFM/serial-flash is not modeled for `M50mk2`
(the model's `serial_flash_*` fields are commented out), so getting past here needs
qemu-model work (add flash/EFM emulation) and/or on-HW validation, not a single ML fix.
The one clear, high-leverage boot bug (the `.data`-copy corruption) is fixed & verified;
what remains is subsystem bring-up.

## Repro / harness
`-M M50mk2,firmware=boot=1` + staged SD (docs/21). qemu+gdb in one foreground shell
(qemu `&` child), `hbreak`/`watch` for ROM/RAM, never `pkill -f 'M50mk2…'`, stderr →
`/dev/null` (exec logs fill the disk fast).
