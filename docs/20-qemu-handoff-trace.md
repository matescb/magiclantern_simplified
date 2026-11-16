# 20 — QEMU dynamic trace of the create_init_task → init handoff (WS-QEMU Phase 2)

**Date:** 2026-07-07 · **Model:** `qemu-eos -M M50mk2,firmware=boot=0` (plain Canon 1.0.1)
**Tool:** `gdb-multiarch` on the qemu gdbstub · **ROM:** gang100.bin

## Purpose

Resolve, from the live emulator, the exact mechanism matescb bisected blind on the
LED (docs/08): why patching the "initial-task slot" to `my_init_task` does **not**
make it the first task to run — Canon's own init-helper runs first and then spins up
the real `init` task "from that slot." Static RE (docs/08/13) told the story; this
phase **confirms it dynamically** and pins down causation.

## Method (reproducible)

qemu and gdb must run in **one foreground shell command** (qemu as a `&` child, not a
managed background task — a `-S`-frozen background qemu with no client attached is
reaped). ROM addresses need **hardware** breakpoints (`hbreak`/`watch`); software
`break` silently no-ops in read-only ROM. Do **not** `pkill -f 'M50mk2…'` — that
pattern also matches the launching shell's own argv and kills it; clean up by `$!` PID.

```
qemu-system-arm -M M50mk2,firmware=boot=0 -S -gdb tcp::1234 … &
QPID=$!
gdb-multiarch -batch -ex 'target remote :1234' -ex 'hbreak *0xADDR' …
kill $QPID
```

## Findings (raw gdb output)

**Breakpoint at the `create_init_task` call site `0xE00401B4`:**

```
r0(init_info)=0x00001f74   r1(task_fn)=0xe0040237   r2=0
MEM[0x417C]=0x00000000            <- init-entry global not yet set
```

`r1 = 0xE0040237` matches the static decode exactly (init_task, Thumb bit set).

**Hardware watchpoint on `0x417C` — who writes it:**

```
Old value = 0
New value = 0xE0040237           (== create_init_task's r1 argument)
writer PC = 0xE0166A6C           (inside the init-helper region ~0xE0166A77)
```

**Breakpoint at the init-helper/trampoline `0xE0166A77` (first dispatched task):**

```
r2=0x001da860 (task struct, sys_objs region)   lr=0xE00A6C87 (scheduler dispatch)
MEM[0x417C]=0xE0040237           <- now populated, read by the trampoline
```

## Mechanism — now dynamically confirmed

```
cstart
  └─ create_init_task(init_info, task_fn = 0xE0040237, 0)   @0xE00401B4
        │   … internally, PC 0xE0166A6C stores task_fn into the global 0x417C …
        ▼
     scheduler dispatches the FIRST task, whose body is the init-helper/trampoline
     0xE0166A77 (entered via 0xE00A6C87)
        │   trampoline reads 0x417C (= 0xE0040237) …
        ▼
     … and creates/dispatches the real task named "init" with entry = MEM[0x417C]
        ▼
     init_task → STARTUP → "[STARTUP] K468 ICU Firmware Version 1.0.1"
```

**The load-bearing fact:** the value written to `0x417C` **equals `create_init_task`'s
`r1`**. So `0x417C` is not a fixed Canon constant — it is literally the task-fn pointer
you hand to `create_init_task`. Whoever controls `r1` controls what the trampoline
dispatches as `init`.

## Consequences for the ML port

1. **The wrap-and-swap is the correct, sufficient hook.** Swapping `r1` to
   `my_init_task` before `create_init_task` runs → `0x417C` receives `my_init_task`
   → the trampoline dispatches it as the `init` task, through Canon's *normal* path.
   No saved-PC patching, no IRQ masking required.
2. **Explains matescb's LED result.** His observation that "the normal Canon
   task-trampoline path did not reach `my_init_task`, only a direct saved-PC patch did"
   is a **patch-timing** artifact: he was overwriting `0x417C` (or the task's saved
   context) relative to when the trampoline latched it, not aiming at the wrong target.
   His IRQ-masking direct-entry (docs/08) is a workaround for that timing race, not a
   necessity of the design.
3. **`my_init_task` legitimately runs "one level down"** — as the body of the `init`
   task the trampoline spawns, not as the very first dispatched task. That is the
   correct DryOS shape, not a bug.

## Status & next

- Handoff mechanism: **confirmed in emulation.** create_init_task→0x417C→trampoline
  →init proven, causation nailed by watchpoint.
- Not yet done in qemu: boot **our** AUTOEXEC (`firmware=boot=1`) and watch the
  wrap-and-swap put `my_init_task` into `0x417C` and dispatch cleanly — the direct
  emulator validation of the port's hook (and of the `BR_BR_CSTART=0xE00400C0` fix).
  That is the next QEMU step; the Canon `boot=0` run here is the reference it must match.
- Cross-check the `0xE0166A6C` store and the trampoline's task-create call against
  Ghidra/romdis (docs/08 already RE'd this helper on hardware).
