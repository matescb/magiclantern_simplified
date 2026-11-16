# C10-review: review of branch offline-integrate1 (candidate for the next hardware session)

Reviewed: `/mnt/data/WORK/eos/wt/C10`, branch `offline-integrate1` (18 characters; no branch
`offline-integrate-1` exists), tip `9633ac828`, 14 commits on `a10b16c23`. Staged card:
`/mnt/data/WORK/eos/sim/C10/card`. Offline review only: nothing here is proven on the camera.
The branch was not modified.

## Verdict

**Safe to flash as a first attempt**, under these conditions:

1. First step is module only: keep the `autoexec.bin` and `.sym` that already boot, copy only
   `sim/C10/card/ML/modules/raw_vidx.mo`, and remove `mlv_lite.mo` from the card. Run
   `check_card_image.py` on the mounted card first, so the module is known to resolve against
   the core that is really there.
2. Leave all four toggles at their defaults (`rvx_checkpoint=0`, `rvx_wait_raw_job=1`,
   `rvx_src_from_raw_info=1`, `rvx_redirect_edmac=0`).
3. The staged `autoexec.bin` (27 MHz timer base) is a separate, second step, after the module
   has recorded a clip on the old core.

Nothing needs fixing before that. I found no race, leak or blocking call that the previous
review did not already know about, and all eight actionable C4-review findings are resolved.
The remaining findings below are follow-ups, none of them on the default first-attempt path
unless the card write hangs for more than 10 s or the card fills up.

## What was done

All output is under `/mnt/data/WORK/eos/sim/C10-review/`.

| Check | How | Result |
|---|---|---|
| Scope | `git diff --stat walkback-milestone..offline-integrate1` | 9 files: 5 in `modules/raw_video/raw_vidx`, `platform/M50mk2.101/{edmac-d8.c,modules.included,stubs.S}`, `src/fps-engio.c` |
| History | `git merge-base --is-ancestor offline/C4`, `git patch-id` | `offline/C4` tip `f8191a47a` is an ancestor; the four cherry-picks have the same patch-id as C4c `a890f1690`, `797627c32`, `1f324fa84` and C8 `8b297cfad`; all 14 commits carry the trailer |
| Rebuild | temporary worktree `wt/C10-review`, branch `offline-c10-review` (18 chars) at `9633ac828`; `make` in `modules/`, then in `platform/M50mk2.101` | both rc=0, no compiler warnings |
| Symbol gate | `check_symbol_stability.py $B/M50mk2_101.sym --elf $B/magiclantern` | PASS, files byte-identical, md5 `d2f46ba5e091fa02d53a8aae9969442f`, 0 added / removed / moved, all section ends OK |
| Module imports | `check_module_syms.py` | PASS, `raw_vidx.mo` 45 imports, 0 missing |
| Card gate (my build and the staged card) | `check_card_image.py ... --elf` | PASS for both; the staged manifest's 5 md5s match |
| Staged core vs baseline | byte diff against the main checkout's `autoexec.bin` (md5 `1df03e45...`, the one in `BASELINE.json`) | 91 bytes: date / build-id strings, 4-byte trailer, and 3 bytes at 0x18e20 (32000000 -> 27000000) |
| Staged core vs my build | byte diff | 27 bytes: branch name, build time, trailer. Same code. |
| Staged module vs my build | `objdump -d -r` of both `raw_vidx.mo` | identical disassembly (md5 `c7ed8bdd...`), same sizes 18991 / 1312 / 960 |
| Host tests, `_c10` harness, branch sources | `sh tools/raw_vidx_hosttest_c10/run.sh` | 17 tests, 124 checks ok, `failed tests: 0` |
| Host tests, original harness, branch sources | `sh tools/raw_vidx_hosttest/run.sh` | 9 of 10 pass; `slow_card` aborts with `harness: too many pending tasks` (see note) |
| Host tests, `_c10` harness, `offline/C4` sources | same runner | the 7 new tests fail, the 10 old ones pass |
| Host MLVs | `tools/mlvcheck/host_writer_c10/build.sh`, `rvx_host_mlv`, `mlv_check.py -v` | clean stop 8/8, 13/13, 3/3: exit 0, 0 errors; never-closed file: `FRAME_COUNT_UNFINALISED`, exit 1 |
| Tool self-tests | `pytest tools/mlvcheck/test_mlv_check.py tools/symgate/tests` | 67 passed |
| ROM check of `task_create` | `python3 tools/romdis.py 0xE05F40BA` | success returns `handle << 1`; failures return 1, 3 or 5 |

Notes:
- The `slow_card` abort is a harness artefact, as the implementer said: the original stub
  returns 1 from `task_create`, which the ROM uses as an error code and the module now checks.
- I built `modules/` before the platform, so I did not re-test the stale-module trap. The
  staged `raw_vidx.mo` has the same code as my clean build, so the staged card is not affected.
- The temporary worktree and branch were removed afterwards. My ELF, `.sym`, `autoexec.bin`
  and `raw_vidx.mo` are kept in `sim/C10-review/build/`.

Excerpts:

```
files are byte-identical
added: 0   removed: 0   moved (ML core): 0   moved (ROM stubs >= 0xE0000000): 0
RESULT: PASS - every pre-existing symbol is at its baseline address

== baseline(main) vs staged          diff bytes 91
0x18e20 3 b'\x00H\xe8' b'\xc0\xfc\x9b'
== mine vs staged                    diff bytes 27

frames: 13 complete VIDF, header declares 13, expected payload 2809856 bytes
  8 0x0000023c VIDF 2809984  33.436 ms  frame=0 space=96 payload=2809856 crop=200,100 pan=200,100
result: 0 error(s), 1 warning(s), 2 info
```

## Status of the C4-review findings

Line numbers are `worker.c` at `9633ac828` unless another file is named.

| # | Finding | Status | Evidence |
|---|---|---|---|
| 1 | One `write_sem` timeout drops every frame until stop | Resolved | `pending_flush` is retried without waiting on every frame (567) and with a 50 ms wait in the both-busy branch (641-645); set at 684-691. Traced by hand: a buffer can only become free through a write task, which requires the pending entry to have been cleared, so the pointer cannot go stale. Host test `flush_sem_timeout`. |
| 2 | Checkpoint on by default, unchecked seek and header write, 0x800 stack | Resolved | `rvx_checkpoint = 0` (77); it sits in `.bss` in the staged module. Header write checked (365-373), seek position compared (391-401), stack 0x1000 only with the toggle on (427-430). Time-based checkpoints were not done. |
| 3 | Stop timeout closes the file and frees buffers under a running write | Resolved | 971-983 neither close nor free; `rvx_wedged` refuses later starts (760-766); buffers kept (1027). See finding M1 for the cost. |
| 4 | `task_create` result ignored while `write_sem` is held | Resolved | 444-452 and 466-475. I checked the ROM routine myself: success is `lsls r0, r4, #1`, errors are 1, 3, 5, so the odd/even test at 420-423 is right. |
| 5 | Short write leaves a truncated block mid-file | Resolved | `rvx_write_failed` set at 326; later buffers discarded (299-306), frames discarded (539-548). |
| 6 | Frame counted before `edmac_sem` is taken | Resolved | Take moved to 590, before the VIDF header; both early exits give it back (650, 661). |
| 7 | Unsynchronised reset of `frames` / `cur` | Resolved | Both `volatile` (129, 131). Disassembly of the staged module confirms the order: `str r3,[r5,#16]` (frames) at 0x1f8, then `str r3,[r5,#8]` (cur) at 0x200. |
| 8 | Stale comments, per-boot log limiters | Resolved | `log_n` cleared at start (802); comments corrected. `ep_log_n` in `event_pusher.c:484` is still per boot, which was not part of the finding. |
| 9 | Observations on older code | Unchanged, as agreed | `take_semaphore(events_sem, 2000)` at `event_pusher.c:117`, `fio_malloc` at `:420` and `:430`, both in the vsync callback. |

## Findings, by severity

No HIGH findings. All of these come from reading the code; none was reproduced.

### M1. MEDIUM: a stop that times out leaves the file unclosed, so the whole clip is probably lost
`worker.c:954`, `967-983`

If a card write does not come back within 10 s, the file is left open for good. That is the
safe choice against a use-after-free, but with `rvx_checkpoint = 0` the file is never closed,
and the one unclosed clip seen on hardware (boot 212) vanished after the power cycle. The late
`raw_write` task does return `write_sem` eventually, and nothing then finalises the file.

Follow-up: let the late task rewrite the header and close when it sees `stopping && rvx_wedged`
(it holds `write_sem` at that point). Not needed for the first attempt.

### M2. MEDIUM: the staged `autoexec.bin` carries an unmeasured core constant
`src/fps-engio.c:332`

The 27 MHz base is the only core code change. The layout is identical and the literal is used
in one place, `calc_tg_freq` (literal pool at 0xe05a8), which only computes values: reported
fps, shutter read-outs, and the fps written into the MLV header. No register write depends on
it unless FPS override is used. It is still a core byte change that has not booted, and the
value itself is unmeasured on this body. This is why the verdict asks for module only first.

### M3. MEDIUM: after a failed card write the recording keeps running with no sign to the user
`worker.c:326`, `539-548`

Once `rvx_write_failed` is set (card full is the likely cause), every frame is discarded and
counted, and the only trace is five UART lines. The recording stays ACTIVE until the key is
pressed. Follow-up: request a stop from the worker when the flag is first set.

### L1. LOW: flipping `rvx_checkpoint` during a recording can run a checkpoint on the small stack
`worker.c:75-76`, `413`, `427-430`

The stack size is chosen when the task is created, the checkpoint decision is made after the
write. A task created with 0x800 just before the flip runs `checkpoint_file()` on it. The
comment says flipping while recording is safe. Set the toggle only while not recording.

### L2. LOW: a failing `task_create` also raises Canon's assert, and is retried every frame
`worker.c:444-452`, `567`

Each of the three ROM failure paths calls `debug_assert` (0xE0674574) before returning. Our own
message is limited to five, the retry is not, so a persistent failure would raise an assert per
frame. Only reachable if the task table is exhausted.

### L3. LOW: VIDF timestamps are taken when the worker handles the frame, not at capture
`mlv_3.c:728`

They are relative now, which was the bug. But a worker that waited 50 ms for `write_sem`, or
is behind the queue, stamps frames late. `fps from VIDF timestamps` in `mlv_check` will be
noisy on hardware and should not be read as the sensor rate. `event->creation_time` is there.

### L4. LOW: three worker messages are not rate-limited
`worker.c:529`, `535`, `577`

"frame while not ACTIVE" is bounded by the queue length. The other two repeat per frame, but
only with `rvx_src_from_raw_info = 0` or a buffer smaller than one frame. Worker context, not vsync.

### L5. LOW: size
`worker.c` is 1088 lines and `worker()` about 580, past the 800-line and 50-line guides.

## Review areas with nothing to report

- **Vsync callback:** the branch adds no blocking call and no per-frame log there. A rejected
  queue post now drops and counts instead of creating a task. The old items of finding 9 remain.
- **Worker / writer races:** `write_sem` has exactly one give for every take on all seven
  paths I traced (normal flush, failed create, stop, stop with failed create, both stop
  timeouts, failed checkpoint). The worker never waits without a timeout except on its queue.
- **Exit paths:** a failed start closes the file, releases raw mode and frees the buffers
  (912-928). A normal stop closes, then releases, then frees. The wedged stop releases raw
  mode and deliberately keeps file and buffers. A refused start has acquired nothing.
- **MLV:** header count is `frames_on_disk` (991); frame numbers stay contiguous because the
  VIDF header is only written for an accepted frame; crop is `((x+7)&~7, y&~1)`, which matches
  the copy arithmetic at 739-741; timestamps are relative.
- **Core:** apart from M2, the core diff is one comment in `stubs.S`, a guard move in
  `edmac-d8.c` that compiles the same code (`CONFIG_EDMAC_MEMCPY_D8` is defined in
  `internals.h:51`), and `modules.included`. The byte diff confirms it.
- **Toggle defaults:** conservative, and confirmed in the binary (`rvx_checkpoint` and
  `rvx_redirect_edmac` in `.bss`, the other two in `.data`).

## Limitations

- Nothing ran on the camera. The host harness is single-threaded with no real timeouts or
  priorities, and the 7 new tests were written by the author of the fixes.
- The host MLV writer replays the worker by hand; only `mlv_3.c` is the real source.
- I assumed ML tasks created with `task_create` all run on CPU 0. If the worker and the write
  task ran on different cores, the `volatile` ordering in finding 7 would not be enough.
- I did not check that the core on the physical card is the baseline build.
- I did not review `bench.mo` and `file_man.mo` on the staged card, only their imports.

## Next hardware checks

1. On the mounted card, before copying anything: `check_card_image.py <card root>`.
2. Module only, defaults. Clip under 5 frames, then stop: expect `MLV closed (finalized)` and
   `frames on disk=N`; `mlv_check.py -v` must show N frames, header N, first VIDF near 0 ms,
   `crop=200,100 pan=200,100`.
3. Clip over 10 frames, then a second clip in the same boot. Read `frames accepted`,
   `dropped (queue full)`, `dropped (bufs busy)`.
4. If `could not start card write for full buffer yet, will retry` appears, the recording must
   carry on and the file must still be complete; that is the fix for finding 1 at work.
5. `rd &rvx_wedged` and `rd &rvx_write_failed` must both read 0 after each clip. If
   `rvx_wedged` is 1, check after the power cycle whether the file survived (M1).
6. Only then the staged `autoexec.bin`. Compare boot with step 2, and read 0xD0406198 and
   0xD04061A4 in 1080p25: 25 x (A+1) x (B+1) near 27.0e6 confirms the constant.
7. `rvx_checkpoint = 1` last, set while not recording (L1).
