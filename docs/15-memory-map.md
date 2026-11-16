# docs/15 — Memory Map & MMU (WS-F5)

**Verified 2026-07-07** — F5 workflow agent + my `cstart` ground truth (docs/13), in exact agreement.

## RAM map (from cstart dryos_init_info, base r5=0xC70D8)
```
0xC78D8   ── user_mem  (0x112E80) ──►  0x1DA758   ML autoexec/RESTARTSTART=0xC78E0 at base
0x1DA758  ── sys_objs  (0xAF80)   ──►  0x1E56D8
0x1E56D8  ── sys_mem   (0xE2200)  ──►  0x2C78D8
```
Kernel globals set by cstart: MEM[0x4168]=0x80000, MEM[0x416C]=0xA90000, MEM[0x4170]=0xA96000.

## MMU (added to stubs.S, all verified)
- `mmu_setup` 0xE004F15C — MMU table RAM base **0xBFF20000** (literal @0xE004F368)
- `mmu_table_init_copy` 0xE00722BC — copies 0x4900-byte table from source **0xE0000000**
- `set_ttbr_checked` 0xE0072310 · `set_ttbr_per_core` 0xE007233C (reads MPIDR) · `restore_orig_mmu` 0xE007232E (→0xE0000000)
- `change_mmu_tables` 0xE007D61A (existing) — the TTBR/TLB leaf.

## Key consts (verified)
- `RESTARTSTART = 0xC78E0` (= user_mem_start 0xC78D8 + 8) — **already correct** in Makefile.
- `CANON_ORIG_MMU_TABLE_ADDR = 0xE0000000` — confirmed as the 0x4900-byte MMU-table source.
- `MMU_TABLE_BASE_RAM = 0xBFF20000` — for CONFIG_MMU_REMAP.
- `MINIMUM_USER_MEM_LEFT = 0x80010` — with ml_reserved_mem=0x87A20 the remaining user_mem is safe.

Wrap-and-swap safety: ML carves its reserve from user_mem (0xC78D8..0x1DA758, len 0x112E80);
`my_create_init_task` bumps user_mem_start (+0x08) and shrinks user_mem_len (+0x0C).

---
## F5 agent detail
# 15 - Memory map, MMU/TTBR config, and wrap-and-swap safety

Date: 2026-07-07
Target: Canon EOS M50 Mark II, firmware 1.0.1, Digic 8 (dual Cortex-A9 + GIC).
Source: static RE of `gang100.bin` (ROM0 @0xE0000000) via `tools/romdis.py`; ROM L1
page-table decode; `build/location.map`; cross-ref `src/boot-d678.c`, donor Makefiles.

Goal: prove the wrap-and-swap `user_mem` bump is safe by recovering (a) the concrete
`dryos_init_info` values Canon builds in `cstart`, and (b) the static MMU/TTBR layout.

---

## 1. DryOS memory pool — concrete values from cstart (0xE0040104)

`cstart` derives a base `r5 = (literal 0x000C70DF @0xE00401BC) & ~7 = 0xC70D8`, builds
`struct dryos_init_info` on the stack (base `sp+4`), then `add r0,sp,#4; bl create_init_task`
(`BR_CREATE_ITASK = 0xE00401B4`, `r1 = init_task 0xE0040236`).

| Off | Field | Formula | Value | ML action |
|---:|---|---|---|---|
| 0x00 | `sys_mem_start` | r5 + 0x11E600 | **0x1E56D8** | untouched |
| 0x04 | `sys_mem_len` | lit | **0xE2200** (926208) | untouched |
| 0x08 | `user_mem_start` | r5 + 0x800 | **0xC78D8** raw | **align +8 -> 0xC78E0, then += reserve** |
| 0x0C | `user_mem_len` | lit | **0x112E80** (1126016) | **-= (8 + reserve)** |
| 0x10 | `sys_objs_start` | r5 + 0x113680 | **0x1DA758** | untouched* |
| 0x14 | `sys_objs_end` | r5 + 0x11E600 | **0x1E56D8** (= sys_mem_start) | untouched |
| 0x18 | `prio_max` | imm | **0x22** (34) | untouched |
| 0x1C | `task_max` | imm | **0xC7** (199) | untouched* |

\* only if `CONFIG_INCREASE_MAX_TASKS`.

**The three regions are exactly contiguous** (verified): `user_mem` end
`0xC78D8+0x112E80 = 0x1DA758 = sys_objs_start`; `sys_objs_end 0x1E56D8 = sys_mem_start`.
The whole DryOS pool is one block:

```
0xC70D8 ─[0x800 init/hdr]─ 0xC78D8 ── user_mem (0x112E80) ── 0x1DA758 ── sys_objs (0xAF80) ── 0x1E56D8 ── sys_mem (0xE2200) ── 0x2C78D8
```

Total pool span 0x200800 (~2.0 MB), entirely inside the cacheable DRAM window
(`< 0x40000000`). (Early boot also writes memory-config globals
`MEM(0x4168)=0x80000`, `MEM(0x416C)=0xA90000`, `MEM(0x4170)=0xA96000` — heap/DMA
boundaries, not part of this struct.)

---

## 2. RESTARTSTART / ML reservation (verified)

`RESTARTSTART = 0xC78E0` = `user_mem_start` after ML aligns the raw `0xC78D8` up to
`ELF_ALIGNMENT (0x10)`. Confirmed by `build/location.map`: `_text_start = 0x000c78e0`.
Matches `platform/M50mk2.101/Makefile`.

Reservation math (`src/boot-d678.c`, shared with RP/200D):

```
user_mem_start: 0xC78D8 --align+8--> 0xC78E0 (=RESTARTSTART)   user_mem_len: 0x112E80 --> 0x112E78
ml_reserved_mem = _bss_end - user_mem_start = 0x14F300 - 0xC78E0 = 0x87A20  (542.5 KB, full build)
user_mem_start += 0x87A20 -> 0x14F300 (== _bss_end)   user_mem_len -= 0x87A20 -> 0x8B458 (557 KB)
invariant: new user_mem end 0x14F300 + 0x8B458 = 0x1DA758 = sys_objs_start   ✓
MINIMUM_USER_MEM_LEFT = 0x80010 ; margin = 0x8B458 - 0x80010 = 0xB448 (~45 KB)   ✓ (tight)
safe max reserve = 0x112E78 - 0x80010 = 0x92E68 (~587 KB)
```

Donor cross-ref (all raw `user_mem_start ≡ 8 mod 16`, aligned +8 — identical pattern):
RP.160 `0xa94f0` (`0xa94e8` in ROM), R.180 `0xb0080` (`0xb0078`), SX70 `0xac8f0`
(`0xac8e8`), M50.110 `0xdc870`, 200D.101 `0x4d0000`. This validates the M50 II
`0xC78D8 -> 0xC78E0` and the shared `boot-d678.c` reserve logic.

> Latent-bug note: `boot-d678.c` aligns with `user_mem_start += align_diff` (should be
> `+= (ELF_ALIGNMENT - align_diff)`). It happens to be correct **only** because the
> residue is exactly 8 (= ALIGN/2) on every D7/D8 cam. Safe here; do not reuse blindly.

---

## 3. MMU / TTBR configuration

Table builder chain (reached via `mmu_setup 0xE004F15C`, a pointer-referenced boot/resume
handler — **not** in the create_init_task/scheduler path):

```
mmu_setup 0xE004F15C
  r7 = 0xBFF20000                         ; L1 table base in RAM (16KB aligned)
  bl mmu_table_init_copy 0xE00722BC       ; memcpy(0xBFF20000, 0xE0000000, 0x4900)
  │                                         ; then patch core0/1 TTBR0[0] -> L2 +0x4000/+0x4400
  bl set_ttbr_checked   0xE0072310        ; require 16KB align, tail ->
      set_ttbr_per_core 0xE007233C        ; TTBR0 = base + core*0x80 + 0x4800 ; TTBR1 = base
          bl change_mmu_tables 0xE007D61A ; mcr TTBR0/TTBR1, TLBIALL, BPIALL, dsb/isb
```

- **L1 table base (both TTBRs share the image):** `0xBFF20000`.
- **TTBR1 (shared, VA ≥ 0x02000000):** `0xBFF20000`.
- **TTBR0 (per-core, low 32 MB, TTBCR.N=7):** core0 `0xBFF24800`, core1 `0xBFF24880`.
- **Source:** the 0x4900-byte image is copied from `0xE0000000`
  (`CANON_ORIG_MMU_TABLE_ADDR`, confirmed); `restore_orig_mmu 0xE007232E` can re-point
  TTBR back to `0xE0000000`.
- Image sub-layout: `+0x0` TTBR1 L1 (16KB) · `+0x4000` core0 L2 · `+0x4400` core1 L2 ·
  `+0x4800` core0 TTBR0 L1 · `+0x4880` core1 TTBR0 L1.

### RAM/MMU region map (decoded from ROM L1 descriptors, all identity-mapped)

| VA range | Type / cache | S/XN | Use |
|---|---|---|---|
| 0x00000000–0x000FFFFF | per-core L2 (4KB pages) | — | vectors, low DryOS state |
| **0x00100000–0x3FFFFFFF** | **Normal, inner WBWA CACHEABLE, outer NC** | S=1 XN=0 | **main DRAM — DryOS pool + ML @0xC78E0** |
| 0x40000000–0xBFFFFFFF | Normal inner NC / outer NC (**uncacheable**) | S=1 XN=0 | DRAM uncacheable alias — DMA/VRAM (e.g. 0x7F41C000), **MMU tables 0xBFF20000** |
| 0xC0000000–0xDEFFFFFF | Device-shareable (**uncacheable**), sparse | S=1 XN=1 | MMIO (GIC 0xC0210000/0xC1001000, 0xD0xxxxxx) |
| 0xDF000000–0xDFFFFFFF | Normal CACHEABLE | S=1 XN=0 | early-boot scratch (cstart fills 0xDF000000..0xDF002000) |
| 0xE0000000–0xE7FFFFFF | Normal WBWA CACHEABLE, RO | XN=0 AP=5 | ROM0 (gang100), executable |
| 0xE8000000–0xEFFFFFFF | Strongly-ordered | XN=1 | unused ROM window |
| 0xF0000000–0xF7FFFFFF | Normal WBWA CACHEABLE, RO | XN=0 AP=5 | ROM1 (gang200), executable |
| 0xF8000000–0xFFFFFFFF | Strongly-ordered | XN=1 | unused |

The cacheable RAM (0x0–0x3FFFFFFF) and its uncacheable alias (0x40000000–0xBFFFFFFF) are
both identity-mapped with different cache attributes, i.e. Canon's classic
`UNCACHEABLE(x) = x | 0x40000000` aliasing (hardware DRAM decode). ML DMA buffers must use
the `0x4......`/`0x7F......` uncacheable view; ML code/heap at `0xC78E0` is cacheable.

---

## 4. Why the wrap-and-swap is MMU-safe (conclusion)

1. **The MMU is static before DryOS partitions memory.** The L1/L2 tables are built once
   in `mmu_setup 0xE004F15C` and TTBRs are locked in; `change_mmu_tables 0xE007D61A` has
   exactly **one** ROM-wide BL caller (`0xE0072358`), reachable only from `mmu_setup` and
   two pointer-referenced restore helpers — **none on the `create_init_task` /
   `scheduler_start` path**.
2. **The bump touches no MMU state.** Editing `dryos->user_mem_start`/`user_mem_len` only
   re-partitions RAM that is already Normal-cacheable identity-mapped. No TTBR write, no
   TLB flush.
3. **No collision.** Reserved ML region `[0xC78E0 .. 0x14F300]` and the whole DryOS pool
   `[0xC70D8 .. 0x2C78D8]` are far below the MMU tables (`0xBFF20000`) and every device
   region; the post-bump invariant `user_mem_start + user_mem_len == sys_objs_start`
   holds exactly. Reserve stays under the safe max `0x92E68`.

Therefore the `user_mem` bump is provably safe with respect to the memory map and MMU.


### struct layouts
```

### struct dryos_init_info (M50 II 1.0.1 concrete values; r0 into create_init_task = sp+4)
  +0x00   sys_mem_start                uint32_t   | = r5+0x11E600 = 0x1E56D8 (r5=0xC70D8). strd r0,[sp,#4] @E004015C with r0=lit0x11
  +0x04   sys_mem_len                  uint32_t   | = 0xE2200 (926208). lit @E00401D4; str [sp,#8].
  +0x08   user_mem_start               uint32_t   | = r5+0x800 = 0xC78D8 raw; str [sp,#0xC] @E0040164. ML aligns +8 -> 0xC78E0 = RES
  +0x0C   user_mem_len                 uint32_t   | = 0x112E80 (1126016). lit @E00401D8; str [sp,#0x10]. ML shrinks by (align 8 + ml
  +0x10   sys_objs_start               uint32_t   | = r5+0x113680 = 0x1DA758. lit @E00401DC + r5; strd [sp,#0x14]. == user_mem end (
  +0x14   sys_objs_end                 uint32_t   | = r5+0x11E600 = 0x1E56D8 (= sys_mem_start; reused r0). strd r0 at [sp,#0x18].
  +0x18   prio_max                     uint32_t   | = 0x22 (34). movs r0,#0x22; str [sp,#0x1C].
  +0x1C   task_max                     uint32_t   | = 0xC7 (199). movs r0,#0xc7; str [sp,#0x20]. (following words sp+0x24..0x2C = 0x

### struct mmu_table_image @ RAM base 0xBFF20000 (copy of ROM 0xE0000000, 0x4900 bytes)
  ++0x0000 TTBR1_L1_table               u32[4096]  | 16KB shared L1; TTBR1=base. Only entries for VA>=0x02000000 consulted (TTBCR.N=7
  ++0x4000 core0_L2_table               u32[256]   | 1KB L2 for VA 0x0-0xFFFFF; pointed to by patched core0 TTBR0[0] (mmu_table_init_
  ++0x4400 core1_L2_table               u32[256]   | 1KB L2; pointed to by patched core1 TTBR0[0] @E00722FA.
  ++0x4800 core0_TTBR0_L1               u32[32]    | per-core low-32MB L1 (0x80 bytes=32 sections); TTBR0(core0)=base+0x4800.
  ++0x4880 core1_TTBR0_L1               u32[32]    | TTBR0(core1)=base+0x4880 (base+0x4800+1*0x80).

```
