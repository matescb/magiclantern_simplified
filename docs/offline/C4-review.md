# C4-review: review of branch offline/C4 (raw_vidx module-only fixes)

Reviewed: `/mnt/data/WORK/eos/wt/C4`, branch `offline/C4`, 8 commits on a10b16c23
(81b7ff105 .. f8191a47a). Offline review only: nothing here is proven on the camera.

## Goal

Independently check the eight commits for correctness, races between the vsync callback,
the worker and the write task, blocking in the vsync context, leaks on exit paths, scope
creep, and the claim that the core is unchanged.

## Verdict

- The five stated fixes are correct on their normal paths, and the three extra fixes
  (second recording per boot, per-session frame count, capture buffers) are correct.
- Scope is clean: only `modules/raw_video/raw_vidx/{worker.c,event_pusher.c,raw_vid_worker.h}` changed.
- The core claim is confirmed: with an 18-character branch name the `.sym` is byte-identical
  to the main checkout's.
- Not ready to flash as is. The default-on checkpoint (c55b53e4d) puts a header rewrite,
  close, reopen and seek on the write path every 5 frames, on a card path that hardware
  logs show is already marginal. It also opens a path where recording silently stops
  accepting frames (finding 1). Recommend `rvx_checkpoint = 0` by default, or a checkpoint
  every N seconds, for the first hardware run.
- The timeout paths at stop still close the file and free buffers under a running write
  (finding 3).

## What was done (how to re-run)

| Check | Command | Result |
|---|---|---|
| Scope | `git -C wt/C4 diff --name-only walkback-milestone..HEAD` | 3 files, all in `modules/raw_video/raw_vidx` |
| Build in wt/C4 | `cd wt/C4/platform/M50mk2.101 && make ARM_BINPATH=/usr/bin ARM_ABI=none-eabi` | rc=0, no warnings from raw_vidx sources |
| .sym vs main (branch `offline/C4`) | `diff build/M50mk2_101.sym <main>/build/M50mk2_101.sym` | 53 lines differ, every one by exactly 8 bytes, names identical, starting at `build_version` |
| .sym vs main (18-char branch) | temporary worktree `wt/C4-review`, branch `offline/C4-review2` at f8191a47a, same make | `cmp` identical, md5 d2f46ba5e091fa02d53a8aae9969442f |
| autoexec.bin vs main (18-char branch) | `cmp -l` | same size 257696; 90 bytes differ in 7 short runs (date/hash/branch strings, last 4 bytes) |
| Module imports | `arm-none-eabi-nm -u raw_vidx.mo` against main `.sym` | all 44 undefined symbols present, including `FIO_OpenFile` (0xcf555) |
| Host test | `sh tools/raw_vidx_hosttest/run.sh wt/C4/modules/raw_video/raw_vidx sim/C4-review/hosttest` | `failed tests: 0` (10 tests) |

Notes:
- `raw_vidx.mo` md5 is not reproducible across builds (I got 98b163a8... in wt/C4 and
  8c6a8ca3... in my worktree; the implementer reported 79cf1d98...). The module embeds
  build strings, so the md5 cannot be used as an identity check.
- The temporary worktree and its branch were removed after the comparison. The name
  `offline/C4-review2` (not `offline/C4-review`) was needed to get 18 characters.
- Build logs: `/mnt/data/WORK/eos/sim/C4-review.build.log`, `/mnt/data/WORK/eos/sim/C4-review/build18.log`.

## Findings, by severity

File:line refer to the tip of `offline/C4` (f8191a47a). All findings are from code reading;
none was reproduced in the host test, whose semaphore model has no real timeouts.

### 1. HIGH: after one write_sem timeout the recording drops every frame until stop
`worker.c:516-520` with `worker.c:463-478`

If `take_semaphore(write_sem, FLUSH_WRITE_WAIT_MS)` times out, the full buffer (say A) is
left full with no write task, and the worker carries on in B. When B fills, the swap test
at 463 sees A still non-empty, drops the frame and stays on B. Nothing ever starts a flush
of A, so every later frame takes the same path. Recording stays ACTIVE and counts
`rvx_dropped_busy` until the user stops; stop then writes both buffers.

This is the same symptom as the `crop_buf = NULL` bug that a1f74a6e0 removed, reached
by a different route.

When it can happen: the holder of `write_sem` at that point is the previous write task,
already past `FIO_WriteFile()`. Without checkpoints it is a few instructions from the
give, so the path is practically dead. With `rvx_checkpoint = 1` it is inside
`checkpoint_file()` (seek, header write, close, open, seek to end), so the timeout fires
whenever that takes more than 1 s. The comment at 513-515 ("at worst a few instructions
away") was true for 0530d6c7d and became false in c55b53e4d.

Suggested fix: in the busy branch (463), if the other buffer is full and `write_sem` can
be taken without waiting, start its flush there. Or make the timeout path retry the
flush on the next frame.

### 2. HIGH (behaviour change, default on, never run on hardware): checkpoint cost is on the critical write path
`worker.c:49`, `292-320`, `328-329`

- A buffer holds 5 frames. Hardware logs show `wbytes=0x00D66280` (14,049,920 bytes =
  5 x 2.81 MB), so a checkpoint runs every 5 frames, about 5 times a second at 24 fps.
- The same logs show `FIO_WriteFile` of one buffer taking 0.2 to 0.4 s
  (`uart.jsonl` lines 547092/547094, 549365/549367, 550993/550995). Five frames arrive in
  0.208 s. The card path is already at or over budget, so every millisecond of
  seek + header write + close + open + seek is paid in dropped frames.
- After the first checkpoint the handle is `O_RDWR | O_SYNC` (307), while the first buffer
  goes through the `FIO_CreateFile` handle. Throughput for the rest of the clip may
  differ from everything measured so far.
- `FIO_SeekSkipFile(fp, 0, SEEK_END)` (315) is unchecked. If it does not land at the end
  on this firmware, the next buffer overwrites the header and the start of the clip. A
  file that would have been fine without checkpoints is destroyed. Compare the returned
  position with header_size + bytes written and stop on mismatch.
- The header `FIO_WriteFile` (300) is unchecked.
- The write task has a 0x800 stack. `FIO_OpenFile` puts a 0x80-byte path buffer on it
  (`src/fio-ml.c:394`) plus Canon's open/close call depth, which this stack size has never
  carried on hardware.

Recommendation: default `rvx_checkpoint = 0` for the first run and A/B it by serial, or
checkpoint on a time interval (for example every 2 to 5 s) instead of every buffer.

### 3. MEDIUM: stop timeout still closes the file and frees buffers under a running write
`worker.c:766`, `779-783`, `786-804`, `829-830`

When `have_write_sem` is 0 (10 s timeout at step 1, or in `flush_buf_and_wait`), the worker
still calls `FIO_CloseFile(raw_vid_fp)` and then `free_crop_bufs()`. A write task may be
inside `FIO_WriteFile()` reading that buffer. This is the bug 0530d6c7d set out to fix,
still present on the timeout path. Further consequences:
- If the task is inside `checkpoint_file()`, it assigns `raw_vid_fp = fp` (316) after the
  worker set it to NULL (804). The handle leaks, and every later COMMAND_START fails with
  "already in use" (687-690) until reboot.
- When the late task returns, `write_buf` resets `crop_buf->cur/frames` (285-286) in the
  worker's `crop_bufs[]`, which a new session may already be using.

Suggested fix: on timeout, do not close and do not free; leak deliberately, log it, and
refuse new starts.

### 4. MEDIUM: task_create result is ignored while write_sem is held
`worker.c:343`, `worker.c:523`

If the task cannot be created, `write_sem` is never given back. In the worker this leads
straight to finding 1 (every later flush times out after 1 s, with the worker blocked for
that second). At stop it costs a 10 s wait and then finding 3.

### 5. MEDIUM: a short card write leaves a truncated block in the middle of the file
`worker.c:256-268`

On a short write the frames are not counted, which is right, but the partial bytes are
in the file and later buffers are appended after them. The header count is then no larger
than the truth, but a reader walking blocks hits a truncated VIDF followed by misaligned
data. After the first short write the session should stop writing (set a flag, or treat
it like the `raw_vid_fp == NULL` state).

### 6. LOW: frame accepted and counted when edmac_sem cannot be taken
`worker.c:490-507`

`cur`, `frames` and `video_frame_count` are advanced before `take_semaphore_now(edmac_sem)`.
On failure the `break` leaves a VIDF header with uncopied data counted in `frames`, and
discards `flush_src` (which leads to finding 1). Unreachable with the current synchronous
copy, since the worker gives the semaphore itself at 582. Older code, but the new
`frames` accounting inherits it.

### 7. LOW: unsynchronised reset of `frames` and `cur` between write task and worker
`worker.c:285-286` against `worker.c:463`, `491`

The worker uses `cur == start` as "buffer free" without a lock. It is correct only if
`frames = 0` is stored before `cur = start`. That is the source order, but the fields are
not volatile, so the compiler may reorder. If it did, the higher-priority worker could
run between the stores, add a frame, and have `frames` zeroed afterwards. The result
would be an under-count in the header, never an over-count. Reset both before giving up
the buffer under a barrier, or make `cur` volatile.

### 8. LOW: stale comments and never-reset log limiters
- `event_pusher.c:70`: "Only the worker receives or releases events" is no longer true;
  the producer releases at 522 and 533. That is safe, since it only releases events it
  alone holds.
- `worker.c:800-802`: says the header keeps count 0; with checkpoints it keeps the last
  checkpoint count.
- `worker.c:318`, `406`: `ckpt_log_n` and `fpnull_n` are per boot, not per session.

### 9. Observations on older code (no change requested in this branch)
- `event_pusher.c:107`: `take_semaphore(events_sem, 2000)` can block in the vsync callback.
- `event_pusher.c:356-365`, `397-427`: `fio_malloc`/`fio_free` run in the vsync callback.
  f8191a47a adds only two NULL checks per idle vsync, and the free still happens once.
- The two capture buffers (2 x about 4.3 MB) are allocated every session but unused with
  the defaults (`rvx_src_from_raw_info=1`, `rvx_redirect_edmac=0`).
- With `rvx_src_from_raw_info=0`, queued events still point at capture buffers that the
  callback frees as soon as `prevent_recording` is set.
- Logging in the vsync callback: the new messages are limited to every 64th drop.

## Per-commit assessment

| Commit | Claim | Assessment |
|---|---|---|
| 81b7ff105 | release unposted first-frame event | Correct. |
| a1f74a6e0 | drop and count instead of stop/stall | Correct for the three stated cases. The failed post now releases its event. The busy branch is right on its own; see finding 1 for the remaining stuck path. Intended behaviour change: a failed post no longer stops the recording. |
| 6a74a067a | comment only | Fine. |
| ba29d4865 | cleanup on failed start | Correct. Undoes exactly what was done, in a sensible order. `FIO_CreateFile` does return NULL on failure (`src/fio-ml.c:597-646`). The short-header check relies on `FIO_WriteFile` returning the byte count, which docs/32 line 1113-1114 confirms on hardware. |
| 0530d6c7d | flush at stop, wait for write | Correct on the normal path: order older then newer is right, the semaphore handoff is sound, the header count comes from `frames_on_disk`. Timeout paths: findings 3 and 4. |
| 01bc47079 | re-allow capture after stop | Correct. `allow_recording()` always runs after the stop was posted, so a late second stop cannot leave `prevent_recording` stuck. `session_reset_pending` is only set while not ACTIVE, so it does not race the callback. |
| c55b53e4d | checkpoint feature | Logic under `write_sem` is sound, and the reopen-failure path leaves a valid file. Risk and cost: findings 1 and 2. This is a new feature, not a fix, and is on by default. |
| f8191a47a | free capture buffers when idle | Correct. |

## Limitations

- Findings 1 and 3 to 7 come from reading the code; no test was written for them.
- The host test passes, but its semaphores cannot time out and its scheduler is
  single-threaded, so it cannot reach the paths in findings 1, 3 and 4.
- Checkpoint cost on the real card is unknown; finding 2 rests on existing hardware
  write timings only.
- I did not disassemble `write_buf` to check the store order in finding 7.

## Next hardware checks

1. First run with `rvx_checkpoint = 0`: confirm a second recording in one boot gets frames
   and the header count equals the frames in the file.
2. Then `rvx_checkpoint = 1`: time one `checkpoint_file()` call (UART timestamps around
   it) and compare `rvx_dropped_busy` / `rvx_dropped_queue` with the run without it.
3. After a checkpoint, check that the next buffer lands at the end of the file (file
   size grows by `wbytes`, header still at offset 0), to verify `SEEK_END` on this firmware.
4. Pull power mid-recording with checkpoints on: is the file present, and does it decode
   up to the last checkpoint count?
5. Watch for a stack overflow or assert in the `raw_write` task on the first checkpoint.
6. If "previous card write still running, buffer not flushed" ever appears on UART,
   finding 1 has been hit; expect `rvx_dropped_busy` to climb until stop.
