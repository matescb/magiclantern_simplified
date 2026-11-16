# 42 - Record-time crashes: the assert storm, the EvfDevState flood, the debug-formatter aborts

Written 2026-11-16 during an offline sprint. No camera was attached. The "ROM analysis" parts are
static disassembly of `m50ii_dumper/dump_1.0.1/gang100.bin` (firmware 1.0.1, base 0xE0000000) plus
register dumps already present in the UART log `tools/mlserial/run/uart.jsonl`. Static analysis
does not prove run-time behaviour, and nothing here was tested on the camera.

Three failure patterns were seen when starting or running a raw recording on the M50 Mark II. For
each this document gives: what was observed, what the chat-memory notes concluded at the time, what
the firmware analysis of this sprint shows, and which earlier conclusions are withdrawn. A fourth
section withdraws two further theories that were attached to record-time failures.

Terms:

- **Assert**: Canon's firmware calls its assert function (ROM 0xE0674574) with a file name and
  line. Magic Lantern (ML) installs a handler at RAM 0x4000 that prints
  `[ML] !!! ASSERT (non-fatal): <msg> @ <file> L:<hex line>` and returns, so the firmware continues
  past a point where Canon's own code would have stopped. ML prints the word `nullptr` when Canon
  passed no message string; it does not mean a NULL pointer was dereferenced (A2 verification
  item 9, `src/init.c:1194`).
- **Abort**: a CPU data or prefetch abort, printed by ML's abort handler as
  `[ABORT] ... pc=... lr=... DFAR=...`, after which the camera hangs.
- **LV**: LiveView. **VD**: the per-frame vertical-sync event. **EDMAC**: Canon's DMA engine.
- **Attempt N**: row N of the recording-attempt table in `docs/offline/C7.md` section (4);
  **db boot N**: boot number in `mllog.db`.
- **A1, A2, ...**: reports in `docs/offline/`. Reports A1-A9 and A13 each end with an independent
  verification section; A11 and A12 were not independently verified.

## 1. The MvRawProDs / RawPro / LvJob assert storm

### 1.1 Observed

- Three assert sites fire together at a record start, in bursts lasting up to 0.2 s:
  `Engine::MvRawProDs.c` line 293 (0x125), `Engine::RawPro.c` line 452 (0x1C4),
  `LiveViewCore::LvJob.c` line 213 (0xD5) (C7 section (2): 12, 12 and 9 lines; the first two in 6 boots, the LvJob line only in the 4 raw_vidx boots).
- Six recording attempts show it: two with mlv_lite (attempts 13, 19) and four with raw_vidx
  (35, 38, 41, 44). None of the six copied a frame, none logged an abort, and in all six the log
  goes silent afterwards (C7 section (4)).
- The memory notes also report `[kern] N interrupt occurs!` lines during the storm.
- ML's handler prints at most 32 assert lines in total (A1 verification, item 6b), so the number of
  lines in the log says nothing about how long or how fast the storm ran.

### 1.2 Explanations recorded at the time (memory/m50ii-symbol-shift-boot-crash.md)

In order of appearance, each announced as the root cause:

1. The EDMAC completion-callback "seed" (`m50ii_seed_edmac_cbrs`), which registered a no-op
   callback on a channel Canon uses; switching it off at run time made one attempt survive
   (db boot 205/206).
2. `raw_lv_redirect_edmac()`, which rewrites the address of Canon's raw EDMAC channel every frame;
   with it off, two attempts did not storm (db boots 214, 217).
3. A "pure Canon asynchronous race": `lv_save_raw(1)` only sets a flag, Canon builds the movie-raw
   job a variable number of VDs later, and consuming frames before the job exists overflows a
   2-slot ring. Fix: poll two "ready" words before going active.

### 1.3 What the ROM shows (A1)

Confirmed by A1's independent verification unless marked otherwise.

- `lv_save_raw(1)` builds nothing. Its handler (0xE06C0D04, tail-calling 0xE075442A) stores one
  flag at `[0x10238 + 0x40]`. The object at 0x10238 belongs to `LiveViewDrive::Mem1Path.c` (assert
  string at 0xE07544F4). Each frame, `Mem1Path_SetParam` (0xE0754298) re-programs **EDMAC write
  channel 3** with a buffer address the LV core passes in.
- The three asserts are overflow checks in the capture pipeline that runs in every LV session:
  - 0xE07B9CB0 (MvRawProDs, context 0x23800) and 0xE0760E74 (RawPro, context 0x292B0) each push
    one entry per VD into a 2-slot ring and assert when the count is already 2.
  - `LvJob_CreateJob` (0xE06AF4D8) asserts line 0xD5 when all 6 job slots (0x50AF8, stride 0x188)
    are in use.
- The ring entries are bit masks of the EDMAC completions expected for that VD. They are popped by
  Canon's EDMAC-complete callback 0xE0760A30, registered on the two RawPro channels at 0xE0760B6C
  and 0xE0760CBA. The pop requires the accumulated completion bits to **equal** the mask exactly.
  (That nothing else pops the rings was not verified exhaustively.)
- The two words the module polls are not "job ready" flags: `[0x23800 + 0x30]` is an argument
  stored at creation (0xE07B9B58), `[0x292B0 + 0x30]` is an EDMAC channel number (0xE0760A9C).

Meaning (Inferred from the above): the storm says "VD events keep arriving while Canon's EDMAC
completion callbacks for the raw pipeline have stopped arriving, or arrive with unexpected bits".
`RegisterEDmacCompleteCBR` keeps one callback per channel, so an ML registration on one of those
channels, a redirect that prevents a channel from completing, or a reset of it would stop the
drain. That is consistent with explanations 1 and 2 above having had an effect. It is not
consistent with explanation 3.

Corrections from A1's verification that must be carried forward:

- A1's own suggestion that Canon's "soft assert" mode explains the repeating lines is **refuted**:
  the log shows the fatal-path file strings (`Engine::MvRawProDs.c`), never the soft-path ones
  (`EPM/Capture/...`). The lines repeat only because ML's handler returns from an assert Canon
  treats as fatal. After the first line the capture pipeline is in a state Canon never continues
  from, so the LvJob assert and possibly the `[kern] N interrupt occurs!` lines may be secondary
  damage.
- With no drain at all, a report occurs every second VD after the first, not every third.

### 1.4 Withdrawn

- **The "raw job ready" poll** (`modules/raw_video/raw_vidx/worker.c:484-510`, toggle
  `rvx_wait_raw_job`). It tests two constants. The two storm-free attempts after it (45, 47) are
  not evidence for it.
- **"`lv_save_raw` constructs a movie-raw job asynchronously"** and the function names attached to
  that story (for example "MvRawProDs Create FUN_e07b9d08"; A1 verification item 7 shows
  0xE07B9D08 is a start-style entry that asserts unless `+0x30 == 0`).
- **"Redirect off kills the storm, validated"**. It rests on two attempts (42, 43); attempt 44
  stormed afterwards, with the module configuration of that attempt not recorded.
- **"The seed is the root cause"** as a complete explanation. Removing an ML callback from a
  Canon-owned channel is required (A1), but storms occurred after the seed was removed
  (attempts 38, 41, 44).

### 1.5 Open

- Which condition actually triggered each storm: a replaced callback, a stopped channel, a mask
  mismatch, or starvation of the task that programs the next frame's EDMACs. A1's verification
  gives the deciding probe: at the first line 0x125, read `0x292B0`, `0x292D8`, `0x29314`,
  `0x23804`, `0x23820`, `0x23834`. Accumulator non-zero and different from ring[0] means a mask
  mismatch; accumulator zero with count 2 means no completions arrived.
- Whether `RAW_LV_EDMAC_CHANNEL_ADDR 0xD0420200` (the register `raw_lv_redirect_edmac` writes) is
  channel 3 (A1 Unknown).
- Whether the LV core supplies a buffer to Mem1Path in plain LV (`param[8]`, A1 Unknown).
- The origin of `[kern] N interrupt occurs!` (string at 0xE05BA4E0; not analysed).

## 2. The EvfDevState flood

### 2.1 Observed

- `LiveViewDrive::EvfDevState.c` line 372 (0x174) x96 and line 542 (0x21E) x32, in 4 bursts of 32
  lines, in 4 boots, all on 2026-07-19, all within a recording attempt (C7 section (2)).
- The order inside every burst is strictly periodic: one 0x21E line per three 0x174 lines
  (A2 verification item 8).
- All four bursts belong to attempts that had copied 5 frames (40, 42, 43, 45) and each was
  followed within 1 s by an abort: three times `pc=0 lr=0xE05D1161`, once `pc=0xE05B9FDA`.
- Attempts 40, 42 and 43 logged a completed card write just before the burst; attempt 45 logged
  no write.

### 2.2 Explanations recorded at the time

1. "EVF_STATE goes null during raw LV": the asserts were read as NULL dereferences of the state
   object ML hooks at 0x10108, on the `raw_lv_release()` teardown path.
2. Later: the asserts are failed message posts to the EvfDev queue; the module's `raw_write` task
   ran at priority 0x9, above Canon's EvfDev task, so a 14 MB `FIO_WriteFile` starved EvfDev for
   about 400 ms and its queue overflowed. Fix: write priority 0x1A.

### 2.3 What the ROM shows (A2, A11)

Confirmed by A2's independent verification unless marked otherwise.

- The object at `*(0x10108)` is Canon's **EvfDevState** (15 inputs x 4 states, matrix
  0xE0BBE0F0). It is not the object the generic ML code expects under the name `EVF_STATE`. ML's
  M50 II vsync hook fires on input 6 in state 3 (`src/state-object.c:225`), which is
  `evfdevUpdateParam` (string referenced at 0xE069B306): a set-parameter event, not "sensor
  readout done".
- The object with the cells generic ML code expects exists: **EvfState** at `*(0x8E8C)`, matrix
  0xE0B5050C, 17 inputs x 8 states; input 5 is `evfReadOutDoneInterrupt`, input 4 is
  `evfSetParamInterrupt`. That the M50 mk1's `EVF_STATE` is this object was not checked against a
  mk1 ROM.
- The two assert lines are in the wrappers 0xE069A670 (line 0x174) and 0xE069A7E8 (line 0x21E).
  Both call the non-blocking `TryPostEvent` on the EvfDev queue and assert when it returns an error.
- Task facts: EvfDev has priority 0x0E, stack 0xC00 and a 200-entry queue (0xE065B630-0xE065B640);
  EvfCap, which runs EvfState and posts into EvfDev, has priority 0x0D. The EvfDevState dispatcher
  is called from the EvfDev task's handler (0xE065B5C0), and ML's hook wraps that dispatcher, so
  **the hook and everything called from it (including the module's vsync callback) run inside the
  EvfDev task**. Time spent there is time EvfDev does not drain its queue.
- A11 (not independently verified): 0x9 is indeed a higher priority than EvfDev's 0x0E; the
  worker task at 0x11 cannot pre-empt EvfDev or EvfCap but outranks EIS, PropMgr, Imager and the
  file tasks.

Corrections from A2's verification:

- A2's estimate of "about 3 EvfDev messages per frame, queue full after about 65 frames" is
  **refuted as argued**: its own sender model predicts a 2:1 ratio of the two lines, while the log
  shows 3:1. If the 0x21E event is once per frame, EvfDev receives 4 messages per frame and 200
  slots fill in about 50 frames. The qualitative conclusion (a stalled EvfDev overflows its queue
  within a couple of seconds) stands.
- The input 3 / state 5 action of EvfState (0xE06EF37A) is `evfVdInterrupt`.

Source facts that connect the module to this (Observed in source): the vsync callback can block for
up to 2 s on a semaphore (`modules/raw_video/raw_vidx/event_pusher.c:98`), allocates and frees
LV-sized buffers with `fio_malloc` (`event_pusher.c:374, 384`), and writes to the UART. Note that
reports A1 and A2 speak of a "copy in the hook"; in the source the frame copy runs in the worker
task (`worker.c:431-435`), not in the hook. What runs in the hook is the allocation, the semaphore
wait and the logging.

### 2.4 Withdrawn

- **"EVF_STATE is null / a NULL dereference in EvfDevState"**. The word `nullptr` is ML's
  placeholder for a missing assert message.
- **"The flood is a `raw_lv_release()` teardown fault"**. The bursts start before the stop path
  runs (C7: burst, then abort within 1 s; no stop-path log line in between).
- **"The vsync tap on 0x10108 (input 6, old state 3) is fine"**. It is a per-frame proxy on a
  different object, firing at set-parameter time in a different task (A2).
- **"Write priority 0x9 is the root cause and 0x1A the fix"** as a complete explanation. A write
  task at 0x9 would starve EvfDev, so the change was right, but attempt 45 shows the burst with
  no write logged, and A2 shows a second, independent way to stall EvfDev (blocking in the hook).

### 2.5 Open

- Which stall actually filled the queue in each attempt: the high-priority write, blocking calls
  inside the hook, or both. Probe (A2 hardware check 4): read the pending-message count of the
  EvfDev queue during a recording (`rd 0x9B48`, then the task-class object at `+8`, then
  `call 0xE05BA079`); a count climbing toward 200 identifies the stall.
- Whether hooking EvfState instead is safe (other DIGIC 8 ports report "EvfCap" crashes; report F1
  adds a run-time selectable hook to test this).
- Which error `TryPostEvent` returned (queue full is inferred, not observed).

## 3. Aborts inside Canon's debug-message formatter

### 3.1 Observed

Three signatures touch Canon's `DryosDebugMsg` (0xE05D10C4) or its formatters (C7 section (1)):

| Signature | Boots | When | Last ML marker |
|---|---|---|---|
| (a) `pc=0x00000000 lr=0xE05D1161` | 24 (07-14 .. 07-19) | 16 during a recording attempt, 8 after boot completed | `big.8` |
| (b) `pc=0xE005DFA2 lr=0xE005DFA1` | 1 (attempt 47, 07-22) | 0.2 s after the last module log line, after one `System::Task.c:229` assert | `big.8` |
| (c) `pc=0xE005DE52 lr=0xE005DE29`, DFAR 0 | 7 (07-14 .. 07-20) | during boot | `big.5.3` |

Signature (c) is a boot crash and is treated in `docs/43-boot-stability-and-layout.md` section 4.
`pc=0xE05B9FDA lr=0xE05B9FDB` (3 boots, one of them attempt 43) belongs with (a) and (b), see 3.3.

### 3.2 Explanation recorded at the time

Signature (a) was attributed to a NULL entry in Canon's EDMAC completion-callback table (RAM
0x83F24): an EDMAC interrupt for a channel without a registered callback jumps to address 0. The
`lr` value was called stale. The fix was to seed every slot with a no-op callback; the port-status
note records "fix validated live" on 2026-07-18.

### 3.3 What the ROM and the dumps show (A3)

Confirmed by A3's independent verification unless marked otherwise.

- All 24 dumps of (a) have one identical register tuple (r0 = 0x39, r1 = 0, r2 = 0xE05BA141,
  r4-r7 = 0, r8 = 2, r9 = 0x5154, r10 = r11 = 6) and differ only in the stack pointer, which is
  between 0xDEFFFFC8 and 0xDF000000. This is exactly the exit state of Canon's small formatter
  (0xE005D89C, epilogue `pop {r4-r7,pc}` at 0xE005D8C0) called from `DryosDebugMsg` at 0xE05D115C,
  with its 20-byte register save lying **below 0xDF000000**, the floor of a stack that the startup
  code fills from 0xDF000000 (0xE0040116-0xE00401AC). Saves written below the floor read back as
  zero, so the function "returns" to address 0. (That writes below the floor are dropped rather
  than faulting cannot be shown statically; it is the only reading that fits every register.)
- The EDMAC explanation cannot produce this register state: the EDMAC dispatch path reloads lr and
  r9 from its own frame before `bx r1` (0xE05B7E6A-0xE05B7E74).
- The string being formatted is `[TASK ERROR] TryPostEvent MsgQue OverFlow : Name = %s`
  (0xE05BA10C). It is printed by `TryPostEvent` (0xE05B9FBE-0xE05B9FD6) when its allocator
  (0xE05B9EEA) finds the **shared event-container free list (head at RAM 0x39B84) empty**. The
  pool has 1536 containers of 16 bytes (base 0xBEE18; A2 verification item 7).
- In every dump the return address 0xE05D1193 sits in the frame above: the failing `TryPostEvent`
  was itself called by `DryosDebugMsg`, which posts a notification for each message. So the chain
  "DebugMsg posts a notification, the pool is empty, the error path calls DebugMsg again" happened
  at least once. Each round costs 0xE0 bytes of stack. Whether more rounds occurred is outside the
  0x100-byte window the abort handler dumps.
- (b) is the same overflow one frame shallower: with sp = 0xDF000010 the formatter survived and
  the register saves of the ROM memcpy (0xE05D0EC0, called from 0xE005DF9C) went below the floor.
- After printing, `TryPostEvent` asserts `System::Task.c` line 0xE5 (229) and then falls through
  to 0xE05B9FDA, which stores through the NULL container pointer. If ML's handler returns from
  that assert, a data abort at `pc=0xE05B9FDA` writing address 0xC follows. The three logged
  `pc=0xE05B9FDA` aborts have DFAR 0xC (C7 section (1)), which fits.
- A3 identifies the stack as CPU 0's interrupt stack (0xDF000000-0xDF001000, top stored at RAM
  0x1014). The stack range and fill are confirmed; that it is the interrupt stack is not (the
  saved status shows supervisor mode with interrupts masked, which also fits a task holding
  interrupts masked).
- raw_vidx's own `[RVX]` logging does not go through `DryosDebugMsg`
  (`modules/raw_video/raw_vidx/raw_vid.h:12-20`), so the module's logging is not what drains the
  pool or the stack. Core ML code does call `DryosDebugMsg`.

Meaning (Inferred): the record-time aborts are the last stage of event starvation. Something
during recording stops Canon tasks from draining their queues until the shared pool of 1536
containers is empty; the first debug message after that overflows a small stack. A stalled EvfDev
alone can hold 200 containers (A2). This links section 2 to section 3: the EvfDevState burst
(queue full) is followed within a second by the formatter abort (pool empty), in four of four
cases.

### 3.4 Withdrawn

- **"`pc=0 lr=0xE05D1161` is a NULL EDMAC callback slot, lr is stale, fixed by seeding 0x83F24"**.
  The register state does not fit (A3), and the signature recurred on 2026-07-19 in three raw_vidx
  attempts after the fix was declared validated (C7 finding 4). The seed itself was later removed
  because it correlated with the assert storm (section 1).
- **Treating (a), (b) and `pc=0xE05B9FDA` as separate bugs**. They are one mechanism at different
  stack depths or with a different handler outcome.

### 3.5 Open

- What exhausts the container pool, and which queues hold the containers at that moment. Probe
  (A3 check 1): walk the free list at 0x39B84 while idle, armed and recording.
- Whether the recursion is unbounded (depends on the byte at `ctx + 0x39`, with `ctx = *(0x5154)`).
- A cheap mitigation proposed by A3 and not tested: clear the DebugMsg notify gate at RAM 0x5158
  while recording, so that pool exhaustion becomes a logged error instead of a stack overflow. It
  does not remove the starvation.

## 4. Two further theories withdrawn

### 4.1 The resource-lock theory

Recorded explanation (commit `31442ed9a`, port-status note): ML's EDMAC copy holds the engine
resource lock for IDs `{0x100AD, 0x100BB}`, which starves Canon's autofocus and stabiliser paths
("DAF/EIS storm"); the storm's error code 0xd was read as the lock's "busy" return.

ROM analysis (A12; single pass, no independent verification section):

- No Canon code path in this ROM locks 0x100AD or 0x100BB. The IDs come from another camera's
  (EOS R) copy routine that does not exist on the M50 II. ML's lock therefore creates two private
  entries and contends with nobody.
- ML's `LockEngineResources` stub (0xE05BE8A6) is the non-blocking try-lock, which has no Canon
  callers; Canon's blocking lock is 0xE05BE6A6. Canon code cannot receive 0xd from lock
  contention; the 0xd in the storm comes from the event-post path (compare section 3 and A2
  verification item 7, where `msg_queue_post` returns 0xD).
- ML never calls `DeleteResLockEntry` (0xE05BE5A2), so each recording leaks one entry.

Withdrawn: "the resource lock starves DAF/EIS". Consequence: for the CPU-copy path no lock is
needed; do not repoint the stub to the blocking variant without the hardware checks in A12.

### 4.2 The movie-format remount theory (docs/32)

Recorded explanation (`docs/32-m50ii-boot-flakiness-fio-race.md:783-804`): at record start Canon
remounts the card through "movie-format mount" functions (`vcmMovieFormatMount@0xe0202328`,
`fmMountCard@0xe02023c0`, `fmUnmountCard@0xe02028b0`); the remount needs memory that ML holds, so
the volume stays unmounted and ML's file calls fail with "Not mounted ID=0".

ROM analysis (A6, confirmed by its independent verification):

- 0xE0202328 and 0xE02023C0 are debug strings, not functions. No pointer anywhere in the ROM
  refers to them.
- The real `fmMountCard` (0xE0202106) and `fmUnmountCard` (0xE0202612) are handlers of the file
  manager's own state machine, referenced only from its state table (0xE0B630A0, 0xE0B630C8), with
  no direct callers. The places where `vcmMovieFormatMount` / `UnMount` would be called return a
  constant 0 (0xE0201F58, 0xE02026BC).
- No mount code was found on the record-start path (`handleMovieButton` 0xE071FE72,
  `IsEnableMovieStart` 0xE0547518).
- The ID in "Not mounted ID=%d" is `path[0] - 'A'` (0xE05B15AC region). ID=0 therefore means a
  path beginning with `A:`; the card on this body is `B:`.

Withdrawn: "record start remounts the card" and every mitigation built on it (mount-bit polling,
freeing memory for the remount). Not established: that the 862 logged "Not mounted ID=0" lines are
harmless enumeration of a non-existent A: drive, and who issues those calls (A6 verification
item 8). One correction to A6 itself: 0xE024A8FC is a wrapper; the function that logs
"mvrRecStart In" is at 0xE024A068.

## 5. Summary

Established (ROM-level, independently re-derived):

- The storm asserts are ring and pool overflow checks of the always-running LV capture pipeline,
  drained only by Canon's EDMAC completion callbacks (A1).
- The polled "ready" words are constants (A1).
- The EvfDevState lines are failed non-blocking posts to the EvfDev queue; ML's hook runs inside
  the EvfDev task; the hooked object is not the one generic ML code assumes (A2).
- `pc=0 lr=0xE05D1161` and `pc=0xE005DFA2` are stack-floor overruns inside `DryosDebugMsg`,
  reached through `TryPostEvent`'s pool-empty error path (A3).
- The docs/32 remount functions do not exist (A6).

Inferred:

- One common cause for sections 2 and 3: the recording path delays Canon tasks (work inside the
  EvfDev hook, CPU copy at priority 0x11, formerly a write at 0x9) until queues and then the
  shared container pool run out. `docs/offline/C9-recommended.md` builds its plan on this.
- The storm occurs when ML disturbs or starves Canon's per-frame EDMAC completions.

Open (each needs hardware): the exact trigger of each storm; what drains the container pool;
whether moving the hook to EvfState or to the Mem1Path channel-3 completion callback removes the
flood; whether a recording survives once the hook no longer blocks and the copy is fast.

Rules that follow for module code (from A1, A2, A3, A12):

1. Never register a completion callback on, redirect, reset or abort an EDMAC channel Canon's LV
   raw pipeline uses (channel 3 and the channels stored at `[0x292B0 + 0x30]` and `+0x34`).
2. Nothing in the per-frame hook may block, allocate or log; it only signals a task.
3. Keep CPU-bound work below Canon's LV task band (0x0D-0x14) or make it short.
4. Do not pass NULL for `%s` to any Canon formatter and do not log through `DryosDebugMsg` from
   interrupt context (each call costs at least 0xF4 bytes of a 4 KB stack).

## 6. Pointers

| Topic | Report |
|---|---|
| LV raw path, Mem1Path, rings, probes | `docs/offline/A1.md` |
| EvfDevState and EvfState, task and queue facts | `docs/offline/A2.md`, `docs/offline/F1.md` |
| DebugMsg formatter aborts | `docs/offline/A3.md` |
| Movie record start, no remount | `docs/offline/A6.md` |
| Canon and ML task priorities | `docs/offline/A11.md` |
| Resource locks | `docs/offline/A12.md` |
| EDMAC stubs for a hardware copy | `docs/offline/A5.md`, `docs/offline/F2.md` |
| Assert sites, bursts, attempts from the UART log | `docs/offline/C7.md` |
| Recording plan built on these findings | `docs/offline/C9-recommended.md` |
| The module itself | `docs/41-raw-vidx-module.md` |
