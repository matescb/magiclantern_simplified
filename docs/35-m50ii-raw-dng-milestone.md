# 35 — M50 Mark II: FIRST RAW FRAME CAPTURED ON HARDWARE ✅ (2026-07-18)

**Milestone:** a real 14-bit raw sensor frame was captured on the Canon EOS M50 Mark II
(DIGIC 8, K468, fw 1.0.1), written to the SD card, unpacked on the host, and confirmed to be
a genuine, recognisable photograph (2096×1164, RGGB Bayer). This closes the long-standing
"do we even have the right buffer?" validation gap (docs/31 R2, docs/32) and, more importantly,
root-causes + fixes the card-write blocker that also gates MLV recording.

Companion docs: docs/30 (stub resolution), docs/31 (raw impl plan), docs/32 (FIO/record arc).

---

## 1. Result

- Capture path: `dump_img_task` (src/debug.c) triggered over the serial harness in **movie-mode
  LiveView standby**, wrote `M50mk2/MV-1080/LCD/RAW-000.RAW` = **4,269,552 bytes** (= 2096×1164×14/8,
  the full frame), then host-unpacked to DNG.
- Verified real image (stats from `tools/mlserial/raw_dng.py`):
  - **The two Bayer green channels matched: G1=3461.2, G2=3465.0** — the decisive tell that the
    geometry + 14-bit unpacking + CFA phase are correct (a wrong unpack scrambles the phase and
    the greens diverge).
  - Per-channel means R=2822 / G=3463 / B=2936, full range 0..16383, mean 3171 (above black 2047).
  - Demosaiced preview = a recognisable photo (a USB charging hub on a desk). Saved under
    `tools/mlserial/captures/first_raw_capture.{raw,dng,png}`.

**Confirmed correct on HW:** raw buffer = `0x6BC61CC8` (uncached) / `raw_info.buffer` `0x2BC61CC8`
(cacheable mirror); geometry reg `0xd0420250` = `0x048B0E54` → xb=3668 B/line (2096px @14bit),
yb=1163 lines; black 2047 / white 16200; CFA = RGGB (even lines start red, per src/raw.h).

---

## 2. THE KEY FIX — `FIO_WriteFile` uncacheable gate (unblocks ALL M50 II card writes)

**Symptom (HW):** with real image data reaching `save_dng`, the card write was rejected:
```
[FSU] WARN Please Designate Uncacheable Addr!!!!!!
[FSU] WARN FSUBlockWrite: NotPrepared
[FIO] ERROR @run_test Write(ecode=0x1003)
```

**Root cause:** `src/fio-ml.c` `FIO_WriteFile()` converts the source buffer to its uncacheable
mirror (`UNCACHEABLE(ptr)`) before the low-level `_FIO_WriteFile` — DIGIC 8 FSU **requires** an
uncacheable DMA source — but that conversion was gated behind **`#ifdef CONFIG_MEM_2GB`**, which the
M50 II deliberately does NOT define (docs/31: 2GB handling lives in edmac-d8.c instead). So every
M50 II card write passed a cacheable address and FSU refused it.

**Fix (src/fio-ml.c ~line 695):**
```c
#if defined(CONFIG_MEM_2GB) || defined(CONFIG_M50mk2)
    return _FIO_WriteFile(stream, UNCACHEABLE(ptr), count);   // FSU needs uncacheable src
#else
    return _FIO_WriteFile(stream, ptr, count);
#endif
```
The preceding `if (ptr == CACHEABLE(ptr)) sync_caches();` makes `UNCACHEABLE(ptr)` coherent.
`UNCACHEABLE(x)` on this body = `x | 0x40000000` (mem_defs.h:16, non-VXWORKS) — verified live
(`0x2BC61CC8 → 0x6BC61CC8`).

**Why this matters beyond the DNG:** this is the SAME "heap-buffer DMA fail / persistent frame-write
blocker" that has blocked mlv_lite MLV writes for weeks (see recent.md 2026-07-15/16). Any code that
writes to the card via `FIO_WriteFile` (save_dng, mlv_lite `write_frames`, logs) now gets an
uncacheable source automatically. **Re-test MLV recording with this fix in place.**

**Pitfall discovered:** do NOT pre-`UNCACHEABLE()` the pointer at the call site AND rely on
`FIO_WriteFile` — passing an already-uncacheable ptr makes the `ptr == CACHEABLE(ptr)` guard false,
so `sync_caches()` is skipped and the FSU reads stale RAM (and, with plain `malloc` memory, faulted:
`[AB] pc=0xE02F281A dfar=0xE0997D80` → reboot). Pass the plain (cacheable) buffer and let
`FIO_WriteFile` do the sync + conversion. For large DMA buffers prefer `fio_malloc` over `malloc`.

---

## 3. Supporting fixes (src/debug.c `dump_img_task`)

1. **`raw_info` zeroing guard** — the code called `raw_update_params()` a second time
   unconditionally and ignored its return; a transient failure zeroed `raw_info`
   (buffer/frame_size → 0) → `malloc(0)`/`memcpy(0)` → 0-byte DNG. Fix: snapshot `raw_info` from the
   known-good first call into a local `ri`, replace only if the re-run succeeds, and gate
   `malloc`/`memcpy`/`save_dng` on `ri.buffer && ri.frame_size > 0`.
2. **Uncached source read** — `memcpy(buf, UNCACHEABLE(ri.buffer), frame_size)` on M50mk2, because
   `raw_update_params()` caches the raw buffer (black/white autodetect) and the cacheable view can
   hold a stale frame.
3. **`.RAW` bypass** — `save_dng`'s header/thumbnail alloc path is still flaky on this body (produces
   a 0-byte DNG / crashes). So `dump_img_task` now writes the plain packed raw buffer to a `.RAW`
   file via one `FIO_WriteFile` (the proven path) BEFORE calling `save_dng` (which byte-swaps in
   place), and the host unpacks it. Movie-mode guard added; the diagnostic NotifyBox/msleep cruft
   was removed.

**Known-still-broken (minor):** `save_dng` itself (0-byte DNG + a post-write reboot from its alloc
path) — next build should skip the `save_dng` call on M50mk2 (the `.RAW`+host route is the keeper).
The `.422` VRAM dumps write NULL (`vram.vram` is NULL on M50 II) — unrelated to raw.

---

## 4. Host tool — `tools/mlserial/raw_dng.py`

Unpacks an ML 14-bit packed raw buffer dump → DNG (+ optional 16-bit PGM), fully off-camera.
Bit layout grounded in `src/raw.h` `raw_pixblock` + the `PA..PH` macros (self-tested: pack/unpack
round-trips all 2.44M pixels exactly). Defaults = the live-confirmed `raw_info`
(2096×1164, 14-bit, black 2047, white 16200, RGGB). Prints a per-channel stats block (the
green-match check) so you can judge a frame before opening it. See tools/mlserial/TOOLS.md.

---

## 5. Reproduce (from a clean boot)

1. Flash a build with the §2/§3 fixes (autoexec + matching `M50mk2_101.sym` together!). For this
   milestone: autoexec `8e18c3e24bce` + sym `65fe2bc29d04`; mlv_lite disabled for a clean boot.
2. Cold-boot into **MOVIE mode + LiveView showing** (photo mode does NOT publish a raw buffer).
3. Trigger over the serial harness (per-build addrs from `nm build/magiclantern`):
   `call <run_in_separate_task> <dump_img_task> 0`  — this build: `call 0x000d5085 0x000d7285 0`.
   Screen shows `R2 RAW: 2096x1164 wrote N/4269552`; N=4269552 = full frame.
4. Card → reader → `python3 tools/mlserial/raw_dng.py <card>/M50mk2/MV-1080/LCD/RAW-000.RAW -o frame.dng --pgm frame.pgm`.

**Do NOT** dump a full frame over the serial link — a sustained multi-minute dump starves the
real-time raw/EVF pipeline (`[LVEVFC] Blanking DeadLine`) and crashes the camera; writing to the
card + host-unpack is the reliable route (this milestone's whole approach).

---

## 6. What this unblocks

- The raw buffer/geometry/params are now HW-PROVEN correct → the R3 MLV effort rests on solid ground.
- `FIO_WriteFile` now delivers uncacheable DMA sources → the mlv_lite per-frame card-write blocker
  (docs/32) should be retest-ready. Next raw session: re-arm mlv_lite (re-enable `mlv_lite.en`,
  rebuild `.mo` against the current sym), record in movie-LV standby, and check whether frames write.
