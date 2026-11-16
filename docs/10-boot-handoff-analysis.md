# 10 - Boot handoff analysis & clean wrap-and-swap design

Date: 2026-07-07

Target: Canon EOS M50 Mark II, firmware 1.0.1, Digic 8 (dual-core Cortex-A9 + GIC).

Phase A goal: replace the fragile hand-mirror of Canon's boot handoff currently in
`magiclantern_simplified/src/boot-d678.c` (the `M50II_TRACE_*` diagnostic paths) with a
clean **wrap-and-swap of `my_create_init_task`**, matching the proven EOS RP / 200D /
Digic X design. This document decodes the Canon handoff end-to-end, then specifies the
swap.

> Source: 12 structured function decodes of the create-init-task / scheduler-start path,
> cross-checked against `docs/08-boot-trace-report.md` (hardware test history),
> `docs/09-digic8-interrupts-registers.md` (named GIC/SGI/UART regs),
> `platform/M50mk2.101/stubs.S`, `platform/M50mk2.101/consts.h`, and the shared
> `src/boot-d678.c` / `src/boot-d678.h`.

---

## 1. Decoded call graph of the Canon boot handoff

Canon's boot, from our relocated `cstart` to the first ML-controllable instruction:

```text
cstart (0xE0040104)
  │  reads MPIDR, bzero32 (bl @0xE0040152),
  │  then bl create_init_task (BR_CREATE_ITASK = 0xE00401B4, r1 = init_task 0xE0040236)
  ▼
create_init_task (0xE02E9D80)                     r0=dryos, r1=init_task fn, r2=c
  ├─ bl create_helper1            0xE00A674B   GIC/interrupt-controller bring-up
  ├─ bl sched_lock_helper         0xE01B2139   ACQUIRE scheduler spinlock 0x4024
  ├─ bl setup_helper(dryos,it,c)  0xE0166A5D   validate dryos layout; **store init_task -> MEM(0x417C)**
  ├─ bl setup_helper_after_create 0xE01669D5   subsystem init chain + DryOS banner
  ├─ bl final_setup_helper        0xE0166AD5   create + enqueue the 2 per-core "idle" tasks
  │                                            (idle entry = canon_first_init_helper 0xE0166A77)
  ├─ pop {…}
  └─ b.w scheduler_start          0xE01B207E   TAIL-CALL, never returns
       │  pick_first_task(core) -> first runnable = an idle task
       │  writes current_task (0x1028) + per-CPU slot (0x4038+core*4)
       ▼
     context_restore              0xE03C51C3   (b.w tail) loads task+0x54 SP,
       │  releases 0x4024 for fresh ctx, pop {r0-r12,lr}, `rfeia sp!`
       ▼
   canon_first_init_helper        0xE0166A77   ← THE FIRST TASK the scheduler runs
       │  guarded by once-flag MEM(0x4178); builds a task_create descriptor whose
       │  ENTRY = MEM(0x417C); spawns the singleton "init" task; then WFI-idles
       ├─ bl task_create_trampoline 0xE00A6C95  (desc+4 = MEM(0x417C) -> new task+0x0C)
       ├─ bl make-ready             0xE01B1BE7  (r1=1, preempt allowed)
       └─ bl WFI idle loop          0xE00A679F  (scheduler preempts to the "init" task)
            ▼
        task_trampoline            0xE00A6C74   saved initial-context PC = 0xE00A6C75
          │  unlock scheduler (0xE01B2237, cpsie i), then:
          └─ blx task->entry (task+0x0C)  ← == MEM(0x417C) == init-task entry
               ▼
             init-task entry  (Canon 0xE0040236, or ML my_init_task after the swap)
```

Key structural facts that make the swap trivial:

- **The init-task entry is single-sourced from `MEM(0x417C)`.** `setup_helper 0xE0166A5D`
  copies `r1` (the `init_task` argument) into `0x417C` with one store
  (`str r4,[r0,#4]` @ `0xE0166A6A`, base `0x4178`). Nothing else writes it. Whatever `r1`
  is when the real `create_init_task` runs becomes the init-task entry.
- **Everything downstream is entry-pointer driven.** `canon_first_init_helper` reads
  `0x417C` into the descriptor (`ldr r0,[r4,#4]` @ `0xE0166AA8`), `task_create` copies
  descriptor+4 into `task+0x0C`, and `task_trampoline` does `blx task->entry`. No code
  re-derives the entry from ROM.
- **Canon starts its own scheduler in its own context.** `scheduler_start 0xE01B207E`
  is a tail-call that never returns; it and `context_restore` release lock `0x4024`,
  install `current_task`, and (via the sibling dispatch path) send SGI 10 to wake CPU1.
  If we let Canon reach this normally, the scheduler/lock/CPU1 handshake is all done by
  Canon code with correct runtime state — exactly what the hand-mirror was faking.

---

## 2. Per-function summary table

| Addr | Name | Purpose | Locks / SGI / CPU1 | Struct writes | Control end |
|---|---|---|---|---|---|
| `0xE02E9D80` | `create_init_task` | Orchestrator: 5 helper `bl`s then tail-branch to scheduler_start | takes 0x4024 via helper 0xE01B2139 | (via helpers) | tail `b.w 0xE01B207E`, never returns |
| `0xE00A674B` | `create_helper1` | GIC distributor + per-CPU init; register SPIs 0x20A/0x22B/0x22D; publish cb ptr to 0x40C8 | GIC MMIO @ `0xC1001000`; serializes on **0x4004** via 0xE021B8F0; SPIs only (no SGI) | int-handler table `0x2AD1C`, global `0x40C8`, GIC state | `pop {r4,pc}`; 0 ok / -1 fail |
| `0xE01B2139` | `sched_lock_helper` | Bounded ldrex/strex ACQUIRE of scheduler lock 0x4024, then tail into dispatch core | writes **0x4024 = 1**; no cpsid here; downstream sends SGI 10 | `0x4024` | tail `b 0xE01B1B10`; register-restore owned by continuation |
| `0xE0166A5D` | `setup_helper_dryos_init_c` | Validate dryos region layout/bounds; **record init_task -> 0x417C** | none (no lock, no cpsid) | **`0x417C = init_task`** (str r4,[r0,#4]); callee fills 0x40F4 / 0x2CE88 | `pop {r4,pc}`; 0 ok / -1 fail |
| `0xE01669D5` | `setup_helper_after_create` | ~10 subsystem inits (aborts on first -1) + DryOS boot banner | none; locking is inside callees | own stack only; **touches no 0x41xx / 0x1028** | `pop {r4,pc}`; 0 ok / -1 fail |
| `0xE0166AD5` | `final_setup_helper` | 2-iter loop: build idle descriptor, create + enqueue idle task for core 0 and core 1 | reads 0x4134/0x413C/0x4104; run-queue touch inside 0xE01B1BE6; provisions CPU1 idle | on-stack idle descriptor only | `pop.w {…,pc}` |
| `0xE01B207E` | `scheduler_start` | First-dispatch: pick_first_task, install current_task, tail into context_restore | reads core id `[0x1008]`; refs 0x4024+0x14 array; lock released downstream | **`0x1028`=first_task**, `0x4038+core*4`, `task+0x51`, clears `0x2C054[core]` | tail `b.w 0xE03C51C2`, never returns |
| `0xE03C51C3` | `context_restore` | Reload task SP (task+0x54), pop regs, `rfeia` PC+CPSR into the task | fresh ctx (flag 0) -> `bleq 0xE01B217A` sets **0x4024=0**; dsb; no cpsid | clears ctx+4 flag; callee zeroes 0x4024/0x4034 | `rfeia sp!`, never returns |
| `0xE0166A77` | `canon_first_init_helper` | First task dispatched; guarded by 0x4178, builds "init" descriptor with **entry = MEM(0x417C)**, creates + readies it, then WFI-idles | brackets create in 0x4024 crit-sec (0xE01B21CD/0xE01B21F3); once-flag 0x4178; no SGI here | **`0x4178 = 1`**; descriptor from 0x417C/0x4140/0x4138/byte 0x1008 | never returns (WFI loop 0xE00A679F) |
| `0xE00A6C74` | `task_trampoline` | Common task entry shim; unlock scheduler then `blx task->entry(arg)` | unlock 0x4024 via 0xE01B2236 (cpsie i); re-lock via 0xE01B21CC | none (reads task+0x0C entry, +0x10 arg) | body `blx`; long-lived tasks never return |
| `0xE01B21CD` | `critsec_save_token` | Crit-sec ENTRY: irqsave (cpsid in 0xE021B8F0) + take 0x4024, save token to *arg0 | **cpsid i** (in callee); reads `[0x1000+8]`, per-task tbl 0x4040 | `*arg0 = saved CPSR`; callee writes 0x4024=1 | `pop`/tail `b 0xE01B1BA4` |
| `0xE01B21F3` | `critsec_restore` | Crit-sec RELEASE: run deferred work, drop 0x4024, restore CPSR | writes **0x4024=0**, 0x4034=0; `msr cpsr_c`; can trigger SGI 10 via 0xE01B1FC0 | `0x4024`, `0x4034` | `pop {…,pc}` |
| `0xE0121128` | ~~send_software_interrupt~~ | **MISATTRIBUTED** — actually `Uhs2CardInit(%d)` SD driver; NOT an SGI sender | Uhs2 semaphore only; no GIC/SGI/CPU1 | slot-state `0x140AC` | `pop {…,pc}` bool |

Note on `0xE0121128`: the `stubs.S` label `send_software_interrupt` is a stale symbol-DB
mapping. The real SGI-10 (`GIC_scheduling`, CPU1 wake) sender is `0xE00D4034`, reached from
the scheduler dispatch continuation and from `0xE01B1FC0`. See `docs/09` §B.

---

## 3. The clean wrap-and-swap design for `my_create_init_task`

### 3.1 What to swap — the init-task pointer, nothing else

Because the init-task entry is single-sourced from `r1 -> MEM(0x417C)` inside
`setup_helper 0xE0166A5D`, the entire swap is: **run the real Canon
`create_init_task 0xE02E9D80` with `r1` replaced by `my_init_task`.** Canon then stores
`my_init_task` into `0x417C` itself, `canon_first_init_helper` builds the "init" task with
that entry, and `task_trampoline` does `blx my_init_task` in a fully-initialized init-task
context. `my_init_task` finishes ML setup and chains to the original Canon `init_task`
(`0xE0040236`).

This is exactly the shape of the default (non-`TRACE`) `my_create_init_task` already in
`src/boot-d678.c` (lines ~896-1068) and is identical to the M50.110 / RP / 200D path — the
platforms share this one file. No `0x417C` patch, no `0x4178` guard poke, no forged
context, no manual `0x4024` clear, no `context_restore` call, no IRQ masking. All of the
`M50II_TRACE_*` scaffolding is deleted.

### 3.2 Where the memory reservation lives and how to bump it

`r0 = dryos` points at `struct dryos_init_info` (`src/boot-d678.h`):

| Offset | Field | Role |
|---:|---|---|
| `0x00` | `sys_mem_start` | top region base (**not** the field we bump) |
| `0x04` | `sys_mem_len` | |
| `0x08` | `user_mem_start` | **bump forward to reserve ML** |
| `0x0C` | `user_mem_len`   | **shrink by the same amount** |
| `0x10` | `sys_objs_start` | (decremented only if `CONFIG_INCREASE_MAX_TASKS`) |
| `0x14` | `sys_objs_end` | |
| `0x18` | `prio_max` | |
| `0x1C` | `task_max` | (+= extra tasks if configured) |

Layout: `DryOS base │ user_mem_start │ … user_mem_len … │ sys_objs_start │ sys_mem_start`.

ML reserves its region at the **bottom of user_mem** (this is the RP/200D-proven pattern;
despite the Phase-A brief saying "bump sys_mem_start", the field actually advanced is
`user_mem_start` — `sys_mem_start` at offset 0 is left untouched):

```c
// reserve [RESTARTSTART .. _bss_end] for ML at the front of user_mem
ml_reserved_mem      = (uintptr_t)_bss_end - dryos->user_mem_start;  // must == our BSS span
dryos->user_mem_start += ml_reserved_mem;   // +0x08
dryos->user_mem_len   -= ml_reserved_mem;   // +0x0C
```

Invariants asserted before chaining (mirror the existing default path exactly):
- ordering `user_mem_start < sys_objs_start < sys_mem_start`;
- `user_mem_start % ELF_ALIGNMENT (0x10) == 0` (align up if needed);
- `RESTARTSTART == dryos->user_mem_start` — **`RESTARTSTART = 0xC78E0`** for M50 II
  (`platform/M50mk2.101/Makefile`), so ML loads exactly at Canon's user_mem base;
- `user_mem_len >= MINIMUM_USER_MEM_LEFT (0x80010)`;
- no overlap of `RESTARTSTART + ml_reserved_mem` with user_mem end / sys_objs_start.

If any invariant fails, fail closed (do not chain). Optional
`CONFIG_INCREASE_MAX_TASKS` additionally shrinks `sys_objs_start` (+0x10) and bumps
`task_max` (+0x1C).

### 3.3 How to chain `0xE02E9D80`

`copy_and_restart()` already installs the hook via the shared macro:

```c
// src/boot-d678.c, in copy_and_restart()
patch_thumb_branch(BR_CREATE_ITASK, (uint32_t)my_create_init_task);
```

`BR_CREATE_ITASK = 0xE00401B4` for M50 II (`consts.h`), the `bl create_init_task` site in
the relocated `cstart`. So `cstart` calls `my_create_init_task(dryos, init_task, c)` with
Canon's real arguments. The wrapper then:

```c
static void my_create_init_task(struct dryos_init_info *dryos,
                                uint32_t init_task, uint32_t c)
{
    // (optional D-X only) fixup_boot_memory();  -- NOT needed on D8 (EOS R hard-locks)
    // 1. validate dryos layout (fail closed otherwise)
    // 2. init_task = (uint32_t)my_init_task;          // THE SWAP (becomes MEM(0x417C))
    // 3. reserve ML memory by bumping user_mem_start / shrinking user_mem_len
    // 4. chain the real Canon orchestrator, unmodified:
    create_init_task(dryos, init_task, c);             // 0xE02E9D80, tail; never returns
    // unreachable
}
```

`create_init_task` is stubbed in `stubs.S` at `0xE02E9D80` and confirmed at the
`BR_CREATE_ITASK` call site. Because it tail-calls `scheduler_start` and never returns,
the wrapper never returns either — control lands in `my_init_task` via Canon's own
trampoline.

### 3.4 Why this removes the fragility

The hand-mirror in `08-boot-trace-report.md` reached slow-`9` only by:
bluntly clearing `0x4004`/`0x4024`, skipping `boot_pre_init_task()`, manually calling
`0xE01B21CD/0xE01B21F3/0xE00A6C95/0xE01B1BE7`, forging a context and calling
`context_restore 0xE03C51C3` directly, and masking IRQ 31/32/33/43 around `cpsie i`. Every
one of those is a symptom of doing the handoff **out of Canon's context**.

The wrap-and-swap deletes all of it:

- **Canon takes its own scheduler lock.** `sched_lock_helper 0xE01B2139` acquires `0x4024`
  itself, at the right moment, after `create_helper1` has finished GIC bring-up — no manual
  `0x4024 = 0`.
- **Canon runs its own scheduler-start in-context.** `scheduler_start 0xE01B207E` +
  `context_restore` install `current_task`, release `0x4024` for the fresh context, and
  drive the real SGI-10 CPU1 wake — no forged `task+0x54`, no direct `context_restore`.
- **Canon services its own hardware.** IRQs are enabled by the CPSR that `rfeia` restores
  inside `task_trampoline`, after Canon's GIC (`create_helper1`) and idle tasks exist — so
  `GIC_MariusIntc`/`GIC_OmarIntc` (IRQ32/33) fire only when their handlers are ready. No
  diagnostic IRQ masking.
- **The only ML deltas are data:** `r1` (init entry) and two words of the dryos struct.
  Both are consumed by Canon exactly where Canon already reads them.

`my_init_task` then does ML init and tail-chains the original Canon `init_task 0xE0040236`
(kept in `stubs.S`), i.e. the standard ML `boot_pre_init_task` / `init_task` / 
`boot_post_init_task` flow — no `M50II_TRACE_SKIP_BOOT_PRE_INIT`.

---

## 4. Remaining runtime-only unknowns (UART / JTAG session TODOs)

Static RE cannot close these; each needs a live hardware session with UART or JTAG:

1. **Lock ownership at AUTOEXEC entry.** `08` shows `0x4004` and `0x4024` non-zero before
   our relocated `cstart`/`0xE01B2139` in the hand-mirror path. The wrap-and-swap assumes
   Canon's own `create_helper1` + `sched_lock_helper` leave both in the state Canon expects
   when entered normally (not pre-cleared by us). **TODO:** confirm on HW that with the
   clean wrapper (no manual clears) Canon reaches `scheduler_start` — i.e. that the earlier
   "0x4004/0x4024 non-zero" was an artifact of the mirror path, not of AUTOEXEC entry
   itself. If they are genuinely stale at AUTOEXEC entry, find the missing Canon side effect
   rather than clearing bluntly.
2. **CPU1 WFI / SGI-10 state.** `scheduler_start`'s sibling dispatch sends SGI 10
   (`GIC_scheduling`, `0xE00D4034`) to wake CPU1 from WFI. Whether CPU1 is parked in the
   state Canon's SGI path expects when we boot from AUTOEXEC is unobservable statically.
   **TODO:** on HW, verify CPU1 actually wakes and runs its idle task (core-1 idle created
   by `final_setup_helper`) — e.g. a marker written by a CPU1-affine task, or JTAG halt of
   both cores.
3. **dryos struct field offsets past `task_max`.** `boot-d678.h` documents only offsets
   `0x00..0x1C`; `semaphore_max`/`event_max`/…/`stack_init` are commented out and
   unverified for D8 (D45 layouts differ). We only touch `0x08/0x0C` (+ optionally
   `0x10/0x1C`), so this is safe, **but** `CONFIG_INCREASE_MAX_TASKS` math depends on the
   pool layout. **TODO:** if enabling extra tasks, confirm `sys_objs`/pool sizing on HW
   (watch for the `df003570`-style size asserts noted in `boot-d678.c`).
4. **Exact `task+0x54` / context frame offsets at runtime.** The mirror relied on
   `task+0x54` (saved SP) and `ctx+0x38`/`+0x3C` (PC/CPSR). The clean path never touches
   these, but if a future debug hook does, the offsets are only inferred. **TODO:** confirm
   via JTAG against a live TCB.
5. **UART sink for real logging.** `docs/09` §A: `0xD01202F0/0xD01302F0` route UART1 to
   ICU (`0x0`) vs CCLIME (`0x2`); TX/RX IRQs UART1 = `0x17D`/`0x18D`; `uart_printf`
   `0xE055A8B4`. **TODO:** on HW, confirm which physical UART maps to the debug connector
   and the `0x0 = ICU` routing, then wire a UART putchar so the next session has text
   instead of LED counts.

---

## 5. Next-build recommendation (validate on hardware with LED markers)

Ship a **minimal** wrap-and-swap build — no `M50II_TRACE_*` mirror code, no IRQ masking,
no manual lock clears — and bisect with a few coarse LED markers only:

1. Build the default `my_create_init_task` path (the `#else` branch), i.e. define **none**
   of the `M50II_TRACE_CREATE_*` / `M50II_TRACE_INIT_HELPER_*` / `SKIP_BOOT_PRE_INIT` /
   `REENABLE_IRQ*` flags. Keep `patch_thumb_branch(BR_CREATE_ITASK, my_create_init_task)`.
2. Instrument only 4 LED markers (reuse `trace_blink_once`, gated so they are cheap):
   - **M1** — first line of `my_create_init_task` (wrapper entered from cstart).
   - **M2** — after the dryos layout checks + memory bump succeed, immediately before
     `create_init_task(dryos, my_init_task, c)`.
   - **M3** — first line of `my_init_task` (proves Canon drove the whole handoff and
     `blx my_init_task` fired in the real init context).
   - **M4** — after `my_init_task` returns from / chains the original `init_task 0xE0040236`
     (proves boot_pre/post_init survived without IRQ masking).
   Expected healthy run: `M1 M2 … M3 … M4`.
3. Interpretation:
   - stops after **M2** (no **M3**) → Canon handoff itself failed with our two data deltas;
     re-check `RESTARTSTART == user_mem_start` and the swap of `r1`; compare against the
     known-good `16dd1f8f…` memory numbers.
   - reaches **M3** but not **M4** → handoff is clean; failure is now inside ML/Canon
     `init_task`, i.e. a normal ML-port problem, not a scheduler/lock problem.
   - reaches **M4** → the wrap-and-swap is validated; retire the hand-mirror path and move
     to the UART sink (TODO §4.5) before FIO/display work.
4. Because `08` showed this image is layout/checksum sensitive, change **only** the boot
   path for this test; keep `ml_init` and later code byte-identical to a known-good build so
   any regression is attributable to the wrapper, not to unrelated content shifts.

Success criterion for Phase A: a rebuildable image reaches **M4** with zero `M50II_TRACE_*`
diagnostics compiled in — i.e. Canon takes its own lock `0xE01B2139` and runs its own
`scheduler_start 0xE01B207E` in-context, and ML rides in purely via the `0x417C` init-task
pointer and the user_mem reservation.
