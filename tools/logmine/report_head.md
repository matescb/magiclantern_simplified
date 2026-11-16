# C7 - Crash catalogue from the hardware UART logs (offline)

Everything below is mined from `tools/mlserial/run/uart.jsonl` (681,550 lines, 2026-07-14 .. 07-22, real camera) and the
`boots`/`deploys` tables of `run/mllog.db` (opened read-only). No camera was attached; nothing here proves behaviour on the camera
beyond what those logs recorded. Tables are generated - regenerate with `tools/logmine/regen.sh`.

## Goal

A regenerable catalogue of (1) abort signatures, (2) Canon asserts by burst, (3) per-build reliability, (4) recording attempts,
(5) copy / write rates, plus a check of the causes the docs / memory notes assign to each signature against the data.

## What was built

| path | what |
|---|---|
| `/mnt/data/WORK/eos/tools/logmine/logmine.py` | parser + all tables. `python3 logmine.py [--jsonl P] [--db P] [--out DIR]` (about 30 s) writes `out/crashes.md asserts.md builds.md recordings.md rates.md summary.json` |
| `/mnt/data/WORK/eos/tools/logmine/test_logmine.py` | 8 unit tests on a synthetic log (`python3 test_logmine.py` -> `Ran 8 tests ... OK`) |
| `/mnt/data/WORK/eos/tools/logmine/regen.sh` | runs tests + logmine and re-assembles this report from `report_head.md` + `out/*.md` + `report_tail.md` |
| `/mnt/data/WORK/eos/tools/logmine/out/` | the generated tables and `summary.json` (per-boot rows) |

Method notes that matter when reading the tables:

- **Boot = cold start.** mllog.db has 367 boots but the log holds 457 `[ML] pre.0` markers; the db both splits boots (heartbeat
  counter resets, daemon restarts) and merges them. logmine makes one boot per `pre.0`, labelled by the db boot id containing it
  (`306`, `306b`, ...). Crash and recording tables also print the **mllog db boot id at the time of the event**, which is the
  numbering used in docs and memory notes.
- **Build key = AUTOEXEC.BIN size.** The Canon bootloader prints `Read File AUTOEXEC.bin (N bytes)` on every cold start (457 lines,
  24 distinct sizes). That is the only per-boot build fingerprint in the log. The mllog `build` column is the last `mark-build`
  md5 (mostly module md5s) and is stale after 07-19 13:12 UTC (142 db boots all tagged `108a45c9`). Two different core binaries
  of equal size would be merged; modules and card config are not visible at all.
- **Silent restart.** 48 cold starts die with no abort line at `post.0c..post.2` about 3.6 s after `pre.0` and the bootloader
  runs again 2-3 s later. They are counted separately and excluded from the reliability denominators (`effective`).
- 3 boots contain only garbled `[ABORT]` text (UART interleaving) and are in no signature row. 212 ASSERT lines are truncated.

## Results

Headline numbers (from `out/summary.json`): 458 boot segments (457 cold starts + pre-history stub), 48 silent restarts, 410
effective; **216 reached big.8 (53%)**; **166 boots have a clean abort**, 36 distinct signatures; 29 assert sites; 47 recording
attempts (32 mlv_lite, 15 raw_vidx).

### Findings where the data disagrees with (or sharpens) the documented cause

1. **`pc=0xE021B8FC lr=0xE04037A1 DFAR=0x46` is not explained by "the boot-fixes changed the core"** (MEMORY.md boot-stability
   entry; m50ii-boot-stability.md 07-21 correction). The core with AUTOEXEC size 256448 ran db boots 288-329 with 50 boots
   reaching big.8 and zero occurrences, then crashed 22 of 22 from db boot 330 on with the same file size. The crash then also
   hit 7 of 7 boots (db 354-359, at post.4) on a different, older-size core (257696) that had 0 occurrences in its 22 earlier
   boots on 07-18. It disappears on db 362-366 (size 257760). So the signature follows something outside AUTOEXEC.BIN (card
   content: modules, module-enable/config files) and a core walkback alone did not clear it. It is fully deterministic
   (29 of 29 boots on the two affected cores from db boot 330 to 359, interrupted only by two boots of a third core at db 348-349; constant DFAR, constant lr, always at big.1b / post.4), so it is not a power, capacitor or
   hardware fault either. Caveat: equal size is not proof of an identical binary.
2. **`pc=0xE00436FC lr=0xE018E213 DFAR=0x16` is not "the old build's ONE crash (boot 193) = card-not-ready race"**
   (m50ii-symbol-shift-boot-crash.md). It occurs in 16 boots over 4 cores (db 147..285), always with last marker
   big.4a / big.4a2, 26-31 s after cold start on slow cores - the same phase and time as `0xE02F281A`. It never occurs on cores
   that do not also show `0xE02F281A` (0/70 on 256448, 0/64 on 256320). It belongs to the big.4a MMU-patch family.
3. **`pc=0xE005DE52 lr=0xE005DE29 DFAR=0` at big.5.3 predates the fast boot**; the notes call it a consequence of the fast boot
   racing storage. 3 of the 7 hits are on slow-boot cores on 07-14 / 07-16 (db 8, 85, 110; about 39 s after cold start).
   Incidence: 4/70 boots that reached big.5 on the fast core (256448) versus 3/67 on cores 254560+256320.
4. **`pc=0 lr=0xE05D1161` was not closed by the 07-18 "NULL EDMAC CBR slot" fix** (ml-m50ii-port-status.md says "fix validated
   live"). It recurs on 07-19 in three raw_vidx attempts (db 212, 215, 224), 0.4-0.8 s after the last recording marker and
   directly after the 32-line `EvfDevState.c:372/542` nullptr burst. Either a second NULL function pointer exists on the
   raw_vidx stop path or the fix never applied to raw_vidx. (lr is the stale DebugMsg address, as the notes say.)
5. **The walkback-era core still carries the big.4a boot killers.** `0xE02F281A lr=0xE05BB769 DFAR=0xE0997D80` (always preceded by
   5x `KerTask.c:412`) hits 19/92 boots that reached big.4 on size 257760 (including db 362 and 365 on 07-22), 13/27 on 256992
   (where 0 of 27 boots reached big.8) and 0/70 on 256448. That supports the note that the empty-`normal_code_patches` change
   removes it - and means any build without that change loses about 1 boot in 5 to this signature alone, plus `0xE00436FC`
   (10/92) and `0xE05D1ED0` (4/92).
6. **Boot success of the fast-boot core over all its logged boots is 53% (68% without the `0xE021B8FC` run), not 87.5%.** Size 256448: 95 effective boots, 50 reached
   big.8; excluding the 22 deterministic `0xE021B8FC` boots, 50/73. The clean bench run (db 309-319) is 10/11. 12 boots of this
   core stall at big.7 with no abort (mostly db 288-301).
7. **No raw_vidx attempt ended cleanly.** Of 15 raw_vidx attempts: 6 copied frames (one copied 1, five copied 5 each and then stopped on queue-full), 3 logged a
   completed 14,049,920-byte write, 4 ended in the MvRawProDs/RawPro/LvJob storm with no frames, and every attempt that copied
   frames after module 81fd5d54 ended in an abort within 1 s of the last marker (4x `EvfDevState` burst then abort; 1x
   `Task.c:229` then `0xE005DFA2` on 07-22). The two attempts after the READY-poll fix show no storm (n=2).

Not contradicted: the constant-DFAR, constant-phase signatures (`0xE02F281A`, `0xE05D1ED0` after `Memory.c:253` +
`PropertyMgr.c:554`, `0xE00A6C44`) are fixed code locations hit with per-build probabilities, which fits timing races and does
not fit hardware or power faults. I found no signature in the log whose pattern looks like a power fault. The silent restarts
at post.1 are the only resets with no software trace; they come in runs (db 348-359 and 363-366: every cold start is preceded by one), and
the log alone cannot say whether they are a Canon double-start, the plug cycling, or a watchdog.

