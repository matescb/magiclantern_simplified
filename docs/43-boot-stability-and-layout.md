# 43 - Boot stability, boot speed, and the core layout ("symbol shift")

Written 2026-11-16 during an offline sprint. No camera was attached. Reliability numbers are mined
from the UART log of earlier hardware sessions (`tools/mlserial/run/uart.jsonl`, 2026-07-14 to
07-22); firmware statements are static disassembly of `m50ii_dumper/dump_1.0.1/gang100.bin`
(firmware 1.0.1). Nothing here was re-tested on the camera.

This document replaces the chat-memory notes `memory/m50ii-boot-stability.md`,
`memory/m50ii-symbol-shift-boot-crash.md` and `memory/commit-every-build-before-deploy.md` as the
reference for boot behaviour. Several conclusions in those notes are withdrawn here; the rule in
section 7 is kept.

Terms:

- **ML**: Magic Lantern. **Core**: the `autoexec.bin` image the camera's bootloader loads from the
  card. **Module**: a `.mo` file the core loads later; it is linked at load time against the core's
  symbol file `M50mk2_101.sym`, which must be on the card and must match the core.
- **Boot markers**: ML prints `[ML] pre.N`, `post.N`, `big.N` lines on the UART as its start-up
  proceeds. `pre.0` is the first; `big.8` means ML's main init task finished. "Reached big.8" is
  the definition of a successful boot used here. It does not imply modules loaded.
- **Abort**: a CPU data or prefetch abort. ML's handler prints `[ABORT]`/`[AB]` with pc, lr and
  the fault address (DFAR) and then hangs.
- **db boot N**: boot number in `tools/mlserial/run/mllog.db`, the numbering the old notes use.
- **Build key**: the size of `AUTOEXEC.BIN` that Canon's bootloader prints on every cold start.
  It is the only per-boot build fingerprint in the log. Two different cores of equal size cannot
  be told apart by it.
- **Layout**: the addresses of the core's functions and data as listed in the `.sym`.

## 1. Boot reliability numbers

Source: `docs/offline/C7.md` (regenerate with `tools/logmine/regen.sh`).

Whole log: 457 cold starts. 48 of them are "silent restarts" that die without any abort line about
3.6 s after `pre.0` and re-enter the bootloader 2-3 s later; they are excluded below. Of the
remaining 410 boots, **216 reached big.8 (53%)**. 166 boots show a clean abort line, with 36
distinct signatures.

Per core build (C7 section (3)), the builds with enough boots to say anything:

| Build key (bytes) | Dates | Boots counted | Reached big.8 | Seconds `pre.0` to `big.8` (median) | Main failures |
|---|---|---|---|---|---|
| 256320 | 07-14 .. 07-16 | 73 | 56 (77%) | 40 | stalls before big.8 without abort (12) |
| 257376 | 07-18 | 18 | 13 (72%) | 40 | `0xE02F281A` |
| 257696 | 07-18, 07-21 | 29 | 9 (31%) | 39 | 7x `0xE021B8FC` on 07-21, `0xE02F281A`, `0xE00436FC` |
| 257760 | 07-18 .. 07-22 | 99 | 48 (48%) | 39 | `0xE02F281A` x19, `0xE00436FC` x10, `0xE05D1ED0` x4 |
| 256992 (fast boot, early) | 07-19 .. 07-21 | 27 | 0 (0%) | - | `0xE02F281A` x13, stalls at `big.7` x8 |
| 256448 (fast boot) | 07-19 .. 07-21 | 95 | 50 (53%) | 8 | `0xE021B8FC` x22, `0xE005DE52` x4, stalls at `big.7` x12 |

Corrections to the memory notes (all from C7 "Findings"):

- The note's "boot success about 35% to about 87.5% (7/8, 14/16), then 90% (9/10)" describes short
  hand-picked runs. Over all logged boots of the fast-boot core (256448) the rate is 50/95 = 53%,
  or 50/73 = 68% when the 22 deterministic `0xE021B8FC` boots are left out. The clean bench run
  (db boots 309-319) is 10 of 11.
- The note's "old build fails about 44%" is roughly right for the 257760 core (48%), but earlier
  slow-boot cores reached 72-77%.
- "Reached big.8" was the only success criterion of the campaign. The note itself records that
  module loading was never verified in those measurements.
- The note's boot time "about 44 s to about 10 s" is measured from power-on; C7 measures 39-40 s
  to 8 s from the first ML marker. The speed-up itself is confirmed by the log.

No signature in the log looks like a power or hardware fault: the frequent ones have a constant
fault address and a constant boot phase and occur with a per-build probability, which fits timing
races in code. The silent restarts are the only resets with no software trace; the log cannot say
whether they are a Canon double start, the power plug cycling, or a watchdog.

## 2. The boot-time abort families

| Signature | Boots | Phase | What is known |
|---|---|---|---|
| `pc=0xE02F281A lr=0xE05BB769`, DFAR 0xE0997D80 | 40 | `big.4a`, `big.4a2` | Always preceded by 5 `KerTask.c:412` asserts. Occurs while ML applies its ROM code patch through the MMU. 19 of 92 boots that reached `big.4` on core 257760, 13 of 27 on 256992, **0 of 70 on 256448** (C7 finding 5). |
| `pc=0xE00436FC lr=0xE018E213`, DFAR 0x16 | 16 | `big.4a`, `big.4a2` | Same phase and time as the row above; never occurs on cores that do not also show `0xE02F281A`. Belongs to the same family (C7 finding 2). |
| `pc=0xE05D1ED0 lr=0xE05BF8AF`, DFAR 0xC | 7 | `big.4a`, `big.4a2` | Always preceded by `Memory.c:253` and `PropertyMgr.c:554` asserts. |
| `pc=0xE021B8FC lr=0xE04037A1`, DFAR 0x46 | 29 | `big.1b`, `post.4` | Section 3. |
| `pc=0xE005DE52 lr=0xE005DE29`, DFAR 0 | 7 | `big.5.3` | Section 4. |

Corrections:

- The symbol-shift note calls `pc=0xE00436FC` "the old build's one crash (boot 193), the known
  card-not-ready race". It occurred in 16 boots on 4 cores and belongs to the `big.4a` MMU-patch
  family. Withdrawn.
- The current baseline, branch `walkback-milestone` (commit `a10b16c23`), does not contain the
  change that removed the `big.4a` family, so about 1 boot in 5 is lost to `0xE02F281A` alone on
  it, plus the two related signatures (C7 finding 5; db boots 362 and 365 on 07-22 show it).

## 3. The boot-speed and stability changes of 2026-07-19

These changes exist on branch `m50mk2-port` (checkpoint commit `fa0834b40`), not on
`walkback-milestone`: `src/module.c` on `walkback-milestone` has no settle delay, and
`platform/M50mk2.101/include/platform/mmu_patches.h:107` there still defines a patch table.
The list below is from `memory/m50ii-boot-stability.md` and the commit diff summary in
`docs/offline/C6.md` section 1.

| Change | File | Purpose | Evidence in the log |
|---|---|---|---|
| Empty `normal_code_patches[]` (the one entry was a GUI hook that is off by default) | `platform/M50mk2.101/include/platform/mmu_patches.h` | ML then skips the MMU table switch at `big.4a2` | `0xE02F281A`: 0 of 70 boots on the core with this change against 19 of 92 without (C7 finding 5). Supported. |
| raw_vidx no longer loaded at boot (`ML/SETTINGS/raw_vidx.en` renamed) | card | its worker task faulted when started early | not separately measurable |
| `msleep(10000)`, later `msleep(30000)`, at the start of `module_load_task` | `src/module.c` | load modules after Canon's storage start-up | 12 stalls at `big.7` remain on core 256448, mostly db boots 288-301 (C7 finding 6) |
| LED beacon delays off (`M50II_LED_BEACONS 0`) | `src/init.c` | speed | - |
| 4.5 s diagnostic loop replaced by a card-mount gate (marker `big.1c`) | `src/init.c` | speed | - |
| On-screen refresh loop 25 to 8 repetitions | `src/init.c` | speed | - |
| One-shot abort handler | `src/init.c` | print once, then hang | - |

Established: the boot is about 30 s faster and the `big.4a` abort family disappears with the empty
patch table.

Inferred in the memory note and kept as an inference: the remaining failures are races between ML's
start-up and Canon's own, made visible because ML's abort handler hangs on any fault instead of
recovering. Canon's own abort vector also hangs, so restoring it is no fix.

Operational facts from the note (hardware observations of 2026-07-19 to 07-21, not re-checked): a
hung camera can drop to 0 W and then cannot be restarted through the switched power plug; a fresh
hang needed about 90 s without power, 30 s was not enough. These matter for unattended boot loops.

## 4. The big.5.3 crash: a NULL string passed to Canon's formatter

Observed: 7 boots abort at `pc=0xE005DE52 lr=0xE005DE29` with DFAR 0, always after the marker
`big.5.3` ("loading RBF fonts").

Explanation in the memory note: a storage-timing race in the font file read
(`src/rbf_font.c:745-754`), caused by the fast boot reaching the font load inside Canon's card
remount window.

What the analysis shows:

- 0xE005DE52 is `ldrsb r1,[r0]` in the `%s` handling of Canon's full `vsnprintf` (0xE005DB9C); the
  string pointer was fetched from the argument list at 0xE005DE42 and is NULL. Canon's formatters
  do not check `%s` arguments (A3, confirmed by its independent verification, item 9).
- The format string is in ML's image and the stack is in ML's range, so an ML caller passed NULL
  for a `%s`. The output buffer is ML static data near 0x146530, not the stack buffer of the
  `snprintf` at `rbf_font.c:90`, so that call is not the one. Which ML call it is has not been
  identified.
- The crash predates the fast boot: 3 of the 7 hits are on slow-boot cores on 07-14 and 07-16
  (db boots 8, 85, 110, about 39 s after cold start). Incidence is 4 of 70 boots that reached
  `big.5` on the fast core against 3 of 67 on two slow cores (C7 finding 3).

Withdrawn: "big.5.3 is a consequence of the fast boot racing storage" and "it is a font file read
fault". It is an ML formatting call with a NULL string argument that occurs in about 1 boot in 20
regardless of boot speed. Deferring the font load, the fix discussed in the memory note, would not address it.

Open: which call, and why its argument is only sometimes NULL (a race on whatever produces the
string is likely). A3's check 7: print the format string at the logged r5 for the faulting build,
or add a NULL guard in ML's `snprintf` wrapper that logs the caller.

## 5. The 0xE021B8FC crash

### 5.1 Observed

- 29 boots on 2026-07-21 (db boots 330-347 and 350-359) abort at
  `pc=0xE021B8FC lr=0xE04037A1`, DFAR 0x46, with r4 = 0x32 in all 29. Last ML marker: `big.1b` in
  21, `post.4` in 7, `big.1c` in 1; median 4 s after cold start (C7, A4).
- It hit two different cores: build key 256448 (22 of 22 boots from db boot 330 on) and build key
  257696 (7 of 7, db boots 354-359). The 256448 core had reached `big.8` in 50 earlier boots with
  zero occurrences, including db boots 322, 324, 325 and 329 (329 ran for 4059 s). The 257696 core
  had zero occurrences in 22 earlier boots on 07-18 (C7 finding 1, C6 section 2).
- Two boots of a third core in between (db 348, 349, key 256992) passed the crash point.
- After db boots 360 and 361, in which the bootloader found no `AUTOEXEC.BIN` on the card, the
  signature did not reappear (db 362-366, key 257760).
- The streak started at a card touch (between db boots 329 and 330) and survived a 7-hour
  power-off.
- The abort line was not printed once per crash. `pc=0xE021B8FC lr=0xE04037A1 dfar=0x00000046`
  occurs 28,494 times in `uart.jsonl`, against 29 register dumps (`pc=0xE021B8FC spsr=...`),
  about a thousand lines per crashing boot (D7-critic section 4 item 5; count repeated by H2 with
  `grep`). So in these boots the abort handler was not "one-shot", or something re-printed the
  line; which of the two is not known. It matters for the probe in 5.4 (b): code armed inside
  that handler may run about a thousand times per boot and must print once and stop.

### 5.2 Explanations recorded at the time, in order

1. "The fast boot only works in photo mode; movie-mode boot races Canon's GUI/LiveView init."
   Withdrawn by the note itself the same day (a photo-mode boot crashed identically).
2. "Enabling `raw_vidx.en` triggers it." Refuted on hardware: 3 of 3 boots with raw_vidx disabled
   crashed (db 344-346).
3. "Root cause found: the uncommitted EDMAC seed in `edmac-d8.c`." A core without the seed was
   built and flashed; the walkback core then crashed 6 of 6 times (db 354-359).
4. "The boot fixes change the core, which shifts symbols, which crashes Canon's EVF init"
   (the entry in `MEMORY.md` and the rationale for the walkback).

### 5.3 What the analysis shows

Fault mechanics (A4, frame chain confirmed by its independent verification; C6 section 3):

- 0xE021B8FC is `ldrex r1,[r2]` in a spinlock acquire (0xE021B8F0) with r2 = r4 + 0x14 = 0x46, so
  the object pointer is 0x32.
- The caller chain is: Canon's property worker 0xE040347C calls a registrant's token callback
  (`blx r1` at 0xE04034B4); that callback tail-calls 0xE05BFD48, which ML's `stubs.S` names
  `_prop_cleanup`; it calls 0xE0403788, which takes the lock at `token + 0x14`.
- So this is a fault in Canon's **property manager**, not in the EVF code, and it happens about
  1 s after ML's init task is created, while that task is still waiting and before ML registers
  any property handler (ML registers at `big.6`).
- In all 29 dumps the callback received the freshly allocated token pointer and forwarded a value
  0x32 as the token. A function shaped like a property handler ran in the token-callback role.
  ML's `global_property_handler` has that shape, and so do 77 Canon functions.
- A plain zero-initialised token global would take a different branch (0xE0403796) and not fault
  at 0x46, so the 0x32 is a real stored value somewhere.

What it is not (C6):

- Not layout-dependent: both core layouts crash with identical pc, lr, r4 and the same ten ROM
  return addresses on the stack, and the "bad" layout had booted four times just before.
- Not the memory budget, alignment or uninitialised statics: `_bss_end` is 0x14FC00 in both
  builds, the BSS symbol lists differ in 2 of 634 entries, and the "bad" image is smaller
  (256448 against 257696 bytes).
- Not a stale `.sym` or module on the card, and not a hard-coded address used by the host: the
  fault precedes module loading by 10-30 s and no host command was sent before it.

F5 continues A4 (single-source, not verified): the crash is still not identified. F5 rules out the
registrations made from the RAM blob copied from ROM 0xE13BB074 and ML relocating that blob, finds
that Canon's early heap layout differed inside the streak without an ML build change (r10 is
0x00809CC4 in dumps 1-14 and 0x00809FD8 in dumps 15-29), and leaves the value 0x32 unexplained.

Corrections from A4's verification: A4's statement that no Canon registration can be the culprit
("0 of 118 call sites") is incomplete, because `prop_register_slave` is also reachable through a
function-pointer table (literal at 0xE02D1A68) and there are 12 more tail calls to `_prop_cleanup`
in a relocated code blob (0xE13DFD6C and following); and A4's claim that Canon's RAM-resident code
at 0x0225xxxx is absent from the ROM dump is refuted.

### 5.4 Status

**The root cause is not identified.** Two candidates remain (C6 section 5):

1. State on the card (a module, a module-enable file or a configuration file). Fits: start at a
   card touch, two layouts failing identically, survival of a long power-off, end after the card
   was rewritten. Against: db boots 348/349 passed the crash point, presumably with the same card.
2. Core content or timing that does not depend on layout. Fits: two other cores never show it.
   Against: the same-size core booted before the streak.

Warning carried over from C6: the build "meant to boot" (the baseline of section 6) has the same
size and abort-stub addresses as the core that crashed 6 of 6 in db boots 354-359. Whether it is
the same source cannot be told from the log.

Withdrawn: explanations 1 to 4 of section 5.2, including "the boot fixes cause it" (A4's
"Implications" paragraph repeats explanation 4 and should be read with C6 and C7 finding 1 in
mind). In particular the boot fixes are not shown to be harmful; the evidence that removed them
from the baseline was this crash, which also hit the core without them.

Deciding experiments (hardware): (a) C6 check 1, alternate only `AUTOEXEC.BIN` between the two
cores on one freshly formatted card with module autoload off, at least 5 cold boots each; (b) the
abort-handler probe as F5 defines it (F5 summary item 6 and section 7, which replaces A4 check 1):
when pc is 0xE021B8FC print 4 words at r8 and 3 words at r6; `r6[1]` is the registrant's handler
and `r8[2]` is the bad slot, which names both in one boot. Branch `offline-abortdump1` (G2) holds
this as code, never run; (c) if the crash returns, image the card before touching it.

## 6. "Symbol shift": the belief and the evidence

### 6.1 The belief

From `memory/m50ii-symbol-shift-boot-crash.md` (2026-07-19): rebuilding the core so that symbol
addresses move can deterministically break Canon's EVF and auto-exposure code at boot, even if the
added code never runs; the evidence was "3 of 3 boots" crashing at `pc=0xE0729566` after edits to
`edmac-d8.c`. The working rule derived from it: keep the core byte-stable and change modules only.

### 6.2 The evidence (C6 sections 2 and 4)

- The "3 of 3" boots (db 197-199, build key 258400) show three different signatures:
  `0xE02F281A` (40 boots in the whole log, all builds), `0xE00A6C44` (5 boots), and `0xE0729566`
  (the only occurrence in the log). The first two are the known intermittent `big.4a` crashes.
  The stable core of that period reached `big.8` in about half its boots, so three failures in a
  row is about a 1-in-8 event.
- The one `0xE0729566` abort has `lr = 0x000E8389`, which is inside ML's non-fatal assert handler
  (`m50ii_nonfatal_assert_handler + 0x70`): ML returned into Canon code after a Canon assert, and
  Canon then read address 4. That is a consequence of continuing after an assert, not of layout.
- The second "symbol shift" episode, the `0xE021B8FC` streak, is layout-independent (section 5).
- For the pair of builds C6 compared (`a10b16c23` and `fa0834b40`), 988 of 1307 symbols differ,
  yet memory budget, BSS layout and alignment are the same; and `fa0834b40` is not a pure relink,
  it changes boot behaviour.

Conclusion: **the logs do not support the claim that moving symbols by itself breaks boot.**
Withdrawn: "symbol shift deterministically crashes boot" and the lesson "if a boot regression
appears, first suspect symbol shift".

### 6.3 What is real about layout

Two hazards exist and are unguarded in the code (C6 hypotheses H1, H2; C5 section (d)):

1. **Core and `.sym` must match.** The module loader (`module_load_symbols` in `src/module.c`)
   reads `ML/MODULES/M50mk2_101.sym` without any check that it belongs to the running core. A core
   flashed without its `.sym` and rebuilt modules resolves module imports to wrong addresses. The
   deploy tools made this easy: the WiFi deploy pushes only `autoexec.bin` by default.
2. **Hard-coded RAM addresses in host scripts.** The serial recipes call ML functions by literal
   address (for example `call 0xcd515` for `handle_module_keys`, `wr 0x000E5318` to patch
   `raw_lv_redirect_edmac`). In the other layout 0xCD515 is the middle of
   `module_send_keypress` and 0xE5318 the middle of `raw_preview_fast_ex`. The audit of the log
   found every logged call matched its layout, so this did not cause the recorded crashes, but
   nothing prevents it.

Neither hazard can explain a fault 1 s after ML's init task starts, before any module load.

Whether a shift confined to read-only data (the case of section 6.4) matters on the camera is
unknown; C5 proposes booting such a build as a test.

### 6.4 The branch-name build-id effect

Observed (C4, C4-review, C5): `platform/Makefile:610` writes
`build_id = "<short hash> <branch name>"` into the generated `version.c`. The string sits in
read-only data directly before `build_version`, so its length moves everything after it.

| Build | `build_id` length | Effect on `M50mk2_101.sym` against the baseline |
|---|---|---|
| baseline, `3ccf499dc walkback-milestone` | 28 | - |
| branch `offline/C6-good` (15 characters) | 25 | 1 symbol moves (`build_version`, -3; absorbed by alignment) |
| branch `offline/C4` (10 characters) | 20 | 53 symbols from `build_version` to `task_create_zebra_init` move by -8 |
| any branch name of 18 characters | 28 | byte-identical (md5 `d2f46ba5e091fa02d53a8aae9969442f`), checked twice independently |

Functions and BSS did not move in any of these cases; only read-only strings and the start of
`.data`. Consequence: a rebuild of identical source on a differently named branch does not
reproduce the baseline layout. Until the build-id is fixed, use branch names of exactly 18
characters for any core build that is meant to match the baseline. Report C11 describes a change
(branch `offline-buildid-fx`) that makes the layout independent of branch name, date, hash and
host strings; see C11 for its state.

Report C4c attributes a similar read-only-data difference to "the version string length differs
between build dates". Its branch name (`offline/C4c`) is not 18 characters long, and C4-review
reproduced the baseline byte for byte on a later date, so the branch name is the more likely cause
there too (Inferred).

### 6.5 The symbol gate (tools/symgate)

Host-side checks to run before a build reaches a card (C5; 26 pytest tests; never run against a
camera):

```sh
G=/mnt/data/WORK/eos/tools/symgate
B=<checkout>/platform/M50mk2.101/build
python3 $G/check_symbol_stability.py $B/M50mk2_101.sym --elf $B/magiclantern   # layout vs baseline
python3 $G/check_module_syms.py $B/modules/*.mo $B/M50mk2_101.sym              # module imports resolve
python3 $G/check_card_image.py $B/zip --elf $B/magiclantern                    # card dir: sym belongs to autoexec
```

Exit codes: 0 pass, 1 gate failed, 2 bad input.

- Baseline: `tools/symgate/baseline/M50mk2_101.sym`, md5 `d2f46ba5e091fa02d53a8aae9969442f`, 1307
  symbols; `autoexec.bin` 257696 bytes. Anchors: `raw_lv_redirect_edmac` = 0xe5319, `raw_info` =
  0x104434, `_bss_end` = 0x14fc00. The symbol `m50ii_seed_edmac_cbrs`, which the memory note used
  as its stability check (`== 0xec2e1`), does not exist in this build.
- The baseline is "the build meant to boot"; its boot status is taken on trust (C5 limitations),
  and see the warning in section 5.4.
- Finding from the import check: `wifidep.mo` cannot resolve 9 socket and WLAN stubs against the
  walkback core, so the WiFi-deploy module will not load with the baseline.
- The four modules staged for the card (`bench`, `file_man`, `mlv_lite`, `raw_vidx`) resolve all
  imports.
- The gate compares exported globals only; a move confined to static functions is seen only
  through section ends (`--elf`).
- Module-only work does not need a matching core rebuild: the raw_vidx fixes on branch
  `offline/C4` move no core symbol (C4, C4-review).

What the gate is for, given 6.2: it does not protect against a mysterious layout sensitivity of
Canon's firmware. It protects against the two real hazards of 6.3 and gives every flashed build a
recorded identity, which the log analysis badly lacked (C6 and C7 both had to fingerprint builds by
file size).

## 7. The commit-before-flash rule

Rule (from the user, 2026-07-21, `memory/commit-every-build-before-deploy.md`): **never flash or
deploy a build from an uncommitted working tree. Commit the source first, so that every
`autoexec.bin` on a card maps to a git commit.**

Why it exists: the core that booted for 67 minutes in db boot 329 was uncommitted working-tree
state; the tree was edited afterwards and the build could not be recovered. Without a known-good
baseline to compare against, several wrong root causes were announced (section 5.2).

What this sprint adds:

- The loss was worse than the note says. The log cannot identify builds: the `build` column of
  `mllog.db` is the last `mark-build` md5, mostly of modules, and is stale after 2026-07-19 13:12
  UTC (142 boots carry the same tag); the bootloader's file size is the only fingerprint (C7
  method notes). The note's `a28a336f` was a build id, not a commit.
- Module md5s are not reproducible across builds (C4-review), so an md5 written in a note does
  not identify source either.
- A commit alone does not reproduce a layout unless the branch name has the same length
  (section 6.4).

How to apply it now:

1. Commit before every flash, including known-broken builds; put the `autoexec.bin` md5 and the
   observed boot behaviour in the commit message or in a deploy log.
2. Build on a branch whose name has 18 characters (or with the C11 change) and run the three
   symgate checks; record the result next to the boot number.
3. Always stage `autoexec.bin`, `M50mk2_101.sym` and all `.mo` files from the same build together.
4. Resolve every address used by host scripts from that build's `.sym`, not from literals.
5. If a deterministic boot crash appears, image the card before changing anything on it.

The checkpoint commit that preserved the crashing work-in-progress state is `fa0834b40` on branch
`m50mk2-port`. The target the user named, an automated commit, build, deploy pipeline that logs
the (commit, md5, boot result) triple, is not built.

## 8. Summary

Established:

- 53% of 410 logged boots reached `big.8`; no core build with more than 30 boots exceeded 77%.
- The empty MMU patch table removes the `big.4a` abort family (0 of 70 against 19 of 92).
- The fast boot is about 30 s faster.
- `big.5.3` is a NULL `%s` argument from ML code and is independent of boot speed.
- The `0xE021B8FC` crash is a property-manager fault that hit two core layouts identically.
- The branch name changes the core's read-only data layout; 18 characters reproduce the baseline.

Inferred:

- Most remaining boot failures are start-up races that ML's hanging abort handler turns into dead
  boots.
- The `0xE021B8FC` streak was caused by card state or by core content, not by layout.

Withdrawn:

- "Symbol shift by itself crashes boot."
- "The boot fixes (or the EDMAC seed, or raw_vidx, or movie mode) cause `0xE021B8FC`."
- "`big.5.3` is a fast-boot storage race."
- "`0xE00436FC` is a one-off card-not-ready race."
- "Boot success is about 90%."

Open:

- The root cause of `0xE021B8FC`.
- Which ML call passes the NULL string at `big.5.3`.
- What the silent restarts at `post.1` are.
- Whether the boot fixes can be put back on the baseline (they remove about 1 failure in 5).
- Whether a read-only-data-only shift has any effect on the camera.

## 9. Pointers

| Topic | Report |
|---|---|
| Abort signatures, per-build reliability, raw dumps | `docs/offline/C7.md`, `tools/logmine/` |
| Relink investigation, per-boot fingerprints | `docs/offline/C6.md`, `tools/C6_relink/` |
| Symbol gate | `docs/offline/C5.md`, `tools/symgate/README.md` |
| Branch-name effect found | `docs/offline/C4.md`, `docs/offline/C4-review.md` |
| Build-id fix | `docs/offline/C11.md` |
| `0xE021B8FC` property-manager analysis | `docs/offline/A4.md`, continued in `docs/offline/F5.md` (probe: F5 section 7; as code: `docs/offline/G2.md`) |
| `big.5.3` formatter analysis | `docs/offline/A3.md` (signature c) |
| RAM initial values, ROM1 | `docs/offline/A15.md` |
| Memory pools and budget | `docs/offline/A8.md` |
| Earlier boot-flakiness notes (partly superseded) | `docs/32-m50ii-boot-flakiness-fio-race.md` |
| Record-time crashes | `docs/42-record-time-crashes.md` |
| The raw_vidx module | `docs/41-raw-vidx-module.md` |
