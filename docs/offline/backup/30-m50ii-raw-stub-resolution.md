# 30 — M50 Mark II Raw-Video Stub Resolution (gang100.bin)

Reverse-engineering resolution of the 30 raw-video-critical stubs and register/RAM
constants for the Canon EOS M50 Mark II firmware 1.0.1 (ROM dump `gang100.bin`,
DIGIC 8). Each symbol was resolved against the Magic Lantern **M50 mk1 / M50.110**
reference by string-anchoring (module asserts such as `Engine::Edmac.c`,
`BoomerSelector.c`, `PwrMng.c`, `SystemIF/KerFlag.c`), xref/callsite structure, and
struct-layout matching, then passed through an adversarial verifier that independently
confirms polarity (set vs clear, wake vs suspend, read vs write) and rejects any
proposal with empty address or `confidence=none`. Addresses below are ROM/RAM
addresses for gang100.bin and are kept in this file (which lives outside the git repo);
only verified values are promoted into `platform/M50mk2.101/{stubs.S,consts.h}`.

## Summary counts

- **CONFIRMED:** 28
- **PLAUSIBLE:** 1
- **REJECTED:** 1
- **Already present** in `stubs.S`/`consts.h` (all CONFIRMED): 6

## Resolution table

| Symbol | Group | M50mk1 ref | Resolved M50II addr | Verdict | Conf | Method | Evidence (short) |
|---|---|---|---|---|---|---|---|
| edmac_set_address | edmac | 0xe054a9ae | 0xe05b848e | CONFIRMED | high | structural | Leaf (ch,addr): `*(*(chanTbl e05b86e4 + ch*8) + 0xa0) = addr` (raw, unmasked); twin of set_size (sits 0xC before geometry core e05b849a); caller e02cdd10 walks scatter-gather `{addr@+0,size@+4}` → `+0`→e05b848e, `+4`→e05b8862 |
| edmac_set_size | edmac | 0xe054a9ba | 0xe05b8862 | CONFIRMED | med | structural | FUN_e05b8862(ch,size): size&3 check, splits 0xFFFE blocks, builds geometry, calls SetEDmac core e05b849a; callers pass desc+4 |
| edmac_set_transfer_mode | edmac | 0xe054aeb0 | 0xe05b88fc | CONFIRMED | high | structural | Leaf: mode 0-3 cap-bit asserts, writes 2-bit mode to chan_base+0xc0 via RMW e062200c; base tbl 0xe0ec37ac |
| StartEDmac_maybe | edmac | 0xe054a7cc | 0xe05b9690 | CONFIRMED | med | structural | Writes go-cmd 0x11 to (*(tbl+ch*8))+8; asserts cfg bit1; sole go-writer; assert @Edmac.c:0x770 |
| ConnectReadEDmac_maybe | edmac | 0xe0549e52 | 0xe05b95e4 | CONFIRMED | med | structural | 1-arg: asserts ModeInfo INF bit1, RMW+0xc4, DSB, shadow=0, kick+0xb4=1; called LAST in m2m FUN_e0275f20 |
| edmac_reset_channel | edmac | 0xe0549e8c | 0xe05b96b8 | CONFIRMED | med | structural | disable base+4=0 -> pulse 0x80000000->0 on FIFO regs -> re-enable base+4=1; assert Edmac.c:0x77c |
| edmac_reset_boomer_vdkick | edmac | 0xe054a35c | 0xe05b5720 | CONFIRMED | med | string-anchor | Engine::BoomerVdKick.c assert 0x153; per-ch write 0 then 1 (clear+set pulse) at struct+4 |
| edmac_select_boomer | edmac | 0xe054a3da | 0xe0683ca0 | CONFIRMED | med | string-anchor | Engine::BoomerSelector.c; sel[param>>16]=param&0xffff w/ connect-once guard; magic 0x373f0126 absent from ROM |
| RegisterEDmacCompleteCBR | edmac | 0xe0549f76 | 0xe05b7c48 | CONFIRMED | high | structural | 3-arg: completeCBR[ch*8]=cbr,+4=id, state+0x3c=1, RegisterInterruptHandler; dispatcher e05b7de6; 86 callers |
| UnregisterEDmacCompleteCBR | edmac | 0xe054a30e | 0xe05b7f68 | CONFIRMED | high | structural | Inverse of register on table 0x83f24: nop handler, clear arg+enable, disable IRQ |
| edmac_reset_packunpack_mode | edmac | 0xe054a4b2 | 0xe05b8f38 | CONFIRMED | med | string-anchor | Engine::Edmac.c; writes chanTbl[ch]+0xd8 pack/unpack mode; called in SetEDmac paths e00fa020/e00ffad6 |
| edmac_stop_boomer_maybe | edmac | 0xe054a46e | 0xe05b80c2 | CONFIRMED | high | structural | 1-arg: ctrl+0xb4=0 + rewrite Boomer selector; exact inverse of select (e05b802e, +0xb4=2); delta 0x94 matches mk1 |
| PwrMng_WakeSubChips | pwrmng | 0xe0557ece | 0xe06b934e | CONFIRMED | high | structural | Locked ENG_PWR_MNG wrapper (PwrMng.c:0x47) -> e027447a enables subchip list (arg=1), powered=1 |
| PwrMng_SuspendSubChips | pwrmng | 0xe0557efc | 0xe06b937c | CONFIRMED | med | string-anchor | PwrMng.c:0x50 -> e027439e per-subchip OFF cb(+8), loop bound 8; twin 934e = resume |
| CreateResLockEntry | reslock | 0xe055819e | 0xE05BE4BE | CONFIRMED | high | structural | allocs 0x24 LockEntry, memcpys resIds into [5], count[6]; adjacent to Lock (e05be6a6, "EFM_ERSC_STATIC") |
| LockEngineResources | reslock | 0xe0558386 | 0xe05be8a6 | CONFIRMED | high | string-anchor | 1-arg sync locker Engine::Ersc.c; clear cbr[7/8]=0, try-acquire e05be638, state=2, ret 0/0xd; neighbor e05be7ee=3-arg async |
| UnLockEngineResources | reslock | 0xe0558426 | 0xe05be740 | CONFIRMED | high | string-anchor | Requires state==2, release e05be5ce, state=3, wake waiters, delete EFM_ERSC_STATIC sem; pairs w/ Lock e05be6a6 |
| CreateEventFlag_strictly | evtflag | 0xe057754a | 0xe05cc5fe | CONFIRMED | high | structural | KerMisc.c wrapper -> CreateEventFlag e05b0b68, asserts if r0==5; neighbor e05cc64c=CreateRecursiveLock |
| DeleteEventFlag | evtflag | 0xe05531be | 0xE05B0BA2 | CONFIRMED | high | already-in-stubs | KerFlag.c; validate handle&1 then FUN_e05bb578(handle>>1) delete-by-index. **In stubs.S** |
| WaitForAnyEventFlag | evtflag | 0xe05532dc | 0xE05B0CC0 | CONFIRMED | high | already-in-stubs | KerFlag.c OR-wait: FUN_e05bb5dc(id>>1,pat,mode=0); sibling WaitForAll@E05B0D40 mode=4. **In stubs.S** |
| SetEventFlag | evtflag | 0xe05534d8 | 0xE05B0EBC | CONFIRMED | high | already-in-stubs | 2-arg, strip tag, mode=0 OR-set; sibling ClearEventFlag@e05b0eea. **In stubs.S** |
| ClearEventFlag | evtflag | 0xe0553506 | 0xE05B0EEA | CONFIRMED | high | already-in-stubs | validate handle -> FUN_e05bb5a6(h>>1,~bits,mode=4) AND-clear; KerFlag.c. **In stubs.S** |
| RAW_LV_EDMAC_CHANNEL_ADDR | mmio | 0xd0420200 | 0xd0420200 | CONFIRMED | high | mmio-xref | EDMAC write-ch base tbl @e0ec37ac idx3 = 0xd0420200; 100+ refs from e05b7-e05b9 cluster; unique in ROM |
| SHAD_GAIN_REGISTER | mmio | 0xd0008030 | — | REJECTED | none | mmio-xref | 0 xrefs, 0 byte-pattern hits ROM-wide; mk1 value flagged WRONG (from 200D/DIGIC-4) |
| FPS_REGISTER_A | mmio | 0xd0406198 | 0xd0406198 | CONFIRMED | high | mmio-xref | In reg-init tbl e131341c {0xd0406198,0x215}...; matches kitor_sx740 fps-engio.c exactly |
| FPS_REGISTER_B | mmio | 0xd04061a4 | 0xd04061a4 | CONFIRMED | high | mmio-xref | 7 sites; regA+0xC=0xd04061a4 (mk1 delta 0xC); legacy 0xC0F06014 absent |
| FPS_REGISTER_CONFIRM_CHANGES | mmio | 0xd0406190 | 0xC0F06000 | PLAUSIBLE | med | structural | No 0xC0F0xxxx literal in ROM, but ENGIO builds {reg,val} tables in RAM at runtime (engio_write e06caeb2) so absence is expected; cross-DIGIC fixed reg |
| EVF_STATE | ramptr | 0x6cec | 0x10108 | CONFIRMED | high | structural | CreateStateObject("EvfDevState") @e069a640 (via FUN_e05bb31a, allocs 0x20-byte StateObject) stores the StateObject* to RAM 0x10108; written EXACTLY ONCE at init, read by accessor e069a662; from LiveViewDrive::EvfDevState.c (per-frame LV drive state, init caller e065b5fc). Not the rejected EvfCapState ctx 0x8e8c; "EvfState" string e137558c has 0 xrefs |
| SGI_HANDLERS_PTR | ramptr | 0x402c | 0x4008 | CONFIRMED | high | already-in-stubs | RAM 0x4008 = intr-ctrl struct(0x4004)+4, handler-tbl base, id*8 entries; init'd from ROM e1394360. **In consts.h** |
| MALLOC_STRUCT_ADDR | ramptr | 0x56a1c | 0x2d160 | CONFIRMED | high | already-in-stubs | DryOS malloc V2 struct (lock+0x24, free-list+4, sizes+8/+0x18/+0x1c); 4 literal pools. **In consts.h** |

## READY TO WIRE (verdict CONFIRMED)

These drop straight into `platform/M50mk2.101/stubs.S` (functions) or `consts.h`
(registers / RAM pointers). The 6 marked *(already present)* are already in the tree
and were re-confirmed by this pass — no action needed.

### stubs.S — functions

```
THUMB_FN(0xe05b848e,  edmac_set_address)
THUMB_FN(0xe05b8862,  edmac_set_size)
THUMB_FN(0xe05b88fc,  edmac_set_transfer_mode)
THUMB_FN(0xe05b9690,  StartEDmac_maybe)
THUMB_FN(0xe05b95e4,  ConnectReadEDmac_maybe)
THUMB_FN(0xe05b96b8,  edmac_reset_channel)
THUMB_FN(0xe05b5720,  edmac_reset_boomer_vdkick)
THUMB_FN(0xe0683ca0,  edmac_select_boomer)
THUMB_FN(0xe05b7c48,  RegisterEDmacCompleteCBR)
THUMB_FN(0xe05b7f68,  UnregisterEDmacCompleteCBR)
THUMB_FN(0xe05b8f38,  edmac_reset_packunpack_mode)
THUMB_FN(0xe05b80c2,  edmac_stop_boomer_maybe)
THUMB_FN(0xe06b934e,  PwrMng_WakeSubChips)
THUMB_FN(0xe06b937c,  PwrMng_SuspendSubChips)
THUMB_FN(0xE05BE4BE,  CreateResLockEntry)
THUMB_FN(0xe05be8a6,  LockEngineResources)
THUMB_FN(0xe05be740,  UnLockEngineResources)
THUMB_FN(0xe05cc5fe,  CreateEventFlag_strictly)
THUMB_FN(0xE05B0BA2,  DeleteEventFlag)          /* already present */
THUMB_FN(0xE05B0CC0,  WaitForAnyEventFlag)      /* already present */
THUMB_FN(0xE05B0EBC,  SetEventFlag)             /* already present */
THUMB_FN(0xE05B0EEA,  ClearEventFlag)           /* already present */
```

### consts.h — registers / RAM pointers

```
#define RAW_LV_EDMAC_CHANNEL_ADDR   0xd0420200
#define FPS_REGISTER_A              0xd0406198
#define FPS_REGISTER_B              0xd04061a4
#define EVF_STATE                   0x10108     /* RAM slot: EvfDevState StateObject* */
#define DRYOS_SGI_HANDLERS_PTR      0x4008      /* already present */
#define MALLOC_STRUCT_ADDR          0x2d160     /* already present */
```

## NEEDS MANUAL RE (REJECTED or PLAUSIBLE-low)

| Symbol | Verdict | What's still unknown | Suggested next step |
|---|---|---|---|
| `SHAD_GAIN_REGISTER` | REJECTED | No MMIO evidence: 0 xrefs and 0 byte-pattern matches for 0xd0008030 ROM-wide. mk1 value is flagged as wrong (inherited from 200D / DIGIC-4). True D8 shadow/gain register address unknown. | Not required for basic raw capture. If needed, RE the ADTG/CMOS/shad path via the `AdtgSet`/`shad` eventproc handlers and diff against a known D8 (200D/SX740) gain-reg map rather than the DIGIC-4 constant. |
| `FPS_REGISTER_CONFIRM_CHANGES` | PLAUSIBLE | No 0xC0F06000 (or any 0xC0F0xxxx) literal, movw/movt, or data word anywhere in ROM. But ENGIO (`DSEngIO.c`, engio_write FUN_e06caeb2) builds `{reg,val}` tables in RAM at runtime, so the 0xC0Fx confirm reg is never ROM-resident — absence is expected, not disproof. 0xC0F06000 is the cross-DIGIC fixed FPS confirm register. | Accept 0xC0F06000 as the working value (matches every DIGIC incl. D8 siblings) and validate empirically on-target: after writing FPS_REGISTER_A/_B, poke 0xC0F06000 and confirm the frame timer latches. No further static RE possible from this ROM. |

## Notes

- All 6 `already-in-stubs` entries were re-verified against the current tree
  (`platform/M50mk2.101/stubs.S` lines for the 4 event-flag fns; `consts.h` for
  `DRYOS_SGI_HANDLERS_PTR` and `MALLOC_STRUCT_ADDR`) — values match, no edits needed.
- The Boomer magic constant `0x373f0126` (mk1) is **absent** from gang100; D8 Boomer
  select/stop are resolved structurally via `Engine::BoomerSelector.c` instead.
- Legacy DIGIC-4/5 FPS register `0xC0F06014` is absent; D8 relocates the readable FPS
  timers to the `0xd04061xx` block (A/B confirmed above).
- **Closing pass (2 previously-REJECTED symbols resolved):**
  - `edmac_set_address = 0xe05b848e` — found by tracing set_size (e05b8862) callers.
    `FUN_e02cdd10` iterates a scatter-gather list of 8-byte `{addr@+0, size@+4}`
    entries and calls `set_size(ch, size)` then `FUN_e05b848e(ch, addr)`.
    `FUN_e05b848e` is the tiny twin leaf `*(*(chanTbl e05b86e4 + ch*8) + 0xa0) = addr`.
    The earlier "only a +0x08 getter" note was wrong: the shadow-struct address field
    is at `+0xa0` (A-bank), not the raw-MMIO `+0x08`; the B-bank setter `FUN_e05b8ae4`
    uses the parallel `+0xa4` for its address, corroborating the offset family.
  - `EVF_STATE = 0x10108` — `FUN_e069a640` (EvfDevState init) calls
    `CreateStateObject`-equivalent `FUN_e05bb31a(name="EvfDevState", 0, table, 0xf, 4)`
    (allocates a 0x20-byte StateObject) and stores the returned pointer to RAM `0x10108`.
    Xrefs to `0x10108`: one WRITE (init) + reads only → written once at init. Its init
    is invoked from the `LiveViewDrive::EvfDevState.c` driver bring-up (`FUN_e065b5fc`),
    i.e. the per-frame LV drive state ML hooks for vsync. The rejected `0x8e8c` was the
    `EvfCapState.c` context (not a StateObject); the `EvfState` string (e137558c) is
    unreferenced.
