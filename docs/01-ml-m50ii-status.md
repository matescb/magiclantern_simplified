# ML status for the EOS M50 Mark II (research findings)

Sources: ML Discord (guild 671072748985909258), GitHub, ML forum/wiki, CHDK wiki. Gathered 2026-07-05.

## Bottom line
- **No usable ML build and no public code** for the M50 Mark II. Main repo `reticulatedpines/magiclantern_simplified` has only branches `dev` + `m6ii_raw_video`. A `m50mk2` skeleton folder a dev made (Nov 2024) was an empty local placeholder, never pushed.
- The M50 II is a **separate camera** from the original M50 — near-identical Digic 8 hardware (only board diff: no NFC), but **different firmware**, so ML binaries are not compatible and stubs must be re-found.

## What had been achieved by the community (before us)
- Code execution via Canon Basic on M50 II **fw 1.0.3** (tryinglobster + kitor, Jun 2023). fw **1.0.4** is locked down.
- A ROM dump on **fw 1.0.3** (ssjrd, Nov 2024; kitor confirmed "sane", real ROM2).
- Hardware fully mapped; kitor found **gdbserver strings** in M50 II fw (Aug 2025).
- Never reached a booting ML / baseline port.

## Firmware lock ladder (important)
`1.0.4` = locked (cbasic failed) → `1.0.3` = cbasic confirmed working → **`1.0.1` (ours)** = below both, should be the most open. **Never update the camera firmware.**

## Key people
- **names__are__hard** = reticulatedpines / Stephen (lead; repo owner). Estimated "2-3 days to reach M50's level WITH a camera in hand."
- **kitor** = Kajetan Krykwiński (active Digic 8 RE; now owns an M50 II; did the D8 raw EDMAC groundwork).
- **bilal_fakhouri** = got raw video working on the original M50 (Aug 2025).
- ssjrd, tryinglobster = prior M50 II volunteers; both ran out of time and dropped off.

## Repos
- `reticulatedpines/magiclantern_simplified` (canonical), `reticulatedpines/qemu-eos` (emulator).
- `kitor/magiclantern_simplified`: branches `edmac_sx740` (D8 raw foundation), `kitor_dev`, `raw_draft`.
- `bilalfakhouri/magiclantern_simplified`: branches `M50_rawvideo`, `M50_mmu`.

## Raw video (relevant to M50 II)
- Long thought impossible on D8. **bilal_fakhouri got 14-bit RAW recording working on the ORIGINAL M50 in Aug 2025** (experimental, not merged): 1080p up to 60fps, 14-bit only (10/12-bit noise — `PACK32_MODE` on D8 not solved), UHS-I bandwidth-capped.
- Architecture: the **D8 raw engine is generic** (`src/edmac*.c`, `src/raw.c`, `modules/mlv_lite`); per-camera you only add a thin shim (~600 lines: `platform/<cam>/edmac-d8.c` ~90% copy + raw stubs + register consts). So for the M50 II, **raw becomes a mechanical follow-on once a baseline boot port exists** — the hard/novel work is done.
- QEMU: devs say "M50_2 won't emulate at all yet" / "D8 doesn't emulate far." Useful at most for earliest-boot/MMU validation.
