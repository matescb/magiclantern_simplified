# docs/17 — DryOS Kernel: Tasks (F2) + Sync/IPC (F3)

**Verified 2026-07-07** — F2/F3 workflow agents; addresses independently spot-checked.
New stubs added to stubs.S; struct layouts below are for Ghidra/reference.

## Tasks (F2)
Verified: `task_create` 0xE05F40BA, `task_trampoline` 0xE00A6C74, `get_current_task` 0xE00A6BF4,
`get_task_by_id` 0xE00A6BBA (id==-1 → current), `current_task` 0x1028, `task_table` 0x40E8.

**CONTESTED — deferred (kept at current values):** the F2 agent proposed swapping
`task_max` 0x40CC→0x4108 and `task_dispatch_hook` 0x4108→0x4144, but its own adversarial
verifier **refuted** task_max=0x4108 (0x40CC is used as `ldrsh [r7,#8]` in the task-mgmt fn
@0xE03EF3A0+), and the **R.180/RP donors confirm `NSTUB(0x40CC, task_max)`**. So both stay:
`task_max=0x40CC`, `task_dispatch_hook=0x4108`. Re-verify on HW before any change.

Other decoded kernel offsets (documentation, not applied): prio_max 0x4104, prio_default 0x412C,
stack_default 0x4130, task_uid_seq 0x4180, task_count_live 0x4184, task_count_peak 0x4188.

## Sync/IPC (F3)
All 10 existing sem/lock/queue stubs **re-confirmed at current addresses** (validates prior work).
Added: `create_counting_semaphore` 0xE05D0CFA, `msg_queue_post_blocking` 0xE05B556C, and event
flags `CreateEventFlag` 0xE05B0B68 / `DeleteEventFlag` 0xE05B0BA2 / `SetEventFlag` 0xE05B0EBC /
`ClearEventFlag` 0xE05B0EEA / `WaitForAnyEventFlag` 0xE05B0CC0 / `WaitForAllEventFlag` 0xE05B0D40.

DryOS class-table pointers (RAM): sem 0x41BC (stride 0x18), evf 0x41C8 (0x18), mq 0x41D4 (0x2C),
rlock 0x41E0 (0x1C). Public handles = internal_id << 1 (DRYOS_HANDLE_SHIFT=1).

---
## F2 agent detail
# 13 - DryOS Scheduler & Task Model (M50 Mark II, fw 1.0.1)

## TL;DR - two inherited stubs are wrong by +0x3C

The whole DryOS scheduler-globals block near `0x41xx` on the M50 II is shifted **+0x3C** relative to the R.180 donor. The two task stubs blind-copied from R.180 are therefore both off:

| Symbol | Current (copied from R.180) | Correct (M50 II) | Proof |
|---|---|---|---|
| `task_max` | `NSTUB(0x40CC)` | **`NSTUB(0x4108)`** | `get_task_by_id` bounds valid ids by `*0x4108`; adjacent to `prio_max@0x4104` in kernel-init pool |
| `task_dispatch_hook` | `DATA_PTR(0x4108)` | **`DATA_PTR(0x4144)`** | dispatcher & scheduler_start `blx *0x4144` with (reason, prev, next); setter at 0xE01669B0 |

R.180 `0x40CC + 0x3C = 0x4108` and R.180 `0x4108 + 0x3C = 0x4144` - the shift is self-consistent and explains why the copies were plausible-but-wrong. `current_task@0x1028` and `current_interrupt@0x1010` (the `0x1000` block) are **not** shifted and remain correct.

## Task creation call chain

```
task_create   (0xE05F40BA)  \
                             > build 0x1C-byte descriptor -> inner -> build_task(0xE00A6C94)
task_create_ex(0xE05F42D2)  /
  inner (task_create)    = 0xE021B938   (applies prio/stack defaults, validates prio<*0x4104)
  inner (task_create_ex) = 0xE004868C   (same + cpu/flags from attr struct)
  build_task             = 0xE00A6C94   (allocates slot, fills task struct, builds initial context)
```

Note: kitor's referenced setup fn `0xe05f5fb4` / `0xe00e96ac` are for a **different model**; on M50 II the real setup path is `0xE021B938 -> build_task 0xE00A6C94`.

### Parameter descriptor (0x1C bytes, on inner-creator stack)
`[0x00]=id(-1=alloc) [0x04]=entry [0x08]=arg [0x0c]=flags/preallocated-stack [0x10]=stackSize [0x14]=name [0x18]=cpu(inherited from current->0x50) [0x19]=flags [0x1a]=priority [0x1b]=ex-flag`

### build_task (0xE00A6C94) slot math
`slot = (*0x40E8) + id*0x5C`, then `-0x5C` => **task table base `*0x40E8`, stride `0x5C`, index `id-1`**. Free slots are marked by `state byte @0x49 == 0xFF`.

## Task struct (0x5C, matches ML CONFIG_TASK_STRUCT_V2_SMP)

Ghidra-ready field list (offset / name / evidence) is in the structs section. Key fields verified against ROM writes:
`entry@0x0c, arg@0x10, stackStart@0x1c, stackSize@0x20, name@0x24, taskId@0x40 (low16=index, high16=monotonic uid from *0x4180), base_prio@0x46, currentState@0x49 (0xFF=free), cpu_requested@0x50, cpu_assigned@0x51 (0xFF=none), context/SP@0x54`.

The M50 II struct **already matches** the port's `CONFIG_TASK_STRUCT_V2_SMP` selection - no struct change needed; this pass supplies per-field evidence and resolves several `unknown_*` fields (uid_seq@0x42, prio_class@0x48, lock_count@0x4f, delete_pending@0x4e, sleep_nesting@0x4c).

## Current-task / kernel data block (base 0x1000)
`+0x08 current_core, +0x0c interrupt_nesting (== current_interrupt-4), +0x10 current_interrupt (0x1010), +0x28 current_task (0x1028), +0x2c dispatch_disable_2, +0x40 current_isr_context`. `get_current_task(0xE00A6BF4)` = `return *(void**)0x1028;`.

## Context switch

1. `pick_runnable (0xE01B19B6)`: walks the ready array in the sched block (`cursor@0x4024+8 .. end@0x4024+0xc`, 8-byte entries), returns the highest task whose `cpu_requested@0x50` matches the core.
2. `dispatcher (0xE01B1D40)`: `next = pick_runnable(core)`; if `*0x4144` (**task_dispatch_hook**) set -> `blx hook(reason=8, prev=current_task, next)`; then `current_task@0x1028 = next`, updates `cpu_assigned@0x51`, writes per-core slot `0x4024+0x14+core*4`.
3. `scheduler_start (0xE01B207E)`: builds the idle task, installs it as current, calls the hook with reason=9 (create), tail-branches to `context_restore`.
4. `context_restore (0xE03C51C2)`: `SP = *task->context (task->0x54)`; `dsb`; optional `bleq 0xE01B217A` when the ctx flag byte@+4 is 0; `pop {r0-r12, lr}`; `rfeia sp!` (restores PC + CPSR). So a saved context frame on the task stack = `{r0-r12, lr, PC, CPSR}` and the task struct's `context@0x54` header = `{saved_SP, flag_byte}`.

The **dispatch-hook reason codes** (arg0) are: `8` = context switch (prev,next), `9` = task create. This matches ML `my_task_dispatch_hook(p_context_old=reason, prev_task, next_task)` under `CONFIG_NEW_DRYOS_TASK_HOOKS`, so installing at `0x4144` will fire on every switch with `next` in arg3.

## Action items for platform/M50mk2.101
- `stubs.S`: `NSTUB(0x40CC, task_max)` -> `NSTUB(0x4108, task_max)`.
- `stubs.S`: `DATA_PTR(0x4108, task_dispatch_hook)` -> `DATA_PTR(0x4144, task_dispatch_hook)`.
- Optionally add `THUMB_FN(0xE00A6BBA, is_taskid_valid)` and `DATA_PTR(0x40E8, task_table)`.
- Keep `current_task 0x1028` and `current_interrupt 0x1010` (verified correct).

### F2 struct layouts
```

### struct task (DryOS V2_SMP, size 0x5C) - M50II confirmed
  +0x00   prev_task                    struct task* | build_task 0xE00A6D20 str 0,[r4]; doubly-linked list back-link.
  +0x04   next_task                    struct task* | build_task 0xE00A6D1E str 0,[r4,#4]; list fwd-link.
  +0x08   run_prio                     uint32     | build_task 0xE00A6D24 str prio,[r4,#8] from desc[0x1a].
  +0x0c   entry                        void*      | build_task 0xE00A6D32 str desc[4],[r4,#0xc]; task_trampoline ldrd r1,r0,[r4,#0xc
  +0x10   arg                          uint32     | build_task 0xE00A6D38 str desc[8],[r4,#0x10]; loaded as r0 before blx entry in t
  +0x14   waitObjId / list-head self-link uint32     | make-ready fn 0xE00A6DEA str r4,[r4,#0x14] (self when not queued); ML waitObjId.
  +0x18   unknown_03                   uint32     | not written by build_task; reserved (ML unknown_03).
  +0x1c   stackStartAddr               void*      | build_task 0xE00A6D36 str r6,[r4,#0x1c]; r6=stack allocated by 0xE0166DCE(size) 
  +0x20   stackSize                    uint32     | build_task 0xE00A6D3C str desc[0x10],[r4,#0x20]; passed as r3 to context init 0x
  +0x24   name                         char*      | build_task 0xE00A6D64 str desc[0x14],[r4,#0x24].
  +0x28   sched_node (embedded)        uint8[0x18] | build_task 0xE00A6D4C add r0,r4,#0x28; bl 0xE013B16C (node init); reused by 0xE0
  +0x40   taskId                       uint32     | callers read create return (inner: ldr r4,[r0,#0x40]); get_task_by_id compares [
  +0x42   uid_seq                      uint16     | build_task 0xE00A6D9A strh seq,[r4,#0x42] from rolling *0x4180 - the upper-bits-
  +0x44   cur_priority                 uint16     | build_task 0xE00A6D2C strh 0,[r4,#0x44] (effective priority, updated by prio-inh
  +0x46   base_priority                uint16     | build_task 0xE00A6D28 strh prio,[r4,#0x46] from desc[0x1a].
  +0x48   prio_class                   int8       | read signed in make-ready 0xE00A6DF0 ldrsb [r4,#0x48] (adds #2 -> scheduling cla
  +0x49   currentState                 uint8      | build_task 0xE00A6DAC strb state(1/2/6),[r4,#0x49]; 0xff=free slot (checked in b
  +0x4a   flags                        uint8      | build_task 0xE00A6D3E orr desc[0x19] then strb [r4,#0x4a]; bit7 tested in task e
  +0x4b   taskswitch_disable / yieldRequest uint8      | build_task 0xE00A6D54 strb 0; scheduler_unlock 0xE01B21A4 checks current_task->0
  +0x4c   sleep_nesting                uint8      | build_task 0xE00A6D50 strb 0; wake path 0xE00A6DC4 decrements [r4,#0x4c] if nonz
  +0x4d   sleepReason                  uint8      | init path 0xE00A6DE4 strb 0,[r4,#0x4d] (ML sleepReason).
  +0x4e   delete_pending               uint8      | build_task 0xE00A6D58 strb 0; task-unlock 0xE021BA4C tests [r0,#0x4e] with lock 
  +0x4f   lock_count                   int8       | build_task 0xE00A6D5C strb 0; task_lock 0xE021BA0C increments, task_unlock 0xE02
  +0x50   cpu_requested                int8       | build_task 0xE00A6D6C strb desc[0x18],[r4,#0x50] (inherited from current->0x50);
  +0x51   cpu_assigned                 int8       | build_task 0xE00A6D72 strb 0xff,[r4,#0x51]; scheduler_start 0xE01B20A2 sets core
  +0x52   pad0                         uint8      | unused (ML unknown_11).
  +0x53   pad1                         uint8      | unused (ML unknown_12).
  +0x54   context                      struct context* | build_task 0xE00A6D14 add r0,r4,#0x54 as context dst to init 0xE00A67A2 (PC=task
  +0x58   context_extra                uint32     | context header tail (ML unknown_13); rounds struct to 0x5C.

### struct DryOS_KernelBlock (base 0x1000)
  +0x08   current_core                 uint32     | read as core index in scheduler_start (ldr r4,[r5,#8]), pick_runnable, enter/lea
  +0x0c   interrupt_nesting            uint32     | checked as cannot-call-from-ISR in task_create/build_task/critical sections (cbn
  +0x10   current_interrupt            uint32     | existing stub 0x1010; interrupt id set from vector handler.
  +0x28   current_task                 struct task* | get_current_task ldr [0x1000+0x28]; dispatcher writes next task here. Existing s
  +0x2c   dispatch_disable_2           uint32     | task_create_ex secondary path 0xE05F436E ldr [0x1000+0x2c]; cmp;ble (second in-c
  +0x40   current_isr_context          void*      | leaf 0xE00A6BFC returns [0x1000+0x40] (10 refs); role likely current-context-for

### struct DryOS_SchedBlock (base 0x4024)
  +0x00   lock_word                    uint32     | enter-critical 0xE01B21D6 str saved-irqstate,[0x4024]; leave clears to 0; acquir
  +0x08   ready_cursor                 void*      | pick_runnable 0xE01B19BE ldr r1,[0x4024+8]; iterates 8-byte ready entries.
  +0x0c   ready_end                    void*      | pick_runnable 0xE01B19C0 ldr r4,[0x4024+0xc]; loop bound (cmp r1,r4; blo).
  +0x10   resched_pending              uint32     | dispatcher 0xE01B1D30 set [0x4024+0x10]; leave/unlock 0xE01B21B2 tests/clears; s
  +0x14   per_core_new_task[core]      struct task*[] | scheduler_start 0xE01B209C str task,[0x4024+0x14+core*4]; dispatcher 0xE01B1D6E 
  +0x1c   per_core_resched[core]       uint32[]   | enter-critical 0xE01B21E0 reads [0x4024+0x1c+core*4] to decide deferred resched.

```

---
## F3 agent detail
## DryOS Sync Primitives & IPC — M50 Mark II 1.0.1

### Architecture (three layers)
Every semaphore / recursive-lock / message-queue / event-flag call passes through three layers:

1. **Public SystemIF wrapper** (`SystemIF::KerSem.c` @0xE05D0xxx, `KerRLock.c` @0xE0587xxx, `KerQueue.c` @0xE05B5xxx, `KerFlag.c` @0xE05B0xxx). Validates args against the debug flag `*(0x1000+0xc)`, decodes the caller handle with `lsrs #1`, builds a timeout descriptor via `0xE00DB14C`, and re-encodes the return handle with `lsls #1`.
2. **Handle-table worker** (`0xE05BBxxx`). Enters a critical section (`0xE01B21CC`/`0xE01B21F2`), maps handle→object through a per-class lookup, and calls the kernel primitive. On create it encodes the public handle from the object's table index + generation.
3. **Kernel object primitive** (`0xE013Axxx`, plus queue-count `0xE0098338`). Owns the actual struct, the counters, and the scheduler wait/wake calls (`0xE01B1AA0` block, `0xE01B1F26` wake).

**Handle encoding:** public handle = `internal_id << 1`; `internal_id = (table_index+1) | (generation<<16)`. The low bit being clear is what every wrapper's `lsls r0,#0x1f; beq` validity test checks. Each object class has a descriptor triple (count, max, base) in RAM at `0x41BC` (sem), `0x41C8` (evf), `0x41D4` (mq), `0x41E0` (rlock).

### Semaphore (0x18 = 24 bytes)
`create_named_semaphore` @0xE05D0CAC builds a binary semaphore (max=1, arg2∈{0,1}); the counting variant @0xE05D0CFA builds max=0x7FFFFFFF. Layout: `+0x00` waiters head, `+0x04` waiters tail, `+0x08` count (negative ⇒ waiters blocked), `+0x0C` max, `+0x10` name, `+0x14` generation, `+0x16` type/state (0=counting,2=binary,0xFF=free). `take` decrements count and blocks if it goes negative; `give` increments count and wakes a waiter when count ≤ 0.

### Recursive lock (0x1c = 28 bytes)
Adds two fields over a semaphore: `+0x0C` recursion depth and `+0x10` owner TCB. `AcquireRecursiveLock` @0xE058738C: if flags bit3 set and owner==current task, just bumps the recursion depth; otherwise it takes the underlying count. `ReleaseRecursiveLock` @0xE0587456 unwinds the recursion depth first, and the worker refuses to release unless the caller is the recorded owner.

### Message queue (0x2c = 44 bytes)
`msg_queue_create` @0xE05B53CC mallocs `capacity*4` and stores it at `+0x14`. Two rings exist: the main FIFO (`+0x14` buffer, `+0x18` write idx, `+0x1a` read idx) and an optional priority/sendhead ring (`+0x1c`). Counters: `+0x0C` blocked receivers, `+0x0E` blocked senders, `+0x10` capacity, `+0x12` pending count. A blocking post lives at `0xE05B556C`; the ordinary non-blocking post @0xE05B55EE passes a zero-timeout.

**msg_queue_count semantics (verified):** `msg_queue_count(q, &n)` @0xE05B5624 calls the count worker `0xE0098338` with a 0x28-byte scratch struct and returns `scratch[0xc]`, which the worker fills from **obj+0x12 = the pending message count**. The queue's **role/state byte (obj+0x28, masked with ~0x80)** is written to `scratch[0]` and is *not* returned. So the existing ML stub returns the message count, not the role byte — correct.

### Event flags (`SystemIF::KerFlag.c`) — optional, ML uses none
Full API located: `CreateEventFlag` 0xE05B0B68, `DeleteEventFlag` 0xE05B0BA2, `SetEventFlag` 0xE05B0EBC (OR bits), `ClearEventFlag` 0xE05B0EEA (AND ~bits), `WaitForAnyEventFlag` 0xE05B0CC0, `WaitForAllEventFlag` 0xE05B0D40. Any-vs-All decoded from kernel wait 0xE013A80E: wait-mode **bit2 (value 4) = ALL** (`bics wanted,current`), **mode 0 = ANY** (`tst current,wanted`); mode **bit3 (value 8) = auto-clear** the flags after the wait is satisfied. Object layout (0x18): `+0x08` flag bits, `+0x0C` name, `+0x12` waiter count, `+0x14` state (0xFF=free). A repo-wide grep of `src/` shows **zero** event-flag references, so these are documented for completeness and only need stubs if a future module requires them.

### Verification status
All 13 pre-existing sync/IPC stubs (create_named_semaphore, give/take/take_now, Create/Acquire/Release RecursiveLock, msg_queue_create/receive/post/count) match their romdis prologue and carry the exact `SystemIF::Ker*.c` assert-file string; `verify_stubs.py --only REVIEW,BAD` flags none of them. No corrections required — only optional additions (counting semaphore, blocking post, event-flag family).
### F3 struct layouts
```

### struct dryos_semaphore
  +0x00   waiters_head                 void* (TCB) | zeroed at create 0xE013A486; wake target passed to 0xE01B1F26 in give 0xE013A536
  +0x04   waiters_tail                 void* (TCB) | zeroed at create; second word of embedded wait-queue anchor
  +0x08   count                        int32      | give: cmp vs max then count++; take: count--, if <0 block. Negative = -(num wait
  +0x0C   max_count                    int32      | give compares count==max to reject overflow; 1 for binary, 0x7FFFFFFF for counti
  +0x10   name                         const char* | create copies desc+0xc -> obj+0x10
  +0x14   generation                   uint16     | handle tag; lookup 0xE013A43A compares handle>>16 vs obj[-4] halfword
  +0x16   type_state                   uint8      | 0=counting,2=binary; 0xFF=free slot (delete 0xE013A508 sets 0xFF); bit0=wait-ord
  +0x17   pad                          uint8      | object stride is 0x18

### struct dryos_recursive_lock
  +0x00   waiters_head                 void* (TCB) | zeroed at create 0xE013AD9A; wake via 0xE01B1F26 in release
  +0x04   waiters_tail                 void* (TCB) | zeroed at create; wait-queue anchor second word
  +0x08   count                        int32      | init 1; acquire count--, ==0 acquired, <0 block; release count++
  +0x0C   recursion_depth              int32      | acquire when owner==current: obj+0xc++ (0xE013AF90); release: obj+0xc-- while >0
  +0x10   owner_task                   void* (TCB) | set on acquire (str current_task); release worker 0xE05BB7F0 checks owner==*0x10
  +0x14   name                         const char* | create copies desc+0 -> obj+0x14
  +0x18   generation                   uint16     | handle tag; encode reads obj+0x18 (worker 0xE05BB770)
  +0x1A   flags                        uint8      | desc+4 -> obj+0x1a; bit3(<<0x1c) = recursion-enabled test in acquire 0xE013AF86;
  +0x1B   pad                          uint8      | object stride 0x1c

### struct dryos_msg_queue
  +0x00   waiters_head                 void* (TCB) | zeroed at create 0xE013A986; direct hand-off reads obj+0 waiter in enqueue
  +0x04   waiters_tail                 void* (TCB) | zeroed at create; wait-queue anchor
  +0x08   name                         const char* | create: desc+4 -> obj+8; count worker copies obj+8 -> out+0x20
  +0x0C   blocked_recv_count           uint16     | enqueue: if obj+0xc!=0 hand msg directly to a blocked receiver then obj+0xc-- (0
  +0x0E   blocked_send_count           uint16     | enqueue when full: obj+0xe++ (0xE013AB66); dequeue wakes a sender if obj+0xe!=0
  +0x10   capacity                     uint16     | create: desc capacity -> obj+0x10; count worker copies to out+4; enqueue compare
  +0x12   count                        uint16     | pending message count; enqueue ++ / dequeue --; msg_queue_count returns this (ou
  +0x14   buffer                       void* (uint32[]) | create: desc buffer or malloc cap*4 -> obj+0x14; dequeue FIFO reads [buffer + re
  +0x18   write_index                  uint16     | main-ring write pos, zeroed at create, used by store helper 0xE013AA9E
  +0x1A   read_index                   uint16     | main-ring read pos; dequeue increments, wraps at capacity obj+0x10 (0xE013ABE8)
  +0x1C   buffer2                      void*      | aux/priority ring = buffer + cap*4 when flag bit1 set (0xE013AA06)
  +0x20   prio_count_or_read           uint16     | dequeue checks obj+0x20 first (priority/sendhead ring) before FIFO (0xE013ABBC)
  +0x24   prio_index                   uint16     | priority-ring read index; increment+wrap at capacity (0xE013ABD6)
  +0x26   generation                   uint16     | handle tag; encode reads obj+0x26 (worker 0xE05BB636)
  +0x28   role_state                   uint8      | free marker 0xFF at slot+0x28 (create 0xE013A9A0); count worker reads obj+0x28&~

### struct dryos_event_flag
  +0x00   waiters_head                 void* (TCB) | zeroed at create 0xE013A69E; wait blocks list via 0xE01B1AA0
  +0x04   waiters_tail                 void* (TCB) | zeroed at create; wait-queue anchor
  +0x08   flags_value                  uint32     | init pattern desc+4 -> obj+8; set/clear (0xE013A74A) OR/AND here; wait matches a
  +0x0C   name                         const char* | create: desc+8 -> obj+0xc
  +0x12   waiter_count                 uint16     | wait 0xE013A862 increments obj+0x12; set/clear re-scans when nonzero
  +0x14   state_waitorder              uint8      | free marker 0xFF at slot+0x14 (create); bit0 = wait-order flag used by 0xE01B1AA

```
