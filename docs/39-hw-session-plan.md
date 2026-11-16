# 39 - Hardware session plan: from the booting build to a validated raw clip

Written 2026-11-16 at the end of an offline sprint. **No camera was attached while this plan was
written. Every expected value, every pass criterion and every branch named here is an offline
prediction or an offline build; none of it has run on the camera.** Statements marked
*established on hardware* come from the UART logs of 2026-07-14 to 07-22
(`tools/mlserial/run/uart.jsonl`), as mined in `docs/offline/C7.md`. Everything else is static
analysis of the ROM dump (firmware 1.0.1, `m50ii_dumper/dump_1.0.1/gang100.bin`) or host testing.

This is the document to follow the next time the Canon EOS M50 Mark II is on the bench. It is
meant to stand alone; the reports it cites are in `docs/offline/` (index: `docs/offline/INDEX.md`).
All paths are relative to `/mnt/data/WORK/eos` unless they start with `/`.

Terms used throughout:

- **Core**: `autoexec.bin`, the Magic Lantern (ML) image the bootloader loads from the card.
  **Module**: a `.mo` file the core links at load time against the symbol file
  `ML/modules/M50mk2_101.sym` on the card.
- **Baseline**: branch `walkback-milestone`, commit `a10b16c23`, built in the main checkout
  `magiclantern_simplified/`; `autoexec.bin` md5 `1df03e45661c501ea986c9823fca7d6b`, 257696 bytes;
  `.sym` md5 `d2f46ba5e091fa02d53a8aae9969442f`.
- **big.8**: the UART marker `[ML] big.8`, printed when ML's init task has finished. "A good boot"
  means this marker appeared. It does not prove that modules loaded (docs/43 section 1).
- **Assert**: a line `[ML] !!! ASSERT (non-fatal): ... @ <file> L:<hex line>`. **Abort**: a line
  `[ABORT] ... pc=... lr=... DFAR=...`, after which the camera hangs (docs/42, "Terms").
- **raw_vidx**: the raw recorder module, `modules/raw_video/raw_vidx` (docs/41).
- **Toggle**: an `int` global of the module that changes its behaviour at run time.

Sessions are lettered A to D and must be done in that order. Steps inside a session are named
SA1, SB3, SC7, SD2 and so on, so that they cannot be confused with the sprint reports A1, B3, C7.
A later session never starts before the stop conditions of the earlier one are cleared.

---

## 0. Preconditions and safety

### 0.1 What is known to boot, and how well

- *Established on hardware*: the build key 257696 bytes (the size of the baseline core) reached
  big.8 in 9 of 29 logged boots, and 7 of those failures were the `pc=0xE021B8FC` streak of
  2026-07-21 (docs/43 section 1). The last logged boots (db boots 362-366) ran a different core of
  257760 bytes. Whether the baseline source is "the build that boots" cannot be decided from the
  log (docs/43 section 5.4; `docs/offline/D7-critic.md` section 3 item 3). The first task of
  session A is therefore to measure it.
- *Established on hardware*: the baseline lacks the empty MMU patch table, so about 1 boot in 5
  dies in the `big.4a` phase with `pc=0xE02F281A`, `pc=0xE00436FC` or `pc=0xE05D1ED0`
  (docs/43 section 2). **One failed boot says nothing about what was just changed.** Repeat.
- Nobody verified what is on the physical card (`docs/offline/C10-review.md`, "Limitations").

### 0.2 Back up before anything is touched

1. With the card in the reader, before copying anything: make a full image and a file copy.
   ```sh
   sudo dd if=/dev/sdX of=/mnt/data/WORK/eos/backup/card-$(date +%Y%m%d-%H%M).img bs=4M status=progress
   mkdir -p /mnt/data/WORK/eos/backup/card-files && cp -a /media/$USER/EOS_DEVELOP/. /mnt/data/WORK/eos/backup/card-files/
   md5sum /media/$USER/EOS_DEVELOP/autoexec.bin /media/$USER/EOS_DEVELOP/ML/modules/*.mo /media/$USER/EOS_DEVELOP/ML/modules/*.sym
   ```
   (`/dev/sdX` and the mount point are placeholders; check with `lsblk`.) Write the md5 values
   into the session notes. If the `autoexec.bin` md5 is not `1df03e45...`, the card does not hold
   the baseline; stop and decide which core to treat as baseline before going on.
2. Copy `tools/mlserial/run/uart.jsonl` to a dated backup. The log is the only record of earlier
   sessions.
3. Do not update the camera firmware. 1.0.1 is the only version this work applies to.

### 0.3 Rules for every build that goes on a card

1. **Commit before flash** (docs/43 section 7). Never copy a binary built from an uncommitted
   tree. Note commit, branch, `autoexec.bin` md5 and `.mo` md5 in the session notes together with
   the first boot number that used them.
2. **Branch names of core builds have exactly 18 characters** unless the build carries the C11
   change. `platform/Makefile:610` embeds `"<hash> <branch>"`; any other length moves 53 symbols
   (docs/43 section 6.4).
3. **Make order.** Run `make` in `modules/` first, then in `platform/M50mk2.101`. The platform
   `make` alone packs stale `.mo` files (`docs/offline/C2.md`, C4).
   ```sh
   cd <worktree>/modules && make ARM_BINPATH=/usr/bin ARM_ABI=none-eabi
   cd <worktree>/platform/M50mk2.101 && make ARM_BINPATH=/usr/bin ARM_ABI=none-eabi
   ```
   Take a module from `<worktree>/modules/raw_video/raw_vidx/build/raw_vidx.mo`, or check that the
   copy in `build/zip/ML/modules/` has the same md5.
4. **Symbol gate before every flash** (`tools/symgate/README.md`):
   ```sh
   G=/mnt/data/WORK/eos/tools/symgate
   B=<worktree>/platform/M50mk2.101/build
   python3 $G/check_symbol_stability.py $B/M50mk2_101.sym --elf $B/magiclantern
   python3 $G/check_module_syms.py $B/zip/ML/modules/*.mo $G/baseline/M50mk2_101.sym
   python3 $G/check_card_image.py $B/zip --elf $B/magiclantern
   ```
   Exit codes: 0 pass, 1 gate failed, 2 bad input. For a module-only deploy the second command is
   the one that matters: the module must resolve against the `.sym` **that is on the card**.
5. **Card check on the mounted card, after copying and before unmounting:**
   ```sh
   python3 $G/check_card_image.py /media/$USER/EOS_DEVELOP
   sync
   ```
   It checks that `autoexec.bin`, the `.sym` and the modules on the card belong together.
6. Core, `.sym` and all `.mo` files of a core build are always copied together. A module-only
   step copies exactly one `.mo` and nothing else.
7. The WiFi deploy module does not load against the baseline core (9 unresolved imports,
   docs/43 section 6.5). Use the card reader.

### 0.4 Host tools and command forms (read from the sources, see `docs/offline/G3.md`)

```sh
cd /mnt/data/WORK/eos/tools/mlserial        # run mlserial.py from here (TOOLS.md note)
python3 mlserial.py daemon start            # once per session; logs to run/uart.jsonl
python3 mlserial.py cmd "ver"               # liveness
python3 mlserial.py cmd "rd 0x39B84"        # one word
python3 mlserial.py cmd "md 0x10278 12"     # 12 words (count is in words, max 0x100)
python3 mlserial.py cmd "wr 0x<addr> <val>" # RAM only
python3 mlserial.py cmd "call 0x<addr> a b" # prints ret
python3 mlserial.py log --grep "big.8" --n 5
```

Three traps, all read from `magiclantern_simplified/src/serial_shell.c` (not tried on the camera):

- `rd` reads **one** word and ignores a second argument. Where a report writes `rd <addr> <len>`,
  use `md <addr> <words>`.
- Numbers must be decimal or `0x` hex (`sh_parse_num`, `serial_shell.c:188`). Forms with names,
  such as `call lv_save_raw 1` (TOOLS.md) or `wr &rvx_sidecar 1` (C12, G1), are **not accepted**
  by the shell as written.
- A `call` whose callee blocks wedges the shell (*established on hardware*, WiFi work, docs/37).
  Never `call` an allocator, a file function or anything that waits.

**Core symbols by name.** Define once per shell, with `SYM` pointing at the `.sym` of the core
that is running:

```sh
SYM=/mnt/data/WORK/eos/magiclantern_simplified/platform/M50mk2.101/build/M50mk2_101.sym
A() { awk -v n="$1" '$2==n{print "0x"$1}' "$SYM"; }     # A handle_module_keys -> 0x000cd515
```

Thumb functions are listed with bit 0 set already (for example `000cd515 handle_module_keys`).

**Module toggles: an open tooling gap that must be closed before session C.** The module is
linked on the camera at a different address each boot, its globals are not in the core `.sym`,
and the core frees the linker state after loading (`CONFIG_TCC_UNLOAD`, `src/module.c:22`), so
`module_get_symbol` cannot be used either. No tool prepared in the sprint resolves `rvx_*`
addresses. Before session C, do one of the following (offline, module only):

1. Preferred: on a new 18-character branch from `offline-plan-step2`, add one UART line per
   toggle to the module's `init()` (`raw_vid.c:10`), for example `[RVX] sym rvx_sidecar=0x........`,
   rebuild, run `check_module_syms.py`, commit. A small host script can then turn the log lines of
   the current boot into a table.
2. Or extend `tools/mlserial/mlresolve.py`, which already walks `module_list` for `mlv_lite`
   (*established on hardware* for that module only).

In the commands below, `T <name>` stands for "the address of module global `<name>` in this boot"
obtained by one of these two means. Session B needs no toggle write, so it is not blocked.

**Starting and stopping a recording over serial.** *Established on hardware* (uart.jsonl,
commands `@759`-`@762`): stage a key event in the shell's scratch buffer, then pass it to
`handle_module_keys`:

```sh
python3 mlserial.py cmd "load 00000000200000000000000000000000"   # reply: loaded 0x10 bytes @0x<scratch>
python3 mlserial.py cmd "call $(A handle_module_keys) 0x<scratch>"
```

Use the scratch address the `load` reply prints (it was 0x0014EEA8 in the July layout). The same
two commands stop the recording. Camera state: movie mode, LiveView standby.

### 0.5 Recovering a wedged camera

*Established on hardware* (docs/43 section 3, observations of 2026-07-19 to 07-21):

- After an abort the camera hangs; ML's handler prints and never returns.
- A hung camera can fall to 0 W and then does not restart from the switched power plug. Turn the
  camera's own switch off, remove the power (battery or dummy battery) for **at least 90 s** (30 s
  was not enough), then power on.
- Keep a normal, non-bootable card at hand. If the camera does not start with the ML card, start
  it once with the normal card to prove the body is fine.

### 0.6 Stop conditions that apply to every session

Stop the session, change nothing on the card, and save the log when any of these occurs:

1. `pc=0xE021B8FC lr=0xE04037A1` at boot. **Image the card before any other action**
   (docs/43 section 5.4; `docs/offline/F5.md` section 7). This crash is not understood, it hit two
   core layouts identically, and the card image is the only way to test the card-state explanation.
2. Three consecutive boots without big.8 on a card that booted earlier in the same session.
3. The camera does not start with the normal card either.
4. A non-stock pointer is left in Canon memory after a stop (session C, sync sources): do not
   record again before a power cycle.
5. Any sign that a file other than the test clip was damaged on the card.

---

## 1. Session A - no new code: boot baseline and read-only probes

Card: untouched. Core: whatever boots today (expected: the baseline). Purpose: measure the boot
rate, and replace static predictions by measured values before any recording.

### SA1. Boot-reliability baseline (10 cold boots)

For each boot: power on, wait 60 s, then

```sh
python3 mlserial.py log --grep "big.8" --n 3
python3 mlserial.py log --grep "ABORT" --n 3
python3 mlserial.py cmd "ver"
```

Record per boot: big.8 yes/no, seconds from `pre.0` to `big.8`, abort signature if any.
Predicted from the log: 3 to 8 of 10 good, about 39 s to big.8.

| Result | Meaning | Next |
|---|---|---|
| 5 or more of 10 reach big.8 | Usable baseline; this number is the reference for session D | SA2 |
| Fewer than 5, failures are `0xE02F281A` / `0xE00436FC` / `0xE05D1ED0` at `big.4a` | The known MMU-patch family (docs/43 section 2) | Continue, but plan the empty patch table (section 4, item SD0b) as the first core change |
| `0xE021B8FC` | Stop condition 1 | Image card, section 5 question 2 |
| `0xE005DE52` at `big.5.3` | Known, about 1 boot in 20 (docs/43 section 4) | Power-cycle, continue |

### SA2. Probe pack, photo-mode LiveView, raw never enabled since boot

`tools/hwprobe` (`docs/offline/G3.md`) runs the read-only checks of reports A1-A15, F1, F4, F5.
It has only been tested against canned replies.

```sh
cd /mnt/data/WORK/eos/tools/hwprobe
python3 hwprobe.py plan --sym $SYM --stages 0,1,2,3          # read what would be sent
python3 hwprobe.py run  --sym $SYM --dry-run --stages 0,1    # exact command list, no camera
python3 hwprobe.py run  --sym $SYM --stages 0,1 --label photo-lv
```

Default flags send plain RAM reads only. Results go to `tools/hwprobe/results/`.

Stop if `S0.ver` gets no answer, or `S0.sym` fails (`ml_started` is not 1 at the address from the
`.sym`): the `.sym` does not belong to the running core, and every later address is suspect.

What the stage 0 and 1 probes answer:

| Probe | Question | Prediction (offline) | If refuted |
|---|---|---|---|
| A2.1 | Is `*(0x8E8C)` the EvfState object (name 0xE137558C, dispatcher 0xE05BB2B3, matrix 0xE0B5050C, 17 inputs, 8 states, state 5)? | yes | Sync source 1 (step SC10) and F1 are off the table |
| A2.2, F1.2 | Is `*(0x10108)` EvfDevState in state 3 during LiveView? | yes | The hook window in `src/state-object.c:225` must be revisited |
| A2.5 | EvfDev priority 0x0E, EvfCap 0x0D? | yes | Redo the priority plan (A11 is single-source, D7 section 4 item 4) |
| A1.1 | Mem1Path context 0x10238: flag +0x40 = 0, +0x4C = 1, +0x50 = 1? | yes | +0x50 = 0: Mem1Path inactive in this mode |
| A1.7 | Slot 0x1029C holds 0xE06AD51B? | yes while LiveView runs | Sync source 2 will refuse |
| F4.1, F4.2 | Event-container pool: head 0x39B84 inside 0xBEE18..0xC4E17, free count near 1536 at idle? | yes | Below 1400 at idle: a receiver is already stalled; watchdog threshold must be reset |
| F4.6, A3.3 | Stack floor 0xDF000000 still filled with 0xEEEEEEEE, top word [0x1014] = 0xDF001000? | yes | Floor overwritten: the stack overflowed once this boot |
| F4.4 | DebugMsg level bytes 05 06 at `*(0x5154)+0x38`, gate 0x5158 non-zero? | yes | Changes the recursion model of A3 |
| F5.tokens | Any property-token field equal to 0x32? | no | Names the module behind `0xE021B8FC` |
| A15.propmgr, A8.1, A8.2, A14c.9 | PropertyMgr, RscMgr, pool handle exist; assert slot 0x4000 points into ML | yes | Redo the corresponding report |
| A1.4, A1.6 | Ring counts `[0x23804]`, `[0x292B0]` below 2; one of six LvJob slots free | yes at idle | Count 2 at idle: recording will storm; do not start session B |
| A1.3 | RawPro channels at 0x292B0 +0x30 / +0x34 are not 3 | 0x29 / 0x4C expected | Either reads 3: stop, re-plan (C9-recommended, "Next hardware checks" 1) |

Additional reads by hand in this state (F7 "Next hardware checks", D7 section 4):

```sh
python3 mlserial.py cmd "rd 0x4108"        # task_max: expect about 0xC7 (F7 check 1); 0x40CC read 1 in July
python3 mlserial.py cmd "md 0xDF002C30 20" # RPC entries: bytes 70 b5 0d 46 at 0xDF002C4A (F7 check 2)
python3 mlserial.py cmd "rd 0xE9F0"        # LiveView dialog slot: non-zero in LiveView (F7 check 3)
python3 mlserial.py cmd "rd 0x15998"       # press INFO between reads: steps 0..3 (F7 check 4)
```

These decide which commits of `offline-stub-fixes` may be flashed in session D.

### SA3. Probe pack, movie mode 1080p25 LiveView

```sh
python3 hwprobe.py run --sym $SYM --stages 0,1 --label movie-1080p25 --allow-risky-read
```

`--allow-risky-read` adds MMIO and page-table reads. Use it only with LiveView running; A5 warns
that EDMAC register reads can lock the camera while the sub-chip sleeps. If the shell stops
answering after this run, power-cycle and repeat without the flag.

| Probe | Question | Prediction | Decision |
|---|---|---|---|
| A9.1 | Timer A 0xD0406198 and B 0xD04061A4: is 25 x (A+1) x (B+1) near 27.0e6? | yes (A9, C4c) | Confirms the 27 MHz constant before the C10 core (step SD1). Near 32.0e6: revert that commit |
| A9.1 | 0xD0420250 = 0x048B0E54 (bytes per line, lines - 1)? | yes in 1080p | Tells the bit depth of the stream |
| A13.1 | L1 entries: 0x6BC and 0x463 non-cacheable, 0x2BC cacheable? | yes | Needed before `rvx_fast_copy` (step SC9) |
| A5.1 | Channel 13 mux 0xD042327C = 0? | yes | Non-zero: the F2 hand test (step SD5) would assert |
| A9.2, A14c.gui | Drive mode, GUI mode words | recorded only | Run once per video mode with a new `--label` |

Optional, gated by `--allow-call`: probe A2.4 calls 0xE05BA079 (queue depth of EvfDev and EvfCap).
The call is predicted to return at once; it has never been made. Leave it out of the first run.

### SA4. Probe pack with raw enabled, no recording

`hwprobe.py run --sym $SYM --stages 2 --label movie-1080p25-raw` asks the operator to enable raw
and press Enter. Named calls do not work (0.4), and whether `raw_lv_request` returns promptly from
the shell task is open (G3). The cheapest proven way to have raw enabled is a recording, so run
stage 2 at the start of session B (step SB3), during the first clip, instead of forcing it here.

**Exit of session A:** boot rate known; `S0.sym`, A1.3, A1.4 and F4.1 as predicted. If A1.3 or
A1.4 is refuted, do not go to session B.

---

## 2. Session B - module only: the reviewed raw_vidx on the old core

What changes: exactly one file. The module from branch `offline-integrate1` (worktree `wt/C10`,
tip `9633ac828`), staged as `sim/C10/card/ML/modules/raw_vidx.mo`, md5
`0d80a5c15e9b2b7fbf881dfa3c925640`. `docs/offline/C10-review.md` reviewed it: "safe to flash as a
first attempt", module only, all toggles at their defaults (`rvx_checkpoint=0`,
`rvx_wait_raw_job=1`, `rvx_src_from_raw_info=1`, `rvx_redirect_edmac=0`). **Do not copy
`sim/C10/card/autoexec.bin`**; that core carries an unbooted constant change (session D, step SD1).

Known weakness, stated so that a failure is read correctly: this module still blocks inside the
per-frame hook (`take_semaphore(events_sem, 2000)` at `event_pusher.c:117`, `fio_malloc` at
`:420`, `:430`). The analysis in docs/42 section 2.3 names exactly this as a way to stall Canon's
EvfDev task. The flood and the abort that ended the July attempts can therefore recur
(`docs/offline/D7-critic.md` section 1 (ii)). Session B measures whether they do.

### SB1. Deploy

```sh
C=/media/$USER/EOS_DEVELOP
python3 $G/check_card_image.py $C                         # the card as it is must pass first
python3 $G/check_module_syms.py /mnt/data/WORK/eos/sim/C10/card/ML/modules/raw_vidx.mo $C/ML/modules/M50mk2_101.sym
cp /mnt/data/WORK/eos/sim/C10/card/ML/modules/raw_vidx.mo $C/ML/modules/raw_vidx.mo
mv $C/ML/modules/mlv_lite.mo $C/ML/modules/mlv_lite.hid 2>/dev/null   # mlv_lite must not load (C8)
ls $C/ML/SETTINGS/                                        # raw_vidx.en must exist for the module to load
md5sum $C/ML/modules/raw_vidx.mo && python3 $G/check_card_image.py $C && sync
```

Stop if either check fails: the `.sym` on the card is not the baseline, so the module would be
linked to wrong addresses.

### SB2. Boot and load check

Cold boot to big.8, movie mode 1080p25, LiveView standby. Look for the module's own lines
(`python3 mlserial.py log --grep RVX --n 20`). No `[RVX]` line and no reaction to the start
command means the module did not load: check `raw_vidx.en`, repeat `check_card_image.py`.

Note from the July log (*established on hardware*): with the module loaded at boot, its worker
faulted in some boots when started early (docs/43 section 3). If boots fail more often than in SA1,
compare over at least 10 boots before blaming the module.

### SB3. First clip: under 5 frames

Start with the two commands of 0.4, stop about 1 s later with the same two commands. While it
records, run `python3 hwprobe.py run --sym $SYM --stages 2 --label rec-sb3 --yes` in a second
terminal if time allows (answers A1.2: does `*(0x10290)` equal `raw_info.buffer`, is channel 3
armed; and A13.2: do the cached and uncached views differ).

Expected UART lines (C10-review "Next hardware checks" 2; the exact wording is in
`wt/C10/modules/raw_video/raw_vidx/worker.c`): `MLV closed (finalized)`, `frames on disk=N`,
`Recording is inactive`, and the counters `frames accepted`, `dropped (queue full)`,
`dropped (bufs busy)`. Then `python3 mlserial.py cmd "ver"` must answer 30 s later.

Card to the reader, then:

```sh
python3 /mnt/data/WORK/eos/tools/mlvcheck/mlv_check.py -v --stats $C/TST00.MLV
python3 /mnt/data/WORK/eos/tools/mlvcheck/mlv_check.py --extract 0 /tmp/f0.raw $C/TST00.MLV
```

Pass: exit 0, N complete VIDF blocks, header declares N, first VIDF timestamp near 0 ms,
`crop=200,100 pan=200,100`, payload 2809856 bytes per frame. Copy the clip to
`tools/mlserial/captures/` before the card goes back.

Known and accepted in this step: the header fps reads about 18.5 % high on the old core
(`docs/offline/C12.md`, section (b)); VIDF timestamps are taken late and must not be read as the
sensor rate (C10-review L3); frames are expected to be torn, because one copy at about 5.7 MB/s
spans about 11 sensor frames (*established on hardware* for the rate, A13; the tearing is
arithmetic, never inspected).

### SB4. Second step: more than 10 frames, then a second clip in the same boot

Same procedure, about 5 s, then again without reboot. Read the three counters. If the line
`could not start card write for full buffer yet, will retry` appears, the recording must continue
and the file must be complete; that is a fixed review finding at work, not a fault.

### SB5. Failure signatures and next actions

Signatures are those catalogued in `docs/offline/C7.md` and explained in docs/42 and docs/43.

| What is seen | What it means | Next action |
|---|---|---|
| Boot abort `0xE02F281A`, `0xE00436FC`, `0xE05D1ED0` at `big.4a` | Known boot race of the baseline core, independent of the module (*established on hardware*) | Power-cycle (0.5), retry. Count it in the boot statistics |
| Boot abort `0xE005DE52` at `big.5.3` | ML passes NULL for `%s`, about 1 in 20 boots | Power-cycle, retry |
| Boot abort `0xE021B8FC` | Not understood | Stop condition 1: image the card |
| Asserts `MvRawProDs.c L:125`, `RawPro.c L:1C4`, `LvJob.c L:D5` at record start, no frame copied, log goes silent | The assert storm: Canon's 2-slot rings overflow because its EDMAC completions stopped (docs/42 section 1). No tested configuration is free of it | If the shell answers: `md 0x292B0 28`, `md 0x23800 16`, `rd 0x29314`, `rd 0x23834` at once (the deciding probe of docs/42 section 1.5: accumulator zero with count 2 means no completions arrived; non-zero and different means a mask mismatch). Power-cycle. Retry up to 3 times. Storm in 2 of 3: do not go to session C continuous modes; go to step SC3 (burst) only after reading A1.4 at idle with raw enabled |
| Asserts `EvfDevState.c L:174` and `L:21E` in bursts of 32, then within 1 s abort `pc=0 lr=0xE05D1161`, or `pc=0xE005DFA2`, or `pc=0xE05B9FDA` DFAR 0xC | Event starvation: EvfDev queue full, then the shared pool of 1536 containers empty, then a stack overrun in Canon's debug formatter (docs/42 sections 2 and 3). In July this followed every attempt that copied frames | This is the predicted failure of the unchanged hook. Check after the power cycle whether the file survived and run `mlv_check.py --crash-ok`. Then go to session C: pool watchdog measure mode (SC2), then burst with throttle (SC3-SC5). Do not repeat SB4 more than twice |
| One assert `System::Task.c L:E5`, then an abort | Same family, one stage earlier | As the row above |
| UART line that the stop timed out; a later start is refused | A card write did not return within 10 s; file left open (C10-review M1) | Power-cycle. Check whether the file exists. A lost clip here is the known cost; note the card and the size written |
| Short-write message, frames discarded, recording still ACTIVE | Card full or write error (C10-review M3) | Stop by hand, free space, retry |
| `mlv_check.py` reports `FRAME_COUNT_UNFINALISED` | The file was never closed | Look for the abort or the stop timeout that explains it |
| `mlv_check.py` clean, but frame content is noise or shifted | Geometry or buffer wrong | Compare with `tools/mlserial/captures/first_raw_capture.raw`; read A1.2 (buffer address) |
| Camera at 0 W, no UART | Wedge after a hang | Recovery of 0.5 |

**Exit of session B:** a clip of at least 3 frames that `mlv_check.py` accepts, and `ver` answers
after the stop. If the only failures are the starvation family, session C may still start,
because its first steps are the countermeasures. If the storm dominates, see section 5 question 4
before continuing.

---

## 3. Session C - recording-plan steps, one toggle at a time

Module: `sim/G1/card/ML/modules/raw_vidx.mo` (branch `offline-plan-step2`, worktree `wt/G1`,
commit `a015861a2`, md5 `631266dec03d817ae8644949d6cb585f`), or the rebuild with the address lines
of 0.4. It contains C10, C12 and G1. **Neither C12 nor G1 was reviewed by a second agent**
(D7 section 5.2). Deploy as in SB1: module only, both gates, never the `autoexec.bin` of that
worktree. Toggles are written while not recording and read at the next start
(`docs/offline/C12.md`, `docs/offline/G1.md`).

The order follows `docs/offline/C9-recommended.md` ("Recommended plan" and "Order rules"). Each
step changes one thing against the previous step. After every step: `ver` must answer 30 s after
the stop, `rd 0x39B84` must be non-zero, and the clip goes through `mlv_check.py`. Any abort ends
the step; power-cycle, repeat once, then apply the row's "fail" column.

Not available as code and therefore missing from this session: the bench task of C9 step 1 (copy
variants, 16 MiB write timing, 32/40/48 MiB allocation hold) and the hook without semaphore and
allocation (C9 step 2, patch P1). The ring stays at 2 x 16 MiB.

| Step | Setting (all other toggles at default) | Expected telemetry | Pass | Fail means | Unlocks |
|---|---|---|---|---|---|
| SC0 Defaults | none. Three behaviours differ from session B: `rvx_integrity=1`, `rvx_pool_watch=1`, no capture buffers allocated in the hook (C12, G1 "what changed") | Stop summary with vsync count, torn count, slowest copy, `fps x1000 measured` near 25000 (0x61A8); `pool: free event containers at start` a few hundred or more, near 0x600 predicted; `rvx_pool_armed` 1 | Same result as SB3 plus the summary; `rd $(T rvx_copy_task_fail)` = 0 | "watchdog NOT armed": start level below 512; decide with `rd 0x39B84` and `rd <that value>` whether the address is right (G1 test 1) | All later steps |
| SC1 Integrity sidecar | `wr $(T rvx_sidecar) 1`; 3 s, moving subject | File `TST00.RVI` beside the clip; "frames torn" close to "frames accepted" | `python3 tools/mlvcheck/rvi_check.py TST00.RVI --mlv TST00.MLV -v` runs and reports per-frame copy time; many torn frames are the expected measurement | No `.RVI`, or the stop wedges: set the toggle back to 0 and use the UART summary only | First measured copy time, tear rate and real fps |
| SC2 Pool level | `wr $(T rvx_pool_watch) 2`, `wr $(T rvx_pool_period) 1`; record until stopped by hand (10 s) or until failure | `pool: free minimum`; `rd $(T rvx_pool_free_last)` during the run | A number. Predicted by F4: the level falls steadily before an abort of the `lr=0xE05D1161` family | A flat level up to such an abort refutes F4's model; the watchdog is then useless and section 5 question 3 is open again | Threshold for SC13; decides whether starvation is the cause |
| SC3 Burst to RAM | `wr $(T rvx_burst_frames) 10` (default window 1792x896); press start once, wait for `Recording is inactive` | No card write during capture; stops by itself | 10 frames, header 10, `mlv_check.py` clean; no flood, no abort | Abort during capture: the copy itself starves Canon, go to SC4 before anything else. Abort during the write: compare with SC6 | First clip candidate with no card I/O while capturing (C9 step 2) |
| SC4 Throttled copy | SC3 plus `wr $(T rvx_copy_chunk_kb) 256` (pause 10 ms per 256 KiB) | Longer copy time per frame, lower frame rate (about 10 fps ceiling at 1280x720) | As SC3; pool minimum (SC2 method) higher than without throttle | No difference in pool minimum: CPU time in the copy is not what drains the pool | Whether throttling is worth its frame-rate cost |
| SC5 Priorities | Do first: read the live priorities (A2.5 from session A). Then 10 recordings each of (1) defaults; (2) `wr $(T rvx_copy_prio) 0x15`, `wr $(T rvx_write_prio) 0x16`; (3) `rvx_copy_prio 0x1B`, `rvx_write_prio 0x1A`, `rvx_copy_chunk_kb 256` | "slowest frame copy, us" per set; count of storm, EvfDevState and `Task.c` asserts per set | Set (2) or (3) shows fewer asserts than (1) and `rvx_copy_task_fail` = 0 | `rvx_copy_task_fail` above 0: 25 task creations per second do not work on this body (never run, C12); keep the copy in the worker | Copy and writer priorities for all later steps |
| SC6 Release order | Burst as SC3 with `wr $(T rvx_burst_release_first) 1`, against 0 | EvfDevState burst after the write present or absent | Recorded only | With 1, a fault in `raw_lv_release()` loses the whole burst (C12); go back to 0 | Stop order for burst mode |
| SC7 Aligned crop | `wr $(T rvx_crop_w) 1280`, `wr $(T rvx_crop_h) 720`, `wr $(T rvx_crop_x) 192`, burst 20 | 20 frames; copy time per byte clearly lower than with x = 200 (A13 predicts 3x or more from alignment alone, unmeasured) | 20 frames, `crop=192,100`, `mlv_check.py` clean | No speed-up: the byte-loop explanation of A13 is wrong for this body; fast copy becomes the only route | The crop for all later steps |
| SC8 Frozen buffer | SC7 with Canon's raw write stopped during the copy. Stage a zero word and call the `lv_save_raw` handler directly: `load 00000000`, then `call 0xE06C0D05 0x<scratch>` (handler 0xE06C0D04 reads its argument through r0, A1 verification item 1; **this call form is inferred and has never been made**). Re-arm with `load 01000000` and the same call. First by hand: `md` four words of `raw_info.buffer` (`rd` the word at `$(A raw_info)` + 4) twice, 1 s apart, 20 times, moving subject | Buffer words identical while the flag is 0; `rvi_check.py` reports torn 0 and "copy differs" 0 for a burst taken in that state | Stable buffer and no assert after 200 toggles | Words change with the flag at 0: the freeze mode is dead (C9 assumption 4), go to SC9 for speed instead | An untorn frame by the cheap route (C9 steps 3-4) |
| SC9 Fast copy | First by hand, buffer address B = `raw_info.buffer` with bit 30 cleared, rounded down to 32: `call 0xE05D088D <B> 0x40`, then compare `md <B> 4` with `md <B|0x40000000> 4` (A13 check 3). **A hang here means: never set the toggle.** Then, in the SC8 state: `wr $(T rvx_fast_copy) 1`, crop x 192, sidecar 1, burst | Median copy time at least 3x shorter than SC7 | `rvi_check.py`: torn 0, "copy differs" 0 | "copy differs" without "torn": stale cache lines (possibly the other core, A13); set the toggle to 0 for good | Whether a live 25 fps copy is possible (under 20 ms for 1280x720, C9 step 5) |
| SC10 Sync source 1 (EvfState) | Requires probe A2.1 passed in this boot. `wr $(T rvx_sync_src) 1`, 10 s. During: `rd` object + 0xC must differ from 0xE05BB2B3. After: it must be 0xE05BB2B3 again and `rvx_sync_active` = 0 | `sync: source 1 installed`, `sync: source 1 removed, stock dispatcher restored`; `sync: signals` about 250; `sync: core vsyncs ignored` gives the old hook's rate for the same 10 s | No assert (`EvfCapState.c L:1CE`, `EvfDevState.c L:174/21E`), LiveView alive, pointer restored | `REFUSED`: read `rvx_sync_last_err`, `rvx_sync_last_val`; nothing was written. Asserts or frozen LiveView: hooking EvfState is not harmless here; toggle 0, power-cycle, drop F1 as well | A hook in which Canon's context only counts and gives a semaphore (the first configuration without blocking work in the hook) |
| SC11 Sync source 2 (Mem1Path) | `md 0x1029C 2` before and during a default recording must show 0xE06AD51B. Then `wr $(T rvx_sync_src) 2`, 10 s | `sync: source 2 installed`, `stock Mem1Path callback restored`; signals about 250 | As SC10; slot holds 0xE06AD51B and the same argument afterwards | Crash at the first frame: `give_semaphore` is not legal in that context (unknown, G1). `no frame signal`: the callback does not fire per frame | Frame-complete signal candidate |
| SC12 Which source | Three 3 s recordings, sidecar on, sources 0, 1, 2, same moving subject | Torn count and storm-assert count per source | One source with clearly fewer torn frames and no asserts | No difference: tearing comes from copy duration; SC8 or SC9 is the route | The sync point to keep |
| SC13 Watchdog stop | Only after SC2 showed a falling level: `wr $(T rvx_pool_watch) 1` with `rvx_pool_stop_below` set from SC2 | `pool: free event containers below the stop threshold`, then `MLV closed (finalized)` | `ver` answers; clip clean; `rvx_pool_tripped` non-zero | Abort after the trip line: the stop itself is too heavy with a low pool; raise the threshold | A recording that ends with a file instead of an abort |
| SC14 Restore paths | With source 1, then 2: (a) `wr $(T rvx_crop_x) 400` and start; (b) `rvx_burst_frames 5`; (c) change the movie mode while recording | Stock pointer at object + 0xC and at 0x1029C; `rvx_sync_restore_fail` = 0 | all restored | Stop condition 4 | Confidence to leave a source enabled |
| SC15 Three bursts in a row | No reboot between them | Same free memory before and after | three clean clips | A leak or a refused third start: read the stop summary of each | End of session C |

Continuous recording is not a step of this session. The arithmetic is against it at the default
window: 2,809,856 bytes x 25 fps = 70.2 MB/s against 75 MB/s measured once on an idle camera
(*established on hardware* for the 75, A7; D7 section 1 (iv)).

---

## 4. Session D - core changes

Rules for every item:

1. One change per card. The branches are parallel, not stacked (D7 section 5.2 risk 2): build each
   in its own worktree, with its own `.sym` and its own modules, all three symgate checks, commit,
   and copy `autoexec.bin`, `.sym` and all `.mo` together.
2. **Boot-reliability run first: at least 10 cold boots, same card, same mode, compared with the
   SA1 numbers** (and 10 fresh boots of the baseline card on the same day if SA1 is older). Because
   the baseline loses boots by chance, a difference of one or two boots in ten means nothing. Stop
   and go back to the baseline card if the new core reaches big.8 in fewer than half as many boots
   as the baseline, or if a new abort signature appears twice.
3. Only after the boot run: the functional test of the item.
4. Keep the baseline card content in `backup/card-files` and restore it between items.

Order and reasons:

| # | Change | Branch / worktree | What it does to the core | Boot run, then functional test | Why at this position |
|---|---|---|---|---|---|
| SD0a | Card-state experiment (no new code) | cores of `a10b16c23` and `fa0834b40` | - | One freshly formatted card, module autoload off, alternate only `AUTOEXEC.BIN` with its matching `.sym`, A-B-A-B, 5 cold boots each (C6 "Next hardware checks" 1) | Decides whether "core unchanged" stays a rule. If `fa0834b40` boots at the baseline rate, the boot fixes are innocent and the rest of this table is ordinary work |
| SD0b | Empty `normal_code_patches[]` | **not prepared on any branch**; one-table change in `platform/M50mk2.101/include/platform/mmu_patches.h`, new 18-character branch | Skips the MMU table switch at `big.4a2` | 10 boots; *established on hardware* on another core: 0 of 70 `0xE02F281A` against 19 of 92 (docs/43 section 2) | Every later boot run is cheaper and clearer once 1 boot in 5 is no longer lost |
| SD1 | 27 MHz frame-timer base | `offline-integrate1`, the staged `sim/C10/card` (md5 `78f33748dcbbc6c8eadb882f4537d1ef`) | Layout identical; 3 bytes at file offset 0x18E20 (C10-review M2) | 10 boots. Then the A9.1 values from step SA3 must give 27.0e6, and the MLV header fps of a clip must read about 25000 instead of about 29600 | Smallest possible core change, reviewed, and the only one with a symbol-identical layout: it tests whether any core rebuild boots |
| SD2 | Abort handler with record dump | `offline-abortdump1`, `wt/G2`, commit `741c021f6` (variant B) | 0 symbols moved, 1 added, `_bss_start` +768, two linker veneers swapped (image entry branch at 0xC78E0 differs); the stability gate prints FAIL for `_bss_start` by design (`docs/offline/G2.md` section 4). Never run, not in QEMU either | 10 boots. Functional test is passive: on any abort, `[ABX]` lines appear before `[ABORT] HANG`. For `pc=0xE021B8FC` read `rec tcb=` and `tok h=` with the table in G2 section 3 | Early, because it turns every later failed boot into data, and it is the only prepared tool against the unexplained `0xE021B8FC`. After SD1, because SD1 tells first whether a rebuilt core boots at all |
| SD3 | Build-id of fixed width | `offline-buildid-fx`, `wt/C11`, tip `765760b5d` | Version text only; removes the 18-character rule | 10 boots; ML version screen shows the build id without trailing garbage (C11 check 1) | Hygiene; makes every later branch name harmless. Not reviewed |
| SD4 | Stub corrections outside EDMAC | `offline-stub-fixes`, `wt/F7`, 19 commits | 13 stubs changed, 9 added, 1 constant. `_request_RPC` is invisible to the symbol gate (D7 section 5.2 risk 3) | First the session-A reads (`rd 0x4108`, `md 0xDF002C30 20`, `rd 0xE9F0`, `rd 0x15998`). Flash in two parts: all commits except the two that change LiveView behaviour (`LV_OVERLAYS_MODE`, `LiveViewApp_dialog`), 10 boots; then those two, 10 boots plus a LiveView and menu check | After the diagnostics are in place, because a wrong RPC address fails when ML patches ROM with the second core alive, a crash class that needs the SD2 dump |
| SD5 | EDMAC stubs and the hardware copy retry | `offline-edmacstubs`, `wt/F2`, commit `3ba436b42` | Changes which Canon functions the core calls on the EDMAC path; default still the CPU copy (`m50ii_edmac_cpu_copy` = 1) | 10 boots with the default. Then F2 section 6: photo mode, LiveView idle; B. stub probes by hand (wake call first, `rd 0xD042327C` must be 0, select on channel 13 must give 0x26 and no assert, deselect returns it to 0); C. self-test with `wr $(A m50ii_edmac_cpu_copy) 0` started **from the ML menu** (Debug, "Don't click me"), expect `[R1] EDMAC mem2mem self-test PASS: N=4096 mism=0 first=-1`, five times; restore with value 1 | Only needed if SC7-SC9 show the CPU copy cannot reach the rate. `ConnectReadEDmac_maybe` 0xE05B7A32 against 0xE05B7B08 is unresolved (D7 section 3 item 1); a wrong pick is predicted to show as a 50 ms timeout or a Canon assert. Stop at the first assert in part B |
| SD6 | EvfState hook in the core | `offline-evfstate-1`, `wt/F1`, commit `c444ae918` | +160 bytes, **494 of 1307 symbols move**; gate fails by design; needs its own `.sym` and rebuilt modules | 10 boots (F1 probe 10). Then `vsync_counter` growth over 10 s with `m50ii_vsync_src` 0 and 1 (F1 probe 4) | Last, and only if SC10 passed and a core-level hook is wanted. The module-only source 1 of session C answers the same question without moving a symbol |

Resolve the ML addresses used in SD5 and SD6 from the `.sym` and `location.map` of that build, never
from the literals printed in F2 section 6.

---

## 5. Open questions only hardware can answer, ranked

Ranked by effect on the plan (after `docs/offline/D7-critic.md` section 6).

| # | Question | Answered by | Decides |
|---|---|---|---|
| 1 | Does the baseline on the card boot, and how often? | SA1 | Whether anything else can be measured; whether SD0b comes first |
| 2 | Is `0xE021B8FC` card state or core content? Who registers the faulting callback? | SD0a; SD2 dump on a recurrence; card image (stop condition 1); probe F5.tokens in SA2 | Whether "core unchanged" remains a rule |
| 3 | Does the reviewed module record a decodable clip without flood or abort, and what drains the event pool? | SB3, SB4; SC2 (pool level); probe A2.4 (queue depth, optional call) | Whether the hook rewrite or the storm is the next offline task |
| 4 | What triggers the assert storm? | The six-word read of SB5 at the first `L:125` line; probe A1.4 at idle with raw enabled | Channel hygiene against task starvation as the design rule |
| 5 | How fast is the copy with an aligned crop and with the cacheable read? What does the card sustain with LiveView running? | SC1, SC7, SC9; the bench task of C9 step 1 (not written) | Continuous recording, smaller crop, or burst as the product |
| 6 | Does the raw buffer stay intact while the save-raw flag is 0? | SC8 | Whether a CPU copy can ever give an untorn frame |
| 7 | Live Canon task priorities; `task_max` | Probe A2.5, `rd 0x4108`; a task-table walk needs module code (A11 check 2) | How to read SC5 |
| 8 | Is EvfState input 5, or the Mem1Path callback, a safe per-frame signal that marks a complete frame? | Probes A2.1, A1.7; steps SC10-SC12 | The sync point; whether SD6 is needed |
| 9 | Does the mem2mem copy work with the corrected stubs? | SD5 | The route above about 40 MB/s |
| 10 | Is the frame-timer clock 27 MHz? | Probe A9.1 in SA3; SD1 | Metadata only |
| 11 | Which ML call passes NULL for `%s` at `big.5.3`? | SD2 dump (0x20 bytes at r4 show the text formatted so far, G2 section 5) | One boot in 20 |
| 12 | Are stage-0 predictions of the remaining single-source reports right (A8, A14c, A15, F4, F5)? | SA2 probe table | Trust in those reports |

---

## 6. Checklist (one page)

**Before the session**
- [ ] Card image and file copy made; md5 of `autoexec.bin`, `.sym`, `.mo` written down (0.2)
- [ ] `uart.jsonl` backed up; daemon started; `ver` answers
- [ ] `SYM` and `A()` defined for the running core (0.4)
- [ ] Normal card and 90 s recovery procedure at hand (0.5)
- [ ] For session C: toggle addresses can be resolved (0.4, open gap)

**Every deploy**
- [ ] Source committed; branch name 18 characters for a core build
- [ ] `make` in `modules/`, then in `platform/M50mk2.101`
- [ ] `check_symbol_stability.py`, `check_module_syms.py`, `check_card_image.py` pass (or the documented by-design failure of SD2 / SD6)
- [ ] `check_card_image.py` on the mounted card, `sync`
- [ ] Commit, md5 and first boot number in the notes

**Session A**
- [ ] 10 cold boots counted (big.8, time, signature)
- [ ] `hwprobe.py run --stages 0,1 --label photo-lv`
- [ ] `rd 0x4108`, `md 0xDF002C30 20`, `rd 0xE9F0`, `rd 0x15998`
- [ ] `hwprobe.py run --stages 0,1 --label movie-1080p25 --allow-risky-read`
- [ ] `S0.sym`, A1.3, A1.4, F4.1 as predicted, otherwise stop

**Session B**
- [ ] Only `raw_vidx.mo` (md5 `0d80a5c1...`) copied; `mlv_lite.mo` disabled; `raw_vidx.en` present
- [ ] Clip under 5 frames: `MLV closed (finalized)`, `ver` answers after 30 s
- [ ] `mlv_check.py -v --stats` exit 0; clip archived
- [ ] Clip over 10 frames, then a second clip in the same boot
- [ ] On a storm: six-word read before the power cycle
- [ ] On flood and abort: not more than two repeats, then session C

**Session C** (module `631266de...`, one toggle per step)
- [ ] SC0 defaults, SC1 sidecar, SC2 pool level
- [ ] SC3 burst, SC4 throttle, SC5 priorities (10 runs per set), SC6 release order
- [ ] SC7 aligned crop, SC8 frozen buffer, SC9 fast copy (hand test first)
- [ ] SC10 source 1, SC11 source 2, SC12 comparison, SC13 watchdog stop, SC14 restore paths, SC15 three bursts
- [ ] After each step: `ver`, `rd 0x39B84` non-zero, `mlv_check.py`, `rvi_check.py`

**Session D** (one change per card, 10 boots each before any test)
- [ ] SD0a card-state A-B-A-B; SD0b empty patch table
- [ ] SD1 27 MHz core; SD2 abort dump; SD3 build-id
- [ ] SD4 stub fixes in two parts; SD5 EDMAC stubs and self-test; SD6 EvfState core hook

**Stop at once**
- [ ] `pc=0xE021B8FC`: image the card first
- [ ] Three boots in a row without big.8
- [ ] A Canon pointer not restored after a stop
- [ ] A risky read, a `call` or the cache invalidate hangs the shell: power-cycle, do not repeat that command in this session
