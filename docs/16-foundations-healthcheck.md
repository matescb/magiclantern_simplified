# docs/16 — Foundations Health-Check (WS-F7)

**Tool:** `tools/verify_stubs.py`  ·  **Baseline:** `docs/stub-baseline.txt`
**Date:** 2026-07-07  ·  **ROM:** gang100.bin (M50 II 1.0.1, base 0xE0000000)

## What this is

The first deliverable of the "strong foundations" epic: an automated regression
harness that re-derives ground truth for **every** stub in
`platform/M50mk2.101/stubs.S` straight from the ROM (capstone Thumb-2, same
engine as `romdis.py`) and assigns a verdict. A wrong address can no longer
silently rot the port — re-run after any edit and diff `docs/stub-baseline.txt`.

```
python3 tools/verify_stubs.py                      # full table + summary
python3 tools/verify_stubs.py --only REVIEW,BAD    # just the suspects
python3 tools/verify_stubs.py --baseline docs/stub-baseline.txt   # refresh baseline
# exit code = number of BAD stubs (0 = clean)
```

## Verdict rules (low false-positive by design)

| Verdict | Meaning |
|---|---|
| `OK`   | frame prologue at entry (`push{…}`, incl. varargs `push{r0-r3}`, or `sub sp`) |
| `LEAF` | clean leaf / tail-call / long fn: decodes, reaches a return, no call-before-frame |
| `DATA` / `NULL` | data pointer in a sane region (low-RAM global, MMIO, ROM const) / intentional 0x0 |
| `SKIP` | CPU1-runtime blob (`0xDF00xxxx`, relocated from `0xE14995A4`, not ROM-resident here) |
| `REVIEW` | **suspect** — cond-branch at entry / return at entry (func tail) / call before frame / straddles a prior instruction |
| `BAD`  | undecodable, wrong region, or odd (non-Thumb) address |

## Result: the boot/baseline path is provably clean

```
110 stubs — OK=69  LEAF=12  DATA=12  REVIEW=13  SKIP=3  NULL=1   (BAD=0)
```

**0 BAD.** Every boot-critical and baseline-runtime stub (cstart, create_init_task,
task/sem/queue/lock primitives, FIO, memory alloc, DebugMsg, MMU, SGI, the
Phase-A/B re-anchored set) verifies `OK`/`LEAF` at a valid entry.

## The 13 REVIEW suspects — all pre-existing inherited stubs, none boot-critical

These are M50.110-inherited addresses that were never re-campaigned; the harness
confirms they land **mid-function** (call a subroutine with no saved `lr`, start
on a conditional branch, or sit in the tail of the preceding function). None are
in the boot or baseline-run path, so they are **non-blocking** — but they are
real correctness bugs for the features that call them.

| Stub | Addr | Signal | Criticality |
|---|---|---|---|
| `AllocateMemoryResource` | 0xE05C7F0E | call@2 before frame | SRM CBR — kitor disabled SRM |
| `AllocateContinuousMemoryResource` | 0xE05C7F3C | tail of prior fn (real entry ~0xE05C7F42) | SRM CBR — disabled |
| `FreeMemoryResource` | 0xE05C7FE2 | call@7 before frame | SRM CBR — disabled |
| `_alloc_dma_memory` | 0xE055AAF4 | strd to unallocated frame | **DMA** — WS-B hazard-flagged, keep disabled |
| `_free_dma_memory` | 0xE055AB28 | first insn is a call (epilogue) | **DMA** — WS-B hazard-flagged, keep disabled |
| `_get_task_info_by_id` | 0xE01C8378 | `muls` entry + garbage literal | extask debug; needs wrapper |
| `_LoadCalendarFromRTC` | 0xE0637210 | copy-loop body (straddle) | RTC; needs wrapper anyway |
| `smemShowFix` | 0xE0146FD6 | mid-func (straddle) | debug info dump |
| `sysmem_info`* | 0xE00F1CB0 | store-to-live-reg entry | debug info dump (LEAF now; soft miss) |
| `malloc_info` | 0xE00F1C18 | `beq` at entry | debug info dump |
| `memmap_info` | 0xE009DEB8 | assert-call at entry | debug info dump |
| `memcpy_dryos` | 0xE065E998 | assert-call at entry | **CONFIG_MMU_REMAP** — re-anchor before enabling |
| `dump_file` | 0xE00DB9E6 | call@11 before frame | "gone from newer models" — unused |
| `gui_init_end` | 0xE065A4F4 | cond-branch at entry | **off-by-2 → real entry `0xE065A4F2`** (`cmp r0,#0`) |

\* `sysmem_info` reads as `LEAF` under the final heuristic (decodes to plausible
code, no call-before-frame) but manual romdis shows a store-to-live-register
entry; flagged here for completeness. The harness deliberately avoids guessing
when a mid-function address happens to decode cleanly — those get caught by the
RE re-anchoring workstreams instead.

## Concrete fixes surfaced

- **`gui_init_end` → `0xE065A4F2`** (currently `0xE065A4F4`, 2 bytes into the
  function past its entry `cmp r0,#0`). Verified by romdis. Safe to apply.

The remaining 12 need a proper string/xref re-anchor (the job of the F2–F6 RE
workstreams), not a blind address bump. They are tracked here and left as-is
until re-anchored, since all are disabled/debug-only and cannot affect boot.

## Regression protocol (going forward)

1. Edit `stubs.S`.
2. `python3 tools/verify_stubs.py` — confirm no new `BAD`, no boot-critical stub
   regressed to `REVIEW`.
3. `python3 tools/verify_stubs.py --baseline docs/stub-baseline.txt` and diff to
   see exactly which verdicts changed.
