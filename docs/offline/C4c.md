# C4c - Frame-timer base, stale selector comment, edmac-d8.c guard

Offline sprint, no camera attached. Nothing here proves behaviour on the camera.

## Goal

1. Decide whether `TG_FREQ_BASE` for the M50 II should be 32 MHz (current, "copy from 700D") or 27 MHz (measured on the M50 mk1 port), and change it if the evidence supports it, without moving any core symbol.
2. Fix the stale boomer-selector value in a `stubs.S` comment.
3. Explain the `#ifndef CONFIG_EDMAC_MEMCPY_D8` guard in `edmac-d8.c` and propose the correct one.

## What was built

Worktree `/mnt/data/WORK/eos/wt/C4c`, branch `offline/C4c` (from `walkback-milestone` a10b16c23), three commits:

| Commit | File | Change |
|---|---|---|
| a890f1690 | `src/fps-engio.c` | `TG_FREQ_BASE 27000000` under `#if defined(CONFIG_M50mk2)`; other DIGIC 8/X cameras keep 32 MHz |
| 797627c32 | `platform/M50mk2.101/stubs.S` | comment only: selector 0x373f0126 -> 0x373f0226 (two places) |
| 1f324fa84 | `platform/M50mk2.101/edmac-d8.c` | headers first, then `#ifdef CONFIG_EDMAC_MEMCPY_D8` (same line count) |

Build: `cd /mnt/data/WORK/eos/wt/C4c/platform/M50mk2.101 && make ARM_BINPATH=/usr/bin ARM_ABI=none-eabi` - rc=0 at baseline, after commit 1 and after commit 3.
Saved builds: `/mnt/data/WORK/eos/sim/C4c/{base,c1,c3}/` (autoexec.bin, M50mk2_101.sym, magiclantern), logs `build0/1/3.log`.

Host tools (read-only), `/mnt/data/WORK/eos/tools/c4c_fps_timebase/`:
- `vsync_rate.py [uart.jsonl] [addr]` - vsync-hook rate from logged `rd 0x14c360` replies.
- `rom_timer_pairs.py` - statistical ROM scan for timer A x B pairs per candidate clock (about 3 minutes; result inconclusive, see below).

## Results

### 1. Symbol comparison (the important one)

**No symbol moved.** `M50mk2_101.sym` is byte-identical between baseline, commit 1 and the final build; `nm -n magiclantern` hashes match (84ba503b...); `size` is unchanged (text 219714, data 34577, bss 261120); `autoexec.bin` stays 257696 bytes.

`cmp -l base/autoexec.bin c1/autoexec.bin` shows 31 differing bytes: the build-version/timestamp strings, the 4-byte trailer, and exactly one literal at file offset 101913..101915 (`00 48 E8 01` -> `C0 FC 9B 01`, i.e. 32000000 -> 27000000). Commit 1 -> final differs only in version strings and trailer (27 bytes); the disassembly and data of `edmac-d8.o` are identical before and after the guard change.

Note: the worktree baseline `.sym` differs from the stale `.sym` in the main checkout (dated Jul 21) by a few bytes of rodata after `build_version` - the version string length differs between build dates. That is independent of these commits, but it means rodata symbols after `build_version` can move from one build day to another.

### 2. Which timer base is right

How fps is derived (`src/fps-engio.c`): `fps_x1000 = (TG_FREQ_BASE / (A+1)) * 1000 / (B+1)` with A = `*0xD0406198 & 0xFFFF`, B = `*0xD04061A4 & 0xFFFF` (`platform/M50mk2.101/fps-engio_per_cam.[ch]`). `fps_get_current_x1000()` feeds raw_vidx (`worker.c:553`, session fps), mlv_lite, mlv_rec, `raw.c`, `lens.c` and `state-object.c`.

Evidence:

- **The "23.6 per second" figure was a short-window estimate. The logged data says 25.0.** `vsync_rate.py` over the original 2026-07-14 readings:
  ```
  samples: 42
  n= 27 span=  38.484s counts=   959  end-to-end= 24.919/s  lsq= 24.984/s  (+-0.130/s from 0.2s host ts jitter)
  ```
  So the hook fires once per frame at 25p. This fixes the frame rate but says nothing about the clock on its own.
- **No timer A/B value was ever logged.** `uart.jsonl` contains no read of 0xD0406198 / 0xD04061A4 and no fps/timer print (0 hits). The base cannot be computed from our own hardware data.
- **ROM**: 0xD0406198 appears once, in an init table at 0xE1313418: `{0xD0406198, 0x215}, {0xD040619C, 0x215}, {0xD04061A4, 0xFFFF}, {0xD04061B8, 0}, {0xD0406184, 1}`. This confirms the register identities (A and its shadow at +4, B) and the default A of 0x215 is the same timer A the mk1 port logged for 720p100/120 (`100 x 0x215 x 0x1F8 = 26.863 MHz`).
- mk1 (same sensor generation, same register block) measured 26.86..26.98 MHz in nine modes.
- No clock constant or divisor was found in the ROM. The pair scan gives the same noise floor for 27, 32 and four decoy clocks (375 / 494 / 607 / 442 / 435 / 379 windows) - **inconclusive**. Ghidra was not used.

Decision: 27 MHz, for this camera only. The evidence is indirect (inheritance from mk1 plus one matching ROM value), not a measurement. 32 MHz has no evidence at all - it is a placeholder copied from a DIGIC 5 camera. If the clock is 27 MHz, the old value makes every derived frame rate read 18.5% high (25p reported as about 29.6), which would go into the MLV header.

### 3. stubs.S comment

`edmac-d8.c:89` defines `MEM2MEM_BOOMER_SELECTOR 0x373f0226` (InSelType nibble flipped 1 -> 2 for the mk2, docs/32); the `stubs.S` comment still quoted the mk1 value 0x373f0126. Corrected in both places. Comment-only.

### 4. edmac-d8.c guard

The file did compile by accident. `CONFIG_EDMAC_MEMCPY_D8` is defined in `platform/M50mk2.101/internals.h`, reached through `dryos.h` -> `config-defines.h`. The guard sat before `#include <dryos.h>`, so the flag was always undefined at that point and the inverted `#ifndef` was true. Preprocessor check:

```
UNDEFINED_BEFORE
DEFINED_AFTER
```

Consequences of the old form: `-DCONFIG_EDMAC_MEMCPY_D8` on the command line would compile the file to nothing (link errors for `mem2mem_emdac_copy_d8`, `edmac_copy_rectangle_cbr_start`, ...); removing the flag from `internals.h` would not disable the file. Correct form, applied in 1f324fa84 because it is symbol-neutral: include the three headers, then `#ifdef CONFIG_EDMAC_MEMCPY_D8`.

## Limitations

- The 27 MHz value is not measured on the M50 II.
- The three commits were not booted in QEMU; the claim is limited to "same symbols, same size, one literal changed".
- Any rebuild changes the version/timestamp strings, so no two builds are bit-identical.
- `edmac-d8.o` / `edmac-d8.d` land in the platform directory as untracked files (Makefile lists `edmac-d8.o` without `$(BUILD_DIR)`); left as is.
- Modules were not rebuilt or compared; they link against the unchanged `.sym`.

## Next hardware checks

1. In movie LV 1080p25: `rd 0xD0406198` and `rd 0xD04061A4`. Compute `25 x ((A&0xFFFF)+1) x ((B&0xFFFF)+1)`. About 27.0e6 confirms the change (mk1 had A+1 = 0x2D1, B+1 = 0x5D7); about 32.0e6 means revert a890f1690.
2. Repeat in one other mode (1080p50 or 4K25) to check the base is constant.
3. `call fps_get_current_x1000` (0xE0715 in this build) should return about 25000 in 25p.
4. Use `vsync_rate.py` over a window of 30 s or more for any future vsync-rate figure; 1-2 s windows are off by several percent because of 0.2 s host timestamp granularity.
