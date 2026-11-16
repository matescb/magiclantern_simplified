# Magic Lantern for the Canon EOS M50 Mark II - project documentation

Working directory: `/mnt/data/WORK/eos/`. This `docs/` tree is not under version control; a copy of
the previous version of this file is in `docs/offline/backup/README.md`.

**Goal.** Bring Magic Lantern (ML, the open-source firmware add-on for Canon cameras) to the EOS M50
Mark II, which uses Canon's DIGIC 8 processor, and get ML raw video recording working, that is,
recording uncompressed sensor frames into an MLV file (ML's raw video container). The project owns
one camera body on Canon firmware **1.0.1**, which must not be updated.

**Last updated: 2026-11-16.**

Terms used below:

- **Stub**: a line in `platform/M50mk2.101/stubs.S` that gives a Canon firmware address a C name.
- **Core**: ML's main binary `autoexec.bin`. **Module**: a separately loaded `.mo` file, linked at
  load time against the core's symbol file (`.sym`).
- **`big.8`**: the last boot marker ML prints on the UART (serial debug port); "reached `big.8`"
  means ML finished starting.
- **raw_vidx**: the raw recorder module in `modules/raw_video/raw_vidx`. **mlv_lite**: the older ML
  raw recorder module.
- **EDMAC**: Canon's DMA engine, which moves image data between the image processor and memory.
- **LiveView (LV)**: the live sensor preview; raw video is recorded from it.

## 1. Status as of 2026-11-16

There are two kinds of result in this project and they must not be mixed:

1. **Hardware results**, obtained on the camera between 2026-07-05 and 2026-07-22. The camera has
   not been used since; the last UART log line is from 2026-07-22 (`docs/offline/C7.md`, header).
2. **Offline sprint results** of 2026-11-16 (`docs/offline/`). **No camera was attached. None of
   the sprint's findings or code has been run on hardware.** They are static analysis of the ROM
   dump, host builds, host tests, emulator runs, and re-reading of the July UART logs.

### 1.1 What works on hardware (July 2026)

| Capability | Evidence |
|---|---|
| Full ROM dump of firmware 1.0.1 (ROM0 32 MB, ROM1 16 MB) | `docs/02`; files in `m50ii_dumper/dump_1.0.1/` |
| ML boots to `big.8` on the camera, both CPU cores up, MMU patching live | memory note `ml-m50ii-port-status.md`; first seen 2026-07-10 |
| ML on-screen display and the ML menu, opened with the FN button and navigable | `docs/28` (verified 2026-07-12) |
| Two-way serial debug shell (read and write memory, call functions, no reboot per probe) | memory note `duplex-uart-harness.md`; `tools/mlserial/TOOLS.md` |
| One real 14-bit raw LiveView frame (2096x1164, RGGB) captured, written to the card and decoded on the host | `docs/35` (2026-07-18) |
| `FIO_WriteFile` from ML works when the buffer is in the uncacheable address range | `docs/35`; mechanism confirmed in the ROM by `docs/offline/A7.md` |
| raw_vidx ran its whole pipeline once and wrote 13.4 MB of sensor data to the card | UART boot 212, 2026-07-19; `docs/offline/C8.md`, Recommendation section |
| Card bus mode SDR104; ML bench 75.1 / 75.7 MB/s write, 90.4 / 93.9 MB/s read on one 16 GB card | `docs/offline/A7.md`, verification row "1, 4" (UART log) |
| Pushing a new `autoexec.bin` to the camera over WiFi and rebooting into it | `docs/37`; memory note `m50ii-wifi-deploy-mvp.md` (2026-07-19) |

### 1.2 What does not work on hardware

| Problem | State |
|---|---|
| **Usable raw video.** No decodable, correctly timed clip has been recorded. | The frames raw_vidx wrote were copied at about 5.7 MB/s (`docs/offline/A13.md`), so one copy spans about 11 sensor frames (`docs/offline/C9-safe-first.md`, arithmetic, not observed on a decoded file). |
| Recording ends in a crash or a wedged LiveView. | 47 recording attempts are catalogued in `docs/offline/C7.md`. Offline explanation: `docs/offline/A3.md` and `F4.md` (stack overflow after Canon's event-container pool runs empty). Inferred, not confirmed on hardware. |
| Canon assert storm (`MvRawProDs.c`, `RawPro.c`, `LvJob.c`) during raw capture, intermittent. | Offline explanation in `docs/offline/A1.md`. Not confirmed on hardware. |
| EDMAC memory-to-memory copy leaves the destination stale. | Offline explanation: wrong stubs (`docs/offline/A5.md`); corrected stubs on branch `offline-edmacstubs` (`F2.md`), untested. |
| Boot is intermittent: 216 of 410 logged boots (53 %) reached `big.8` across all builds. | `docs/offline/C7.md`. |
| Boot crash `pc=0xE021B8FC lr=0xE04037A1` in Canon's PropertyMgr. | Cause not identified (`docs/offline/A4.md`, `F5.md`). The logs show it follows the card contents, not the core layout (`C6.md`, `C7.md`). |
| mlv_lite recording. | Reads a buffer that is never filled on this body (`docs/offline/C8.md`); recommended to drop it for now. |
| ML menu buttons in all camera modes; starting recording with the REC button. | Parked; key codes depend on the camera mode (`docs/34`). |
| Reduced bit depth, crop modes, SRM memory pool, lossless compression. | Not started. Background in `docs/offline/A8.md`, `B2.md`, `B3.md`. |

### 1.3 What the offline sprint added (not hardware-tested)

Read `docs/38-firmware-map-raw-video.md` first: it is the synthesis of the firmware reports for a
new reader. `docs/offline/INDEX.md` lists every report with its verification outcome.

- **Firmware understanding** (reports A1-A15): how Canon produces LiveView raw frames (A1), which
  state object signals "frame read out" (A2), why recording crashes (A3, F4), the EDMAC channel map
  and the wrong stubs (A5), the movie-record and card-mount path (A6), the card write path and its
  speed limit (A7), memory pools (A8), raw geometry and the 27 MHz frame-timer clock (A9), register
  write paths (A10), Canon task priorities (A11), resource locks (A12), why the frame copy was slow
  (A13), a full stub and constant audit (A14a-c), and ROM1 and the boot-time RAM images (A15).
  Ten of these reports were checked by a second agent; the refuted claims are listed in
  `docs/offline/INDEX.md`.
- **Corrections to earlier project beliefs**: the EDMAC addresses in `docs/30` (A5, F2); the
  "movie-format remount" theory in `docs/32` (A6); "a core relink that shifts symbols breaks boot"
  (C6, C7: the real hazard found is that the branch name is embedded in the binary, C5);
  `EVF_STATE = *(0x10108)` being the readout-done object (A2).
- **Community knowledge** (B1-B4) and a line-level comparison with the working M50 mk1 port (B5).
- **Host tooling**: MLV validator and host writer replay (C1), QEMU harness (C2, C3), symbol gate
  (C5), UART crash catalogue (C7), raw_vidx host test (C4). See section 3.
- **Code on branches, built but never flashed**: raw_vidx fixes (C4, reviewed in C4-review),
  timer base (C4c), mlv_lite removal (C8), integration branch for the next hardware session (C10),
  build-id fix (C11), selectable vsync source (F1), EDMAC stubs (F2), other stubs (F7), first
  plan steps (C12), module-only sync sources and pool watchdog (G1), abort-handler dump (G2).
  Only C10 was reviewed by a second agent when the critic wrote. See
  section 2.
- **Recording plan**: `docs/offline/C9-recommended.md`.

Every one of these needs a hardware session before it can be called a result. The hardware checks
each report asks for are in the reports' own "Next hardware checks" or "Hardware probes" sections.

### 1.4 What this sprint did NOT achieve

Taken from the critic's report (`docs/offline/D7-critic.md`). No camera was attached, so nothing
below could have been settled by this sprint.

- **No blocker has a hardware-tested fix.** Every branch was built and, at most, host-tested.
- **Blockers (i) to (iii)** (assert storm after raw enable, EvfDevState flood, record-time aborts
  in the debug formatter) **have a mechanism but the fix is only partly prepared.** The storm has
  only an avoidance (`rvx_redirect_edmac=0`), and its trigger is unknown. The vsync hook still
  blocks and allocates on C10 and C12, which is the behaviour `docs/42` names as the stall. The
  gate at RAM 0x5158 that A3 proposes is on no branch. G1 adds module-only sync sources and a pool
  watchdog; they are unreviewed and never run.
- **The boot crash `pc=0xE021B8FC` is unidentified.** A4 and F5 remove candidates; G2 turns F5's
  probe into code that has never run.
- **The baseline's boot rate is taken on trust.** The log cannot tell `a10b16c23` (257696 bytes)
  from the core that crashed 7 of 7 on 2026-07-21, and nobody checked which core is on the card.
- **The one measured boot-reliability fix, the empty `normal_code_patches[]`, is not on the
  baseline** or on any sprint branch (0 of 70 `big.4a` aborts with it, 19 of 92 without).
- **QEMU cannot load modules.** Canon's storage does not come up in the emulator (C3, C3b), so the
  recorder was never exercised there.

## 2. Repository and branch layout

### 2.1 Directories

| Path | Content |
|---|---|
| `docs/` | This documentation (not under version control). |
| `docs/offline/` | Offline sprint reports, `INDEX.md`, label proposals (`labels/*.csv`), `tools/raminit.py`, `backup/`. |
| `m50ii_dumper/dump_1.0.1/` | ROM dump: `gang100.bin` (ROM0, loaded at 0xE0000000, Thumb code), `gang200.bin` (ROM1, 0xF0000000, no code - `docs/offline/A15.md`). |
| `magiclantern_simplified/` | ML source, main checkout. The port is `platform/M50mk2.101/`; the recorder is `modules/raw_video/raw_vidx/`. |
| `wt/` | Git worktrees of `magiclantern_simplified`, one per sprint branch (section 2.3). |
| `sim/` | Scratch builds, logs and staged card directories of the sprint (for example `sim/C10/card`). |
| `tools/` | Host tools (section 3). |
| `research/` | Clones of reference ports (M50 mk1 raw video, kitor's SX740 EDMAC and EOS R raw drafts, M6 II). See `docs/29`. |
| `ml_discord/` | Dump of the ML developer Discord, mined in `docs/07` and `docs/offline/B1`-`B4`. |
| `qemu-eos`, `qemu-eos-build` | Emulator source and build used by `tools/qemu/`. |

### 2.2 Remotes and long-lived branches of `magiclantern_simplified`

Remotes: `origin` = `reticulatedpines/magiclantern_simplified` (upstream), `fork` =
`matescb/magiclantern_simplified` (the project's fork). Output of `git branch -a -vv` on 2026-11-16:

| Branch | Tip | Meaning |
|---|---|---|
| `dev` | b9d9a9557 | Upstream development branch (`origin/dev`); the port's base. |
| `m50mk2-port` | fa0834b40 | The port: 15 commits on top of `dev`. Its tip is a work-in-progress checkpoint that does not boot. |
| `fork/m50mk2-port` (remote) | bb10c937b | **Only the first 6 of the 15 port commits are pushed.** The local branch is 9 commits ahead; everything from raw video onwards exists only on this machine. |
| `walkback-milestone` | a10b16c23 | Current baseline, checked out in the main checkout: `3ccf499dc` (an earlier commit of `m50mk2-port`) plus one commit that walks the core back to the last booting layout and keeps the raw fixes in modules. The work-in-progress tip `fa0834b40` of `m50mk2-port` is **not** an ancestor of the baseline (`git merge-base --is-ancestor fa0834b40 a10b16c23` fails; the parent of `a10b16c23` is `3ccf499dc`). Not pushed. All sprint branches start here. |
| `origin/m6ii_raw_video` (remote) | f3d555dc8 | Upstream reference branch, not ours. |

Rules in force: do not modify `walkback-milestone` or `m50mk2-port`; commit every build before it
is deployed (memory note `commit-every-build-before-deploy.md`); nothing from the sprint has been
pushed.

### 2.3 Sprint branches and worktrees

Output of `git worktree list` on 2026-11-16, corrected at close-out (report H2). The branches are stacked only along one line: C10 -> C12 -> G1 (-> H1). C11, F1, F2, F7 and G2 each start at `a10b16c23` and are parallel; no merged build of them exists. "Commits" counts commits on top of `a10b16c23`.

| Worktree | Branch | Tip | Commits | Content | Report |
|---|---|---|---|---|---|
| `wt/C4` | `offline/C4` | f8191a47a | 8 | raw_vidx correctness fixes, module only | C4, C4-review |
| `wt/C4c` | `offline/C4c` | 1f324fa84 | 3 | `TG_FREQ_BASE` 27 MHz, selector comment, `edmac-d8.c` guard | C4c |
| `wt/C8` | `offline/C8` | e32ef27ea | 2 | mlv_lite removed from `modules.included`, mlv_lite cleaned up | C8 |
| `wt/C6-good` | `offline/C6-good` | a10b16c23 | 0 | Reference build of the baseline for the relink study | C6 |
| `wt/C6-bad` | `offline/C6-bad` | fa0834b40 | - | Reference build of the non-booting `m50mk2-port` tip | C6 |
| `wt/C10` | `offline-integrate1` | 9633ac828 | 14 | **Integration branch for the next hardware session**: C4 + review fixes + C4c + C8 | C10 |
| `wt/C12` | `offline-plan-step1` | 09a4aaf4b | 15 | C10 plus one commit: priority toggles, throttled copy, burst to RAM, frame-integrity counters (module only). Not reviewed by a second agent | C12 |
| `wt/G1` | `offline-plan-step2` | a015861a2 | 16 | C12 plus one commit: module-only frame-sync sources and an event-pool watchdog. **Latest recording branch.** Not reviewed | G1 |
| `wt/C11` | `offline-buildid-fx` | 765760b5d | 6 | Fixed-width build id; host `mlv_dump` build; M50 camera entries | C11 |
| `wt/F1` | `offline-evfstate-1` | c444ae918 | 1 | Runtime-selectable vsync hook source (changes the core) | F1 |
| `wt/F2` | `offline-edmacstubs` | 3ba436b42 | 1 | Corrected EDMAC stubs | F2 |
| `wt/F7` | `offline-stub-fixes` | ea62b792e | 19 | Corrected stubs and constants outside the EDMAC block | F7 |
| `wt/G2` | `offline-abortdump1` | 741c021f6 | 2 | Abort-handler extension that dumps the PropertyMgr registration record at `pc=0xE021B8FC` (changes the core; starts at `a10b16c23`) | G2 |
| `wt/H1` | `offline-plan-step3` | see `git worktree list` | - | Close-out stage worktree on top of G1; when this table was corrected it had no commit beyond G1's tip a015861a2 | H1 (if present) |

**Why the sprint branch names without a slash are exactly 18 characters long** (`offline-integrate1`, `-plan-step1`, `-plan-step2`, `-plan-step3`, `-buildid-fx`, `-evfstate-1`, `-edmacstubs`, `-stub-fixes`, `-abortdump1`). The Makefile embeds
`"<commit> <branch>"` in the core as `build_id`. A branch name of a different length shifts 53 core
symbols, and a module built against one layout must not be loaded by a core with another
(`docs/offline/C5.md`; confirmed by `C4-review.md` and `C10.md`). `walkback-milestone` is 18
characters, so an 18-character branch reproduces the baseline layout. The `offline/C*` branches are
shorter and therefore do not; they are sources for cherry-picks, not for card builds. Branch
`offline-buildid-fx` removes the dependency (C11) but is not merged anywhere.

Build: `cd platform/M50mk2.101 && make ARM_BINPATH=/usr/bin ARM_ABI=none-eabi`. Check the layout
with `python3 tools/symgate/check_symbol_stability.py` (see `tools/symgate/README.md`).

## 3. Tools index

| Tool | Purpose | Reference |
|---|---|---|
| `tools/romdis.py` | Disassemble the ROM at an address (`python3 tools/romdis.py <hexaddr> [count]`) or read a word (`word <addr>`). | `docs/04` |
| `tools/stubfind/stubfind.py` | Find functions in the ROM by string anchors and references. | `docs/06` |
| `tools/verify_stubs.py` | Re-check every stub in `stubs.S` against the ROM structurally. Known blind spots: skips 0xDF00xxxx stubs and cannot tell a wrong function from the right one (`docs/25`, `docs/offline/A14a.md`). Baselines: `docs/stub-baseline.txt`, `docs/stub-baseline-2026-11-16.txt`. | `docs/16` |
| `docs/offline/tools/raminit.py` | Boot-time value of a RAM address, from the ROM images. | `docs/offline/A15.md` |
| `tools/mlserial/` | Hardware harness: serial shell client (`mlserial.py`), logging daemon (`mlserald.py`), symbol resolver (`mlresolve.py`), log database (`mllog.py`), card flashing (`flash.sh`), deploy (`mldeploy.py`), WiFi push server (`mlwifi.py`), raw frame to DNG (`raw_dng.py`). Hardware logs are in `run/uart.jsonl` and `run/mllog.db` (open the database read-only). | `tools/mlserial/TOOLS.md`, `PROTOCOL.md` |
| `tools/plug/` | Control of the mains plug and HDMI capture used for remote power-cycling. | `tools/plug/README.md` |
| `tools/mlvcheck/` (new) | `mlv_check.py`: dependency-free MLV file validator that tolerates a cut-off file; `verify_image.py`; `host_writer/`: the camera's MLV writer replayed on the host. | `docs/offline/C1.md` |
| `tools/qemu/` (new) | `mk_sd_image.sh`, `run_m50mk2.sh`, `smoke_test.py`: build an SD image from the current build, boot it in QEMU, check for `big.8` and the serial shell. QEMU does not model storage far enough to record (`docs/offline/C3.md`). | `docs/offline/C2.md` |
| `tools/symgate/` (new) | `check_symbol_stability.py`, `check_module_syms.py`, `check_card_image.py`: refuse a build whose core layout differs from the booting baseline, whose modules import missing symbols, or whose card image mixes builds. | `tools/symgate/README.md`, `docs/offline/C5.md` |
| `tools/logmine/` (new) | `logmine.py`, `regen.sh`: generate the crash, assert, per-build and recording tables from the UART history. | `docs/offline/C7.md` |
| `tools/raw_vidx_hosttest/` (new) | Host behavioural test of the raw_vidx module with a fake MLV backend (`run.sh`). `tools/raw_vidx_hosttest_c10/` is the extended copy used for the integration branch. | `docs/offline/C4.md`, `C10.md` |
| Ghidra project (MCP server `ghidra`) | Decompiler view of `gang100.bin`. Label proposals from the sprint are in `docs/offline/labels/*.csv` and have not been applied. | - |

## 4. Document index

Status values: **current** (still the reference for its subject), **historical** (a record of work
that is finished or overtaken; correct for its date), **superseded by X**, and **contains refuted
claims - see X** (do not act on it without reading X).

### 4.1 Numbered documents

| Doc | One-line description | Status |
|---|---|---|
| [01](01-ml-m50ii-status.md) | State of ML for the M50 II in the community before this project (2026-07-05). | historical |
| [02](02-rom-dump-procedure.md) | How the 1.0.1 ROM was dumped with Canon Basic; reproducible. | current |
| [03](03-port-progress.md) | First port skeleton, boot constants, method. | historical |
| [04](04-build-and-tooling.md) | Environment, toolchain, build and disassembly commands. | historical; the build command is still valid, the "no Ghidra, no QEMU" statements are not. For tools see section 3 |
| [05](05-next-steps-handoff.md) | Early plan and draft hand-off to the ML developers. | historical |
| [06](06-stubs-ledger.md) | Ledger of the first stub-finding campaign (81 targets). | historical; several entries later found wrong - see 11, 12, 25, offline/A14a, A14b |
| [07](07-discord-findings.md) | ML Discord mined for boot hand-off, QEMU, UART and raw clues. | current for boot topics; extended for raw video by offline/B1-B4 |
| [08](08-boot-trace-report.md) | LED-based boot tracing on hardware up to the first-init blocker. | historical; blocker solved in 10, 20-22 |
| [09](09-digic8-interrupts-registers.md) | Generic DIGIC 8 interrupt and register map from the ML wiki. | current as a generic reference; not M50 II-specific, so each address must be re-checked in this ROM |
| [10](10-boot-handoff-analysis.md) | Canon's boot hand-off decoded; the wrap-and-swap design. | historical; implemented |
| [11](11-stub-verify-phaseB.md) | Verification of boot-critical stubs inherited from M50 mk1. | historical; continued by 25 and offline/A14a |
| [12](12-stub-verify-phaseB-full.md) | Verification of the remaining inherited stubs and constants. | historical; continued by offline/A14a-c |
| [13](13-boot-chain-cold-path.md) | Cold boot chain from reset to `cstart`. | current; RAM image offsets added by offline/A15 |
| [14](14-interrupt-dispatch-gic.md) | Interrupt dispatch and the interrupt controller. | current |
| [15](15-memory-map.md) | RAM map and MMU. | current; cache attributes and pools extended by offline/A13, A8 |
| [16](16-foundations-healthcheck.md) | The stub regression harness `tools/verify_stubs.py`. | current; blind spots described in 25 and offline/A14a |
| [17](17-dryos-kernel-tasks-sync.md) | Kernel tasks, semaphores, queues. | current, except `task_max`: its value 0x40CC is **refuted statically** (offline/A14a, F7, and the D7-critic spot-check: the bound check at ROM 0xE00A6BC6 loads 0x4108; on hardware 0x40CC read 1). Use 0x4108; hardware check `rd 0x4108`. Canon task table in offline/A11 |
| [18](18-uart-debugmsg-sink.md) | UART and Canon's debug message path. | current; the formatter crash is analysed in offline/A3 |
| [19](19-qemu-baseline.md) | First QEMU boot baseline. | superseded by 24 and offline/C2 |
| [20](20-qemu-handoff-trace.md) | Dynamic trace of the init-task hand-off in QEMU. | historical; finding stands |
| [21](21-qemu-ml-autoexec-boot.md) | First ML boot in QEMU: data abort on 0x40. | historical; root cause in 22 |
| [22](22-qemu-boot-rootcause-datacopy.md) | Root cause: corrupted `.data` copy in `cstart`. | historical; fixed |
| [23](23-hw-test-plan.md) | Hardware test plan for the fix of 22. | historical; test done |
| [24](24-qemu-eep-i2c-bypass.md) | QEMU: whether the EEPROM model is the ceiling. | historical; current QEMU state is in offline/C2 and C3 |
| [25](25-tier-a-stub-audit.md) | Semantic audit of the 40 crash-class stubs (2026-07-11). | current; extended by offline/A14a (which found the RPC pair wrong) |
| [26](26-display-frontier.md) | Display, on-screen display and raw-video plan as of early July. | historical; display solved in 28, raw plan superseded by 38 and offline/C9-recommended |
| [27](27-menu-input-api-registration.md) | Plan: menu input through Canon's task-switch callback. | superseded by 28 |
| [28](28-m50ii-menu-input-WORKING.md) | ML menu input working on hardware (2026-07-12). | current; mode-dependent key codes are in 34 |
| [29](29-d8-raw-reference-lineage.md) | Which reference port supplies which part of DIGIC 8 raw video. | current; line-level comparison in offline/B5 |
| [30](30-m50ii-raw-stub-resolution.md) | Resolution of 30 raw-video stubs and constants. | **contains refuted claims - see offline/A5, F2 (EDMAC and boomer addresses), A12 (`LockEngineResources`), A2 (EVF_STATE)** |
| [31](31-m50ii-raw-video-implementation-plan.md) | Raw video implementation plan written before any port code. | historical; superseded by 38 and offline/C9-recommended. Its `EVF_STATE = *(0x10108)` premise is corrected by offline/A2 |
| [32](32-m50ii-boot-flakiness-fio-race.md) | Long append-log: boot flakiness, card writes, first record attempts. | **contains refuted claims - see offline/A6 (movie-format remount theory)**; the write fix it led to is confirmed (35, offline/A7); boot statistics in offline/C7 |
| [33](33-m50ii-menu-input-fixes.md) | Append-log of the menu and button investigation. | superseded by 34 |
| [34](34-m50ii-menu-input-findings.md) | Conclusions of the menu and button investigation; how to resume. | current (work parked) |
| [35](35-m50ii-raw-dng-milestone.md) | First raw frame captured on hardware; the card-write fix. | current |
| [36](36-m50ii-wifi-deploy-analysis.md) | Feasibility analysis of deploying builds over WiFi. | superseded by 37 |
| [37](37-m50ii-wifi-deploy-mvp.md) | WiFi deploy implementation. | current, but its "blocked" status header is stale: it worked end to end on 2026-07-19 (see the status note in the file) |
| [38](38-firmware-map-raw-video.md) | How LiveView raw works on this camera and what a recorder must respect; synthesis of the sprint. | current; not hardware-tested |
| [39](39-hw-session-plan.md) | Plan for the next hardware session: probes, order of tests, stop conditions. | current; not hardware-tested |
| [40](40-port-history.md) | Narrative of the 15 port commits and which exist only locally. | current |
| [41](41-raw-vidx-module.md) | The raw_vidx module: structure, known defects, what the sprint branches change. | current; not hardware-tested. Canon priorities in it come from offline/A11 (single-source) |
| [42](42-record-time-crashes.md) | The record-time failures: assert storm, EvfDevState flood, formatter aborts. | current; mechanisms verified statically, triggers unknown, no tested fix |
| [43](43-boot-stability-and-layout.md) | Boot reliability numbers, the `0xE021B8FC` crash (unidentified), the "symbol shift" belief, the symbol gate. | current; causal statements are inferred from logs |

Other files: `stub-baseline.txt` (output of `verify_stubs.py`, July), `stub-baseline-2026-11-16.txt`
(same, for the F7 branch).

### 4.2 Offline sprint reports (`docs/offline/`)

All have the status "current; not hardware-tested" unless noted. "Verified" means the report ends
with an independent verification section; details and the refuted claims are in
[`offline/INDEX.md`](offline/INDEX.md).

| Report | One-line description | Status |
|---|---|---|
| [INDEX](offline/INDEX.md) | Every report with headline finding and verification outcome. | current |
| [A1](offline/A1.md) | LiveView raw path: Mem1Path, EDMAC channel 3, completion rings, the three asserts. | current; verified, contains one refuted inference (claim 6) |
| [A2](offline/A2.md) | EvfDevState versus EvfState; which input is "readout done". | current; verified, contains one refuted argument (claim 8) |
| [A3](offline/A3.md) | Debug-formatter crashes are a stack overflow after the event pool empties. | current; verified, one claim refuted in part (claim 11) |
| [A4](offline/A4.md) | PropertyMgr boot crash; culprit not identified. | contains refuted claims (claims 4, 5) - see its verification section and F5 |
| [A5](offline/A5.md) | EDMAC channel map; the boomer stubs are wrong. | current; verified, nothing refuted |
| [A6](offline/A6.md) | Movie-record state machine; no card remount on record start. | current; verified, one function identity partly refuted (claim 5) |
| [A7](offline/A7.md) | Card write path; about 75 MB/s write measured. | current; verified, two side claims refuted (2b, 12) |
| [A8](offline/A8.md) | Memory pools; real SRM allocate/free pair. | current; verified, nothing refuted |
| [A9](offline/A9.md) | Raw geometry table; 27 MHz frame-timer clock. | current; verified, nothing refuted |
| [A10](offline/A10.md) | Sensor and engine register write paths. | current; not verified |
| [A11](offline/A11.md) | Canon task table and priorities; recommended ML priorities. | current; not verified |
| [A12](offline/A12.md) | Engine resource locks; the lock stub is the try-lock. | current; not verified |
| [A13](offline/A13.md) | Why the frame copy ran at 5.7 MB/s; faster paths. | current; verified, nothing refuted |
| [A14a](offline/A14a.md) | Stub audit: kernel, tasks, memory, file I/O, debug. | current; not verified |
| [A14b](offline/A14b.md) | Stub audit: GUI, display, properties, EDMAC, misc. | current; not verified |
| [A14c](offline/A14c.md) | Missing functions and guessed constants. | current; not verified |
| [A15](offline/A15.md) | ROM1 contents; boot-time RAM images; `raminit.py`. | current; not verified |
| [B1](offline/B1.md) | Discord: DIGIC 8 LiveView raw capture. | current |
| [B2](offline/B2.md) | Discord: crop modes and sensor registers. | current |
| [B3](offline/B3.md) | Discord: card speed and recording throughput. | current |
| [B4](offline/B4.md) | Discord: history of the M50 / M50 II raw effort; untried techniques. | current |
| [B5](offline/B5.md) | Line-level comparison with the working M50 mk1 port. | current |
| [C1](offline/C1.md) | Host MLV lane: `mlv_dump`, writer replay, `mlv_check.py`. | current |
| [C2](offline/C2.md) | Reproducible QEMU harness. | current |
| [C3](offline/C3.md) | QEMU model work toward the raw path; storage does not come up. | current (negative result) |
| [C4](offline/C4.md) | raw_vidx correctness fixes on `offline/C4`. | superseded by C10 as the branch to test; see C4-review |
| [C4-review](offline/C4-review.md) | Review of `offline/C4`; two HIGH findings. | current; findings addressed in C10 |
| [C4c](offline/C4c.md) | Frame-timer base 27 MHz; LiveView is 25 fps. | current; picked into C10 |
| [C5](offline/C5.md) | Symbol-stability gate; branch-name length moves symbols. | current |
| [C6](offline/C6.md) | A core relink is not shown to break boot. | current |
| [C7](offline/C7.md) | Crash catalogue from the UART logs. | current (generated) |
| [C8](offline/C8.md) | Drop mlv_lite for now. | current; picked into C10 |
| [C9-safe-first](offline/C9-safe-first.md) | Recording design with least firmware risk. | superseded by C9-recommended |
| [C9-throughput-first](offline/C9-throughput-first.md) | Recording design for maximum throughput. | superseded by C9-recommended |
| [C9-recommended](offline/C9-recommended.md) | The merged recording plan. | current |
| [C10](offline/C10.md) | Integration branch `offline-integrate1` and staged card. | current |
| [C11](offline/C11.md) | Build-id layout fix; host `mlv_dump`; camera entries. | current |
| [F1](offline/F1.md) | Selectable vsync hook source; Mem1Path alternative. | current |
| [F2](offline/F2.md) | Corrected EDMAC stubs. | current |
| [F4](offline/F4.md) | Event-container pool: mechanism and rules for raw_vidx. | current |
| [F5](offline/F5.md) | PropertyMgr boot crash, continued; still not identified. | current |
| [F7](offline/F7.md) | Corrected stubs and constants outside the EDMAC block. | current |
| [D3](offline/D3.md) | Report on this documentation update. | current |
| [C10-review](offline/C10-review.md) | Review of `offline-integrate1`: safe as a module-only first attempt; no HIGH findings; one core constant differs (M2). | current |
| [C12](offline/C12.md) | First plan steps as module code on `offline-plan-step1` (priorities, throttled copy, RAM burst). | current; not reviewed |
| [C3b](offline/C3b.md) | QEMU past the "EEPROM stage": real stall found (serial flash), machine still hangs before PropertyMgr; no module loaded. | current (negative result) |
| [G1](offline/G1.md) | Module-only frame-sync sources and event-pool watchdog on `offline-plan-step2`. | current; not reviewed |
| [G2](offline/G2.md) | Abort-handler dump for the `0xE021B8FC` crash on `offline-abortdump1` (F5's probe as code). | current; never run |
| [G3](offline/G3.md) | Hardware probe pack `tools/hwprobe`. | current; never run on the camera |
| [D2](offline/D2.md) | Report on docs written from the memory notes. | current |
| [D5](offline/D5.md) | Merged Ghidra label proposals (`labels/MERGED.csv`); not applied. | current |
| [D6](offline/D6.md) | Report on `docs/40-port-history.md`. | current |
| [D7-critic](offline/D7-critic.md) | Completeness critique of the sprint: what is not fixed, corrections, unresolved contradictions. | current; read before trusting any summary |
| [H2](offline/H2.md) | Report on the documentation corrections asked for by D7-critic. | current |

Reports F3 and F6 were announced during the sprint and never produced. The rows from C10-review
downwards were added at close-out (report H2).

## 5. Where to start

- New to the project: this file, then `docs/38`, then `docs/offline/C9-recommended.md`. For the
  history read `docs/40`; for the module `docs/41`; for the two crash classes `docs/42` (while
  recording) and `docs/43` (at boot).
- What is still wrong or unproven: section 1.4 and `docs/offline/D7-critic.md`.
- Preparing the next hardware session: `docs/39-hw-session-plan.md` first, then
  `docs/offline/C10.md` and `C10-review.md` (what to put on the card), `G3.md` (probe pack), then the
  hardware-check sections of `F1.md`, `F2.md`, `F4.md` and `C9-recommended.md`.
- Looking up a crash signature: `docs/offline/C7.md`.
- Looking up a stub or constant: `docs/offline/A14a.md`, `A14b.md`, `A14c.md`, `F2.md`, `F7.md`.
