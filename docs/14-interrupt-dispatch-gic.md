# docs/14 — Interrupt Dispatch & GIC Foundation (WS-F4)

**Verified 2026-07-07.** GIC map + `isr_dispatch` from my independent romdis; the VBAR
exception table (@0xE03C50C0) and `irq_vector_handler` 0xE03C50FC below were also verified
directly (reset literal 0xE0040001→firmware_entry; IRQ literal 0xE03C50FD→handler; handler is
a textbook ARM IRQ entry `sub lr,lr,#4; srsdb; cpsid; ldr [0x100C]`).

## isr_dispatch 0xE018E17C (verified)
```
r7 = [0xC100010C]           ; GICC_IAR (ack -> interrupt id)
id = r7 & 0x3FF
ldrd fp,sl,[0x4004,#8]      ; fp=[0x400C]=pre_isr_hook, sl=[0x4010]=post_isr_hook
handler = [0x2AD1C + id*8]  ; isr_table_handler ; param = [0x2AD20 + id*8] = isr_table_param
... pre_isr_hook -> handler(param) -> post_isr_hook ...
str r7,[0xC1000110]         ; GICC_EOIR (end of interrupt)
```

## GICv2 register map (all evidence-backed; added as named refs)
| reg | addr |   | reg | addr |
|-----|------|---|-----|------|
| GICD_CTLR (base) | 0xC1001000 | | GICD_ITARGETSR | 0xC1001800 |
| GICD_ISENABLER | 0xC1001100 | | GICD_ICFGR | 0xC1001C00 |
| GICD_ICENABLER | 0xC1001180 | | GICD_SGIR | 0xC1001F00 |
| GICD_ISPENDR | 0xC1001200 | | GICC_CTLR (base) | 0xC1000100 |
| GICD_IPRIORITYR | 0xC1001400 | | GICC_PMR | 0xC1000104 |
| GICC_IAR | 0xC100010C | | GICC_EOIR | 0xC1000110 |

Interrupt API added to stubs.S: `isr_dispatch` 0xE018E17C · `register_interrupt` 0xE018E384 ·
`enable_interrupt` 0xE018E2A8 · `disable_interrupt` 0xE018E2E4 · `arm_gic_init` 0xE00A69EE ·
data `pre_isr_hook` 0x400C · `post_isr_hook` 0x4010 · `isr_table_handler` 0x2AD1C · `isr_table_param` 0x2AD20.

---
# 14 - Interrupt dispatch + GIC foundation (M50 II 1.0.1)

Decoded from `gang100.bin` (ROM0 @ 0xE0000000, Thumb-2) this session. Every address
below is justified by a romdis prologue **and** an anchor (xref / literal / donor match).
Builds on docs/09 (ICU 0x000-0x1FF + GIC 0x200-0x23f map) and docs/10 (boot handoff).

---

## 1. IVT -> handler map (VBAR table @ 0xE03C50C0)

The exception vector table is a **mixed ARM/Thumb** table: the RESET vector is ARM
(`ldr pc,[pc,#0x18]`, used at power-on with SCTLR.TE=0); the other 7 are Thumb-2
(`ldr.w pc,[pc,#imm]`, taken after DryOS sets SCTLR.TE=1). Targets are the literals the
`ldr.w pc` instructions actually resolve to (NOT naive pool position):

| VBAR off | Vector | Vector insn @addr | literal | handler (Thumb) |
|---|---|---|---|---|
| +0x00 | Reset | E03C50C0 `ldr pc,[pc,#0x18]` (ARM) | E03C50E0=E0040001 | **0xE0040000** firmware_entry |
| +0x04 | Undef | E03C50C4 `ldr.w pc,[pc,#0x1C]` | E03C50E4 | 0xE02E9C70 |
| +0x08 | SVC   | E03C50C8 `ldr.w pc,[pc,#0x1C]` | E03C50E8 | 0xE02E9CCE |
| +0x0C | PAbort| E03C50CC `ldr.w pc,[pc,#0x1C]` | E03C50EC | 0xE02E9C9A |
| +0x10 | DAbort| E03C50D0 `ldr.w pc,[pc,#0x1C]` | E03C50F0 | 0xE02E9CB4 |
| +0x14 | Reserved | E03C50D4 `nop.w` | - | - |
| +0x18 | **IRQ** | E03C50D8 `ldr.w pc,[pc,#0x18]` | E03C50F4=E03C50FD | **0xE03C50FC** irq_vector_handler |
| +0x1C | FIQ | E03C50DC `ldr.w pc,[pc,#0x18]` | E03C50F8=E03EF0DD | 0xE03EF0DC (`b .`, parked/unused) |

### IRQ path

1. **`irq_vector_handler` = 0xE03C50FC** (VBAR+0x18 target). Entry is the classic IRQ
   return-fixup `sub.w lr,lr,#4`, then `srsdb sp!,#0x13` / `cpsid i,#0x13`. Reads the
   interrupt-nesting counter `[0x100C]`; on the first-level interrupt it switches to the
   IRQ stack `[0x1014]` and calls the dispatcher; nested interrupts take a shorter path.
   Returns with `rfeia sp!`.
2. **`isr_dispatch` = 0xE018E17C** (the real dispatcher; called only from 0xE03C5134 /
   0xE03C5184 inside the vector handler). This is the function that reads the controller,
   writes `current_interrupt` (0x1010) and indexes the handler table:
   - `r0 = 0xC1000000`; `r7 = [0xC100010C]` = **GICC_IAR** (acknowledge); `id = r7 & 0x3FF`.
   - **ICU-cascade path** (IAR id == 0x20 or 0x21 = GIC_MariusIntc SPI32 / GIC_OmarIntc
     SPI33, docs/09): bank = id&1; ICU base = `[0xE0997D48 + bank*4]`; real ICU id
     `r5 = [icu_base+0x1000]`. Then **`str r5,[0x1010]` = writes `current_interrupt`**.
     Handler = `[0x2AD1C + id*8]`, arg = `[0x2AD1C + id*8 + 4]`.
     Calls `pre_isr_hook` (`[0x400C]`, if non-null, arg=id) **before** the handler,
     writes **GICC_EOIR `[0xC1000110] = IAR`**, `blx handler(arg)`, then
     `post_isr_hook` (`[0x4010]`, arg=id) **after**. Unhandled id -> `uart_printf` spurious.
   - **GIC-native path** (all other ids): handler table = `[0x4008]`
     (`DRYOS_SGI_HANDLERS_PTR`); handler = `[[0x4008] + id*8]`; id 0x3FF = spurious return;
     `GICC_EOIR = IAR` on exit.

So on M50 II almost every peripheral interrupt arrives via the **ICU->GIC cascade**
(SPI 32/33), is expanded to a 0x000-0x1FF ICU id, stored in `current_interrupt`, and
dispatched through the **ICU handler table at 0x2AD1C** with `pre_/post_isr_hook` wrappers.
Native GIC SGIs/PPIs (scheduler IPI, private timer) dispatch through `[0x4008]`.

---

## 2. Interrupt-registration API (what ML hooks)

ML's DIGIC 6/7/8 interrupt code (`src/log-d678.c`, `src/tskmon.c`, `src/gdb.c`) does **not**
call a Canon `register_interrupt`; it (a) reads the ICU handler table directly and (b)
installs `pre_isr_hook`/`post_isr_hook`. `log-d678.c` uses exactly:
`isr_table_handler[2*isr]` and `isr_table_param[2*isr]` (8-byte stride) -> table base
0x2AD1C. So the stubs ML needs are four data pointers:

| ML symbol | address | evidence |
|---|---|---|
| `pre_isr_hook`      | **0x400C** | `isr_dispatch` `ldrd fp,sl,[0x4004,#8]`; `fp` called before handler |
| `post_isr_hook`     | **0x4010** | same `ldrd`; `sl` called after handler (post = pre+4) |
| `isr_table_handler` | **0x2AD1C** | dispatcher `r4=[0x2AD1C + id*8]`; matches `isr_table_handler[2*isr]` |
| `isr_table_param`   | **0x2AD20** | dispatcher `r6=[0x2AD1C + id*8 + 4]`; matches `isr_table_param[2*isr]` |

The underlying DryOS API (documentation / optional stubs), all in the E018E2xx cluster
guarded by `cli` (0xE021B8F0) + the 0x4004 dispatch spinlock:

| function | addr | signature / effect |
|---|---|---|
| `register_interrupt` | **0xE018E384** | (id,handler,arg) -> `[0x2AD1C+id*8]=handler; [+4]=arg`. 8 callers incl. core-init 0xE007D6F0/6FC |
| `enable_interrupt`   | **0xE018E2A8** | (id): id>0x200 -> gic_enable_int(id-0x200) @0xE00A69BA; else ICU enable @0xE00D464E |
| `disable_interrupt`  | **0xE018E2E4** | (id): id>0x200 -> GIC @0xE00A6A94; else ICU @0xE00D467E |
| `set_int_config`     | **0xE018E320** | (id,cfg): id>0x200 -> GIC ICFGR helper @0xE00A6A58 |
| `set_int_target/pri` | **0xE018E352** | (id,val): id>0x200 -> GIC helper @0xE00A6A64 |

Canonical usage (core-init helper 0xE007D6E4, registering cascade SPI 0x22b/0x22d):
`register_interrupt(id,h,0)` -> `set_int_config(id,cfg)` -> `enable_interrupt(id)`.
The `id < 0x200` = ICU, `id >= 0x200` = GIC (id-0x200 = GIC id) split is the universal
convention across the whole cluster.

---

## 3. GIC init summary

**GIC memory map (ARM GIC-400 / PL390 offsets, all confirmed by literals/writes):**

| register | address | notes |
|---|---|---|
| GICD_CTLR       | 0xC1001000 | distributor enable |
| GICD_ISENABLER  | 0xC1001100 | set-enable (`gic_enable_int`) |
| GICD_ICENABLER  | 0xC1001180 | clear-enable (init disables ints 0-63) |
| GICD_ISPENDR    | 0xC1001200 | set-pending (accessor @0xE000C5AE) |
| GICD_IPRIORITYR | 0xC1001400 | byte/int; init writes 0x10 @ 0xC1001420/21 |
| GICD_ITARGETSR  | 0xC1001800 | byte/int (`gic_set_target` @0xE00A6B38) |
| GICD_ICFGR      | 0xC1001C00 | 2 bits/int (config accessor @0xE000C5CA) |
| GICD_SGIR       | 0xC1001F00 | software interrupt gen (`send_software_interrupt` 0xE00D4034) |
| GICC_CTLR       | 0xC1000100 | CPU interface enable |
| GICC_PMR        | 0xC1000104 | priority mask |
| GICC_IAR        | 0xC100010C | acknowledge (read in dispatcher) |
| GICC_EOIR       | 0xC1000110 | end-of-interrupt (written in dispatcher) |

**`arm_gic_init` = 0xE00A69EE** (called from core-init 0xE007D73E, alongside ICU init
0xE00D45F4):
1. `GICD_ICENABLER[0..1] = 0xFFFFFFFF` -> disable ints 0-63.
2. `GICD_CTLR = 1` -> enable distributor.
3. `bl 0xE00A69A8` = **gic_cpu_interface_init**: `GICC_CTLR=1`, `GICC_PMR=0xFF`.
4. For the ICU->GIC cascade lines SPI 32 (Marius, id 0x20) and SPI 33 (Omar, id 0x21):
   set config, `GICD_IPRIORITYR[id]=0x10`, `gic_enable_int(id)`.

This is exactly why docs/09's boot-handoff re-enable of "IRQ32/IRQ33" makes hardware
serviceable: those are the two GIC SPIs that carry every ICU interrupt into the Cortex-A9.

Per-CPU note: `send_software_interrupt` 0xE00D4034 (docs/10) writes GICD_SGIR
(0xC1001F00); SGI 10 = GIC_scheduling (CPU1 wake), SGI 12 = GIC_suspend.

---

## 4. Data structures

**DryOS interrupt-control block @ 0x4004** (base pointer used as `ldr r0,=0x4004`):

| off | addr | field |
|---|---|---|
| +0x00 | 0x4004 | dispatch spinlock (ldrex/strex) |
| +0x04 | 0x4008 | GIC handler-table base ptr (`DRYOS_SGI_HANDLERS_PTR`) |
| +0x08 | 0x400C | `pre_isr_hook` fn ptr |
| +0x0C | 0x4010 | `post_isr_hook` fn ptr |

(0x4000 = `DRYOS_ASSERT_HANDLER`, already in consts.h — the block is 0x4000..0x4010.)

**Low-RAM interrupt globals @ 0x1000:**

| off | addr | field |
|---|---|---|
| +0x08 | 0x1008 | current core's ICU bank index (0/1), used by enable/disable |
| +0x0C | 0x100C | interrupt-nesting counter (0 = first-level) |
| +0x10 | 0x1010 | `current_interrupt` (ICU id of interrupt in service) |
| +0x14 | 0x1014 | saved IRQ stack pointer |
| +0x28 | 0x1028 | `current_task` (already in stubs.S) |

**ICU handler table @ 0x2AD1C** (and GIC table @ `[0x4008]`): 8-byte entries indexed by id:
`+0x00` handler fn ptr, `+0x04` handler arg.

---

## 5. Proposed stubs.S / consts.h additions

```c
/* stubs.S -- Interrupts (docs/14) */
DATA_PTR(   0x400C,  pre_isr_hook)     /* isr_dispatch ldrd fp,[0x4004,#8]; called before ISR */
DATA_PTR(   0x4010,  post_isr_hook)    /* isr_dispatch ldrd sl,[0x4004,#0xC]; called after ISR */
DATA_PTR(  0x2AD1C,  isr_table_handler)/* ICU handler table; dispatcher r4=[0x2AD1C+id*8] */
DATA_PTR(  0x2AD20,  isr_table_param)  /* +4: dispatcher r6=[0x2AD1C+id*8+4] */
/* current_interrupt 0x1010 and DRYOS_SGI_HANDLERS_PTR 0x4008 already present - CONFIRMED */
THUMB_FN(0xE018E17C, isr_dispatch)     /* master dispatcher: GICC_IAR->current_interrupt->table */
THUMB_FN(0xE018E384, register_interrupt) /* (id,handler,arg)->ICU_TABLE[id]; 8 callers */
THUMB_FN(0xE018E2A8, enable_interrupt) /* (id) ICU/GIC split at 0x200 */
THUMB_FN(0xE018E2E4, disable_interrupt)/* (id) ICU/GIC split at 0x200 */
THUMB_FN(0xE00A69EE, arm_gic_init)     /* dist+cpu-if init; enables SPI32/33 cascade */
```

Verify on HW: whether runtime VBAR points at ROM (0xE03C50C0) or a RAM copy; that
pre/post-hook fire order matches ML semantics; GICC_PMR=0xFF (init value, not 0xF0).
