# C3b - QEMU: getting Canon's firmware past the "EEPROM stage" (M50 Mark II)

Date: 2026-11-16. Offline sprint, no camera. Nothing here proves behaviour on hardware.

**Furthest stage reached: Stage B, partially.** The real cause of the stall was found and fixed in
the model (it is NOT the EEPROM), Canon's init stage 1 now runs for the first time
(`ExpandROM 1 Get Data from FROM`, I2C/DCDC driver activity), but the emulated machine then
hangs before PropertyMgr is created. Storage does not come up; Stage C was not reached.
The patched binary is currently WORSE for ML smoke tests than the C3/known-good one
(ML stops after `big.1`), so do not use it as the default QEMU binary.

## Goal

(A) find what Canon does at the EEPROM stage and where boot stalls, (B) smallest model change that
lets Canon continue and measure how far it gets, (C) if B: mounts, run the C2 smoke test.

## What was done

- Copy: `cp -a sim/C3/qemu-eos sim/C3/qemu-eos-build sim/C3b/` (1.1 GB, 32 GB were free), then
  the absolute paths `sim/C3/` -> `sim/C3b/` rewritten in 1344 build files and 80 symlinks of the
  build dir (same method as C3). `make -j6` rebuilt cleanly. `sim/C3/` untouched.
  Binary: `sim/C3b/qemu-eos-build/arm-softmmu/qemu-system-arm`.
- Model patch: `sim/C3b/m50mk2-model-eep.patch` (`diff -u` of `hw/eos/eos.c` against the C3 copy,
  2 hunks, described below). No ML source was changed, no worktree/branch was created.
- Helper tools (new, in `sim/C3b/tools/`): `adrxref.py` (find Thumb `adr`/`adr.w`/literal
  references to an address - Canon's strings are mostly referenced by `adr`, which
  `stubfind.py xref` does not see), `blscan.py` (list BL/B.W call sites of a target),
  `rungdb.sh OUTDIR SCRIPT SECS` (frozen QEMU on gdb port 1241 + gdb batch + kill by PID),
  gdb scripts `g1..g12.gdb`. RAM dump of Canon's flash driver: `sim/C3b/ram_022F0000.bin`.
- Runs (UART log, gdb output, QEMU stderr) in `sim/C3b/{g1..g12,io1,int1}/`.
  Ports used: gdb 1241 only (serial to file).

## Results

### A1. "[EEP ERROR] eep_i2c__Init" is not an error and not the stall

`0xE0087C04` prints that string unconditionally on entry (DebugMsg level 6), then calls
`0xE020908C(0,0)`; gdb shows it returns 0 (success) in QEMU (`sim/C3b/g1/gdb.out`):

```
HIT pc=e0087c04 ... lr=e00878f5      <- eep_i2c__Init entered (from the EEPROM task)
HIT pc=e0087c22 r0=00000000          <- I2iDmac init returned 0 = OK
```
The real camera prints the same line. docs/24 and C3 were right that it is harmless.

### A2. What the EEPROM task does and where it waits

EEPROM task = `0xE0081038` (BootPhase `0xE0080F58`, ReadyPhase `0xE0081A6E`, StartPhase
`0xE0080E90`). State at RAM `0x625C`; event flag handle at `[0x626C]`; two "mirror" units
(sizes 0x8000 and 0x100) described by 0x24-byte entries at `0x6290`.

- Before the task starts, `0xE0081968` reads the EEPROM header and `0xE0080D50` checks it: each
  unit needs `[+8] == 0xAA00AA00` (or 0xFFFFFFFF) and `[+0xC] == 0x00BB00BB` (or 0xFFFFFFFF),
  and size fields matching 0x8000 / 0x100. In QEMU everything reads 0, giving the eight
  `WARN [EEP] DataMgrParamCheck Swap:n 0x0 Line:228/234/247/255` lines. This is tolerated:
  the unit is marked invalid and data comes from the flash ROM instead ("Get Data from FROM").
  So no EEPROM contents are needed to continue; an all-0xFF (erased) header would even pass the
  two magic checks.
- BootPhase then sets flags 0x10/0x20 and waits (`WaitForAnyEventFlag` at `0xE0080FFE`) for
  flags 0x2|0x4 (`sim/C3b/g2/gdb.out`: `WAITSTART r5(mask)=00000006`, `WaitForAny mask=00000006`,
  no SetEventFlag on that object afterwards). Those two bits are only set by `0xE0081596`
  (SetDramParam, message "SetDramParam Size MissMatch"), whose only caller is `0xE0082CD6`
  (Get_DRAMParam), a callback of Canon's PropertyAdmin ROM expansion
  (`pfSendDramParameterCBR` / `pfExpandROMSelectorCBR`, registered through `0xE05CEF92/98`).
  So the EEPROM task waits for the property system, not the other way round.

### A3. The real stall: serial-flash chip identification fails, init stage 1 never runs

Canon's startup sequencer has a stage table at ROM `0xE13947EC` (`{event mask, function}`):
`{0x02010000, 0xE0040C4D}`, `{0x100032, 0xE00413AD}`, `{0x4, 0xE004189B}`, ... Stage 1
(`0xE0040C4C`) is the one that calls CreatePropertyMgr (`0xE05BF99E` at `0xE0040CAE`).
It needs bits 0x10000 (posted at `0xE0040B36`) and 0x2000000. Bit 0x2000000 is posted at
`0xE0040BB8` only if `r5 != 0`, and `r5` is the return value of `0xE0048786` (called at
`0xE00407DE`): a serial-flash identification.

- `0xE004874C` copies three u16 from `0xBFE01FD0` (written by the bootloader) into the flash
  driver; built-in fallback `{0x0020, 0xBB19, 0x0001}`.
- `0xE0048786` calls the RAM-resident driver (`0xDF00312C` -> `0x022F0DF0`), which sends JEDEC
  command 0x9F to flash unit 1 through the controller at `0xD7100000`, then looks the answer up
  in a 12-entry table at `0xDF0032D8` (`{manufacturer, device, -, handler}`: 20/BB18, 20/BB19,
  20/BB20, C2/2539, C2/2538, C2/2537, 01/2018, 01/0219, 01/0220, EF/8019, 9D/7018, 34/2B1A).
- The model answered nothing (`0xD71000xx` mostly reads 0 / `rand()`), so the key was all zeros
  (`sim/C3b/g8/gdb.out`: `key=df0033a6: 0x0000 0x0000 0x0000`), the lookup failed,
  `0xE0048786` returned 0, bit 0x2000000 was never posted, stage 1 never ran. gdb confirmed
  stage 1 was never entered in the unpatched model (`sim/C3b/g4`), and that forcing `r0=1` at
  `0xE00407E2` makes it run (`sim/C3b/g5`).

Controller registers used by the driver (unit 1 base `0xD7100000`, unit 0 base `0xDE000000`),
read from `0x022F147E`: `+0x3C` u16 total length, `+0x50` u8 transmit byte, `+0x14` bit 1 byte
sent, `+0x20` bits 5/6 receive-available / end, `+0x40` bits 8..12 receive FIFO level,
`+0x90` u8 receive byte, `+0x3E` u16 remaining, `+0x4C`/`+0x3B`/`+0x38`/`+0x34`/`+0x1C`/`+0x28`
configuration and interrupt clear.

### B. Model change (`sim/C3b/m50mk2-model-eep.patch`)

Hunk 1 - new function `eos_m50mk2_sflash1()` plus a hook at the top of `eos_handle_digic6()`
(M50mk2 only, addresses `0xD7100000..0xD71000FF`): minimal manual-mode SPI-NOR model. Tracks a
transaction (`+0x3C` length, `+0x50` command/address bytes) and answers reads through
`+0x40`/`+0x90`: 0x9F -> `20 BB 19`, 0x05 -> status (WEL after 0x06), 0x70 -> 0x80, anything
else -> 0xFF. `+0x14` returns `rand()|2`, `+0x20` returns 0x60, `+0x3E` returns 0. Program and
erase commands are accepted and ignored. Each transaction is printed to QEMU stderr as
`[SFLASH1] ...`.

Hunk 2 - `eos_handle_rom_id()`: for M50mk2 the words at `0xBFE01FD2/4` now read `0xBB19` /
`0x0001` (the layout Canon's own fallback uses) instead of `0x00BB` / `0x0019`.
This hunk turned out not to be sufficient alone (the firmware re-reads the chip), and I did not
test whether it is necessary once hunk 1 is in; it is kept for consistency with the driver's
stored copy at `0xDF0033A0`.

Result with the patch (`sim/C3b/g10`, no gdb forcing):

```
HIT pc=e00407e2 r0=00000001 ... lr=e00487fd     <- flash check now returns 1
HIT pc=e0040c66 ...                              <- init stage 1 entered (first time ever in QEMU)
HIT pc=e0040c6e / c74 / c78 / c7e / c82 / c8c    <- stage 1 calls up to 0xE0040C88 return
[SFLASH1] cmd 9F (tx 1: 9F 00 00 00 00) total 4 -> rx 3: 20 BB 19 10
[SFLASH1] cmd 06 ... / cmd 81 (81 83) / cmd 06 ... / cmd 61 (61 E7)     <- Micron config writes
```
New UART lines (compare C3 section 4):
```
       23: WARN [EEP] SetDramParam Size MissMatch !! 216 - 256
       26: [EEP] ExpandROM 1 Get Data from FROM
       33: [STARTUP] K468 ICU Firmware Version 1.0.1 ( 1.9.0 )
       35..38: ERROR [I2C] I2C_Write[CH1] : 0x1b,0x02,0x01,0xd2      (x4)
[ML] !!! ASSERT (non-fatal): nullptr @ Device::DcdcDrvAvs.c L:00000047
       39: ERROR [EEP ERROR] eep_i2c__Init
```
The real camera prints `ExpandROM 1 Get Data from EEPROM` at the same place, so the sequence
now follows the real one, with FROM data instead of EEPROM data.

Not reached: `0xE0040C90` (return of `0xE0044630`), CreatePropertyMgr (`0xE0040CAE`),
`*0x4FF4` stays 0, no storage semaphore, no SD device, no B: mount.

### Next blocker (precise description, cause not proven)

- Stage 1 calls, in order: `0xE05B51CE`, `0xE05BBE8A(100)`, `0xE063BF9C`, `0xE00A6F88`,
  `0xE018A3D8`, `0xE065750E`, `0xE0044630`, ... The call `0xE0044630` at `0xE0040C8C` never
  returns. `0xE0044630` starts with `0xE0656FDA(0, 10000)` and then loops over a byte table at
  `0xE0998CE0` calling `0xE0656F34` with up to 3 retries - consistent with the
  `I2C_Write[CH1] : 0x1b,0x02,0x01,0xd2` errors (I2C channel 1, device 0x1B, not modelled) and
  the `DcdcDrvAvs.c:71` assert.
- After that the whole guest stops: three samples 3 s apart (`sim/C3b/g11` procedure, output in
  this session) all show CPU0 at `pc=0xE005B316`, `cpsr=0x600000B3` (IRQ masked, SVC), i.e.
  parked right after the `wfi` at `0xE005B314` inside Canon's idle function `0xE005B2AC`
  (low-power branch: `[0x5BF8]==1`, clock gating through `0xE0611DF2` on `0xD010D000`).
  Forcing the plain `wfi` branch (`0xE005B324`) with gdb did not help (`sim/C3b/g12`).
  ML's own output also stops (`big.1` is the last marker; before the patch ML reached `big.8`).
- `-d int` run (`sim/C3b/int1/uart.log.qemu-stderr`), last interrupt events before silence:
  ```
  0xe00a6f95: cpu 0 sending SGI 0xa
  0xdf002bab: cpu 0 sending SGI 0xc
  0xe03c5139: cpu 1 ack SGI 0x0, iar: 0xc      (CPU1 then clears SCTLR.C: 0x48C5187D -> 0x48C5107D)
  0xe03c5139: cpu 0 ack SGI 0x0, iar: 0xc
  ```
  `0xDF002BAB` is in the RAM flash-driver area. Hypothesis (not verified): before a flash
  operation the driver parks the other core with SGI 0xC; the model also delivers that SGI to
  the sending core (or the release SGI never arrives), both cores end up parked, and the
  timer interrupt (0x16D, 1163 deliveries before that point) is no longer serviced. The I2C
  failures may be an independent second problem inside `0xE0044630`.

### C. Smoke test

Not run with the patched binary as a pass/fail test: B: does not mount, and the patched model
hangs before `big.8`. For ML-side QEMU work keep using the C3 or original binary.

## Limitations

- The fix is evidence-backed up to "stage 1 runs"; everything after is a regression in terms of
  how far ML gets (`big.1` instead of `big.8`).
- The serial-flash model is the minimum for identification. It does not implement read-back of
  written configuration, program, erase, or unit 0 (`0xDE000000`). The JEDEC id 20 BB 19 was
  chosen because it is Canon's own fallback and is in the driver table; the real chip id of this
  body is unknown. `+0x20` is forced to 0x60, which could hang a loop that waits for those bits
  to clear (none seen so far).
- Hunk 2 was not tested in isolation after hunk 1.
- The SGI explanation of the hang is a hypothesis from one interrupt log; the idle-loop samples
  only prove where CPU0 sits, not why no interrupt wakes it. CPU1 state was not sampled.
- Timestamps in Canon's DebugMsg lines count DOWN in QEMU (4294884 -> 4290813 ms) while they
  count up on the camera; not investigated, may matter for timeouts.
- No real EEPROM or serial-flash dump exists in the workspace; none was needed for this step.

## Suggested next steps (offline)

1. Check the SGI model in `eos.c` (the `sending SGI` code, around the `0xf00` GIC case): target
   filter handling for SGI 0xC sent from `0xDF002BAB`, and whether the sender must be excluded.
   Sample both CPUs' PC at the hang.
2. If that is fixed and `0xE0044630` still blocks: model I2C channel 1 device 0x1B (DCDC/AVS)
   with plain ACKs; the driver is `0xE0656F34` / `0xE0656FDA`.
3. Then re-check `0xE0040CAE` (CreatePropertyMgr), `*0x4FF4`, `0x2DF60` (FIO drive table),
   `0x6450` (sd_device) with `sim/C3b/tools/g7.gdb` / `g12.gdb`.

## Next hardware checks

1. Read-only: JEDEC id of the two serial flash chips as Canon sees them: `md 0xDF0033A0 2`
   (driver's stored id, expect `0020 BB19 0001` or another table entry) and `md 0xBFE01FD0 2`.
2. Read-only: `md 0x625C 16` and `md 0x6290 20` after boot (EEPROM module state, unit
   headers/sizes) and the first 0x20 bytes of each EEPROM unit header if a safe reader exists
   (an `eep_header_read` eventproc name exists at ROM `0xE0082B84`, its function was not
   traced; do not call any write/erase/sync procs).
3. Read-only: `rd 0x5BF8` (idle low-power flag) and `rd 0x4FF4`, `md 0x2DF60 12`, `rd 0x6450`
   for comparison with QEMU.
4. A clean Canon-only UART capture from power-on to the SD mount line, to compare the order
   of `ExpandROM`, `[I2C]` and `[STARTUP]` lines with `sim/C3b/g10/uart.log`.
