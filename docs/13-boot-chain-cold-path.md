# docs/13 — Cold Boot Chain (WS-F1)

**Verified 2026-07-07** — dual-source: my main-thread `romdis` decode of `cstart` +
the F1 workflow agent, cross-checked and in exact agreement. Every address below
independently disassembled (valid prologue) and adversarially verified.

## Authoritative summary (verified ground truth)

Reset `firmware_entry` @0xE0040000: sets `VBAR=0xE03C50C0`, `sp=0x2000`, reads MPIDR,
gates secondary cores; core 0 runs a relocation preamble (blob copies + BSS zero,
`dcache_clean` 0xE05D08F4 / `icache_invalidate` 0xE05D09C8) then reaches **cstart 0xE0040104**.

cstart (core 0 path @0xE0040138): builds a 0x78-byte `dryos_init_info` on the stack
(`bzero32` @0xE0040152), fills 0xDF000000..0xDF002000 with 0xEEEEEEEE (CPU1 SRAM poison),
then at **BR_CREATE_ITASK 0xE00401B4** calls:

    create_init_task(r0 = &dryos_init_info[sp+4], r1 = 0xE0040236 init_task, r2 = 0)

with RAM/reloc base `r5 = 0xC70DF & ~7 = 0xC70D8`.

### dryos_init_info values (contiguous RAM map)
| off | field | value | derivation |
|----|-------|-------|-----------|
| 0x00 | sys_mem_start | 0x1E56D8 | r5+0x11E600 |
| 0x04 | sys_mem_len | 0xE2200 | → sys_mem_end 0x2C78D8 |
| 0x08 | **user_mem_start** | **0xC78D8** | r5+0x800 |
| 0x0C | **user_mem_len** | **0x112E80** | → user_mem_end 0x1DA758 |
| 0x10 | sys_objs_start | 0x1DA758 | r5+0x113680 (== user_mem_end ✓) |
| 0x14 | sys_objs_end | 0x1E56D8 | (== sys_mem_start ✓) |
| 0x18 | prio_max | 0x22 | |
| 0x1C | task_max | 0xC7 | |
| 0x20 | semaphore_max | 0x1CF | |

**`RESTARTSTART = 0xC78E0 = user_mem_start(0xC78D8) + 8`** → ML autoexec loads at the base
of DryOS user memory. Matches the M50mk2.101 Makefile. This is the number the wrap-and-swap
`user_mem` bump depends on — now double-confirmed.

## Wrap-and-swap hook
ML relocates the 0x800-byte firmware_entry region and patches **BR_BR_CSTART** to redirect
into `copy_and_restart`; `my_create_init_task` overwrites r1 with `my_init_task` before
tail-calling Canon `create_init_task` (Canon single-sources the init entry from r1→MEM(0x417C),
so the swap lands at the `task_trampoline` blx).

### ⚠ Boot fix applied (F1)
`BR_BR_CSTART` corrected **0xE00400C6 → 0xE00400C0** in consts.h. The old value sits on the
`bx r0`; a 4-byte patch there corrupts the copy-loop source literal @0xE00400C8 (=0xE1394358,
read by preamble ldr@0xE004003E). 0xE00400C0 is the (now-dead) `ldr r0,[pc]→0xE0040035`,
matching RP.160. **NEEDS HW VERIFY.**

---
## F1 agent detail (supporting, addresses independently verified)

# 13 - Cold-boot chain: reset -> cstart -> create_init_task -> init_task, and the ML wrap-and-swap hook

Target: Canon EOS M50 Mark II, firmware 1.0.1, Digic 8 (dual Cortex-A9 + GIC). ROM: `gang100.bin` @ base 0xE0000000, all Thumb-2. MAIN_FIRMWARE_ADDR = 0xE0040000.

This document narrates the full native Canon cold-boot chain instruction-by-instruction, decodes the `dryos_init_info` structure cstart builds, pins the exact instruction ML's wrap-and-swap boot hook intercepts, and gives an M1..M4 LED bring-up validation ladder.

## 1. Map of the chain

```
reset/firmware_entry 0xE0040000
  set VBAR=0xE03C50C0, sp=0x2000, read MPIDR
  core!=0 ---------------------------------> b.w 0xE0040034 -> b.w cstart (skip relocation)
  core 0:
    [0xD0110404]==1 ? write 3               (D8 power/SCU handshake - HW verify)
    b.w 0xE004003C  relocation preamble:
       blob copy 1: ROM 0xE1394358 -> RAM 0x00004000..0x0002AD1C
       blob copy 2: ROM 0xE13BB074 -> RAM 0x0223B000..0x02319530  + dcache_clean/icache_invalidate
       BSS zero 1 : 0x0002AD1C..0x000C70D8
       blob copy 3: ROM 0xE14995A4 -> 0xDF002800..0xDF0033A0     + dcache_clean/icache_invalidate
       BSS zero 2 : 0xDF0033A0..0xDF0033AC
    0xE00400C0 ldr r0,=0xE0040035 ; orr r0,#1 ; 0xE00400C6 bx r0  -> 0xE0040034 -> b.w cstart
cstart 0xE0040104
  read MPIDR, per-CPU init (0xE00401E8), CPU1 gate (0xE00403D0)
  MEM(0x4168/0x416C/0x4170) = 0x80000 / 0xA90000 / 0xA96000
  bzero32(sp+4, 0x78)                        (BR_BZERO32 = 0xE0040152)
  build dryos_init_info on stack (sp+4)
  fill 0xDF000000..0xDF002000 with 0xEEEEEEEE
  create_init_task(r0=&dryos_init_info, r1=0xE0040237(init_task|T), r2=0)   (BR_CREATE_ITASK = 0xE00401B4)
create_init_task 0xE02E9D80
  ... DryOS low-level init; stores r1(init_task) -> MEM(0x417C); tail into scheduler
init_task 0xE0040236  (Canon)  <-- ML swaps this pointer to my_init_task
```

## 2. Annotated disassembly (0xE0040000 .. 0xE00401E4)

### 2.1 Reset entry + VBAR (0xE0040000)
```
E0040000 ldr   r0,[pc,#0x34]        ; r0 = [0xE0040038] = 0xE03C50C0  (exception vector base)
E0040002 mcr   p15,0,r0,c12,c0,0    ; write VBAR = 0xE03C50C0
E0040006 isb   sy
E004000A movw  r0,#0x2000
E004000E movt  r0,#0
E0040012 mov   sp,r0                ; early SVC stack = 0x2000 (low RAM / TCM)
```

### 2.2 MPIDR secondary-core gate (0xE0040014)
```
E0040014 mrc   p15,0,r5,c0,c0,5     ; MPIDR
E0040018 ands  r0,r5,#0xf           ; core id (Aff0)
E004001C bne   0xE0040034           ; secondary cores -> 0xE0040034 (b.w cstart), skip relocation
E004001E movw  r0,#0x404
E0040022 movt  r0,#0xd011           ; r0 = 0xD0110404  (D8 MMIO - power/SCU, HW verify)
E0040026 ldr   r1,[r0]
E0040028 cmp   r1,#1
E004002A itt   eq
E004002C moveq r1,#3
E004002E streq r1,[r0]              ; if MEM(0xD0110404)==1 -> write 3
E0040030 b.w   0xE004003C           ; core 0 -> relocation preamble
E0040034 b.w   0xE0040104           ; the classic 'b.w cstart' trampoline (reached by secondary cores and, after relocation, by core 0)
```

### 2.3 Core-0 relocation preamble (0xE004003C)
Three ROM->RAM blob copies interleaved with cache maintenance and two BSS-zero loops. All source/dest pointers come from the literal pool 0xE00400C8..0xE0040103.

- Copy block 1: `[0xE1394358] -> 0x00004000 .. 0x0002AD1C` (word loop, 0xE0040042).
- Copy block 2: `[0xE13BB074] -> 0x0223B000 .. 0x02319530` (0xE0040056), then `dcache_clean(0x0223B000,len)` bl @0xE0040068 -> 0xE05D08F4 and `icache_invalidate(...)` bl @0xE0040072 -> 0xE05D09C8.
- BSS zero 1: `0x0002AD1C .. 0x000C70D8 = 0` (0xE004007E). Note 0xC70D8 is the top of the low DryOS image and becomes cstart's `r5` (DryOS RAM base).
- Copy block 3: `[0xE14995A4] -> 0xDF002800 .. 0xDF0033A0` (0xE004008E), then `dcache_clean`/`icache_invalidate` bl @0xE00400A0 / @0xE00400AA (uncached AHB / coprocessor descriptor region).
- BSS zero 2: `0xDF0033A0 .. 0xDF0033AC = 0` (0xE00400B6).

### 2.4 Reconverge to cstart (0xE00400C0)
```
E00400C0 ldr   r0,[pc,#0x3c]        ; r0 = [0xE0040100] = 0xE0040035  (abs addr of the b.w cstart trampoline, Thumb)
E00400C2 orr.w r0,r0,#1             ; force Thumb bit
E00400C6 bx    r0                   ; -> 0xE0040034 -> b.w 0xE0040104 (cstart)
```
This is the "indirect branch to cstart" ML must redirect (see section 5.4). BR_BR_CSTART should target 0xE00400C0 (the `ldr`), not 0xE00400C6 (the `bx`).

### 2.5 cstart (0xE0040104)
```
E0040104 push  {r4,r5,r6,lr}
E0040106 sub   sp,#0x80             ; frame holds the dryos_init_info at sp+4
E0040108 mrc   p15,0,r0,c0,c0,5     ; MPIDR
E004010C and   r6,r0,#0xf           ; r6 = core id
E0040110 mov.w r0,#0x800 ; str [sp] ; [sp] = 0x800 (arg5 to helper)
E0040116 mov.w r4,#0xDF000000
E004011A ldr   r0,=0x000C70DF       ; [0xE00401BC]
E004011C mov.w r2,#0x2000
E0040120 mov   r1,r4                ; r1 = 0xDF000000
E0040122 bic   r5,r0,#7             ; r5 = 0x000C70D8  (DryOS RAM base)
E0040126 mov   r0,r6                ; r0 = core id
E0040128 mov   r3,r5
E004012A bl    0xE00401E8           ; per-CPU init: writes per-CPU struct at VA 0x1000
E004012E cbz   r6,0xE0040138        ; core 0 -> main path
E0040130 bl    0xE00403D0           ; CPU1-only path (never taken by ML)
E0040134 add   sp,#0x80 ; pop {r4,r5,r6,pc}
```

Core-0 main path from 0xE0040138: writes three low-RAM globals then bzero32's the struct:
```
E0040138 MEM(0x4168) = 0x80000
E0040140 MEM(0x416C) = 0xA90000
E0040148 MEM(0x4170) = 0xA96000
E0040150 add   r0,sp,#4 ; movs r1,#0x78
E0040152 bl    0xE05D1F40           ; bzero32(&dryos_init_info, 0x78)   <-- BR_BZERO32
```

### 2.6 dryos_init_info build (0xE0040156 .. 0xE00401A2)
See section 4 for the full decoded table. Fields are all `r5`-relative or literal-pool constants (e.g. `add r0,r5` at 0xE004015A gives sys_mem_start = 0x11E600 + 0xC70D8 = 0x1E56D8).

### 2.7 Coprocessor region fill (0xE00401A4)
```
E00401A4 mov.w r0,#0xEEEEEEEE
E00401A8 stm   r4!,{r0} ; cmp r4,r1(=0xDF002000) ; blo   ; fill 0xDF000000..0xDF002000 with 0xEEEEEEEE
```

### 2.8 create_init_task call (0xE00401AE)
```
E00401AE movs  r2,#0                ; r2 = 0
E00401B0 ldr   r1,=0xE0040237       ; [0xE00401E4] = init_task | Thumb   (== Canon init_task 0xE0040236)
E00401B2 add   r0,sp,#4             ; r0 = &dryos_init_info
E00401B4 bl    0xE02E9D80           ; create_init_task(r0,r1,r2)          <-- BR_CREATE_ITASK  (THE HOOK)
E00401B8 b     0xE0040134           ; -> add sp,#0x80 ; pop {r4,r5,r6,pc}
```

## 3. cstart literal pool (0xE00401BC .. 0xE00401E4)

| Addr | Word | Meaning |
|---|---|---|
| 0xE00401BC | 0x000C70DF | -> bic #7 -> r5 = 0xC70D8 (DryOS RAM base) |
| 0xE00401C0 | 0x00004168 | global MEM(0x4168)=0x80000 |
| 0xE00401C4 | 0x0000416C | global MEM(0x416C)=0xA90000 |
| 0xE00401C8 | 0x00A96000 | value for MEM(0x4170) |
| 0xE00401CC | 0x00004170 | global MEM(0x4170) |
| 0xE00401D0 | 0x0011E600 | +r5 -> sys_mem_start / sys_objs_end |
| 0xE00401D4 | 0x000E2200 | sys_mem_len |
| 0xE00401D8 | 0x00112E80 | user_mem_len |
| 0xE00401DC | 0x00113680 | +r5 -> sys_objs_start |
| 0xE00401E0 | 0xDF002000 | fill end |
| **0xE00401E4** | **0xE0040237** | **r1 = init_task (Thumb) == 0xE0040236** |

The two create_init_task arguments the task asked for:
- **r1 (init_task)** = word @0xE00401E4 = **0xE0040237** (Canon init_task 0xE0040236 with Thumb bit).
- **r0 (dryos_init_info\*)** = **sp+4** - a stack-built struct, not a pooled pointer (decoded in section 4).
- r2 = 0.

## 4. Decoded dryos_init_info (M50 II 1.0.1) and RESTARTSTART cross-check

Struct base = sp+4; r5 = 0xC70D8. Field names per `src/boot-d678.h`.

| Off | Field | Value | Derivation / store site |
|---|---|---|---|
| 0x00 | sys_mem_start | 0x1E56D8 | 0x11E600 + r5 (strd @0xE004015C) |
| 0x04 | sys_mem_len | 0xE2200 | lit 0xE00401D4 |
| 0x08 | **user_mem_start** | **0xC78D8** | r5 + 0x800 (str @0xE0040164) |
| 0x0C | user_mem_len | 0x112E80 | lit 0xE00401D8 |
| 0x10 | sys_objs_start | 0x1DA758 | 0x113680 + r5 |
| 0x14 | sys_objs_end | 0x1E56D8 | = sys_mem_start |
| 0x18 | prio_max | 0x22 (34) | movs #0x22 |
| 0x1C | task_max | 0xC7 (199) | movs #0xC7 |
| 0x20 | semaphore_max | 0x1CF (463) | movw #0x1CF |
| 0x24 | event_max | 0x8E (142) | movs #0x8E |
| 0x28 | message_q_max | 0xE6 (230) | movs #0xE6 |
| 0x2C | mutex_max | 0x3D (61) | movs #0x3D |
| 0x30 | condition_max | 0 | movs #0 |
| 0x34 | timer_max | 0 | str |
| 0x38 | vector_max | 0 | str |
| 0x5C | prio_default | 0x10 | movs #0x10 |
| 0x60 | stack_default | 0x1000 | lsls #8 |
| 0x64 | stack_idle | 0x100 | asrs #4 |
| 0x68 | stack_init | 0x1000 | str |

Consistency checks (all pass):
- `user_mem_start + user_mem_len = 0xC78D8 + 0x112E80 = 0x1DA758 = sys_objs_start`.
- `sys_objs_end = sys_mem_start = 0x1E56D8`; ordering `user_mem_start < sys_objs_start < sys_mem_start` satisfies the assert in `my_create_init_task`.
- **RESTARTSTART**: `user_mem_start = 0xC78D8`, `0xC78D8 % 0x10 = 8` -> ELF-aligned up to **0xC78E0**, which is exactly `RESTARTSTART` in `platform/M50mk2.101/Makefile`. This validates both the struct decode and the RESTARTSTART constant.

## 5. The ML wrap-and-swap hook

### 5.1 Where the relocation/patching happens
`src/reboot.c` `_start` -> checksum -> `cstart` -> `copy_and_restart()` (`src/boot-d678.c`). `copy_and_restart` copies `FIRMWARE_ENTRY_LEN = 0x800` bytes from 0xE0040000 into the ML BSS buffer `_reloc`, patches branches in the copy, then jumps to `RELOCADDR + 1` (the relocated firmware_entry). Because 0x800 covers cstart (0x104), BR_CREATE_ITASK (0x1B4) and the literal pool (up to 0x1E4), no separate `CSTART_LEN` is needed for the M50 II.

### 5.2 Patch list applied to the relocated copy (CONFIG_DIGIC_78X)
- BR_DCACHE_CLN_1/2 (0xE0040068, 0xE00400A0) -> `my_dcache_clean` (R3-preserving shim)
- BR_ICACHE_INV_1/2 (0xE0040072, 0xE00400AA) -> `my_icache_invalidate`
- BR_BR_CSTART -> relocated cstart (redirect the indirect branch into the patched copy)
- BR_BZERO32 (0xE0040152) -> `my_bzero32`
- **BR_CREATE_ITASK (0xE00401B4) -> `my_create_init_task`  <-- the wrap point**

### 5.3 The exact hook and the r1 swap
The single intercept that matters for control handoff is the `bl create_init_task` at **BR_CREATE_ITASK = 0xE00401B4**. Its relocated copy is rewritten to call `my_create_init_task(dryos, init_task, c)`.

The ROM still loads `r1 = 0xE0040237` (Canon init_task) at 0xE00401B0 and passes it in. The swap is **not** done by patching that `ldr`; it is done inside the C wrapper:
```c
// src/boot-d678.c  my_create_init_task (default, non-TRACE)
if (dryos->sys_objs_start < dryos->sys_mem_start && dryos->user_mem_start < dryos->sys_objs_start) { }
init_task = (uint32_t)my_init_task;                 // <-- THE SWAP (overwrites the r1 arg)
... align user_mem_start, check RESTARTSTART, steal ml_reserved_mem from user_mem ...
create_init_task(dryos, init_task, c);              // tail-call real Canon create_init_task with r1=my_init_task
```
Why a plain register overwrite is sufficient: Canon `create_init_task` (0xE02E9D80) forwards `r1` to `setup_helper 0xE0166A5D`, which stores it once into `MEM(0x417C)` (`str r4,[r0,#4]`, base 0x4178). The whole DryOS handoff is entry-pointer-driven from 0x417C: `canon_first_init_helper 0xE0166A77` reads 0x417C into the "init" task descriptor, `task_create` copies it into `task+0x0C`, and `task_trampoline 0xE00A6C74` does `blx task->entry`. So `my_init_task` runs as the first real OS task, in a fully initialized context; it does ML pre-init, then chains to Canon `init_task` (0xE0040236) via `init_task_func(a,b,c,d)` in `src/boot.h`. (See docs/10 for the full Canon handoff decode.)

### 5.4 BR_BR_CSTART correction (0xE00400C6 -> 0xE00400C0)
`platform/M50mk2.101/consts.h` currently sets `BR_BR_CSTART 0xE00400C6` (the `bx r0`). RP.160 - a byte-for-byte structural twin (every other BR_ matches) - uses `0xE00400C0` (the `ldr r0,=0xE0040035`). `patch_thumb_branch` writes a 4-byte `bl` at the target; at 0xE00400C6 that write spills into 0xE00400C8..0xE00400C9, corrupting the copy-block-1 source literal (0xE1394358) that core 0 re-reads from the relocated buffer at 0xE004003E - which would break the ROM->RAM relocation. Patching at 0xE00400C0 overwrites only the dead `ldr`/`orr` bytes and leaves the literal pool intact. Recommend changing to 0xE00400C0 and verifying on hardware.

## 6. M1..M4 LED bring-up validation ladder

LED (M50mk2 consts.h, HW-verify): `CARD_LED_ADDRESS=0xD01300E4`, `LEDON=0xD0002`, `LEDOFF=0xC0003` (alt 0xD01301A4). The existing `trace_blink_once(n)/trace_blink_forever(n)` helpers in `boot-d678.c` already drive this LED. Build each marker as a "reach here then blink-forever" bisect (one build per marker, or a single laddered build).

| Marker | Fires at | ROM/patch anchor | Proves | Instrument (existing) |
|---|---|---|---|---|
| **M1** | relocated cstart reached | BR_BZERO32 0xE0040152 -> `my_bzero32` | BR_BR_CSTART redirect + reloc jump landed in the patched cstart copy; VBAR/relocation preamble survived | `-DM50II_TRACE_STOP_AT_BZERO` (blinks 4 forever) or blink 1 in `my_bzero32` |
| **M2** | wrap point entered | BR_CREATE_ITASK 0xE00401B4 -> `my_create_init_task` | The create_init_task hook fired - this is the intercept | `-DM50II_TRACE_STOP_AT_CREATE_ITASK` (blinks 5 forever) or `M50II_CREATE_STAGE(1)` |
| **M3** | just before real create_init_task | `my_create_init_task` after all checks (RESTARTSTART==user_mem_start, layout, MINIMUM_USER_MEM_LEFT) | r1 swapped to `my_init_task` and ML memory reservation is self-consistent | `M50II_CREATE_STAGE(10)` (`-DM50II_TRACE_CREATE_LADDER`) |
| **M4** | ML init task running | `my_init_task` entry (src/boot.h) | DryOS completed low-level init, created the "init" task with entry=my_init_task (the swap took), and the scheduler dispatched it | `M50II_INIT_STAGE(1)` (`-DM50II_TRACE_...` init trace) |

Interpretation:
- Stops at M1 but not M2: the relocated cstart runs but never reaches BR_CREATE_ITASK - suspect BR_BR_CSTART (section 5.4) or a bad bzero32/cache shim.
- Reaches M2 not M3: layout/RESTARTSTART/`MINIMUM_USER_MEM_LEFT` check failed (`goto fail; while(1);`) - re-check the section-4 struct decode and RESTARTSTART=0xC78E0.
- Reaches M3 not M4: Canon `create_init_task`/DryOS scheduler handoff did not deliver `my_init_task` - the deepest and most fragile step (matches the extensive `M50II_TRACE_*` handoff experiments already in `boot-d678.c`).
- Reaches M4: cold-boot handoff is complete and ML owns the init task; proceed to `boot_pre_init_task`/Canon `init_task` chaining.

Once M4 is green on hardware, all `M50II_TRACE_*` scaffolding can be removed in favor of the clean default `my_create_init_task` (the swap path in section 5.3).

---
### struct layouts
```

### struct dryos_init_info (M50 II 1.0.1 decoded values, base = sp+4 in cstart)
  +0x00   sys_mem_start                uint32_t   | strd @0xE004015C; r0 = [0xE00401D0]=0x0011E600 + r5(0xC70D8) = 0x001E56D8
  +0x04   sys_mem_len                  uint32_t   | strd @0xE004015C; r1 = [0xE00401D4]=0x000E2200
  +0x08   user_mem_start               uint32_t   | str @0xE0040164; add.w r1,r5,#0x800 = 0xC70D8+0x800 = 0x000C78D8. ELF-aligns (0x
  +0x0C   user_mem_len                 uint32_t   | str @0xE0040168; r1 = [0xE00401D8]=0x00112E80. Check: user_mem_start+len = 0xC78
  +0x10   sys_objs_start               uint32_t   | strd @0xE004016E; r1 = [0xE00401DC]=0x00113680 + r5 = 0x001DA758
  +0x14   sys_objs_end                 uint32_t   | strd @0xE004016E; r0 = sys_mem_start = 0x001E56D8 (sys_objs region ends where sy
  +0x18   prio_max                     uint32_t   | movs r0,#0x22; str @0xE0040174 -> 0x22 (34)
  +0x1C   task_max                     uint32_t   | movs r0,#0xc7; str @0xE0040178 -> 0xC7 (199)
  +0x20   semaphore_max                uint32_t   | movw r0,#0x1cf; str @0xE004017E -> 0x1CF (463). Beyond the fields ML currently u
  +0x24   event_max                    uint32_t   | movs r0,#0x8e; str @0xE0040182 -> 0x8E (142)
  +0x28   message_q_max                uint32_t   | movs r0,#0xe6; str @0xE0040186 -> 0xE6 (230)
  +0x2C   mutex_max                    uint32_t   | movs r0,#0x3d; str @0xE004018A -> 0x3D (61)
  +0x30   condition_max                uint32_t   | movs r0,#0; str @0xE004018E -> 0 (also cleared by bzero32 0x78)
  +0x34   timer_max                    uint32_t   | str @0xE0040190 -> 0
  +0x38   vector_max                   uint32_t   | str @0xE0040192 -> 0
  +0x5C   prio_default                 uint32_t   | movs r0,#0x10; str @0xE0040196 -> 0x10 (16). Offset 0x5C = 0x3C + 8*4 (after 8 u
  +0x60   stack_default                uint32_t   | lsls r0,#8 (=0x1000); strd @0xE004019C -> 0x1000
  +0x64   stack_idle                   uint32_t   | asrs r1,r0,#4 (=0x100); strd @0xE004019C -> 0x100
  +0x68   stack_init                   uint32_t   | str r0,[sp,#0x6c] @0xE00401A2 -> 0x1000

### struct boot_info_block @0x4160 (low-RAM/TCM globals written by cstart before bzero32)
  +0x4168 unk_heap_ptr_a               uint32_t   | cstart @0xE0040138: mov.w r0,#0x80000; ldr r1,=[0xE00401C0]=0x4168; str r0,[r1] 
  +0x416C unk_heap_ptr_b               uint32_t   | cstart @0xE0040140: mov.w r0,#0xA90000; ldr r1,=[0xE00401C4]=0x416C; str -> MEM(
  +0x4170 unk_heap_ptr_c               uint32_t   | cstart @0xE0040148: r0=[0xE00401C8]=0x00A96000; ldr r1,=[0xE00401CC]=0x4170; str

```
