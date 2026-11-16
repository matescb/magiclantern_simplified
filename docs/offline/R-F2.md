# R-F2 - second-agent review of `offline-edmacstubs` (wt/F2, commit 3ba436b42)

Static review only. Nothing here is proven on the camera. The branch was not modified.
Not done (15-minute box): no rebuild (the existing build artifacts in wt/F2, timestamped at
the commit time, were gated instead); no mk1 / SX740 / R ROM is on this machine, so reference
function bodies could not be read, only their `stubs.S` addresses and `edmac-d8.c` usage.

## Verdict

**ready for a hardware boot test.** The CPU copy stays the default (`m50ii_edmac_cpu_copy = 1`,
`edmac-d8.c:247`), so a boot never reaches the re-pointed stubs; no ML core symbol moves. The
EDMAC mem2mem path itself is NOT cleared for recording: exercise it only through the manual
self-test in F2.md section 6, expecting the two open risks listed under MEDIUM.

## What was re-derived (all from `gang100.bin` with `tools/romdis.py`)

| Stub | Address | Result |
|---|---|---|
| `edmac_select_boomer` | 0xE05B802E | CONFIRMED. (ch, sel); table 0xE0EC3BD4 stride 12; asserts Edmac.c 0x301 / 0x304 / 0x306; `reg+0xB4 := 2`; tail call 0xE0683CA0 with `row[0] | (sel & 0xFF)`. = `stop_boomer - 0x94`, the mk1 and SX740 delta. |
| selector 0x373F0126 | `edmac-d8.c:90` | CONFIRMED. Row 13 = {0x000C0000, 0x100, 0x20000000}; ch13 descriptor {0xD0420C00, flags 5}. 0x0100 == row[+4]; 0x373F0000 & 0x20000000 != 0. 0x373F0226 would assert 0x304. |
| `edmac_reset_boomer_vdkick` | 0xE05B7FB6 | Body CONFIRMED (one arg, assert 0x2E7 if node == -1, tail call 0xE05B56CC). = `UnregisterCBR + 0x4E` as on mk1 and SX740. Which of the three siblings (7FB6 off / 7FDE on / 8006 pulse) mk1 uses stays inferred. |
| `StartEDmac_maybe` | 0xE05B82AE | Body CONFIRMED. Calls 0xE05B7B08 (`reg+4 := 0`), 0xE05B7AA4 (reset pulse on +0x18); flag bit0 clear -> tail call 0xE05B7A6C (`reg+4 := 1`); bit0 set -> `reg+0xB4 := 1` when `(+0xC4 & 0x80) == 0`. No assert for either direction. |
| `ConnectReadEDmac_maybe` | 0xE05B7A32 | See next section: right on semantics, not provable without a reference ROM. |
| `LockEngineResources_blocking` / `DeleteResLockEntry` | 0xE05BE6A6 / 0xE05BE5A2 | Unused in C (grep: no reference). Callers 105 / 28 reproduced. Not otherwise re-checked. |

Caller counts (my scan, every Thumb BL and B.W in the ROM): 0xE05B7A32 = 63 BL + 93 B.W = 156;
0xE05B82AE = 265 + 2 = 267; 0xE05B802E = 130 + 1; 0xE05B7FB6 = 139 + 30 = 169; 0xE05B7B08 = 5 BL;
0xE05B9690 = 1; 0xE05B95E4 = 2; 0xE05BE8A6 = 0. **F2's numbers are reproducible**; the critic's
63 for 0xE05B7A32 counted BL only and missed 93 tail calls.

## ConnectReadEDmac: 0xE05B7A32 or 0xE05B7B08

Conclusion: **0xE05B7A32 is the right stub for how `edmac-d8.c` uses it; 0xE05B7B08 is excluded on
function, not on position.** The mk1 identity cannot be proven statically here (no mk1 ROM).

- 0xE05B7B08 is a 12-byte leaf: `reg+4 := 0` for the channel. Its only callers are five BLs inside
  Engine::Edmac.c (0xE05B82B2, 8326, 83D2, 96D6, 976A); the first is `StartEDmac_maybe`'s own
  opening "disable before reset" step.
- `edmac-d8.c:186-188` runs Start(WR 13), Start(RD 46), ConnectRead(RD 46), identical on SX740,
  R and M50 mk1. Channel 46 has flags 6 (bit0 clear), so Start(46) ends with `reg+4 := 1`. With
  0xE05B7B08 as the last call, the read channel would be disabled again immediately and the copy
  could never complete. The copy works on mk1 with this call order, so the mk1 function is not a
  disable leaf.
- 0xE05B7A32 takes one argument, asserts Edmac.c 0x3AC unless flag bit1 (channel 46: set, no
  assert), writes `reg+8 := 0x12`, `dsb`, writes 0 to 0xC1100730, `reg+0xB4 := 1`. That matches
  kitor's description in `R.180/edmac-d8.c:59-63` ("takes only one arg: read edmac channel"); a
  generic leaf has nothing read-specific.
- Canon uses exactly this order at 0xE00BF3FE-0xE00BF450: 0xE05B82AE on `[0x1A724+0x10]`, `[+8]`,
  `[+0xC]`, then 0xE05B7A32 on `[+8]` and `[+0xC]` (second one a B.W tail call).
- The positional rule (`reset_channel - 0x3A`) is not evidence either way: 0xE05B7A32 is itself
  0x3A bytes long, and local layout changed between bodies (`Register - reset_channel` is 0xEA on
  mk1 and SX740, 0x106 on mk2; `Unregister - Register` 0x398 / 0x394 against 0x320).
- Residual doubt: the old stub 0xE05B95E4 is a sibling with the same shape (read-only assert
  0x74D, clears 0x300 in +0xC4, same 0xC1100730 kick, `+0xB4 := 1`), paired with 0xE05B9690
  (`reg+8 := 0x11`) in one Canon path (0xE0275D56, 0xE0275F72). Whether mk1's function writes 0x12
  or the 0x11 variant is undecidable here. F2's fallback order (F2.md line 243) covers it.

## Call-by-call check of the mem2mem path (`edmac-d8.c:160-199`)

| Line | Call | Against the ROM |
|---|---|---|
| 160-161 | `edmac_reset_channel(46)`, `(13)` | 0xE05B7B42, one arg, assert 0x288 if ch >= 0x4C. OK (stub unchanged). |
| 164 | `edmac_reset_boomer_vdkick(13)` | one arg; row 13 node 0xC0000 != -1, no assert 0x2E7. OK. |
| 165 | `edmac_select_boomer(13, 0x373F0126)` | passes 0x301 / 0x304 / 0x306. See MEDIUM 1. |
| 178-184 | set_address / set_size / transfer_mode | stubs unchanged, not re-derived. |
| 186 | `StartEDmac_maybe(13)` | flags 5, bit0 set: reset pulse, `+0xB4 := 1`; `reg+4` is left 0. Same as Canon's write channel at 0xE00BF404. OK. |
| 187 | `StartEDmac_maybe(46)` | bit0 clear: `reg+4 := 1`. OK. |
| 188 | `ConnectReadEDmac_maybe(46)` | bit1 set: no assert. OK. Never pass channel 13 (assert 0x3AC). |
| 199 | `edmac_stop_boomer_maybe(13)` | 0xE05B80C2: `+0xB4 := 0`, selector register := 0 via 0xE0683CA0. OK. |

## Findings

**CRITICAL / HIGH:** none.

**MEDIUM**
1. 0xE0683CA0 (reached from `edmac-d8.c:165`) asserts `BoomerSelector` line 0x10C when the mux
   register of node 0xC (0xD042327C per F2) is already nonzero and the new value is nonzero
   (0xE0683CAE-0xE0683CBE). If Canon LiveView has write channel 13 connected at the time of the
   copy, the first mem2mem call asserts. F2 notes the assert but the hardware plan should read
   0xD042327C before the first self-test. Unknown until hardware.
2. `ConnectReadEDmac_maybe` 0xE05B7A32 vs its sibling 0xE05B95E4 (`+8 := 0x12` vs the 0x11 path):
   not decidable statically. Expected failure mode is a 50 ms timeout (`edmac-d8.c:191`), not a crash.
3. `stubs.S` comment for `ConnectReadEDmac_maybe` calls 0xE05B7B08 "the mk1 positional homolog".
   Position proves nothing here (above); the comment should say "excluded: disable leaf".

**LOW**
4. `edmac-d8.c:60` keeps kitor's stale comment "call only for write channel!" with parameter name
   `wr_channel`; the function asserts on a write channel. Misleading for the next editor.
5. `tools/symgate/check_symbol_stability.py` on the existing wt/F2 build prints `RESULT: FAIL`:
   0 ML core symbols moved, 0 removed, all section ends OK, 2 added (the new names), 4 ROM stubs
   moved (exactly the intended ones). The FAIL is the gate's rule for any moved symbol and is
   expected here, as F2.md lines 150-161 state. Modules importing these four names must be built
   against this `.sym`.
6. F2.md 3.3 says Start "for a read channel tail-calls 0xE05B7A6C". The test is on flag bit0
   (clear -> enable), not on the read bit1. Same outcome for channels 13 and 46.

## Files

- Branch: `/mnt/data/WORK/eos/wt/F2` (`platform/M50mk2.101/stubs.S`, `platform/M50mk2.101/edmac-d8.c`)
- Reviewed report: `/mnt/data/WORK/eos/docs/offline/F2.md`
- References: `/mnt/data/WORK/eos/research/bilal_M50_rawvideo/platform/M50.110/stubs.S:229-246`,
  `/mnt/data/WORK/eos/research/kitor_edmac_sx740/platform/SX740.102/stubs.S:286-299`,
  `/mnt/data/WORK/eos/research/kitor_edmac_sx740/platform/R.180/edmac-d8.c:59-69`
