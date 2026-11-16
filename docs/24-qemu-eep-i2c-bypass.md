# 24 — QEMU eep_i2c: is it really the ceiling? (bypass analysis)

**Date:** 2026-07-10 · **Model:** `qemu-eos -M M50mk2,firmware=boot=1` · **ROM:** gang100.bin/gang200.bin (1.0.1)
**Binary:** `qemu-eos-build/arm-softmmu/qemu-system-arm` · **Card:** `m50ii_dumper/qemu_stage/sd_mltest.raw` (ML AUTOEXEC, rebuilt 2026‑07‑10)

> **Scope:** analysis only. No source edits, no build, no hardware. This doc supersedes the
> "eep is the ceiling" framing in **docs/19** for the current **boot=1 + ML AUTOEXEC** flow.

---

## TL;DR (headline result — the premise has moved)

The task premise was: *"QEMU stalls inside Canon `init_task` at `eep_i2c__Init`; `init_task` never
returns, so we never reach `boot_post_init_task` (post.0)."*

**That is no longer true for the current card + current `eos.c`.** A fresh run of the exact command
in the task boots **straight past eep_i2c** and reaches ML `post.0` → `post.4` → `big.0` → `big.1`,
then **data‑aborts inside ML's own `_find_ml_card()`** (not eep). Evidence below.

- `eep_i2c__Init` **still prints `[EEP ERROR]`** in QEMU, but it is **non‑fatal**: Canon's routine
  has a **bounded retry/timeout**, takes the error/fallback path, and **`init_task` returns normally**.
- The eep register block is touched only a **handful of times** (`0xD00000C4` read 4×), with the guest
  **timer advancing monotonically** — there is **no spin/hang** on any eep address.
- **The real current ceiling** is a NULL‑pointer dereference (DFAR=`0x58`) at PC `0x000D8D36`,
  reached *after* `big.1: _mem_init done` and *before* `big.2: _find_ml_card done` → i.e. inside
  **`_find_ml_card()`**. That is a post‑eep, post‑`boot_post_init_task` ML porting bug, unrelated to i2c.

**Recommendation: do NOT add an eep bypass.** It is not what is blocking, and "faking success"
is *riskier* than the current error‑and‑fallback behavior (see §5). Redirect effort to the
`_find_ml_card()` NULL deref. An optional insurance stub is given in §4 with explicit caveats.

---

## 1. What actually happens now (reproduction)

Command (verbatim from the task):

```
timeout 40 env QEMU_EOS_WORKDIR=/mnt/data/WORK/eos/roms \
  qemu-system-arm -M M50mk2,firmware=boot=1 \
  -drive if=sd,format=raw,file=…/sd_mltest.raw \
  -drive if=ide,format=raw,file=…/cf.base.raw,snapshot=on \
  -display none -serial file:/tmp/LOG.txt
```

Serial (`/tmp/LOG.txt`), condensed and in order:

```
[ML] pre.0 … pre.9: boot_pre_init_task DONE
K468 READY
[ML] post.0: boot_post_init_task ENTER -- Canon init_task RETURNED   <-- init_task DID return
[ML] post.0c: rgb_vram_preinit BYPASSED
[ML] post.1: waiting for bmp_vram_raw (Canon display init)
   0: WARN [EEP] DataMgrParamCheck Swap:0 0x0 Line:228               <-- eep DebugMsg AFTER post.0
  20: [STARTUP] K468 ICU Firmware Version 1.0.1 ( 1.9.0 )
  24: ERROR [EEP ERROR] eep_i2c__Init                                <-- errors, but boot continues
[ML] post.2: bmp_vram_raw TIMEOUT -> proceeding anyway
[ML] post.3: creating ml_init task (my_big_init_task)
[ML] post.4: ml_init task created; boot_post_init_task returning
[ML] big.0: my_big_init_task ENTER
[ML] big.1: _mem_init done
[ABORT] type=0x1 pc=0x000D8D36 spsr=0x20000033
[ABORT] DFSR=0x00000007 DFAR=0x00000058 IFSR=0x0 IFAR=0x0
[ABORT] r0=0 r1=0 r2=0 r3=0x74 r12=0
```

Two facts kill the old premise:

1. **`post.0` prints "Canon init_task RETURNED".** The `[STARTUP]` banner and `[EEP ERROR]` appear
   *after* it in the log because they are emitted by Canon startup/EEPROM **tasks** that `init_task`
   *spawned*; those tasks run later under the scheduler. `init_task` itself finished and handed back
   to ML. (This is standard DryOS: `init_task` creates tasks and returns; it does not block on them.)
2. The boot proceeds all the way to **`big.1`** and dies on a **data abort at DFAR=0x58**, which is
   an ML NULL deref — nothing to do with i2c.

### Why docs/19 said "eep is the ceiling"
docs/19 measured **`boot=0`** (plain Canon, no ML). There, `[EEP ERROR] eep_i2c__Init` was simply the
**last serial line printed** before Canon's *own* later init blocked on other unmodeled D8 hardware
(display/sensor/MPU handshakes) — so it *looked* like the stall point. Under **`boot=1` + ML**, ML's
`my_init_task` wraps Canon `init_task`, does not wait on those Canon tasks, and continues. The eep
line is a **red herring** for the ML flow.

---

## 2. Exactly which registers eep touches, and the polled value

Captured with `-d io` (`/tmp/LOG2.stderr`). The Canon task named **`EEPROM`** performs the eep_i2c
sequence. On Digic 8 this is **not** the legacy I2C peripheral:

| Register | Access (from trace) | Caller PC | eos.c handler | Returned |
|---|---|---|---|---|
| `0xD000C01C` | `<- 0x1` (enable) | `E020916C` | `eos_handle_digic6` default | (write) |
| `0xD000C41C` | `<- 0x1` (enable, 2nd lane) | `E020916C` | `eos_handle_digic6` default | (write) |
| `0xD00000C4` | `-> 0x0` then `<- 0x2` / `<- 0x4` (read‑modify‑write of command/trigger bits) | `E0367620`/`E0367626` | `eos_handle_digic6` default | **read = 0x0** |
| `0xC0090000` (legacy I2C) | **0 accesses** | — | `eos_handle_i2c` | — |
| `0xD01302B4` (EEP_CS2/RP GPIO) | 35× writes `0x4C0003`/`0x4D0002` — this is the serial‑flash/RP GPIO **chip‑select** toggle used all over boot, **not** eep completion | | catch‑all `D0130xxx → 0` | 0 |

Disassembly of ROM0 at `E0367620` (`ldr r2,[r0]; movs r3,#1; lsls r3,r4; orrs r3,r2; str r3,[r0]`)
confirms `0xD00000C4` is a **control register** being OR'd with trigger bits (`1<<r4` → 0x2, 0x4),
**not** a status spin.

**The polled/expected value:** Canon's `eep_i2c__Init` kicks the transfer via `0xD00000C4` and then
waits on a **completion/ready status** that `eos.c` currently answers with **`0x0`** (the
`eos_handle_digic6` default for any unhandled `0xD000xxxx` address). Because "ready" never asserts,
the routine runs its **bounded retry loop** (backed by the DryOS timer at `0xC021000C`, which is why
the trace shows a short burst of timer reads `0x14300→0x14800`), gives up, prints
`[EEP ERROR] eep_i2c__Init`, and **returns an error that `init_task` ignores**. `0xD00000C4` is read
only **4 times total** in the whole boot — categorically **not** an infinite poll.

> So the answer to "which register / what value / why don't the handlers satisfy it": the guest waits
> on the eep completion bit in the `0xD000C000` block (status read‑back at/near `0xD00000C4`); `eos.c`
> returns `0x0` (default), which is "not ready"; the handlers don't satisfy it **by design** — and it
> doesn't matter, because Canon tolerates the timeout.

---

## 3. How other Digic 7/8 cameras handle eep in qemu-eos

**They don't emulate it at all — and neither the model struct nor `eos.c` has any eep‑specific knob.**

- `model_list.h` has **no `eep`/`i2c` fields**. There is **no per‑model flag or MMIO stub** for eep_i2c.
- The only related device is the **serial‑flash SPI** store (`serial_flash.c`, tagged `[EEPROM]`),
  which is a *different* peripheral (properties/calibration), created **only if
  `model->serial_flash_size != 0`**. That is set for **Digic 4–6 only**: A1100, 6D, 650D, 700D, EOSM,
  EOSM2, 100D, 70D, 80D, 750D, 760D, 5D4. **Every Digic 7/8 model** — 200D, 6D2, SX70, SX740,
  **EOSM50**, **EOSM50mk2**, EOSM6mk2, EOSR, EOSRP, 850D, 90D — leaves `serial_flash_size` unset
  (M50's is explicitly commented out at `model_list.c:690‑694`). So no SPI‑flash device *and* no eep
  device on any D7/D8 model.
- The legacy `eos_handle_i2c` (`0xC0090000`) has canned replies for slave addrs `0x38`/`0x3D` — that
  is Digic 5/6 I2C (sensor/EEPROM), and it is **never hit** on M50 (0 accesses).
- Consequently **all D7/D8 models take the same path M50mk2 does**: the eep block reads back `0` from
  `eos_handle_digic6`'s default, Canon's `eep_i2c__Init` times out benignly. Under `boot=0` they reach
  `[STARTUP]` then stall at the *next* unmodeled peripheral; under `boot=1`+ML they wrap `init_task`
  and continue. No model "solves" eep — they all rely on Canon's error tolerance.

---

## 4. Options, and the recommended change

| Option | What | Verdict |
|---|---|---|
| **(0) Do nothing** | Leave eep to error‑and‑fallback; fix `_find_ml_card()` instead. | **RECOMMENDED.** eep is not the blocker; the error path is the *safe* emulation (§5). |
| (a) MMIO stub in `eos.c` | Return a "ready/done" status for the eep completion register so `eep_i2c__Init` succeeds. | Optional insurance only. **Guesses** the ready bit; risks making Canon *accept* all‑zero eep data (§5). |
| (b) Emulate eeprom in `serial_flash.c` | Model the D8 eep block + back it with a data file. | Large effort, **zero** current payoff (boot already passes eep). Not worth it. |
| (c) Guest‑side skip (patch ML/ROM) | NOP out `eep_i2c__Init`. | Unnecessary; Canon already survives the error. Adds a maintenance patch. |

### Recommended: Option 0 (no eep change)
Nothing about eep is stopping the boot. The one concrete next action is the `_find_ml_card()` NULL
deref (see §6).

### If an eep stub is still wanted (Option a) — optional, low‑risk insurance
Purpose: (i) push the **`boot=0` plain‑Canon** baseline a little further, and (ii) silence the
`[EEP ERROR]`/`DataMgrParamCheck` noise. It does **not** help the current `boot=1` ceiling. Because
the exact completion bit is unverified, the safest stub returns an "all bits ready" word for the eep
status read so the poll exits immediately via the *success* branch rather than the timeout branch.

```diff
--- a/qemu-eos/hw/eos/eos.c
+++ b/qemu-eos/hw/eos/eos.c
@@ eos_handle_digic6()  (around the existing EEP_CS2 / D0130 cases, ~line 7048)
         case 0xD0132280:
             msg = "M50 SD detect";
             ret = 0;
             //~ ret = 0x10000;
+
+        /* D8 eep_i2c completion/status stub (OPTIONAL — see docs/24 §5 for risk).
+         * Canon's eep_i2c__Init kicks a transfer via 0xD00000C4 and polls a ready
+         * bit that the default handler answers with 0 (=> [EEP ERROR] timeout, but
+         * non-fatal). Returning a "done" word lets the routine exit via its success
+         * branch. Only enable if you specifically want a quieter boot=0 baseline. */
+        case 0xD00000C4:
+            msg = "D8 eep_i2c status (stub: ready)";
+            ret = 0xFFFFFFFF;   /* every candidate ready/ack bit asserted */
+            break;
     }
```

> Note: `0xD00000C4` is inside the `0xD0000000–0xDEFFFFFF` range routed to `eos_handle_digic6`, so this
> case is reachable. It sits *before* the `if (address >= 0xD0130000 …)` RP‑GPIO catch‑all and does not
> collide with it. Ship it **only** after confirming on a `boot=0` run that eep switches from
> `[EEP ERROR]` to a clean init and that nothing downstream regresses.

---

## 5. Risk of skipping / faking eep

- **Faking "success" can be worse than the current error.** The log shows Canon's `DataMgr` already
  runs `DataMgrParamCheck` and, on the eep failure, **falls back to known defaults** (that is what the
  `WARN [EEP] DataMgrParamCheck … Line:228/234/247/255` lines are). If Option (a) makes `eep_i2c__Init`
  report *success* while the emulated eeprom still returns **all zeros**, `DataMgr` may **accept the
  zero blob as valid calibration/config** and skip the fallback — producing *different, possibly worse*
  downstream state (bad white balance / sensor / power tables) than the current clean fallback. The
  **error‑and‑fallback path is the safer emulation** until a real eeprom image is supplied.
- **eep data feeds later init.** Any change that alters what `DataMgr` believes about eep contents can
  shift property values, power/sensor setup, and DebugMsg timing — i.e. it can move or mask the
  *next* bug. Keep eep behavior fixed while chasing the `_find_ml_card()` fault so the variable under
  test stays isolated.
- **Doing nothing has no risk:** boot already passes eep every run, deterministically.

---

## 6. The real current blocker (hand‑off to next task)

- **Fault:** data abort, `DFSR=0x7` (translation fault), **`DFAR=0x58`**, `PC=0x000D8D36`,
  `r3=0x74`, `r0=r1=r2=0`.
- **Location:** between checkpoints `big.1: _mem_init done` and `big.2: _find_ml_card done`
  (`magiclantern_simplified/src/init.c:534–537`) → the fault is **inside `_find_ml_card()`**.
- **Nature:** dereference of a **NULL base + 0x58** — almost certainly an ML‑side read of a Canon
  file‑I/O / partition / drive struct that is still NULL this early (a global that Canon populates via
  a task that hasn't run, or a D8 address/offset that differs from the D6/D7 assumption `_find_ml_card`
  was ported from). This is the same class as the already‑bypassed `rgb_vram_preinit`/`bmp_vram_raw`
  waits — an ML early‑boot ordering/porting issue, **not** a QEMU peripheral gap.
- **Next step:** resolve `PC 0x000D8D36` to an ML symbol via the build's `.map`/ELF, inspect what
  `_find_ml_card()` reads at `+0x58`, and either defer it (like the other post‑init waits) or supply
  the missing pointer. eep_i2c can be left exactly as it is.

---

## 7. Appendix — evidence files

- `/tmp/LOG.txt` — serial for the 40 s `boot=1` run (reaches `big.1`, then abort).
- `/tmp/LOG2.stderr` — `-d io` MMIO trace; eep block = `0xD000C01C/0xD000C41C/0xD00000C4`,
  timer `0xC021000C`, `0xC0090000` legacy‑I2C count = 0, `0xD00000C4` read count = 4 (no spin).
- ROM0.BIN == `dump_1.0.1/gang100.bin` (32 MB @ 0xE0000000); RMW at `E0367620` confirms `0xD00000C4`
  is a command register, not a poll.
