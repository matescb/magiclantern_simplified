# D7 - Completeness critique of the offline sprint (2026-11-16)

No camera was attached. Nothing in the sprint, and nothing in this critique, is proven on hardware.
My own checks are static reads of `m50ii_dumper/dump_1.0.1/gang100.bin` with `tools/romdis.py`, a
raw scan of the ROM for `bl` targets, `git` queries on `magiclantern_simplified`, and `grep` over
`tools/mlserial/run/uart.jsonl`.

**How much I read.** In full: `docs/offline/INDEX.md`, `docs/README.md`, `docs/42`, `docs/43`, the
"Independent verification" sections of A1-A9 and A13 (verdict tables and the refuted items). By
targeted search, not line by line: `docs/38`, `docs/40`, `docs/41`, and reports A10-A15, B1-B5,
C1-C12, C10-review, C3b, F1-F7, D2, D6. D5 (labels) was not examined. A claim in those documents
that I did not search for can be wrong without appearing below.

Words used: **verified** = a second agent re-derived it (verification section or review report);
**single-source** = one agent, self-checked at most; **inferred** = a reading that fits the facts
but that no static check can decide; **spot-checked (D7)** = I re-derived it myself, section 4.

---

## 1. The six blockers

### (i) Assert storm after raw enable (MvRawProDs.c 0x125 / RawPro.c 0x1C4 / LvJob.c 0xD5)

- **Understood: the mechanism of the assert, yes; the trigger, no.** The asserts are overflow
  checks of 2-slot rings and a 6-slot job table, popped by Canon's EDMAC-complete callback
  0xE0760A30 (A1 claims 1-5, 7: verified). What stopped the pops in each of the six storming
  attempts is not known (`docs/42` section 1.5).
- **Fix prepared: no.** There is only avoidance: `rvx_redirect_edmac=0` and no ML callback on Canon
  channels (C10-review verdict, item 2). The same review keeps `rvx_wait_raw_job=1` as a default,
  although `docs/42` section 1.4 withdraws that poll as testing two constants. It is harmless, but
  it is not a fix and should not be counted as one.
- **Open:** the trigger (replaced callback, stopped channel, mask mismatch, or starvation); whether
  register block 0xD0420200 is channel 3; whether the LV core gives Mem1Path a buffer in plain LV;
  the origin of `[kern] N interrupt occurs!`. The six-word probe in `docs/42` section 1.5 decides
  the first.
- **Evidence strength:** mechanism verified; "only Canon's callback pops the rings" is
  UNVERIFIABLE (A1-verify claims 4, 8); the meaning "ML disturbs or starves the completions" is
  inferred. Storms occurred after both the seed and the redirect were removed (attempts 38, 41, 44,
  `docs/42` section 1.4), so no tested configuration is storm-free. D7 count check: 12 / 12 / 9
  lines in the log, and zero lines with the soft-path string `EPM/Capture` (confirms the refutation
  of A1 claim 6).

### (ii) EvfDevState flood (EvfDevState.c 0x174 / 0x21E)

- **Understood: largely.** The lines are failed `TryPostEvent` calls on the 200-slot EvfDev queue;
  ML's hook runs inside the EvfDev task; the hooked object `*(0x10108)` is not the readout-done
  object (A2 claims 1-3, 5-7, 9: verified).
- **Fix prepared: no, and this is the largest gap between analysis and code.** The stall mechanism
  the analysis names is blocking work inside the hook: `take_semaphore(events_sem, 2000)` and
  `fio_malloc`/`fio_free` in the vsync callback (`docs/42` section 2.3). C10 lists exactly these as
  "Not changed, on purpose" (C10.md line 59), and C12 states "The plan's hook hygiene ... is not
  done" (C12.md, section (a), "Left out"). The branch reviewed as safe to flash therefore still
  contains the behaviour that `docs/42` rule 2 forbids. F1 (move the hook to EvfState input 5) is a
  core change that moves 494 symbols and was not reviewed.
- **Open:** which stall filled the queue (write task at 0x9, blocking in the hook, or both); which
  error `TryPostEvent` returned (queue-full is inferred); whether EvfState is the mk1 `EVF_STATE`
  (UNVERIFIABLE without an mk1 ROM); whether hooking EvfState is safe.
- **Evidence strength:** structure verified; the per-frame message model is REFUTED as argued (A2
  claim 8); the causal chain is inferred from four attempts. D7 count check: 96 and 32 lines.

### (iii) Record-time crashes in the debug formatter (`pc=0 lr=0xE05D1161`, `pc=0xE005DFA2`, `pc=0xE05B9FDA`)

- **Understood: the fault, yes; the cause of the pool exhaustion, no.** Stack-floor overrun below
  0xDF000000 inside `DryosDebugMsg`, reached through `TryPostEvent`'s pool-empty path (A3 claims
  1-5, 7: verified). One recursion round is proven; more rounds, and "interrupt stack", are
  UNVERIFIABLE (A3-verify items 6, 8).
- **Fix prepared: no.** A3's mitigation (clear the notify gate at RAM 0x5158 while recording) is
  not implemented on any branch. F4's rules (nothing slow in the hook, no CPU-bound ML task above
  Canon's LV tasks) are only partly applied: C12 adds priority toggles whose defaults I did not
  confirm to differ from 0x11, and the hook is unchanged (see (ii)).
- **Open:** what exhausts the 1536-container pool and which queues hold the containers; whether the
  recursion is bounded (byte at `ctx+0x39`, `ctx=*(0x5154)`).
- **Evidence strength:** fault site verified; F4 (pool geometry, rules) single-source; the link
  "flood then abort within 1 s" is four of four attempts, inferred.

### (iv) Frame copy and card throughput

- **Understood: the copy, yes (verified); the card ceiling, from one card.** The copy ran in
  memcpy's byte loop from an address that is 2 mod 4, non-cacheable to non-cacheable, about
  5.7 MB/s (A13: verified, speed-ups not measured). Card: 75.1 / 75.7 MB/s write on one 16 GB card
  (A7, log-derived).
- **Fix prepared: partly, untested.** C12 (`offline-plan-step1`, tip 09a4aaf4b, one commit on top
  of C10) adds burst-to-RAM, a throttled copy and fast-copy toggles. It was **not reviewed by a
  second agent**, creates one task per frame (25 `task_create` per second, never run on the camera,
  C12 section (a)), and takes ROM address 0xE05D088C and the priority table from A13 and A11
  "as written" (C12 Limitations). The hardware EDMAC copy (F2) is a research item: its start
  sequence is unknown (C9-recommended line 96, 180).
- **Open:** real copy speed after alignment and cacheable read; whether the cache invalidate
  reaches the other core (A13, C12 section (d)); whether a frame in flight completes after the
  flag is cleared (C9-recommended assumption 4, "the central unknown"); sustained write speed with
  LV running. Arithmetic that nobody stated plainly: the current crop is 2,809,856 bytes per frame
  (`docs/41` line 149); at the 25 fps C4c measured, that is 70.2 MB/s against a 75 MB/s bench on
  an idle camera. Continuous recording at this size has no margin even with a perfect copy.
- **Evidence strength:** A13 and A7 verified; C12 and the plan single-source; all speeds for the
  fixes are estimates.

### (v) Boot crashes and layout sensitivity, incl. the PropertyMgr crash `pc=0xE021B8FC`

- **Understood: no.** The root cause of `0xE021B8FC` is not identified (A4 verified in its
  mechanics, F5 single-source, both say so). "Symbol shift breaks boot" is withdrawn on log
  evidence (C6, C7; two single-source reports reading the same log, not two independent sources).
  The `big.5.3` NULL `%s` caller is not identified. The 48 silent restarts are unexplained.
- **Fix prepared: none for any boot crash.** What exists is hygiene: `tools/symgate`, 18-character
  branch names, C11's fixed-width build id (not merged, not reviewed). The one change with
  measured benefit, the empty `normal_code_patches[]` (0 of 70 against 19 of 92 for
  `0xE02F281A`, `docs/43` section 2), is **not on the baseline or on any sprint branch**; the next
  session will lose about 1 boot in 5 to that family.
- **Open:** see section 6, items 1 and 2. In particular the status of the baseline itself:
  `docs/43` section 5.4 warns that the baseline has the same size (257696 bytes) as the core that
  crashed 7 of 7 on 2026-07-21, and the per-build table in section 1 gives that build key 9 of 29
  boots to `big.8`. The last boots in the log (db 362-366) are a different key (257760). "Last
  build known to boot" is therefore taken on trust (C5 limitations), and C10-review did not check
  which core is on the physical card (C10-review.md line 175).
- **Evidence strength:** counts are reproducible from the log (D7: 216 `big.8` lines; 29 register
  dumps with `pc=0xE021B8FC`); every causal statement is inferred.

### (vi) raw_vidx correctness bugs

- **Understood and fixed on a branch, with host tests only.** C4 fixes, C4-review's two HIGH
  findings, and C1's writer bugs are resolved on `offline-integrate1` (C10-review: verified, "No
  HIGH findings").
- **Still open in the reviewed branch** (C10-review M1, M3, L3): a stop that times out leaves the
  file unclosed and the clip is probably lost; after a failed card write the recording continues
  with no sign to the user; VIDF timestamps are taken when the worker handles the frame, not at
  capture. The MLV header fps is about 18.5 % high on the old core (C12.md line 108).
- **Evidence strength:** verified by review, but the host harness is single-threaded and cannot
  show races, starvation or timing (C12 Limitations; C10-review Limitations).

---

## 2. Claims in the synthesis docs that need correcting

The synthesis docs carried over the verifiers' refutations well; I searched 38, 41, 42, 43 and the
README for every refuted item of A1-A9 and found each one already corrected (for example `docs/38`
lines 151-160, 394-397, 625-627, 755-756; `docs/42` lines 85-91, 166-169, 253-255, 331-332;
`docs/43` section 5.3 last paragraph). What remains:

| # | File and place | Problem | Correct statement |
|---|---|---|---|
| 1 | `docs/42-record-time-crashes.md` line 339 (section 5, under "Established (ROM-level, independently re-derived)") | "drained **only** by Canon's EDMAC completion callbacks (A1)". A1-verify marks "only" UNVERIFIABLE (claims 4 and 8); section 1.3 of the same document says so. | Move "only" out of the Established list or write "popped by ...; that nothing else pops them was not verified". |
| 2 | `docs/README.md` line 119 | "`walkback-milestone` ... `m50mk2-port` plus one commit". `git merge-base --is-ancestor fa0834b40 a10b16c23` is false. `a10b16c23`'s parent is `3ccf499dc`, an earlier commit of the port. | "`3ccf499dc` (an earlier port commit) plus one commit"; the work-in-progress tip `fa0834b40` is not contained in the baseline. |
| 3 | `docs/README.md` line 138 and lines 277-278; `docs/offline/INDEX.md` lines 78 and 103-104 | Stale: say C10-review, C12, C3b are absent and that `offline-plan-step1` equals C10's tip. All three reports exist; `offline-plan-step1` is at 09a4aaf4b, one commit ahead. D2, D5, D6 are also missing from INDEX. | Add the reports; give C12's real tip. |
| 4 | `docs/README.md` section 4.1 (table ends at line 219 with doc 38) | Docs 40, 41, 42, 43 are not in the document index at all, and section 5 "Where to start" does not mention them. | Add four rows. |
| 5 | `docs/README.md` line 144 | "the last five branch names"; six sprint branches have 18 characters (integrate1, plan-step1, buildid-fx, evfstate-1, edmacstubs, stub-fixes). | "six". |
| 6 | `docs/README.md` line 198 | Says docs/17 "keeps 0x40CC" for `task_max` and treats A14a as an unverified counter-argument. My spot-check (section 4, item 1) confirms 0x4108 in the ROM. | Mark the docs/17 value as refuted statically; hardware check `rd 0x4108`. |
| 7 | `docs/43-boot-stability-and-layout.md` sections 5.3, 5.4 and 9 | F5, the continuation of A4, is never cited (no occurrence of "F5" in the file). Section 5.4 (b) describes the probe as "8 words at r8"; F5 line 39 specifies 4 words at r8 and gives the complete probe in its section 7. | Cite F5; use F5's probe definition. |
| 8 | `docs/offline/INDEX.md` line 78 and `C10.md` line 21 | "Core `.sym` byte-identical to the booting baseline" is true but invites the reading "core unchanged". The staged `autoexec.bin` differs in code: 3 bytes at file offset 0x18E20, 32000000 to 27000000 (C10-review.md line 38, finding M2). | Say "layout identical; one core constant changed (`TG_FREQ_BASE`)". |
| 9 | `docs/41-raw-vidx-module.md` lines 61-67 and 247-248 | Canon priorities (Fwrite / MovWriter 0x15 and others) are stated as fact with an (A11) tag. A11 is single-source; my check (section 4, item 4) shows the priority at those two create sites is a caller-supplied argument, not a constant. | Tag as "A11, not independently verified". |
| 10 | `docs/offline/A11.md` line 129 | Hardware check tells the reader to walk the task list using `task_max` 0x40CC. | 0x4108. |

Not an error but a wording risk: `docs/42` rule 4 speaks of "interrupt context" although the
interrupt-stack identification is UNVERIFIABLE (the same document says so at lines 253-255).

---

## 3. Contradictions between reports that nobody resolved

1. **`ConnectReadEDmac_maybe`: 0xE05B7B08 or 0xE05B7A32.** A5 and its verification derive
   0xE05B7B08 by position (`reset_channel - 0x3A`; identity UNVERIFIABLE). F2 put 0xE05B7A32 in
   the stub by Canon call order and says so plainly (F2.md lines 114-125, 268-269). `docs/38`
   lines 312 and 900 name 0xE05B7B08 "disable leaf". The branch `offline-edmacstubs` carries F2's
   choice. See section 4, item 2: the call order is real, the identity stays open.
2. **`task_max`: docs/17 (0x40CC, "settled") against A14a / F7 (0x4108).** Not resolved by any
   report; README line 198 leaves both standing. Resolved statically by D7 in favour of 0x4108.
3. **Is the baseline a booting build?** The task brief and README line 119 call `a10b16c23` the
   last build known to boot. `docs/43` section 5.4 and C6 say the log cannot distinguish it from
   the core that crashed 7 of 7, and C5 takes its boot status on trust. Nobody reconciled these.
4. **Are the boot fixes harmful?** The memory index still says the boot fixes "CHANGE THE CORE ->
   deterministic EVF crash" and prescribes the walkback. `docs/43` section 5.4 withdraws that. The
   sprint did not act on its own conclusion: no branch re-applies the empty patch table.
5. **Where the frame copy runs.** A1 and A2 argue from "a copy in the hook"; the source has the
   copy in the worker task (`worker.c:431-435`). `docs/42` section 2.3 notes it, but A1's and A2's
   hardware-check sections were not updated, and C9's stall arithmetic should be read with the
   corrected picture.
6. **Caller counts for the F2 stubs.** F2.md lines 34-35 give 267 callers for 0xE05B82AE and 156
   for 0xE05B7A32. My scan of the ROM for Thumb `bl` instructions finds 265 and 63. Tail calls by
   `b.w` (one is visible at 0xE00BF450) are not in my count, so the numbers may measure different
   things; the 156 was not reproduced. The counts are only supporting evidence.
7. **C10-review defaults against `docs/42`.** The review's first-attempt defaults include
   `rvx_wait_raw_job=1`; `docs/42` section 1.4 withdraws that poll. Either the default or the
   document should change.
8. **C4c's explanation of its layout difference** ("version string length differs between build
   dates") against C5 / C4-review (branch-name length). `docs/43` section 6.4 flags it as inferred;
   C4c itself was not corrected.

---

## 4. Five single-source findings spot-checked by D7

Chosen because each is applied on a prepared branch or steers the next hardware session.

**1. A14a / F7: `task_max` = 0x4108, `_request_RPC` = 0xDF002C4A. Result: CONFIRMED statically.**
- 0xE00A6BC6: `ldr r2,[0xE00A6F6C]` (= 0x00004108); `ldr r2,[r2]; cmp r0,r2; bgt 0xE00A6BF0`, then
  an index into the table at `*(0x40E8)` with element size 0x5C. That is a bound on a task id.
- Literals 0x40CC occur at 0xE03EF4C8 and 0xE03EF888, as A14a says. `uart.jsonl` line 11753 is
  `0x000040CC: 0x00000001`: on hardware the current stub reads 1.
- Blob descriptor: `[0xE00400EC]=0xE14995A4`, `[0xE00400F0]=0xDF002800`, `[0xE00400F4]=0xDF0033A0`.
  0xDF002C32 maps to ROM 0xE14999D6, which is the second half of `dmb sy` at 0xE14999D4. ROM
  0xE14999EE (RAM 0xDF002C4A) is `push {r4,r5,r6,lr}`, a function start; 0xE1499A0E (RAM
  0xDF002C6A) is `push {r4,lr}`.
- Not checked: that the function at 0xE14999EE is semantically `_request_RPC`. Note for the
  reader: `tools/romdis.py` annotates 0x4108 as `task_dispatch_hook`. That is a stale symbol map
  (the stub is at 0x4144, `stubs.S:167`), not a second conflict.

**2. F2: `StartEDmac_maybe` = 0xE05B82AE, `ConnectReadEDmac_maybe` = 0xE05B7A32. Result: bodies and
Canon call order CONFIRMED; identity with the mk1 names remains open.**
- 0xE05B82AE calls 0xE05B7B08 (`reg+4 := 0`) and 0xE05B7AA4, then either tail-calls 0xE05B7A6C
  (`reg+4 := 1`) at 0xE05B8304 or sets `reg+0xB4 := 1`. It ends with the channel enabled, which
  fits "start".
- 0xE05B7A32 asserts line 0x3AC unless a flag bit is set, writes `reg+8 := 0x12`, writes 0 to
  0xC1100730, sets `reg+0xB4 := 1`.
- 0xE00BF3FE-0xE00BF450: Canon calls 0xE05B82AE on `[0x1A724+0x10]`, `[+8]`, `[+0xC]`, then
  0xE05B7A32 on `[+8]` and `[+0xC]`, as F2 section 3.3 states.
- What this does not show: that this Canon function is a mem2mem start with the same roles as
  `edmac-d8.c:185-187`, or that 0xE05B7A32 is right for read channel 46. A wrong pick is expected
  to show as a 50 ms timeout, not a crash (F2 line 253), but 0xE05B7A32 on a non-read channel is
  a Canon assert (F2 line 241).

**3. A12: the stub 0xE05BE8A6 is a try-lock, the blocking lock is 0xE05BE6A6. Result: CONFIRMED.**
- 0xE05BE6A6 contains `bl 0xE05D0DA8` (`take_semaphore`) at 0xE05BE728 and a string reference at
  0xE05BE714; 0xE05BE8A6 has no semaphore call and returns constants (0, 1, 2 paths, 7 on a bad
  argument).
- ROM scan for `bl`: 0 calls to 0xE05BE8A6 and 0 literal words 0xE05BE8A7; 105 calls to
  0xE05BE6A6, the number A12 gives.
- Not checked: that no Canon path locks 0x100AD / 0x100BB.

**4. A11: Canon task priorities (basis of C12's toggles and the 0x15 / 0x16 recommendation).
Result: PARTLY CONFIRMED, and weaker than the table suggests.**
- EvfDev 0x0E and EvfCap 0x0D, stack 0xC00, queue 200 were already confirmed by A2-verify.
- For the two "bulk writers" the recommendation rests on, the priority is not a constant at the
  create site. MovWriter: 0xE04A7680 `mov r4,r1` then 0xE04A76DA `mov r1,r4` before
  `bl 0xE05B9E60`; stack 0x2000 and queue 0xFA0 are constants as listed. Fwrite: 0xE02F26E2
  `mov r5,r3` then 0xE02F2736 `mov r1,r5` before `bl 0xE05BBC58`. A11's 0x15 therefore comes from
  tracing callers, which I did not repeat. A11's own note (line 103) admits a second creator entry
  for several tasks.
- Consequence: "copy at 0x15 is level with Canon's writers" is single-source. A11 check 2 (print
  the live task table) should run before any priority A/B test is interpreted.

**5. C7 / C6: log-derived counts behind the boot and crash narrative. Result: CONFIRMED.**
- `[ML] big.8` lines: 216. Register dumps `pc=0xE021B8FC spsr=...`: 29. EvfDevState lines: 96 at
  L:0x174 and 32 at L:0x21E. Storm lines: 12 MvRawProDs, 12 RawPro, 9 LvJob.
- Side observation that no report mentions: the string `pc=0xE021B8FC lr=0xE04037A1 dfar=0x46`
  occurs 28,494 times in the log, about a thousand lines per crashing boot. The abort handler was
  therefore not "one-shot" in those boots, or something re-printed the line. This matters for F5's
  probe, which must be armed inside that handler.
- Not checked: the per-build table (it depends on C7's boot segmentation and build-key logic).

Additional quick run: `docs/offline/tools/raminit.py 0x10108` returns 0 from the image at ROM
0xE13A0460, consistent with A15's "0 at boot" for that pointer. The other six pointers were not
re-run.

Not spot-checked and still single-source: A10, A14b's non-EDMAC rows, A14c (incl. `LV_OVERLAYS_MODE`
= `MEM(0x15998)`, applied in F7 and expected to change LiveView behaviour), F4, F5, B1-B5, C8.

---

## 5. Work planned but not done, and risks of the prepared branches

### 5.1 Not done or only partly done

| Item | State | Reference |
|---|---|---|
| QEMU stage A (Canon storage mounts) | Not achieved. C3b found the real stall (serial-flash identification, not the EEPROM) and gets Canon's init stage 1 running, then both cores park before PropertyMgr is created. | C3.md line 5; C3b.md lines 5-8, 131 |
| QEMU stage B (load raw_vidx / bench in the emulator) | "Partially" per C3b, but no module was loaded. | C3b.md line 5 |
| QEMU stage C (write an MLV in the emulator) | Not reached. The emulator cannot exercise the recorder at all. | C3.md line 100; C3b.md line 8 |
| Hook hygiene (no semaphore, allocation or UART in the vsync hook) | Not done on any branch. | C10.md line 59; C12.md section (a) |
| Plan steps 0 and 1 (probe script, bench task), per-second summary, header fps from timestamps | Not done. | C12.md Limitations |
| Resident copy task | Not done; per-frame `task_create` instead. | C12.md section (a) |
| Time-based checkpoints | Not done. | C10-review.md line 79 |
| A3 mitigation (gate at 0x5158), NULL-guard for the `big.5.3` crash | Proposed only. | docs/42 section 3.5; docs/43 section 4 |
| F5 abort-handler probe for `0xE021B8FC` | Specified, not written as code. | F5.md section 7 |
| Empty `normal_code_patches[]` back on the baseline | Not done although it is the only measured boot improvement. | docs/43 sections 2, 8 |
| `tools/verify_stubs.py` handling of 0xDF00xxxx stubs | Not fixed; the blind spot that hid the RPC error remains. | F7.md line 106 |
| Ghidra labels | Proposals only (`labels/*.csv`); not applied (by rule). | INDEX "Other files" |
| Commit, build, deploy pipeline that logs (commit, md5, boot result) | Not built. | docs/43 section 7 |
| Reports F3 and F6 | Never produced. | INDEX lines 103-104 |
| Stale-module trap re-test | C10-review built modules first and did not re-test it. | C10-review.md line 51 |
| Review of `bench.mo` and `file_man.mo` on the staged card | Not done. | C10-review.md line 176 |
| C12 against the real `mlv_3.c` and `mlv_check.py` | Not re-run. | C12.md Limitations |
| `fork` remote | 9 of 15 port commits and all sprint work exist only on this machine. | README section 2.2 |

### 5.2 Prepared branches: core changes and review state

"Core diff" is `git diff --name-only a10b16c23..<branch>` outside `modules/`, taken by D7.

| Branch (worktree) | Files changed outside `modules/` | Effect on the core binary | Second-agent review |
|---|---|---|---|
| `offline-integrate1` (C10) | `edmac-d8.c`, `modules.included`, `stubs.S`, `src/fps-engio.c` | Layout identical; code constant `TG_FREQ_BASE` changed (3 bytes); `mlv_lite` no longer packed | **Yes** (C10-review): safe, module only first |
| `offline-plan-step1` (C12) | same four files (inherits C10) | Same core as C10; module grows (`worker.c` about 1190 lines) | **No** |
| `offline-buildid-fx` (C11) | `platform/Makefile`, `src/raw.h` | Intended to keep the baseline layout; changes how every build is stamped | **No** |
| `offline-edmacstubs` (F2) | `edmac-d8.c`, `stubs.S` | Changes which Canon functions the core calls on the EDMAC path | **No** (D7 spot-check of two stubs only) |
| `offline-stub-fixes` (F7) | `consts.h`, `stubs.S`, `src/raw.c` | 13 stubs changed, 9 added, 1 constant; two commits expected to change LiveView behaviour | **No** (D7 spot-check of `task_max` and the RPC pair only) |
| `offline-evfstate-1` (F1) | `state-object.h`, `src/state-object.c` | +160 bytes of code, **494 of 1307 symbols move**, symbol gate fails by design | **No** |
| `offline/C4` | none | Module only; branch name is 10 characters, so its own core build does not match the baseline | Yes (C4-review); superseded by C10 |
| `offline/C4c` | `edmac-d8.c`, `stubs.S`, `src/fps-engio.c` | As C10's core part; 11-character name | No; picked into C10 and reviewed there |
| `offline/C8` | `modules.included` | Packaging only | No; picked into C10 and reviewed there |

Risks that follow:

1. Only one branch has been reviewed, and the review covers the module on the old core. Every
   branch that fixes something the analysis identified as a cause (F1 hook source, F2 EDMAC stubs,
   F7 stubs, C12 priorities and copy) is unreviewed.
2. The branches are parallel, not stacked. F1, F2, F7 and C11 all start at `a10b16c23`; F2 and F7
   both edit `stubs.S`, F2 and C10 both edit `edmac-d8.c`. No merged build of them exists, no
   symbol gate was run on a combination, and F1 alone breaks the "core unchanged" rule.
3. F7 changes `_request_RPC` and `clear_RPC_request`, which `.sym` files do not list (names with a
   leading underscore, F7.md line 61). The symbol gate cannot see this change. The RPC path is
   reached when ML patches ROM with the second core alive (A14a line 82); a wrong new address
   fails there, in a crash class the gate was built to prevent.
4. F7's `LV_OVERLAYS_MODE` and `LiveViewApp_dialog` commits change run-time behaviour in LiveView
   and rest on A14b / A14c, which nobody checked.
5. C12's build directory contains a core with C10's core commits. Copying that `autoexec.bin`
   instead of only the `.mo` puts an unbooted core on the card (C12 Limitations, last item).
6. The module-only first step depends on the card really holding the baseline core and `.sym`.
   Nobody verified the physical card (C10-review.md line 175), and the WiFi-deploy module cannot
   load against the baseline (9 unresolved imports, `docs/43` section 6.5), so the card must be
   handled by hand.
7. The baseline lacks the empty patch table, so roughly one boot in five dies at `big.4a`
   regardless of what is tested. A failed boot of a new module must not be read as the module's
   fault without repeats.

---

## 6. Questions only hardware can answer, ranked by effect on the plan

1. **Does the baseline on the card still boot, and how often?** Ten cold boots of the untouched
   card before anything is changed. If `0xE021B8FC` returns, everything else waits: image the card,
   run F5's probe. If the rate is near the logged 31 % for that build key, the session plan needs
   the empty patch table first. Every later result depends on this number.
2. **Is `0xE021B8FC` tied to card state or to core content?** C6 check 1 (alternate two cores on a
   freshly formatted card). Decides whether "keep the core unchanged" stays a rule; if it falls,
   F1, F2, F7 and the boot fixes become usable and the plan changes from module-only to a normal
   core workflow.
3. **Does the C10 module record a decodable clip on the old core without flood or abort?** With
   the hook still blocking, the analysis predicts the flood can recur. Read the EvfDev queue depth
   (A2 check 4) and walk the free list at 0x39B84 (A3 check 1) during the attempt. Outcome decides
   whether the hook rewrite is the next task or whether the storm is.
4. **What triggers the assert storm?** Six words at the first 0x125 line (`docs/42` section 1.5).
   Decides between "ML disturbed a channel" and "ML starved a task", which lead to different
   module designs (C12 priorities against channel hygiene).
5. **How fast is the copy after the A13 fixes, and what does the card sustain with LV running?**
   The 70.2 MB/s requirement against 75 MB/s idle bench leaves no margin; this decides between
   continuous recording, a smaller crop, and burst-to-RAM as the product.
6. **Does a frame stay intact while it is copied (which buffer, how many VDs)?** C9-recommended
   assumption 4. Decides whether any CPU-copy design yields untorn frames or whether the hardware
   route (F2) is mandatory.
7. **Live task table and priorities** (A11 check 2; `rd 0x4108` for `task_max`). Cheap, and it
   converts A11 from single-source to measured before C12's priority tests are interpreted.
8. **Does the EDMAC mem2mem copy work with F2's stubs?** F2 section 6 hand test. Only matters if
   items 5 or 6 rule out the CPU copy; it needs a core change, so it also depends on item 2.
9. **Is EvfState input 5 a safe and better hook than EvfDevState 6/3?** F1's module-only `statespy`
   variant first. Matters for frame timing, not for getting a first clip.
10. **Frame-timer clock 27 MHz** (A9, C4c): measured fps against the header value. Affects
    metadata only; lowest priority, and it is the one core byte change on the staged card.
