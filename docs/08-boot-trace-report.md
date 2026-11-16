# 08 - M50 Mark II boot trace report

Date: 2026-07-07

Target: Canon EOS M50 Mark II, firmware 1.0.1, Digic 8.

Goal of this phase: get from "camera accepts our bootable SD card" to a controlled
Magic Lantern `AUTOEXEC.BIN` handoff, with enough trace signal to know where the
boot path stops.

## Executive summary

We have proven all of this on the physical camera:

- The boot flag is enabled and the camera executes `AUTOEXEC.BIN` from the SD card.
- The normal/nonboot card still boots Canon firmware, so the camera is recoverable.
- Our relocated cstart path runs.
- Our replacement `create_init_task` runs.
- Canon's early create-task setup helpers return when two observed state words are
  cleared diagnostically.
- We reach Canon's scheduler/context handoff.
- Canon context restore can execute our patched Thumb code.
- The saved initial-task slot at `0x417C` can be changed to ML `my_init_task`.
- The first task created by Canon setup is still Canon's init helper
  `0xE0166A77`, not `my_init_task`.
- The second `"init"` task can be created manually and its saved context can be
  restored.
- Patching that second task's saved PC directly to our wrapper reaches
  `my_init_task`; the normal Canon task-trampoline path did not.
- Skipping `boot_pre_init_task()`, manually clearing scheduler lock state, masking
  IRQ31/32/33/43 during `cpsie i`, and then re-enabling IRQ32/33/43 one at a
  time lets Canon `init_task()` return.
- The latest physical result reached slow marker `9`, which is after
  `boot_post_init_task()` returned.

The current blocker is no longer "does autoexec run?" and is no longer "can we
enter Canon init?" The current frontier is whether post-init DryOS services are
usable from this direct-entry path, starting with direct FIO writes to the SD card.

Current proven working path:

```text
AUTOEXEC/cstart
  -> Canon create_init_task setup
  -> scheduler restores first init task
  -> first init task entry is Canon helper 0xE0166A77
  -> our mirror creates a second task named "init"
  -> second task entry is MEM(0x417C), now ML my_init_task
  -> saved PC is patched directly to our wrapper
  -> wrapper clears scheduler lock state, masks selected IRQs, enables IRQs
  -> my_init_task runs with boot_pre_init_task skipped
  -> IRQ32, IRQ33 and IRQ43 are re-enabled one by one
  -> Canon init_task returns
  -> boot_post_init_task returns
```

The remaining risk is that this is still a diagnostic boot path, not the final
clean port. It bypasses the normal Canon helper/trampoline behavior and uses
selected IRQ masking to survive the handoff.

Realistic progress estimate as of the current state:

| Milestone | Status |
|---|---:|
| SD boot / `AUTOEXEC.BIN` execution | 100% proven |
| controlled early LED trace | 100% proven |
| reaching Canon `init_task()` from AUTOEXEC | ~80% proven |
| returning from Canon `init_task()` and `boot_post_init_task()` | proven in exact `16dd1f8f...` image |
| reliable rebuild/patch stability | ~40%; current blocker |
| SD card file write from this path | 0% proven, probably partially enabled |
| display/framebuffer write from this path | 0% proven |
| stable ML-style port | ~20-25% overall |

Important regression note: both rebuilt FIO-probe attempts caused a
no-blink/steady-LED boot before any numbered marker. The first put the probe
directly in `my_init_task`; the second moved it to `init.c` at the end of
`boot_post_init_task()`. This points to build/layout sensitivity or another
early-path perturbation, not a clean FIO result.

In-place patching also appears unsafe in at least one region: replacing only the
existing `ml_init` task body with either an FIO probe or a marker-only slow `2`
loop made the camera stop at slow `10`, before the patched task's unique marker.
That means either `ml_init` code/data is involved earlier than expected, or this
boot path is sensitive to even small content/checksum changes.

New Digic 8 interrupt/register table information is now useful for interpreting
the IRQ experiment. The exact slow-`9` image survived by masking vector
0x01F/0x020/0x021/0x02B before `cpsie i`, then re-enabling 0x020/0x021/0x02B
before Canon `init_task()`. These are not anonymous bits anymore:

| Vector | Name from Digic 8 table | Current observation |
|---:|---|---|
| `0x01F` | `Eeko_TIMER_IC1_INT*ASYMMETRIC` | masked and not re-enabled in the known-good path |
| `0x020` | `EDOMAIN_SYNGEN_2` | masked during wrapper entry, then re-enabled before Canon init |
| `0x021` | `EDOMAIN_OPERA_OPEKICK2` | masked during wrapper entry, then re-enabled before Canon init |
| `0x02B` | `TCU_T2OUT *IRQEXT` | masked during wrapper entry, then re-enabled before Canon init |

Working hypothesis: the handoff was being interrupted by timer/sync/display or
image-engine events before Canon's real context was ready for them. Masking
those vectors is a diagnostic workaround, not a final port design.

## Current staged test

Latest file staged on the card:

```text
AUTOEXEC.known-good-control-version-byte-patch.20260707-065045.BIN
SHA256: 6c9bc38e9dcfdbdf9279bb6dc6d7ce3b2a9e74d69cf7bd2954f6ab174fca3fff
```

This is a control test based on the exact prior image that reached slow `9`.
It changes only:

- one byte in the human-readable version text near the start of `AUTOEXEC.BIN`
  (`file offset 0x42`, `0x4d -> 0x6d`);
- the final checksum word (`file offset 0x3a0de` within the checksum word).

Original known-good SHA256 was:

```text
16dd1f8f2b0d1cde3dd49dfa227e8bd85f5ca39cf916c4fb3cd4575cca267149
```

Build flags for this known-good image:

```sh
make clean
make CFLAGS_PLATFORM="-mthumb -mlong-calls \
  -DM50II_TRACE_KEEP_SOURCE_ORDER \
  -DM50II_TRACE_QUIET_UNTIL_HELPER_TAIL \
  -DM50II_TRACE_INIT_HELPER_DIRECT_RESTORE \
  -DM50II_TRACE_INIT_HELPER_SKIP_UNLOCK \
  -DM50II_TRACE_INIT_HELPER_SKIP_LOCKS \
  -DM50II_TRACE_INIT_HELPER_SKIP_READY \
  -DM50II_TRACE_INIT_HELPER_RESET_STATE \
  -DM50II_TRACE_INIT_HELPER_COMPACT_TAIL \
  -DM50II_TRACE_INIT_HELPER_DIRECT_ENTRY \
  -DM50II_TRACE_INIT_HELPER_DIRECT_ENTRY_MANUAL_UNLOCK \
  -DM50II_TRACE_INIT_HELPER_DIRECT_ENTRY_MASK_IRQS \
  -DM50II_TRACE_INIT_LADDER \
  -DM50II_TRACE_SKIP_BOOT_PRE_INIT \
  -DM50II_TRACE_REENABLE_IRQ32_33_43_BEFORE_INIT"
```

Expected important tail:

```text
same as exact known-good: should end at slow 9
```

Marker decode for the current staged test:

| Marker | Meaning |
|---:|---|
| slow `7` | about to call Canon `init_task()` |
| slow `8` | Canon `init_task()` returned |
| slow `9` | `boot_post_init_task()` returned; control patch did not perturb boot |
| slow `10` then dark | even a harmless text/checksum change perturbs boot; content/checksum/timing sensitivity is broader than `ml_init` |

## Current source locations

Main diagnostic file:

```text
magiclantern_simplified/src/boot-d678.c
```

Other touched files for trace hooks:

```text
magiclantern_simplified/src/boot.h
magiclantern_simplified/src/init.c
magiclantern_simplified/platform/M50mk2.101/consts.h
```

Current relocation size:

```c
#define FIRMWARE_ENTRY_LEN 0x800
```

## Confirmed test history

These are the important staged builds and physical-camera observations.

| Build | SHA256 prefix | Camera result | Meaning |
|---|---:|---|---|
| `AUTOEXEC.create-passthru-canon-init.20260706-220101.BIN` | `56805bf7` | `2` blinks, then nothing | early path entered, then died later |
| `AUTOEXEC.create-passthru-canon-init-len800.20260706-221305.BIN` | `54892246` | `2` blinks, then nothing | larger relocation alone did not solve it |
| `AUTOEXEC.cstart-stop-at-bzero-len800.20260706-221711.BIN` | `8e619faa` | initial marker, then repeating `4` | cstart reaches the `bzero32` hook |
| `AUTOEXEC.cstart-stop-at-create-itask-len800.20260706-222121.BIN` | `c3a1af11` | initial marker, then repeating `5` | cstart reaches the `create_init_task` call site |
| `AUTOEXEC.create-internals-len800.20260706-223244.BIN` | `e7d627cb` | `2 1 2`, then black | `0xE00A674B` returns, then `0xE01B2139` blocks or dies |
| `AUTOEXEC.create-clear-task-lock-len800.20260706-223902.BIN` | `7623c95b` | `2 1 2 8 3 4 5 6 7` | clearing `0x4024` lets create setup reach scheduler handoff |
| `AUTOEXEC.sched-restore-probe-late-len800.20260706-234216.BIN` | `838f4506` | `9 4 5 6 7 9 9 9` | context restore can execute our patched probe |
| `AUTOEXEC.sched-bootstrap-wrapper-late-len800.20260706-234641.BIN` | `56ce2b7f` | `5 6 7 9 dark` | wrapper after restore ran; Canon unlock returned in that path; saved task entry was still Canon-side |
| `AUTOEXEC.sched-ml-init-ladder-len800.20260706-235103.BIN` | `81d41f` | `3 10 4 5 6 7 dark` | ML init slot substitution and memory reservation ran, but normal scheduling did not reach ML init ladder |
| `AUTOEXEC.sched-context-classifier-mlinit-len800.20260706-235431.BIN` | `2d49296e` | `3 10 4 5 6 7 8 9 10 11 3 3 3` | task/context valid; `0x417C` is ML init; saved PC is Canon trampoline; first task entry is Canon helper `0xE0166A77` |
| `AUTOEXEC.init-helper-direct-restore-len800.20260707-000259.BIN` | `220aa4e1` | `8 3 10 4 5 6 7` | reached handoff with direct-helper build, but wrapper-entry position was ambiguous |
| `AUTOEXEC.init-helper-entry-split-len800.20260707-000736.BIN` | `166dfae5` | `6 7 8 1` | direct PC patch enters our wrapper; it hangs before marker `2`, inside manual `0xE01B2237()` |
| `AUTOEXEC.init-helper-via-trampoline-len800.20260707-001059.BIN` | `95d1614c` | `6 7 8 dark` | patching task entry and leaving saved PC on Canon trampoline did not enter our wrapper |
| `AUTOEXEC.init-helper-context-pc-probe-len800.20260707-004831.BIN` | `c416c9ec` | `9 13 13 13` | second context restore reaches the patched saved PC |
| `AUTOEXEC.init-helper-direct-entry-len800.20260707-005204.BIN` | `85e9cf39` | `13` then slow `1 2 3` | direct-entry wrapper reaches `my_init_task`, then dies around `boot_pre_init_task()` |
| `AUTOEXEC.init-helper-direct-entry-skip-preinit-len800.20260707-010015.BIN` | `8afdb0d` | `13` slow `1 2 3 14 4 5 6 7 dark` | skipping pre-init reaches Canon `init_task()`, then dies before return |
| `AUTOEXEC.init-helper-direct-entry-mask-irqs-skip-preinit-len800.20260707-011228.BIN` | `716bc5c7` | slow `1..7` | manual unlock with selected IRQs masked survives into `my_init_task` and reaches Canon `init_task()` |
| `AUTOEXEC.init-helper-direct-entry-reenable-irq32-33-43-skip-preinit-len800.20260707-011838.BIN` | `16dd1f8f` | ended with slow `9` | Canon `init_task()` returned and `boot_post_init_task()` returned |
| `AUTOEXEC.post-init-direct-fio-probe-len800.20260707-012904.BIN` | `0f11bd86` | steady LED, no blink | adding FIO code into `my_init_task` perturbed the fragile handoff before trace |
| `AUTOEXEC.boot-post-direct-fio-probe-len800.20260707-013440.BIN` | `e887d2a2` | steady LED, no blink | moving FIO code to `init.c` still perturbed boot before trace |
| `AUTOEXEC.exact-known-good-slow9-restore.20260707-013807.BIN` | `16dd1f8f` | blinking same way | exact restore confirmed camera/card path still healthy |
| `AUTOEXEC.known-good-ml-init-fio-patch.20260707-014614.BIN` | `1d6c5767` | `5 6 10 dark` | ambiguous: marker `10` can be the old IRQ re-enable step or patched `ml_init` starting before `_FIO_CreateFile`; need unique marker-only `ml_init` test |
| `AUTOEXEC.known-good-ml-init-marker-slow2-patch.20260707-020248.BIN` | `2c6be5d6` | ended with slow `10` | patched `ml_init` did not reach its repeated slow `2`; patch perturbs earlier path |
| `AUTOEXEC.known-good-control-version-byte-patch.20260707-065045.BIN` | `6c9bc38e` | pending | control: changes one non-code version byte plus checksum only |

## What each key discovery means

### Boot and cstart are proven

The bootflag/card path is real because a bootable card changes camera behavior
and a normal card still boots. LED markers then proved the relocated cstart path
reaches `bzero32` and the `create_init_task` call site.

### Two state words matter early

Observed diagnostic state:

| Address | Observation |
|---:|---|
| `0x4004` | nonzero before relocated cstart; clearing it lets cstart continue |
| `0x4024` | nonzero before `0xE01B2139`; clearing it lets setup continue |

These clears are diagnostic, not final fixes. They prove what Canon code is
blocked on, but the proper final port may need to reproduce the missing ordering
or side effect instead of bluntly clearing state.

### Context restore itself is not the blocker

The restore-probe build reached a repeating `9` after the scheduler handoff.
That proves:

- the task pointer is plausible;
- `task + 0x54` is the saved-context pointer for this path;
- `0xE03C51C3(task + 0x54)` can restore a context;
- a patched saved PC can execute our Thumb code.

### The first task is Canon's init helper

The context-classifier build showed:

- saved init slot `MEM(0x417C)` equals ML `my_init_task`;
- saved CPSR is expected;
- saved PC is Canon task trampoline `0xE00A6C75`;
- the first task entry is not ML and not the ordinary Canon init task;
- the first task entry is Canon helper `0xE0166A77`.

This changed the plan. Instead of expecting the first restored task to be ML
`my_init_task`, we now expect the first restored task to be a Canon helper that
creates the second real init task using `MEM(0x417C)`.

### Manual unlock is unsafe in the direct wrapper

The split direct-helper build showed:

```text
6 7 8 1
```

Meaning:

- scheduler mirror reached marker `8`;
- context restore entered our wrapper (`1`);
- the wrapper did not reach marker `2`;
- the only code between `1` and `2` was manual call `0xE01B2237()`.

Therefore direct-calling `0xE01B2237()` from that restored wrapper context is
unsafe. The currently staged build skips it.

### Patching task entry did not work

The via-trampoline build patched `task + 0x0C` to our wrapper and left the saved
PC as Canon task trampoline. It produced:

```text
6 7 8 dark
```

So in this context, "let Canon trampoline do the unlock and then call our entry"
did not transfer to our wrapper. The direct saved-PC patch remains the only
currently proven way to run wrapper code after context restore.

## Important addresses in the current path

| Address | Meaning in current tests |
|---:|---|
| `0x4004` | early cstart lock/state word; nonzero before cstart in our boot path |
| `0x4024` | scheduler lock/state word; nonzero before `0xE01B2139` |
| `0x1008` | current CPU/task slot used by scheduler handoff mirror |
| `0x1028` | current task pointer written by scheduler handoff |
| `0x2C054` | task table touched by scheduler handoff |
| `0x4178` | Canon init-helper state flag |
| `0x417C` | saved init-task entry; currently patched to ML `my_init_task` |
| `0x4138` | descriptor field used by Canon helper when creating second `"init"` task |
| `0x4140` | descriptor field used by Canon helper when creating second `"init"` task |
| `0xE00A674B` | first helper inside Canon create-init path |
| `0xE00A6C75` | Canon task trampoline saved in new task contexts |
| `0xE00A6C95` | task create call used by the mirrored Canon helper |
| `0xE01B19B7` | helper used by handoff to get/create current task |
| `0xE01B1BE7` | task-ready call used by the mirrored Canon helper |
| `0xE01B2139` | scheduler/lock helper that previously blocked until `0x4024` was cleared |
| `0xE01B21CD` | helper critical-section entry/save-token call |
| `0xE01B21F3` | helper critical-section restore call |
| `0xE01B2237` | task-trampoline unlock call; unsafe when direct-called from our wrapper |
| `0xE0166A5D` | Canon create-task setup helper taking `(dryos, init_task, c)` |
| `0xE01669D5` | Canon setup helper after create-task setup |
| `0xE0166A77` | Canon first-init helper task entry |
| `0xE0166AD5` | final known setup helper before scheduler handoff |
| `0xE03C51C3` | Canon context restore entry used by the mirror |

## Digic 8 interrupt/register findings

Source context from the local Discord scrape:

- `ml_discord/Platform_Specific_Development__m50.md` documents an M50 Canon Basic
  script that dumps the Digic 8 interrupt vector names, with
  `printInterruptVectorTable: 0xe0156444`;
- the same thread says the R / RP / M50 tables are equal and likely shared across
  Digic 8 models;
- `docs/07-discord-findings.md` already records the D8 UART and DebugMsg clues
  gathered from RP, R, M50 and Digic X discussions.

Relevant IRQ names for this port:

| Vector | Description |
|---:|---|
| `0x01F` | `Eeko_TIMER_IC1_INT*ASYMMETRIC` |
| `0x020` | `EDOMAIN_SYNGEN_2` |
| `0x021` | `EDOMAIN_OPERA_OPEKICK2` |
| `0x02B` | `TCU_T2OUT *IRQEXT` |
| `0x0BE` | `SDDomain_ADMAC0` |
| `0x0CE` | `SDDomain_ADMAC1` |
| `0x0DE` | `SDDomain_ADMAC2` |
| `0x0EE` | `SDDomain_SDCON0` |
| `0x0FE` | `SDDomain_SDCON1` |
| `0x10E` | `SDDomain_SDCON2` |
| `0x15D` | `UART0 RX_IntReqRx` |
| `0x16D` | `UART0 TX_IntReqTx` |
| `0x17D` | `UART1 RX_IntReqRx` |
| `0x18D` | `UART1 TX_IntReqTx` |
| `0x19D` | `UART2 RX_IntReqRx` |
| `0x1AD` | `UART2 TX_IntReqTx` |

Relevant GIC-level names:

| Vector | Description |
|---:|---|
| `0x20A` | `GIC_scheduling` |
| `0x20B` | `GIC_timer` |
| `0x21B` | `GIC_GlobalTimer` |
| `0x21D` | `GIC_PrivateTimer` |
| `0x21F` | `GIC_LegacyIrq` |
| `0x220` | `GIC_MariusIntc` |
| `0x221` | `GIC_OmarIntc` |

This matters for QEMU and hardware debugging: the low 0x000-0x1FF vectors appear
to be the Digic interrupt-controller side, while the GIC sees aggregated lines
such as Marius/Omar plus SGIs/PPIs/SPIs.

UART notes now worth testing before more FIO work:

| Address | Meaning |
|---:|---|
| `0xC0800000` | D8 UART TX register from Discord findings |
| `0xC0800014` | D8 UART busy/status register; bit `0x2` appears relevant |
| `0xC0810000` | second UART base from RP/D8 notes |
| `0xD01202F0` | `UART1_RX_SELECT_CONFIG` |
| `0xD01202F4` | `UART1_TX_SELECT_CONFIG` |
| `0xD01302F0` | `UART1_RX_SELECT_MASK` |
| `0xD01302F4` | `UART1_TX_SELECT_MASK` |

The table says `0x0` selects ICU/DryOS on the ARM core, while `0x2` selects the
CCLIME/network core. This makes a tiny raw UART putchar a better next logging
experiment than FIO if a UART adapter is physically available.

Display notes now worth testing only after late patching is stable:

| Address | Meaning |
|---:|---|
| `0xD0304230` | VRAM pointer |
| `0xD0304234` | VRAM pitch |
| `0xD0304238` | resolution |
| `0xD0304220` | LCD/EVF highlight/zebra enable |
| `0xD0304488` | LCD/EVF overexposure threshold/style |
| `0xD030448C` | LCD/EVF overexposure color |
| `0xD0304490` | LCD/EVF underexposure threshold/style |
| `0xD0304494` | LCD/EVF underexposure color |

Direct register writes may be overwritten by Canon state objects. If that
happens, the next step is not blind repeated writes; it is finding the memory
state object that Canon later copies into these registers.

## Event-procedure and Canon Basic findings

We first tried to avoid full `AUTOEXEC.BIN` boot by using Canon Basic and
`ExecuteEventProcedure`.

What we learned:

- `ExecuteEventProcedure` itself works well enough to call registered DryOS event
  procedures.
- `EnableBootDisk` and `DisableBootDisk` values near `0xE07067C8` and
  `0xE07067D8` are string addresses, not function addresses.
- Event procedures are called by name. For example, the practical call is
  conceptually `ExecuteEventProcedure("EnableBootDisk")`.
- Enabling the boot flag worked: bootable card behavior changed, normal card
  behavior remained normal.

What did not work reliably:

- `Fopen_Fut` and related file-I/O paths were unstable or crashed with camera
  error messages.
- `WriteFileString` did not become a dependable early logging path.
- debug display/event procedures such as debug color or brightness changes did
  not produce a visible on-screen proof.
- framebuffer attempts without a stable ML/DryOS context did not produce visible
  output.

Conclusion: Canon Basic was useful for enabling boot and probing event-procedure
availability, but not reliable enough for early "hello world" or SD-card logging
on this body.

## Why LED bisection is still necessary

This is runtime bisection under severe observability limits, not random guessing.

The ROM dump, Ghidra, and M50 mk I donor code tell us:

- where code and strings live;
- what many functions appear to do;
- what the original M50/R/RP/200D families did;
- where likely hook points are;
- where Canon branches and literal pools are.

They do not directly tell us:

- which lock words are already held when booting from `AUTOEXEC.BIN`;
- whether CPU1 has completed the expected early path;
- whether interrupt/GIC state matches Canon's expected state;
- whether a context structure has exactly the expected runtime contents;
- whether a static-looking function is safe to call from this exact context;
- whether a copied/relocated ROM region is missing a side effect.

Until UART/card logging works before or immediately after the scheduler boundary,
the LED is the only reliable output.

The confirmed boundary moved like this:

```text
unknown AUTOEXEC behavior
  -> cstart reached
  -> bzero32 reached
  -> create_init_task reached
  -> first create helper returned
  -> scheduler lock helper returned after clearing 0x4024
  -> create-task setup returned
  -> scheduler/context handoff reached
  -> context restore can execute patched code
  -> saved init slot is ML my_init_task
  -> first task is Canon helper 0xE0166A77
  -> direct helper wrapper enters
  -> manual task-trampoline unlock is unsafe there
```

That is real progress.

## QEMU status

QEMU should still be improved, but it has not replaced hardware testing for this
blocker.

Reasons:

- Existing qemu-eos support is stronger for older Digic generations.
- Digic 8 startup needs correct GIC/SGI/CPU1 behavior.
- The failure we are chasing is exactly in the scheduler/context/interrupt
  handoff area.
- Discord findings repeatedly warn that D7/D8 startup can hang in QEMU when
  GIC/CPU1 pieces are incomplete.

Practical conclusion:

- Use QEMU for static validation, loader/layout checks, and future regression
  tests.
- Do not treat it as authoritative for this hardware handoff until D8 GIC/CPU1
  behavior is modeled well enough to reproduce the same boundary.

## SD-card logging status

Writing debug logs to the SD card is still the right goal after the OS services
are alive enough. It is not reliable at the current boundary.

Reasons:

- Canon Basic file APIs were unstable on this camera/path.
- Early `AUTOEXEC.BIN` runs before normal DryOS task and filesystem state is
  available.
- Calling high-level FIO/event procedures too early risks hiding the scheduler
  bug behind a file-I/O crash.
- We reached `boot_post_init_task()` once in the exact `16dd1f8f...` image, but
  patched/rebuilt variants have not yet shown that same late point reliably.

The immediate logging problem is therefore two-step:

- first prove that a minimally changed image can still reach slow marker `9`;
- only then add a late marker-only hook, followed by a one-call-at-a-time FIO
  probe.

Likely future logging options once late patching is stable:

- `DryosDebugMsg` plus `dumpf`;
- `dm_set_store_level` / `dm_set_print_level`;
- redirect Canon UART output into a RAM buffer and flush later;
- hook `uart_printf`/DebugMsg with an MMU patch;
- raw UART MMIO if physical or emulated UART becomes available.

## Next decisions after the current staged test

The current staged file is a control patch of the exact slow-`9` known-good
image. It changes one non-code version-text byte and the checksum word only.

### If the camera reaches slow `9`

That means harmless content/checksum changes are not the broad problem. The
failed `ml_init` body patch should then be treated as a bad patch target, not as
proof that the whole binary is unpatchable.

Next step in that case:

- leave the early boot and `ml_init` body untouched;
- patch only the known late tail after slow `9`, or an already executed late
  call site, with a marker-only loop first;
- after that survives, add FIO one call at a time:
  create file, then write file, then close, then flush.

### If the camera stops at slow `10` or earlier

That means even a non-code byte plus checksum can perturb this fragile path, or
the loader/checksum placement has another side effect we have not isolated. In
that case, do not add FIO/display code yet.

Next step in that case:

- restore the exact `16dd1f8f...` known-good file to confirm the camera still
  reaches slow `9`;
- compare the loaded/checked regions of the control file against known-good;
- find a patch location that is demonstrably ignored until after slow `9`;
- avoid rebuilding until we can reproduce the exact known-good layout.

### If the camera shows steady LED with no markers

That matches the previous rebuilt FIO-probe failures. Treat it as an early
layout/loader regression, not as a result from the FIO or display code itself.

## Current state in one sentence

We have proven `AUTOEXEC.BIN` execution through Canon init and post-init once,
but the active blocker is making a minimally changed image still reach that
same slow-`9` point before attempting SD-card writes or display/framebuffer
output.
