# 19 — QEMU boot baseline (WS-QEMU Phase 0)

**Date:** 2026-07-07 · **Model:** `qemu-eos` `-M M50mk2` · **ROM:** gang100.bin / gang200.bin
(1.0.1) symlinked at `roms/M50mk2/ROM0.BIN` / `ROM1.BIN`
**Binary:** `qemu-eos-build/arm-softmmu/qemu-system-arm` (already built)

## Why this exists

Discord steer from `names_are_hard` (a core ML/qemu dev): doing early-boot dev in
qemu is **lower risk** than HW UART bring-up. This phase measures whether that lane
is actually viable for the M50 II — i.e. how far the in-tree `M50mk2` model boots
today, before we invest in tuning it. Go/no-go gate for the QEMU workstream.

## What was already in place (not built this phase)

- `qemu-eos/hw/eos/model_list.c:699` already defines `MODEL_NAME_EOSM50mk2` ("M50mk2"),
  described in-source as *"near-identical hardware to M50 (Digic 8, 1GB). Clone of M50
  params."* — a **rough stub**: UART IRQs `0x15D/0x16D` are flagged
  *"TODO: taken from 200D … has to be validated"*, `current_task_addr = 0x28` is a "fixme".
- ROMs are staged; the machine type builds and runs.

## Run performed

```
QEMU_EOS_WORKDIR=roms ./arm-softmmu/qemu-system-arm \
  -M M50mk2,firmware=boot=0 -display none \
  -drive if=sd,format=raw,file=…/sd.base.raw \
  -drive if=ide,format=raw,file=…/cf.base.raw \
  -serial file:uart.log
```

`boot=0` = plain Canon firmware (bootflag cleared), so this measures **model quality
independent of ML** — the truest baseline.

## Result: Canon DryOS boots to STARTUP with live serial

Serial (UART DebugMsg) output:

```
BootLoade K468 READY
   0: WARN [EEP] DataMgrParamCheck Swap:0 0x0 Line:228
   … (EEPROM param-check warnings) …
  20: [STARTUP] K468 ICU Firmware Version 1.0.1 ( 1.9.0 )
  22: ERROR [EEP ERROR] eep_i2c__Init      <-- current ceiling; boot stalls here
```

**This is the decisive datapoint.** To print `[STARTUP] … Firmware Version`, DryOS
has already run the full cold-boot chain **through** `create_init_task` and the
scheduler's first-task handoff into `init_task` / the STARTUP sequence. So:

- The **create_init_task → init-helper → init-task handoff completes in emulation.**
  That is exactly the step being bisected blind on the LED (docs/08). In qemu it is
  already past, fully traceable with `-d` / gdb.
- **The UART/DebugMsg console works in qemu with zero hardware.** No testpoints, no
  teardown — the serial log above is the camera's own boot console.

## Current ceiling & known emulation gaps

| Signal | Meaning |
|---|---|
| `ERROR [EEP ERROR] eep_i2c__Init` — boot stalls | EEPROM-over-I2C peripheral not modeled for M50mk2; DataMgr can't read config → STARTUP blocks. Typical D8-in-qemu gap, **not** a broken model. |
| `[MPU] FIXME: using generic MPU spells for M50mk2` | MPU (button/comms co-processor) messages are generic; fine for boot. |
| `[MPU] FIXME: no MPU button codes for M50mk2` | No UI button injection yet; irrelevant to boot bring-up. |

Both CPU cores come up (CPU0 + CPU1 MMU/cache bring-up seen in the exec log; the D8
default `ram_extra_addr[0]=0xDF000000` matches our CPU1-SRAM region from docs/15).

## Verdict: **GO**

The QEMU lane is viable. The model already runs Canon's OS through the exact handoff
matescb is stuck on and gives us the serial console for free. The remaining work is
**tuning, not resurrection**:

1. Replace the model's guessed fields with F1–F7 ground truth (UART IRQs, task addr,
   interrupt IDs — docs/14/17/18). *(Phase 1)*
2. Trace `create_init_task → init-helper → init-task` in emulation to explain why the
   Canon trampoline path diverges from `my_init_task`, and find the clean hook vs.
   matescb's IRQ-masking direct-entry hack (docs/08). *(Phase 2)*
3. Optional: model the EEP/I2C stub enough to push the Canon baseline past
   `eep_i2c__Init`, raising the ceiling toward property/FIO/GUI init. *(stretch)*

Note the asymmetry to chase in Phase 1/2: plain Canon (`boot=0`) reaches STARTUP,
while the ML AUTOEXEC (`boot=1`) experiments stopped earlier in ML's own
cache/pre-reloc code — i.e. our early path has a qemu incompatibility Canon's does
not. That is a diff-against-a-working-baseline problem, which is the good kind.
