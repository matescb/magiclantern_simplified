# 11 - Stub verification Phase B (boot / scheduler / SMP-critical inherited stubs)

Verification lead pass over the boot-critical stubs in
`magiclantern_simplified/platform/M50mk2.101/stubs.S` that were inherited
**verbatim** from the M50.110 sister port (raw address copies, no independent
M50 II 1.0.1 anchor). Method and grading follow `docs/06-stubs-ledger.md`
(capstone string-anchoring + independent verify pass on our own ROM
`m50ii_dumper/dump_1.0.1/gang100.bin`, base `0xE0000000`, Thumb-2).

**Grading (same as docs/06):** `OK` = anchor + verified prologue/disasm;
`SUSPECT` = questionable / not yet re-anchored; `WRONG` = current address is
not a valid entry for the claimed role on this ROM (mid-function or unrelated
function). A `WRONG` stub can hang/brick the camera - do **not** build against
it. Only `OK` entries are safe.

Phase A already fixed one misattribution: `send_software_interrupt` was
`0xE0121128` (a UHS2/SD card-init) and is now re-pointed to the real GICv2 SGI
sender `0xE00D4034` (writes `GICD_SGIR @0xC1001F00`). That fix is re-confirmed
`OK` below.

**Result:** 13 boot-critical stubs reviewed - **1 OK, 0 SUSPECT, 12 WRONG**.
Of the 12 WRONG, **9 have a verified replacement address** (fix list, section 2)
and **3 could not be statically re-anchored** and must stay `notfound` / be
verified on HW (section 3).

---

## 1. Summary table

| stub | current addr | verdict | evidence (abridged) | recommended action |
|---|---|---|---|---|
| send_software_interrupt | `0xE00D4034` | **OK** | Clean 7-insn leaf entry (prev fn ends `bx lr` @0xE00D402E). Body: `orr r0,r0,r1,lsl#16; movt/movw r1=0xC1001F00; str r0,[r1]; dsb; mov r0,#0; bx lr` = GICv2 SGI write to `GICD_SGIR`. 3 direct BL/B.W callers target it exactly (E00A6F90/E013B39E/E01B2062). Only 2 GICD_SGIR writers ROM-wide (bootloader + this). | keep as-is (Phase A fix confirmed). TODO-verify-on-HW only the cpu_id->target-bitmask contract (arg1 must be `1<<cpu_id`; ML `src/patch_mmu.c` already encodes this). |
| change_mmu_tables | `0xe05772d6` | **WRONG** | Not an entry: first hw `0x4620` (`mov r0,r4`), no push; interior of DebugMsg/task fn @0xE057724E. Zero `mcr p15` anywhere in the 0xE0577xxx region. Verbatim M50.110 copy. | re-point to **`0xE007D61A`** (real TTBR/TLB leaf). |
| memcpy_dryos | `0xe065e998` | **WRONG** | Not an entry: real fn @0xE065E98C (`push {r4,lr}`); address is a cbz fall-through. Body is a DebugMsg/state-teardown routine, no copy loop, no r0/r1/r2 dst/src/len. libc cluster relocated (bzero32 moved E0578D64->E05D1F40). | re-point to **`0xE0DBBEB4`** (newlib memcpy; TODO-verify on QEMU/HW - no string anchor). Fallback `0xE0E1B1A0`. |
| _get_task_info_by_id | `0xe01c8378` | **WRONG** | Not an entry: first hw `0x4377` (`muls`), flows in from 0xE01C8376; mid-body of fssNotifyJob fn @0xE01C7DBA (strings 'fssNotifyJob'/'FM_NotifyCreateFile'/'fssPostJob'). FSS file-job code, not extask. genuine extask anchors are RAM-relocated, 0 static xrefs. | **no verified static replacement** - see section 3. Blank / mark notfound; re-anchor via kitor extask method or QEMU/Ghidra. |
| CreateRecursiveLock | `0xe0577598` | **WRONG** | Not an entry: `bl DebugMsg; pop {r4,lr}` fragment; enclosing veneer's ADR string = 'GRAPHICS_PermitAudioOutput'. Whole inherited RLock trio is bogus on this ROM. Real KerMisc.c cluster anchored via 'SystemIF::KerMisc.c' @0xE05CC728. | re-point to **`0xE05CC64C`** (KerMisc.c public wrapper). Alt: primitive `0xE0587318`. See discrepancy note in section 2. |
| AcquireRecursiveLock | `0xe06d77dc` | **WRONG** | Not an entry: `str.w r5,[r6,#0x288]` mid-fn; enclosing fn @0xE06D7782 = DFMKNAVI path mgmt ('DFMKNAVI_TerminatePath/ReconfigPath'). Real KerRLock.c @0xE0587xxx. | re-point to **`0xE058738C`** (KerRLock.c L198; 2-arg lock+timeout). |
| ReleaseRecursiveLock | `0xe06d78a6` | **WRONG** | Not an entry: `ubfx r2,r2,#0,#0xc` mid-basic-block of an MMIO config routine (base 0xD0411000). Structural clincher: `0xE0587456-0xE058738C = 0xCA` = inherited delta `0xE06D78A6-0xE06D77DC` (correct pair, wrong base). | re-point to **`0xE0587456`** (KerRLock.c release; 1-arg; recursion-count decrement). |
| take_semaphore_now | `0xe0577c2c` | **WRONG** | IS a real entry, but it is Canon `MultiPrepareVramForTouchInput` (self-names via 4 DebugMsg fmt strings; graphics/EDMAC calls, zero semaphore behavior). Stale un-relocated copy; its 3 siblings were moved to the KerSem cluster, this one was not. | re-point to **`0xE05D0D78`** (KerSem.c non-blocking take: `take_semaphore 0xE05D0DA8 - 0x30`; no IRQ assert, fixed poll desc 0xE0997E20). |
| msg_queue_count | `0xe054437c` | **WRONG** | Not an entry: first hw `0x7845` (`ldrb`), interior of fn @0xE05442F6. Raw un-relocated M50.110 addr; queue cluster maps to M50 II by constant delta +0x712A8 (create/receive/post all match verified stubs). | re-point to **`0xE05B5624`** (KerQueue.c; `push {r4,lr}`; `(queue,count*)->status` ABI; anchored via GetNumOfEvent 0xE05BA078). |
| _alloc_dma_memory | `0xE055AAF4` | **WRONG** | Not an entry: `strd r2,sb,[sp,#0x1c]` mid-fn; enclosing fn @0xE055AA98 = GUI 'GUI_DrawHdrImage GRAPHICS_DrawOneYuvImage' - no _AllocateMemory / no uncacheable fixup. docs/06 already marks alloc_dma_memory notfound. | **no verified static replacement** - see section 3. Keep notfound; would corrupt graphics if called. |
| _free_dma_memory | `0xE055AB28` | **WRONG** | Not an entry: `bl DebugMsg` mid-fn, epilogue-tail of the same GUI fn @0xE055AA98 as _alloc. Invoking it pops 9 regs + 0x2c stack never pushed = corruption. | **no verified static replacement** - see section 3. Keep notfound (mirror the paired alloc). |
| uart_printf | `0xE055A8B4` | **WRONG** | Not an entry: `bl 0xe0559b42`, 14 bytes inside fn @0xE055A8A6; a 2-arg tail-call wrapper, no va-list, no formatter. docs/06 already lists `0xE01C7280` as the verified uart_printf (maybe). | re-point to **`0xE01C7280`** (varargs `push {r0-r3}`; formatter bl 0xe021c432; IRQ-locked; used as printf by boot-handoff dump 0xE00D407C). |
| vsnprintf | `0xE0096060` | **WRONG** | Not an entry: `add.w r0,r5,#0x1aa` mid-fn; interior of byte-field serializer @0xE0095F82. Address did not transfer between ROMs. | re-point to **`0xE005DB9C`** (bounded full-format vsnprintf; `(dst,size,fmt,ap)`; anchored via LogFile emit fn 0xE005E6D2 + '*Data Lost*' string). |

---

## 2. stubs.S fixes to apply (WRONG with a verified replacement) - FOR HUMAN REVIEW, do NOT auto-apply

Prioritized. Each line shows the exact `THUMB_FN` change (old -> new). **P0** =
scheduler/SMP/MMU boot-critical; **P1** = kernel primitives used widely; **P2**
= logging/helpers.

### P0 - boot / MMU / SMP correctness

```
- THUMB_FN(0xe05772d6, change_mmu_tables)   ->   THUMB_FN(0xE007D61A, change_mmu_tables)
```
Real TTBR0/TTBR1 write + TLBIALL/BPIALL/dsb/isb leaf; sole caller 0xE0072358 reads MPIDR, computes `ttbr0 = base + (cpu_id<<7) + 0x4800`, matches ML `change_mmu_tables(L1+off, L1, cpu_id)`. Current addr is mid-function of an unrelated DebugMsg/task routine (no p15 ops in the region).

```
- THUMB_FN(0xe065e998, memcpy_dryos)        ->   THUMB_FN(0xE0DBBEB4, memcpy_dryos)   /* TODO-verify QEMU/HW */
```
Canonical newlib `memcpy(dst=r0,src=r1,len=r2)` (co-alignment, 0x20-byte ldm/stm loop, tbb tail, returns dst); one of exactly 2 byte-identical block-copy cores (fallback 0xE0E1B1A0). No string anchor exists for memcpy, so smoke-test before shipping. Current addr is a DebugMsg cbz fall-through.

### P1 - kernel sync primitives (recursive locks, semaphores, queues)

```
- THUMB_FN(0xe0577598, CreateRecursiveLock) ->   THUMB_FN(0xE05CC64C, CreateRecursiveLock)
```
SystemIF::KerMisc.c public Create wrapper (`push {r4,lr}; bl 0xE0587318; assert result!=5`), matches the claimed KerMisc.c role. Current addr is a `GRAPHICS_PermitAudioOutput` DebugMsg veneer fragment.
**Discrepancy to resolve before applying:** this stub's own verdict recommends the wrapper `0xE05CC64C` (underlying primitive `0xE0587318`); the sibling Acquire/Release verdicts instead cite the primitive as `0xE058731C` / `0xE0587318` (a 4-byte disagreement, and wrapper-vs-primitive choice). Pick one convention on-target - the wrapper 0xE05CC64C most faithfully matches the M50.110 "Create = KerMisc.c wrapper" intent.

```
- THUMB_FN(0xe06d77dc, AcquireRecursiveLock)->   THUMB_FN(0xE058738C, AcquireRecursiveLock)
```
KerRLock.c acquire (L198); 2 args (lock, timeout), builds timeout descriptor via 0xe00db14c, blocking-wait worker 0xe05bb826, owner-recursion increment. Current addr is mid-function DFMKNAVI path-management code.

```
- THUMB_FN(0xe06d78a6, ReleaseRecursiveLock)->   THUMB_FN(0xE0587456, ReleaseRecursiveLock)
```
KerRLock.c release; 1 arg (lock); owner check + recursion-count decrement via 0xE05BB7F0/0xE013AE18. Delta from Acquire (0xCA) byte-matches the inherited pair, confirming the correct pair copied to the wrong base. Current addr is mid-basic-block of an MMIO config routine.

```
- THUMB_FN(0xe0577c2c, take_semaphore_now)  ->   THUMB_FN(0xE05D0D78, take_semaphore_now)
```
KerSem.c non-blocking take (`= take_semaphore 0xE05D0DA8 - 0x30`, delta identical on both ports); `push {r4,lr}`, 1 arg, NO interrupt-context assert, fixed no-wait poll descriptor 0xE0997E20, same worker 0xe05bb4da, returns 0/0xd/7. **Current addr is `MultiPrepareVramForTouchInput` - catastrophic if called as a semaphore op.**

```
- THUMB_FN(0xe054437c, msg_queue_count)     ->   THUMB_FN(0xE05B5624, msg_queue_count)
```
KerQueue.c count; real `push {r4,lr}`; ML-matching `int32_t msg_queue_count(msg_queue*, uint32_t* count)` (writes *count via `str r0,[r4]`, returns 0). Constant-delta (+0x712A8) match to the three verified sibling queue stubs; independently anchored by GetNumOfEvent (0xE05BA078) '[TASK ERROR] GetNumOfEvent'. Bind the 2-arg 0xE05B5624, NOT the 1-arg GetNumOfEvent.

### P2 - logging / formatting helpers

```
- THUMB_FN(0xE055A8B4, uart_printf)         ->   THUMB_FN(0xE01C7280, uart_printf)
```
Verified varargs UART printer (`push {r0-r3}` va-spill; formatter bl 0xe021c432; IRQ-locked); used as printf by boot-handoff register dump 0xE00D407C and by the assert path. Already the docs/06 candidate (maybe). Note: `verify`/`prologue` false-negative on push{r0-r3} prologues. Current addr is a 2-arg wrapper, no formatter.

```
- THUMB_FN(0xE0096060, vsnprintf)           ->   THUMB_FN(0xE005DB9C, vsnprintf)
```
Bounded full-format `vsnprintf(dst,size,fmt,ap)` (`push {r0-r8,sb,sl,fp,lr}`; end-bound dst+size; '%' parse; flag handling `#/+/0/space`); anchored via LogFile emit fn 0xE005E6D2 ('*Data Lost*' @0xE004E284, ADR-only). Unclaimed in both ports. Current addr is a mid-function byte-field serializer.

---

## 3. SUSPECT / TODO-verify-on-HW (WRONG, no statically-anchored replacement)

These are confirmed WRONG (invalid/unrelated entry) but the correct M50 II
1.0.1 address could not be pinned by static string-anchoring. Leave `notfound`
/ blank - do **not** build against the current value. Re-anchor via QEMU/Ghidra
runtime resolution or on-camera trace.

| stub | current (bad) addr | why unresolved | how to resolve |
|---|---|---|---|
| _get_task_info_by_id | `0xe01c8378` | current addr enters FSS `fssNotifyJob` file-job code (hang/brick risk if called as the getter); genuine extask anchors ('extask','display task information','usage : task [-l \| task ID]' @0xE0F005FC/0xE0E93C2C) are RAM-relocated with 0 static code/word/ADR xrefs. | kitor extask method: resolve the RAM-relocated console command table -> extask handler -> task-id-verify helper -> its BL to the real getter; or QEMU/Ghidra runtime. |
| _alloc_dma_memory | `0xE055AAF4` | current addr is a live GUI YUV-draw fn (`GRAPHICS_DrawOneYuvImage`) - would corrupt graphics/hang. docs/06 already marks alloc_dma_memory notfound. No 'Uncacheable'/'alloc_dma'/'DMA memory' strings; no small _AllocateMemory wrapper ORs the uncacheable bit 0x40000000. Only "+0x40 aligned" candidates (0xE03DB3F8 / 0xE0617360) have a `(mode,size,&raw)` signature and no uncacheable mapping - not drop-in. | runtime: `drysh memmap`/`meminfo` (docs/07) to get the real address; do not wire the +0x40 candidates blind. |
| _free_dma_memory | `0xE055AB28` | current addr is mid-function (epilogue tail) of the same GUI fn @0xE055AA98 as _alloc; invoking it corrupts the stack (pops 9 regs + 0x2c never pushed). DMA pair lies outside the shifted memory cluster; naive 0x63D8C shift lands mid-function. | same as _alloc_dma_memory - resolve the pair together at runtime; keep notfound meanwhile. |

---

## 4. Out-of-scope: 0xDF00xxxx stubs (not in gang100.bin)

Three inherited stubs live at `0xDF00xxxx`, **below the 0xE0000000 base of
gang100.bin (ROM0)** and therefore cannot be verified in this pass at all -
they are outside the dumped image:

```
THUMB_FN(0xdf002c32, _request_RPC)
THUMB_FN(0xdf002c52, clear_RPC_request)
THUMB_FN(0xdf00d365, fsuDecodePartitionTable)   // in the "STUFF BELOW IS WRONG!" block
```

The `0xDF000000` region is a separate ROM area (ROM1 / AMEM-mapped code), not
present in our ROM0 dump. To verify or re-anchor these, obtain the **ROM1 /
AMEM dump** or use the full **Ghidra project DB** that has that region mapped.
No verdict is issued here. Note that `fsuDecodePartitionTable` is already under
the `// STUFF BELOW IS WRONG!` banner in stubs.S and, per the M50.110 comment,
is inlined like on EOS R (see R180 `function_overrides.c` for the WIP
alternative) - so it likely needs a code override rather than a stub address
regardless.

---

## Appendix - line references in current stubs.S

`magiclantern_simplified/platform/M50mk2.101/stubs.S`:
send_software_interrupt L93 (OK) · change_mmu_tables L112 · memcpy_dryos L118 ·
_alloc_dma_memory L126 · _free_dma_memory L127 · uart_printf L138 · vsnprintf
L139 · _get_task_info_by_id L155 · CreateRecursiveLock L164 ·
AcquireRecursiveLock L165 · ReleaseRecursiveLock L166 · take_semaphore_now L172
· msg_queue_count L203 · _request_RPC L95 · clear_RPC_request L108 ·
fsuDecodePartitionTable L283.
