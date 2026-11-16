# Tier-A Stub Audit — M50 II 1.0.1 (2026-07-11)

Semantic verification of the 40 crash-class ("Tier-A") stubs in
`platform/M50mk2.101/stubs.S` — memory/sync/MMU/GIC/task/RPC. Method: a parallel
Ghidra fan-out (one agent per stub) decompiled each target in `gang100.bin`,
judged **name-vs-behavior**, and adversarially re-checked every non-MATCH.

**Why:** three stubs crashed the camera this session by pointing at the wrong
function (`_alloc_dma_memory`/`_free_dma_memory` → a graphics routine;
`_LoadCalendarFromRTC` → a struct-copy). `tools/verify_stubs.py` catches only
*structural* rot (bad region / mid-instruction / no frame); it rated the DMA stub
OK because it pointed at a real function entry — just the **wrong** function. This
audit adds the semantic layer that catches that class.

## Result: 28 MATCH · 4 WRONG · 2 false-positive · 6 need re-verify

### ✅ MATCH — 28 confirmed-good (behavior fits the name)
Memory suite: `AddMemoryChunk` 0xE05BD70C · `CreateMemoryChunk` 0xE05BD22E ·
`CreateMemorySuite` 0xE05BDBE8 · `DeleteMemorySuite` 0xE05BD95C ·
`GetFirstChunkFromSuite` 0xE05BD790 · `GetMemoryAddressOfMemoryChunk` 0xE05BD552 ·
`GetNextMemoryChunk` 0xE05BDFB0.
Sync: `CreateRecursiveLock` 0xE05CC64C · `AcquireRecursiveLock` 0xE058738C ·
`ReleaseRecursiveLock` 0xE0587456 · `take_semaphore_now` 0xE05D0D78 ·
`create_counting_semaphore` 0xE05D0CFA · `DeleteEventFlag` 0xE05B0BA2 ·
`SetEventFlag` 0xE05B0EBC · `ClearEventFlag` 0xE05B0EEA ·
`WaitForAnyEventFlag` 0xE05B0CC0 · `WaitForAllEventFlag` 0xE05B0D40 ·
`msg_queue_count` 0xE05B5624 · `msg_queue_post_blocking` 0xE05B556C.
MMU: `mmu_setup` 0xE004F15C · `mmu_table_init_copy` 0xE00722BC ·
`set_ttbr_checked` 0xE0072310 · `set_ttbr_per_core` 0xE007233C ·
`restore_orig_mmu` 0xE007232E.
GIC/task: `enable_interrupt` 0xE018E2A8 · `disable_interrupt` 0xE018E2E4 ·
`arm_gic_init` 0xE00A69EE · `send_software_interrupt` 0xE00D4034.

### ✅ FIXED — 4 mis-ported stubs RE-corrected (2026-07-11)
All were latent (not on the big.8 boot path). RE'd the correct ROM addresses,
verified by disassembly, applied to `stubs.S` with `[M50II 1.0.1 OK]` tags; the
rebuild links clean and `verify_stubs.py` now rates all four OK.

| Stub | ~~Bad addr~~ → **Correct** | Real function |
|---|---|---|
| `AllocateMemoryResource` | ~~0xe05c7f0e~~ → **0xe0608904** | `System/RscWrap.c` dispatch thunk, vtable slot 0, 4 args (size,cbr,ctx,type) |
| `AllocateContinuousMemoryResource` | ~~0xe05c7f3c~~ → **0xe0608932** | RscWrap slot 4, 4 args; confirmed by caller `FUN_e02058aa` logging `"AllocateContinuousMemoryResource failed"` |
| `FreeMemoryResource` | ~~0xe05c7fe2~~ → **0xe0608960** | RscWrap slot 8, **3 args** (hSuite,cbr,ctx) |
| `_get_task_info_by_id` | ~~0xe01c8378~~ → **0xe01d77c4** | calls `get_task_by_id(id)` then fills task-attr struct; used by exception dumper `FUN_e00d41ee` |

The three allocators are one adjacent `RscWrap.c` cluster (manager vtable @0xe0608ac4);
deltas Cont=Alloc+0x2e, Free=Alloc+0x5c are **identical** to reference ports RP.160
(0xe061d398/3c6/3f4), 200D.101, 850D.100 — and the 4/4/3 arg counts match the ML
signatures exactly. `_get_task_info_by_id` found via the RP.160 `" CORE : %d"` anchor.
The memory-**suite** family (Create/Add/Get*Chunk…) was correct all along — only this
`m_pf*MemoryCBR` cluster + the task-info stub were mis-ported.

### ⚠️ FALSE-POSITIVE — 2 (tooling artifact, actually correct)
`_request_RPC` 0xdf002c32 · `clear_RPC_request` 0xdf002c52. These are the CPU1
**runtime-relocated blob** (0xDF00xxxx is copied to RAM at boot; ROM source near
0xE14995A4), so Ghidra sees no code at the address → auto-MISMATCH.
`verify_stubs.py` correctly marks them `SKIP`. CPU1 bring-up is HW-proven working
(`cpu1rdy=0x1`), which exercises `_request_RPC` → these are **fine**. Do not touch.

### ✅ RE-VERIFIED — 6 (had no fan-out verdict; agent hit StructuredOutput retry cap)
`memcpy_dryos` 0xe0329d10 · `CreateEventFlag` 0xE05B0B68 · `change_mmu_tables`
0xE007D61A · `isr_dispatch` 0xE018E17C · `register_interrupt` 0xE018E384 ·
`get_task_by_id` 0xE00A6BBA. All confirmed **clean function entries** (Ghidra
`get_function_by_address` entry == stub address); 5 are already Ghidra-named
(`CreateEventFlag`/`change_mmu_tables_M50II`/`isr_dispatch`/`register_interrupt`/
`get_task_by_id`). All are boot-path-critical and run on the stable `big.8` boot;
`get_task_by_id` was HW-proven live (the self-test's `_get_task_info_by_id` PASS
calls it). Formally closed.

## Status: audit COMPLETE
- **4 fixed stubs HW-VALIDATED** (self-test build `4d1a0167`): `_get_task_info_by_id`
  PASS; `AllocateMemoryResource` allocated a real 1 MB suite; `FreeMemoryResource`
  freed clean; `AllocateContinuousMemoryResource` allocated 256 KB contig — no `[ABORT]`.
- Corrected addresses tagged `[M50II 1.0.1 OK]` in `stubs.S`.
- Self-test disabled for the clean keeper (`M50II_STUB_SELFTEST` commented out;
  re-enable to re-test). Also fixed: `lens.c:1363` non-fatal size assert (M50mk2
  tolerant guard). Clean keeper build `1e40ee89`.
- Raw-video allocator foundation is proven on hardware. Next frontier: display OSD / raw.
