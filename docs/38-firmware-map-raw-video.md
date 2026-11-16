# 38 - How LiveView raw works on the EOS M50 Mark II (fw 1.0.1) and what an ML recorder must respect

Status: synthesis of the offline sprint reports in `docs/offline/` (A1, A2, A3, A5, A6, A7, A8, A9, A10,
A11, A12, A13, A15, B1-B5), written by agent D1. **No camera was attached during the sprint. Everything
marked Observed is observed in the ROM image, in ML source, or in previously captured UART logs; none of
it was re-run on the camera. Nothing in this document is proven on hardware unless it explicitly quotes
an earlier hardware log.**

Conventions used throughout:

- ROM0 is `m50ii_dumper/dump_1.0.1/gang100.bin`, loaded at 0xE0000000; all code is Thumb, so a function
  at 0xE0xxxxxx is called at the odd address 0xE0xxxxxx+1. ROM1 is `gang200.bin` at 0xF0000000.
- **[O]** = Observed (a ROM address, a source `file:line`, or a log line backs it).
  **[I]** = Inferred (a reading of observed facts; could be wrong).
  **[U]** = Unknown (nobody established it).
- "(A1)", "(A5-verify)" etc. name the sprint report, or its "Independent verification" section, that the
  claim comes from. Claims that a verification section marked REFUTED are not repeated here; where the
  refutation changes a conclusion, the corrected statement is given and marked "corrected".
- "D1 re-check" marks a point where two reports disagreed, or a report left a gap, and I re-read the ROM
  with `tools/romdis.py` during this synthesis.
- A glossary is in section 12.

---

## 1. One-page summary

1. LiveView (LV) raw on this camera is a side tap of Canon's always-running capture pipeline. Calling the
   event procedure `lv_save_raw(1)` builds nothing; it sets one flag in a module called Mem1Path. From
   then on, once per frame, Mem1Path points EDMAC write channel 3 (register block 0xD0420200) at a buffer
   supplied by the LiveView core and the sensor raw data is written there. `lv_set_mm(1)` selects raw
   instead of YUV for that tap. (A1; B1 for the hardware-confirmed recipe on EOS R.)
2. The pipeline is driven by two Canon tasks, EvfCap (priority 0x0D) and EvfDev (priority 0x0E), each a
   message-queue task with a 200-entry queue. Canon posts to those queues with a non-blocking call and
   asserts if the post fails. Any ML code that blocks or burns CPU inside those tasks, or that starves
   the tasks just below them, ends in a Canon assert or an interrupt-stack overflow. (A2, A3, A11.)
3. The "MvRawPro storm" asserts (`MvRawProDs.c` line 0x125, `RawPro.c` line 0x1C4, `LvJob.c` line 0xD5)
   are overflow checks in Canon's normal capture pipeline. They mean "frame kicks kept arriving but the
   EDMAC completion callbacks stopped draining". They are fatal on stock firmware; they only repeat
   because ML installs a handler that returns. (A1, A1-verify.)
4. ML's EDMAC memory-to-memory copy never worked on this body because two stubs pointed at functions of
   a different Canon module (`Engine::Outer.c`), not at the EDMAC functions. The CPU copy is slow
   (about 5.7 MB/s measured) because of an alignment accident, not because of hardware. (A5, B5, A13.)
5. The card bus already runs at UHS-I SDR104; the one benchmark on record gives 75 MB/s write. Full-size
   14-bit frames at 24 fps need 102 MB/s, so continuous recording needs a crop. (A7, B3.)
6. Canon does not remount the card when H.264 recording starts; the two "functions" that theory was
   built on are text strings. (A6.)
7. Several constants in the ML tree are wrong for this body: the frame-timer clock (27 MHz, not 32 MHz),
   the EVF state object, the SRM allocator addresses, the bit-depth and digital-gain registers, the MVR
   structure pointer, the resource-lock function. Section 11.1 lists them.

---

## 2. The per-frame pipeline: sensor readout to Canon's raw buffer

### 2.1 Layers, top to bottom

| Layer | Canon source file (from assert strings) | Context / object | Runs in task |
|---|---|---|---|
| EvfState, the EVF capture state machine | `LiveView::EvfCapState.c` | state object pointer at RAM `*(0x8E8C)`, matrix 0xE0B5050C, 17 inputs x 8 states | EvfCap, priority 0x0D |
| EvfDevState, the "develop" state machine | `LiveViewDrive::EvfDevState.c` | state object pointer at RAM `*(0x10108)`, matrix 0xE0BBE0F0, 15 inputs x 4 states | EvfDev, priority 0x0E |
| LvJob pool | `LiveViewCore::LvJob.c` | context 0x9A04; 6 job slots at 0x50AF8, stride 0x188 | called from EvfDev (`evfdevSendJob`) |
| CapPath | `LiveViewDrive::Path.c` | context 0x10220 | [U] |
| MvRawProPath -> MvRawProDs -> RawPro | `LiveViewDrive::MvRawProPath.c`, `Engine::MvRawProDs.c`, `Engine::RawPro.c` | contexts 0x1910C, 0x23800, 0x292B0 | [U] (VD interrupt or EVF task; A1 lists this as unknown) |
| Mem1Path, the raw-to-memory tap | `LiveViewDrive::Mem1Path.c` | context 0x10238 | called from CapPath |
| ImagerState, Canon's own consumer of the tap | `LiveView::ImagerState.c` | context 0x93A4 | Imager, priority 0x14 |
| Sensor programming | `ImageSequence::ImageSequenceLv.c`, `ImageSequence::Sio0Driver.c`, `EngineDS::DSEngIO.c` | drive mode at RAM 0x10654 | [U] |

All addresses [O] (A1, A2, A10, A11). Which task executes `MvRawProPath_Start` is [U] (A1).

### 2.2 The two state machines

A *state object* is Canon's table-driven state machine: a matrix of {next state, action function} cells
indexed by (input, current state). The generic dispatcher at 0xE05BB2B2 looks up
`matrix + (input * max_states + current_state) * 8`, writes the next state **first**, then calls the
action (0xE05BB2EC..0xE05BB2FA) [O] (A2, A2-verify). ML's "state object hook" replaces the dispatcher
pointer at object+0x0C, so ML's hook runs inside whichever Canon task dispatches that object.

**EvfState** (`*(0x8E8C)`), created at 0xE06EEC38 with `CreateStateObject("EvfState", 0, 0xE0B5050C,
0x11, 8)` [O] (A2; D1 re-check: 0xE06EEC3C loads 0xE0B5050C, 0xE06EEC4C loads 0x8E8C). The cells that
matter in steady LiveView (state 5, inferred) are:

| Input | Action | Debug-string name | What it does |
|---|---|---|---|
| 3 | 0xE06EF37A | `evfVdInterrupt` (name per A2-verify; A2 called it unnamed) | per-frame vertical-drive tick; posts event 8 to EvfDev (0xE06EF3A6) |
| 4 | 0xE06EF51E | `evfSetParamInterrupt` | posts event 6 to EvfDev (0xE06EF6EE) |
| 5 | 0xE06EF732 | `evfReadOutDoneInterrupt` | sensor readout of the frame is finished |
| 6 | 0xE06EF7E8 | `evfResetPSaveInterrupt` | may post event 7 to EvfDev (0xE06EF818), condition-dependent |

Inputs 4, 5 and 6 are self-transitions in states 3..6 [O] (A2-verify, raw matrix dump).

**EvfDevState** (`*(0x10108)`), created by 0xE069A640 [O]. In state 3 ("sync running", name inferred):

| Input | Action | Debug-string name |
|---|---|---|
| 6 | 0xE069B2EA | `evfdevUpdateParam` |
| 7 | 0xE069B628 | `evfdevStartPath` |
| 8 | 0xE069AEDE | `evfdevSendJob` (creates the LvJob for the frame; strings `LV_JOB_CreateJob[FrameNo:%d]`, `32 frames continuous EngineError!!!`) |
| 13 | 0xE069B812 | unnamed sync tick, posted by 0xE069A7E8, a callback registered at `evfdevSyncStart` |

**Which one is ML's "EVF_STATE"?** Reports disagree in emphasis. B5 recommends keeping the port's current
definition, `EVF_STATE = *(0x10108)` with the vsync hook on (input 6, state 3), because it is backed by
an earlier on-camera state-spy run (`platform/M50mk2.101/include/platform/state-object.h:7`,
`src/state-object.c:225`). A2 shows that 0x10108 is EvfDevState and that (6, 3) is `evfdevUpdateParam`,
a parameter update that EvfCap posts *before* the frame is read out. D1 re-check of the ROM: A2 is right
about identity. EvfDevState has only 4 states, so generic ML tests such as `input == 5 && old_state == 5`
(`src/state-object.c:173, 227, 250`) can never match on it, and the object whose cells match ML's
generic meanings cell for cell is EvfState at `*(0x8E8C)` [O]. So: the current hook fires roughly once
per frame (that is what the state spy saw) but it is not the readout-done event, and it runs in the
EvfDev task. Whether hooking EvfState instead is safe on this body is [U]; B1 records that hooking the
EVF state crashed an EOS RP in 2022, and that M50 mk1's `EVF_STATE` (`*(0x6cec)`) being the EvfState
object is itself only inferred (no mk1 ROM is on disk, A2-verify).

### 2.3 What happens each frame (as far as the ROM shows)

1. Vertical-drive interrupt -> EvfState input 3 -> `evfVdInterrupt` -> EvfDev event 8 -> `evfdevSendJob`
   -> `LvJob_CreateJob` 0xE06AF4B2 takes one of 6 job slots [O] (A1, A2).
2. Set-parameter interrupt -> EvfState input 4 -> EvfDev event 6 -> `evfdevUpdateParam` [O] (A2).
   CapPath's per-frame SetParam at 0xE06CFC2C calls the SetParam of each path module and tail-calls
   `Mem1Path_SetParam` 0xE0754298 (from 0xE06CFC46) [O] (A1). The exact caller chain from
   `evfdevUpdateParam` to `CapPath_SetParam` was not traced [U].
3. `MvRawProPath_Start` 0xE076FDF2 calls `MvRawProDs_PushFrame` 0xE07B9CB0, which pushes an
   "expected completions" bit mask into a 2-entry ring and tail-calls `RawPro_PushFrame` 0xE0760E74,
   which does the same in its own 2-entry ring [O] (A1, A1-verify).
4. The EDMAC channels finish their transfers; their completion interrupts call
   `RawPro_EDmacCompleteCBR` 0xE0760A30, which pops the RawPro ring and calls up to
   `MvRawProDs_SubCompleteCBR` 0xE07B9AD0, which pops the MvRawProDs ring and calls
   `MvRawProPath_CompleteCBR` 0xE076FCC4, then `CapPath_CompleteCBR` 0xE06CFA70 [O] for each link
   except the last, which A1-verify did not re-check.
5. Readout-done interrupt -> EvfState input 5 -> `evfReadOutDoneInterrupt` [O] (A2). Its phase relative
   to the channel-3 completion is [U].
6. If the raw tap is armed, EDMAC channel 3 completes and its completion callback (trampoline
   0xE0753D9C) calls the function stored at Mem1Path+0x64 with the argument at +0x68. Canon stores
   `imagerMem1PathWrDmacCompleteCBR` 0xE06AD51A there, which only gives a semaphore, depending on the
   mode word at `0x93A4+0xC` [O] (A1, A1-verify).

### 2.4 The completion rings, and what the three asserts mean

| Ring | Count | Entries | Accumulator | Overflow report | Popped by |
|---|---|---|---|---|---|
| MvRawProDs (ctx 0x23800) | +0x04 | +0x34, +0x38 | +0x20 | `MvRawProDs.c` line 0x125 at count >= 2 | 0xE07B9AD0 |
| RawPro (ctx 0x292B0) | +0x00 | +0x64, +0x68 | +0x28 | `RawPro.c` line 0x1C4 at count >= 2 | 0xE0760A30 |
| LvJob pool (ctx 0x9A04) | - | 6 slots at 0x50AF8 + n*0x188, word 0 non-zero = free | - | `LvJob.c` line 0xD5 when none free | job completion (not traced) |

All [O] (A1, A1-verify). Details that matter to a recorder:

- Ring entries are bit masks of the completions expected for one kick, not frame pointers [O].
- The pop is an **exact-equality** test: completion bits are ORed into the accumulator and the ring is
  popped only when accumulator == ring[0]. A completion bit that is not in ring[0] can never be cleared,
  so the drain would stall permanently, not for one frame [O for the code; I for the consequence]
  (A1-verify).
- On overflow each function first asks 0xE06C811E for a "soft assert" flag (`[0x22D9C+8]`). The UART
  history shows the **fatal** path was taken (file strings `Engine::MvRawProDs.c` / `Engine::RawPro.c`,
  never the soft-path strings `EPM/Capture/...`), so the flag was 0 at run time [O] (A1-verify,
  `tools/mlserial/run/uart.jsonl`). *Corrected:* A1's body explained the repeating storm by the soft
  path; that explanation is refuted. The lines repeat only because ML's
  `m50ii_nonfatal_assert_handler` returns from Canon's assert (`src/init.c:1238-1262`). On stock
  firmware the first overflow would hang the camera at 0xE06745AA. After the first such line the
  pipeline is in a state Canon never continues from, so later symptoms (line 0xD5, `[kern] N interrupt
  occurs!`) may be secondary damage [I].
- *Corrected:* with no drain at all, a report fires on every second push after the first one, not every
  third (A1-verify). The handler stops logging after 32 lines, so line counts in the log say nothing
  about rate or duration [O] (`src/init.c`, `logged < 32`).
- That *only* 0xE0760A30 pops the rings is [U]: other code references 0x292B0 (0xE0761750, 0xE076184C)
  and 0x23800 (0xE07BA224) and was not audited (A1-verify).

RawPro registers its completion callback on two EDMAC channels: `RegisterEDmacCompleteCBR([0x292B0+0x30],
0xE0760A31, 2)` at 0xE0760B6C and `RegisterEDmacCompleteCBR([0x292B0+0x34], 0xE0760A31, 1)` at
0xE0760CBA [O] (A1-verify). D1 re-check of `MvRawProDs_Create` 0xE07B9B28: the configuration block it
passes has word 0 = 0x00120021 (mode), word 1 = 0x29 (0xE07B9B92..0xE07B9B98) and word 2 taken from the
caller's configuration `[cfg+0x10]` (0xE07B9BA6); RawPro stores word 2 at +0x30 and word 1 at +0x34. So
one RawPro channel is **0x29 = channel 41, a read channel** (register block 0xD0421100 in the table of
section 4), and the other is a run-time value that is not a ROM constant [O]. The alternative
configuration uses 0x4C (0xE07B9B8C); since valid channels are 0..75 (0x4B), 0x4C is the "no channel"
value, which fits the second registration being skipped in those modes [I]. A15's hardware check that
expects "0x29 / 0x4C" in `0x292E0` should therefore be read as "0x29 in +0x34, an unknown channel in
+0x30".

The flags the `raw_vidx` module currently polls as "raw job ready", `*(0x23800+0x30) == 1` and
`*(0x292B0+0x30) != 0` (`modules/raw_video/raw_vidx/worker.c:496-510`), are a mode argument stored when
the pipeline is created and an EDMAC channel number. They do not change per frame and say nothing about
pipeline health [O for what they are; I that they pass whenever LV is up] (A1, A1-verify).

### 2.5 Where the raw buffer address comes from

- `Mem1Path_SetParam` 0xE0754298 receives a parameter block from the LiveView core. If Mem1Path is set up
  (`[0x10238+0x50] != 0`), the tap is armed (`[0x10238+0x40] != 0`) and `param[8] != 0`, it stores
  `param[8]` at Mem1Path+0x58 (RAM 0x10290), latches the first such value at +0x5C (RAM 0x10294) and
  sets +0x60 = 1, then programs channel 3 [O] (A1, A1-verify).
- D1 re-check of the calls in that function: 0xE0754336 `bl 0xE05B93C4` (channel 3 plus the block at
  ctx+0x6C), 0xE075433E `edmac_set_size`, 0xE0754346 `edmac_set_address`, 0xE075434C disconnect
  (0xE05B80C2), 0xE0754354 `bl 0xE05B802E` (the EDMAC input select of section 4), then three engine
  calls [O]. So the buffer address is whatever the LiveView core puts in `param[8]`; Canon rewrites the
  channel every frame.
- If the tap is not armed or `param[8] == 0`, the function disconnects channel 3 (0xE07542E6 ->
  0xE05B80C2) [O].
- Line length is `width * 7 / 4` bytes (14 bits per pixel); width and height are picked from Mem1Path
  fields by "raw type" `[0x10238+0]` [O] (A1). Getters: 0xE0754424 returns +0x58, 0xE0754436 returns
  +0x5C, 0xE0754402 returns `w*h*7/4` [O] (A1).
- Earlier hardware logs: the buffer seen on this camera in 1080p standby is 0x6BC61CC8 through the
  uncached alias (0x2BC61CC8 cacheable), read from the channel-3 address register 0xD04202A0, with
  size register 0xD0420250 = 0x048B0E54 (1164 lines, 3668 bytes per line = 2096 pixels at 14 bit)
  (docs/35:26, docs/31:420, quoted in A8, A9). On EOS R the address "doesn't change ... even between
  video and photo modes, just dimensions" (B1, kitor, hardware on R, not on this body).
- [U]: whether `param[8]` is non-zero in plain LiveView and who supplies it (A1); which Canon memory area
  owns 0x2BC61CC8 - it lies 0x5ACC8 bytes past the end of a 0x2AB1000-byte area that would start at
  0x29156000, and before the next area base 0x2F0D7000 (A8-verify).

Because Canon overwrites the same single buffer every frame, a copy that takes longer than the interval
between channel-3 completions mixes two frames (tearing), whatever the copy method [I] (A13).

---

## 3. What `lv_save_raw` and `lv_set_mm` really do

Both are *event procedures* (Canon functions registered by name and callable through `call("name", ...)`).

| Name | Handler | Effect (all [O], A1 and A1-verify) |
|---|---|---|
| `lv_save_raw(arg)` | 0xE06C0D04 | stores arg at `[0x19094+4]`; if arg != 0 and `[0x19094+0xC]` and `[0x19094+0x10]` are both non-zero, clears those two words (meaning unknown); tail-calls 0xE075442A, which is `str r0,[0x10238+0x40]; bx lr` |
| `lv_set_mm(arg)` | 0xE0753D86 | `[0x10238+0x44] = arg`; `[0x10238+0x78] = (arg == 0)`. The block at ctx+0x6C (which contains +0x78) is the one passed to 0xE05B93C4 for channel 3 |

Mem1Path also registers `lv_af_raw`, `lv_rshd_raw` and `lv_set_raw` in `Mem1Path_Init` 0xE0753DAA [O].

What they do **not** do:

- They do not create a job, allocate a buffer, or register a callback. `Mem1Path_SetPath` 0xE0754236 sets
  up channel 3 (reset, transfer mode 1, `RegisterEDmacCompleteCBR(3, 0xE0753D9D, 0)`) whenever the
  capture path is set, without testing the `lv_save_raw` flag [O] (A1-verify).
- They are not what the MvRawPro asserts check. Those asserts are in the always-running capture pipeline
  (section 2.4).
- No acknowledgement, release or buffer return is owed by whoever consumes the frame, as far as Canon's
  own consumer shows: it only waits on a semaphore given from the channel-3 completion callback
  [I from one function] (A1-verify marks "only consumer" and "no ack" as unverifiable).

Meaning of the arguments, from other bodies (hardware-confirmed on EOS R by kitor, B1; not on this
camera): `lv_set_mm` 0 = YUV, 1 = raw; `lv_set_raw_wp` selects where in the processing path the raw is
taken (default 0, not needed); `lv_save_raw 1` "starts raw to mem edmac transfers". Without
`lv_set_mm(1)` the same path delivers YUV. The port's enable sequence `lv_set_mm(1)` + `lv_save_raw(1)`
matches the working M50 mk1 code (`research/bilal_M50_rawvideo/src/raw.c:2456-2461`) [O] (A1, B1, B5).

Rules that follow for a recorder:

1. Never call `RegisterEDmacCompleteCBR`, redirect, reset or abort on channel 3, channel 41 (0x29), or
   whatever channel `[0x292B0+0x30]` holds. `RegisterEDmacCompleteCBR` keeps one callback per channel
   (RAM table 0x83F24), so an ML registration replaces Canon's and stops the drain [O for the table; I for
   the consequence] (A1, A5). This matches the two earlier hardware fixes: removing the ML callback
   "seed" and turning `raw_lv_redirect_edmac` off (memory note `m50ii-symbol-shift-boot-crash.md`).
2. A frame-ready signal that respects Canon's design is the channel-3 completion: chain the function
   pointer at RAM 0x1029C (Mem1Path+0x64; argument at 0x102A0), calling the saved Canon pointer
   (expected 0xE06AD51B) afterwards. This is an untested proposal (A1 hardware check 9) [I].
3. Start-of-recording health check: ring counts `[0x23804] < 2` and `[0x292B0] < 2` over several frames,
   and not all six LvJob slots zero (A1) [I].
4. `[0x10290]` is Canon's own record of the address it programmed this frame, a better source for the
   buffer address than the hardware register [I] (A1).

---

## 4. EDMAC on this body

EDMAC is Canon's DMA engine that moves image data between processing modules and memory. A *write
channel* writes module output to memory; a *read channel* feeds memory into a module. *Boomer* is the
input multiplexer in front of each write channel: a "selector" register chooses which source feeds it.

### 4.1 Tables (all ROM0, [O], A5 and A5-verify)

| Address | Layout | Content |
|---|---|---|
| 0xE0EC37AC | 8 bytes x 76 | channel -> {register block base, capability flags}; channels 0-38 are write (flag bit 0), 39-75 read (bit 1) |
| 0xE0EC3BD4 | 12 bytes x 76 | channel -> {boomer node id << 16, input-select type, class bit} |
| 0xE0EC3F64 | 8 bytes x 76 | channel -> {interrupt id, ISR}; ISR 0xE05B97D1 (write) or 0xE05B9889 (read) |
| 0xE1086394 | 4 bytes x 0xE1 | boomer node -> selector register |
| 0xE1087A8C | 20 bytes x 56 | `Engine::Outer.c` port table - **not** EDMAC channels |
| RAM 0x83F24 | 8 bytes x 76 | completion callback {function, context}; idle function 0xE05B7C73 (`bx lr`) |
| RAM 0x84184 | 8 bytes x 76 | a second callback table (0xE05B83F2); meaning not established |
| RAM 0x83B94 | 4 bytes x 76 | per-channel "hold" bit mask |

RAM 0x83F24 is in the zero-filled `.bss`, so it is 0 at boot and filled by Canon's EDMAC init (A15).

### 4.2 Channels with a known owner

| Channel | Dir | Register base | Owner | Evidence |
|---|---|---|---|---|
| 3 | W | 0xD0420200 | Mem1Path: the LV raw tap | 0xE075426A, 0xE0754298; table word at 0xE0EC37C4 = 0xD0420200 (D1 re-check). This settles A1's open question whether `RAW_LV_EDMAC_CHANNEL_ADDR 0xD0420200` is channel 3: it is |
| 2 | W | 0xD0420100 | DirectCapture "Mem1Direct" | 0xE0650F0A (A12) |
| 5, 6 | W | 0xD0420400, 0xD0420500 | SantaPath (dual-pixel AF raw, "DafRaw") - see note | select calls 0xE077038C (ch 5, selector 0x3F7F0114) and 0xE0770396 (ch 6, 0x3F7F0115) (A5, A5-verify) |
| 20 | W | 0xD0474000 | unknown Canon path | 0xE03BEDDE, selector 0x00400401 (A5) |
| 41 (0x29) | R | 0xD0421100 | RawPro (LV raw processing), second completion channel | 0xE07B9B96, 0xE0760CBA (section 2.4) |
| 13 / 46 | W / R | 0xD0420C00 / 0xD0421600 | chosen by ML for memory-to-memory copy (`platform/M50mk2.101/edmac-d8.c:83-84`); no Canon user found | A5 |

Note on channels 5 and 6: A12 attributes them to MvRawProPath; A5 only names the call sites. D1 re-check
of the string pools: `LiveViewDrive::MvRawProPath.c` strings end at 0xE0770264, and the next file strings
after the code at 0xE0770302..0xE077067B are `LiveViewDrive::SantaPath.c` (0xE0770694) and
`SetParam is not completed, DafRaw don't output` (0xE07706D8); CapPath's init calls 0xE0770302 as a
separate module after MvRawProPath's init (A1). So the call sites 0xE0770338..0xE0770486 are most likely
in SantaPath.c, not MvRawProPath.c [I, medium-high]. A12's resource-lock ID lists are unaffected (they
are keyed by the Create call site, 0xE07701A8 for MvRawProPath and 0xE0770818 for SantaPath).

The full 76-row channel map is in A5. Channel register fields seen in code: +0x04 enable, +0x08 state
(0x11 read-prepared, 3 write-prepared, 0 aborted; exact meaning [U]), +0x3C interrupt enable, +0x50 size
(lines-1 << 16 | bytes per line, from the hardware read above and B1), +0xA0 address, +0xB4 connect state
(0, 1, 2; meaning [U]), +0xC0 transfer mode. The DIGIC 4/5 `struct edmac_mmio` layout does not apply
(B1, kitor on EOS R).

### 4.3 API addresses on 1.0.1 ([O], A5; rows marked * were re-read by A5-verify)

| Role | Address | Current ML stub |
|---|---|---|
| set address (reg +0xA0) * | 0xE05B848E | correct |
| set size / geometry | 0xE05B849A | correct |
| transfer mode | 0xE05B88FC | - |
| prepare channel | 0xE05B7B42 | - |
| enable leaf (reg +4 = 1) * / disable leaf * | 0xE05B7A6C / 0xE05B7B08 | - |
| **write-channel input select (real "boomer select")** * | **0xE05B802E** (channel, selector) | **wrong: stub points at 0xE05B752E** |
| write-channel disconnect | 0xE05B80C2 | correct (`edmac_stop_boomer_maybe`) |
| VdKick off / on / pulse, per EDMAC channel * | 0xE05B7FB6 / 0xE05B7FDE / 0xE05B8006 | **wrong: stub points at 0xE05B7520** |
| abort write / abort read * | 0xE05B8308 / 0xE05B83B2 | - |
| RegisterEDmacCompleteCBR * / Unregister * | 0xE05B7C48 / 0xE05B7F68 | correct |
| read-channel-only helper (asserts `Edmac.c` 0x770 on a write channel, then writes reg +8 = 0x11) * | 0xE05B9690 | misused as "StartEDmac" on channel 13 |

### 4.4 Why memory-to-memory copy failed

[O] 0xE05B752E and 0xE05B7520 belong to `Engine::Outer.c` and index the 56-port table 0xE1087A8C. Port
13 there is boomer node 0x4E (selector register 0xD0441268), not EDMAC write channel 13 (node 0x0C,
selector register 0xD042327C). The reference selector 0x373F0126 passes all three checks of the real
select function 0xE05B802E for channel 13 and fails the Outer function's check; the port's substitute
0x373F0226 does the opposite (asserts `Edmac.c` 0x304 on the real function) (A5-verify, B5).
D1 re-check adds direct evidence that 0xE05B802E is the function Canon itself uses for a raw write
channel: `Mem1Path_SetParam` calls it for channel 3 at 0xE0754354.

[I] So ML's copy never connected read channel 46 to write channel 13; it wrote selector value 0x26 into
an unrelated module's input and pulsed that module's VdKick. That is consistent with both hardware
symptoms (destination unchanged; Canon-side DAF/EIS error storm) but is not provable statically
(A5-verify: unverifiable). Every earlier conclusion drawn from mem2mem behaviour on hardware,
including the resource-lock theory, was made with the wrong node and needs a retest (A5, A12).

[U] The correct start sequence for a write channel on this body; whether VdKick "off" (0xE05B7FB6, the
positional match to mk1) or "pulse" (0xE05B8006, what the current wrong stub does on its own table) is
the right homolog (A5-verify).

### 4.5 Engine resource locks

A *resource lock* (`Engine::Ersc.c`, state at RAM 0xB0F0) is Canon's mutex over engine hardware blocks,
identified by 32-bit IDs (`block << 16 | device`).

- [O] The IDs ML locks for mem2mem, `{0x100AD, 0x100BB}`, come from EOS R code and appear in **no** Canon
  lock list in this ROM (94 category-1 IDs found, highest 0x100BA) (A12). `CreateResLockEntry`
  0xE05BE4BE silently creates nodes for unknown IDs, so ML's lock is private and contends with nothing.
- [O] The port's `LockEngineResources` stub, 0xE05BE8A6, is a non-blocking try-lock with zero Canon
  callers. Canon's blocking lock is 0xE05BE6A6 (105 callers); unlock is 0xE05BE740; delete is
  0xE05BE5A2, which ML never calls, so each recording leaks one entry (A12;
  `platform/M50mk2.101/edmac-d8.c:219-237`).
- [O] Canon's own lists: Mem1Path locks `{0x10062, 0x10072, 0x100B1}` (Create at 0xE0754454);
  MvRawProPath locks 15 IDs including 0x100B0 and 0x100B2 (Create at 0xE07701A8) (A12).
- [I] The "resource lock starves DAF/EIS" theory is not supported by the ROM. The `TryPostEvent(0xd)`
  errors in the storm come from the event-post path (section 5); the try-lock's own 0xD return value is
  a coincidence (A12).
- [U] Whether Canon holds its LV locks per session or per frame. Until that is read on hardware, do not
  repoint the stub to the blocking lock: with real IDs it could block the ML task for ever (A12).
- Rule: the CPU-copy path uses no engine hardware and should take no lock.

---

## 5. The event-posting system, and why starving Canon tasks is fatal

### 5.1 TaskClass, queues and the shared node pool

A *TaskClass* is Canon's wrapper for "a task plus a message queue plus a handler". `CreateTaskClass`
0xE05B9E60(name, priority, stack, queue_len, handler) creates the queue and a task whose loop
(0xE05B9E20) receives a message and calls the handler [O] (A2, A11).

- [O] Every message needs a 16-byte node from **one global pool shared by all TaskClass tasks**: 0x600
  (1536) nodes at RAM 0xBEE18, free-list head at RAM 0x39B84, lock word 0x39B80 (A2-verify, A3-verify).
- [O] `TryPostEvent` 0xE05B9F9E is the non-blocking post. If the pool is empty it prints
  `[TASK ERROR] TryPostEvent MsgQue OverFlow : Name = %s`, asserts `System::Task.c` line 0xE5, and then
  **falls through** to a store through the NULL node pointer at 0xE05B9FDA (reachable only if the assert
  handler returns, which ML's does). If the pool is not empty but the target queue is full, the queue
  post (0xE05B55EE, timeout 0) fails and the node is freed; the error code is odd (0xD or 7)
  (A2-verify, A3-verify).
- [O] `PostEvent` 0xE05B9F10 is the blocking variant; 0xE05BA078 returns the number of pending messages
  of a TaskClass.

### 5.2 The EvfDev queue

- [O] EvfDev: priority 0x0E, stack 0xC00, queue length 200, created at 0xE065B640 (caller 0xE00444BA).
  EvfCap: priority 0x0D (0x13 during init), stack 0xC00, queue length 200, created at 0xE06CB038
  (A2-verify).
- [O] EvfCap posts to EvfDev through `evfLvComEvent` 0xE069A670, which uses `TryPostEvent` and asserts
  `EvfDevState.c` line 0x174 on failure. The sync callback 0xE069A7E8 posts event 13 and asserts line
  0x21E on failure. The word "nullptr" in ML's log line for these asserts is only the empty expression
  string (r0 = 0 at 0xE069A696 / 0xE069A804), not a NULL dereference (A2-verify).
- [O] The hook that ML installs on EvfDevState runs inside the EvfDev task (handler 0xE065B5C0 calls the
  dispatcher). Time spent in the hook is time EvfDev is not receiving, while EvfCap, at higher priority,
  keeps posting (A2).
- *Corrected:* A2 modelled 3 EvfDev messages per frame. The UART log shows a strictly periodic pattern of
  three line-0x174 failures per line-0x21E failure, which implies about **4** messages per frame
  (events 8, 6, one more through `evfLvComEvent` - candidate event 7 from `evfResetPSaveInterrupt` -
  and event 13), if event 13 is once per frame. Then 200 slots fill in about 50 frames, under 2 seconds
  at 30 fps [I] (A2-verify; the sample is 128 lines in 4 bursts). The qualitative conclusion stands: a
  stalled EvfDev overflows its queue within a couple of seconds and every later post asserts.

### 5.3 The crash signatures seen during recording

- [O] `DryosDebugMsg` 0xE05D10C4 uses 0xC0 bytes of stack per call and, when its notify gate
  `[0x5158]` is set, itself calls `TryPostEvent` to a target taken from its context (`*(0x5154)` + 4)
  (A3-verify).
- [O] The 24 logged aborts with pc = 0, lr = 0xE05D1161 are identical except for sp, which lies between
  0xDEFFFFC8 and 0xDF000000; every register matches the exit state of Canon's small formatter
  0xE005D89C returning through a frame that was pushed below 0xDF000000. The single abort at
  pc = 0xE005DFA2 is the same event one stack frame shallower (A3-verify). In all of them the stack
  shows the chain `DryosDebugMsg` -> `TryPostEvent` (pool empty) -> `DryosDebugMsg`.
- [I] So these crashes are a stack running off its floor at 0xDF000000 while the debug logger is nested
  inside `TryPostEvent`'s pool-empty error path. The region 0xDF000000-0xDF002000 is filled with
  0xEEEEEEEE and split per CPU by cstart (0xE0040104, 0xE00401E8; `[0x1014]` = 0xDF001000 for CPU 0)
  [O]; that it is the *interrupt* stack is [U] (A3-verify: not traced to an IRQ-entry reader). That
  writes below 0xDF000000 are dropped and read back as zero is implied by the dumps, not shown by the
  ROM [U]. How many recursion rounds occur is [U]; one round is proven by the dumps.
- [O] These dumps do not fit the older theory of a NULL EDMAC callback slot at 0x83F24: the EDMAC
  dispatcher reloads lr and r9 before `bx r1` (0xE05B7E70..0xE05B7E74) (A3-verify).
- [O] The `raw_vidx` module's own `[RVX]` logging goes through ML's direct UART writer
  (`modules/raw_video/raw_vidx/raw_vid.h:12-20`), not through `DryosDebugMsg`, so it does not consume
  pool nodes (A3-verify, correcting A3). Core ML does call `DryosDebugMsg` (for example
  `src/rbf_font.c:742`).
- [O] A third signature (pc = 0xE005DE52, at boot after the `big.5.3` marker) is unrelated: Canon's
  `vsnprintf` 0xE005DB9C dereferences a NULL `%s` argument passed by ML code. Canon's formatters never
  check for NULL (A3-verify).

### 5.4 What "starving" means in practice

The chain, as far as it is established: something in the record path keeps Canon tasks from draining
their queues -> producers keep posting -> either a 200-entry queue fills (asserts line 0x174 / 0x21E)
or the 1536-node pool empties (the `MsgQue OverFlow` path and the stack overflow above). What exactly
empties the pool and who holds the nodes is [U] (A3). The log just before the first pool-empty crash is
a flood of `[EIS] ERROR ... TryPostEvent(0xd)` [O] (A3).

Rules for a recorder:

1. The hook must not block, copy, allocate, or log. It may only signal.
2. No CPU-bound ML task inside or above Canon's LiveView band (section 6).
3. Never pass NULL for a `%s` to any Canon formatter.
4. Untested mitigation: clearing `[0x5158]` while recording removes the logger's recursion, turning pool
   exhaustion into a logged error; it does not fix the starvation (A3) [I].

---

## 6. Tasks and priorities

In DryOS a lower number is a higher priority. A11 recovered 184 task creations by scanning direct calls
to `task_create` 0xE05F40BA, `task_create_ex` 0xE05F42D2 and their wrappers. A11 has no independent
verification section; its EvfDev and EvfCap rows agree with A2-verify, the rest is single-source.
For Fwrite and MovWriter the priority is not a constant at the create site (0xE02F2736 `mov r1,r5`,
0xE04A76DA `mov r1,r4`): A11's 0x15 is caller-traced, and the trace was not repeated (D7-critic
section 4 item 4). Read the live task table before interpreting any priority test.

| Priority | Canon tasks (stack) | ML tasks today |
|---|---|---|
| 0x0D | EvfCap (0xC00) | |
| 0x0E | EvfDev (0xC00), EventMgr, PhySw | |
| 0x0F | Epp, CamConductor | mlv_lite `compress_task` |
| 0x10 | ISManager, TimeCodeMaster | |
| 0x11 | DAF, MovieRecorder (0x2000) | raw_vidx `raw_v_worker` (does the inline frame copy), `worker.c:675` |
| 0x12 | AFTask, RscMgr, Gmt, display control | raw_vidx `stop_com` |
| 0x13 | EIS, AlgsMgr, ImageCodec, DAFPath | |
| 0x14 | PropMgr, Imager, LVEZM, FileCache | |
| 0x15 | Fwrite, MovWriter (Canon's own bulk writers) | caller-traced, not a constant; single-source |
| 0x17 | GuiMainTask | `cpu_copy` (`platform/M50mk2.101/edmac-d8.c:137`) |
| 0x18 | CtrlSrv (inferred through one indirection) | `livev_hiprio_task`, `focus_task` |
| 0x19 | FileMgr, Fstorage, Fcreate, Fread, MovieTSMTask | `clock_task`, mlv_lite `raw_rec_task1` |
| 0x1A | DOSDriver | raw_vidx `raw_write` (`worker.c:377`), `menu_task`, `shoot_task` |
| 0x1C-0x1E | CSMgrTask (card services), TempSvc, HotPlug | console, `ml_init`, `module_task` |
| 0x1F-0x20 | PowerMgr | serial shell, heartbeat |

- [O] Canon's LiveView core occupies 0x0D-0x14 (A11).
- [I] A CPU-bound copy at 0x11 can delay every ready task from 0x11 downward on the same core, which
  includes PropMgr, Imager, EIS, AlgsMgr and DAFPath. A11 proposes copy at 0x15 and write at 0x16 (level with
  Canon's writers only if the caller-traced 0x15 is right) as a
  starting point for a hardware A/B; the sprint's merged plan (C9-recommended) starts lower still
  (capture 0x1B, writer 0x1A, throttled) and treats 0x15/0x16 as the later throughput variant.
- [U] Tasks made with plain `task_create` are not pinned to a core on this 2-core SoC, which weakens every
  static starvation argument; whether equal priorities time-slice; whether `FIO_WriteFile` runs entirely
  in the caller's task or hands work to DOSDriver, FileCache, Fwrite or CSMgrTask; the run-time default
  stack `[0x5BD8]` and priority ceiling `[0x5BDC]` (A11).
- Do not put anything CPU-bound above 0x0F.

---

## 7. Memory map and pools

### 7.1 Address space ([O], A13-verify, decoded from the ROM page table image at 0xE0000000)

| Virtual range | Attributes | Use |
|---|---|---|
| 0x00000000-0x01FFFFFF | per-core tables, cacheable | low RAM: Canon data, DryOS pools, ML |
| 0x02000000-0x3FFFFFFF | Normal, inner cache write-back, outer non-cacheable | main DRAM, cacheable view (1 GB total, inferred) |
| 0x40000000-0xBFFFFFFF | Normal, non-cacheable | **uncached** views of the same DRAM |
| 0xC0000000-0xDEFFFFFF | Device memory, sparse | MMIO (EDMAC 0xD04xxxxx, timers 0xD0406xxx) |
| 0xDF000000-0xDFFFFFFF | Normal, cacheable | CPU stacks at 0xDF000000-0xDF002000, small code blob at 0xDF002800 |
| 0xE0000000-0xE7FFFFFF, 0xF0000000-0xF7FFFFFF | read-only, cacheable | ROM0, ROM1 |

- [O] Both the cacheable and the uncached ranges are identity mapped; the page table does not remap
  0x4xxxxxxx onto 0x0xxxxxxx. [I] The aliasing onto the same DRAM therefore happens in physical address
  decoding (A13-verify). ML's `UNCACHEABLE(x)` is `x | 0x40000000` for x < 0x40000000
  (`src/mem_defs.h:16`).
- [O] Canon's raw buffer is seen at 0x6BC61CC8, in the uncached range; ML's write buffers from
  `fio_malloc` are also uncached (for example 0x46331080) (A13).
- The live page table contents are not observable offline (A13-verify) [U].

### 7.2 Boot-time RAM images ([O], A15; copy code at 0xE004003C..0xE00400C6)

| Name | RAM range | ROM source | Note |
|---|---|---|---|
| data | 0x4000-0x2AD1C | 0xE1394358, length 0x26D1C | initialised variables |
| bss | 0x2AD1C-0xC70D8 | none (zeroed) | |
| ramcode | 0x0223B000-0x02319530 | 0xE13BB074, length 0xDE530 | AE, face-tracking and detection code only; no EVF, LV or EDMAC code |
| blob2 | 0xDF002800-0xDF0033A0 | 0xE14995A4, length 0xBA0 | cache-control helpers |

`python3 docs/offline/tools/raminit.py <addr> [n]` prints the initial value of any RAM word. All the
pointers a recorder depends on (0x10108, 0x8E8C region, 0x4FF4, 0x5154, 0x23800 and 0x292B0 fields,
0x83F24, 0x216C4..0x216E4) are **zero at boot** and filled by Canon init code; a zero there on hardware
means "not initialised yet", not corruption (A15; D1 re-check for 0x216C4..0x216F0). ML code must test
for non-zero before dereferencing them. The LV raw path executes from ROM, so ROM addresses for it are
not shadowed by RAM code [I] (A15). ROM1 contains no code: property areas (FIX at 0xF0010000, TUNE at
0xF0420000, RASEN network settings) and GUI resources (A15). `gang200.bin` contains device-unique data
and should be treated as private.

### 7.3 Pools

| Pool | Entry points | Size | Usable for frames? |
|---|---|---|---|
| DryOS `malloc` (`MALLOC_STRUCT_ADDR 0x2d160`) | `_malloc` 0xE03C5270 | under 1 MB (user_mem 0xC78D8, length 0x112E80, also holds ML itself) | no |
| `AllocateMemory` | 0xE05BEE20 / 0xE05BEFD8, pool handle at RAM 0x51F4 | [U] | no (small objects) |
| Shoot memory (`AllocateMemoryResource`, type 0x50) | thunks 0xE0608904 / 0xE0608932 / 0xE0608960 through the RAM table at 0x5128 into `Resource::RscMgr.c` | about 63 MB measured earlier in movie LV (docs/32:13) | yes, partially |
| SRM "1stJob" buffers | 0xE057B7BA (allocate) / 0xE057DA00 (free) | [I] 2 x 0x2AB1000 (42.7 MiB each) | untested, see below |
| Canon's LV raw buffer | none (Canon-owned) | 1 frame | source only |

All addresses [O] (A8, A8-verify). Notes:

- **Do not take the whole shoot pool.** An earlier hardware run tied "grab all shoot memory" to the
  "Over the Deadline Aewb" error storm, and the same call from the serial-shell task never returned
  (docs/32:1121-1145, quoted in A8). The safe partial size is [U].
- 63 MB holds at most 14 full frames (4,269,552 bytes each at 2096x1164, 14 bit) or 22 frames of
  1792x896, assuming decimal megabytes (A8-verify). This is a write-smoothing buffer.
- **SRM**: the commented-out stubs in `platform/M50mk2.101/stubs.S:266-270` (0xE056A5C6 / 0xE056DA22) are
  M50 mk1 addresses and are not code on this ROM [O]. The real pair is 0xE057B7BA / 0xE057DA00,
  identified by Canon's caller at 0xE0079344 and its failure string
  `SRM_AllocateMemoryResourceFor1stJob failed [%#x]` (0xE00794C4). Calling convention seen in Canon's
  caller: `alloc(cbr, ctx)`, the callback receives `(ctx, buffer)`, `free(buffer, 0, 0)`; whether the
  callback gets a size in r2, as `src/exmem.c:469` assumes, is [U]. The buffer size 0x2AB1000 is a guess
  from the area-size table at 0xE04B3468 [I]. Cautions: the only located Canon caller is factory code
  (A8-verify), and the developer community reports this allocator as a factory path that "returns
  nothing on D8" and warns against it (B4, other bodies). The earlier "alloc crashes / free does not
  work" notes for this body were made with the wrong addresses, so they prove nothing either way.
- Other ideas recorded from the community for M50 mk1, addresses not resolved for this ROM (B4):
  `SRM_AllocateMemoryResourceForCaptureWork` (19 contiguous buffers of about 1.06 MB that coexist with
  video mode), and borrowing the H.264 recorder's work area ("MOVIE_RECWORK", untested by anyone).
- Canon partitions almost all of the 1 GB DRAM into fixed per-feature areas (area address function
  ending at 0xE04B3466, highest constant 0x3F421000) [O]; there is no large unclaimed region [I] (A8).

### 7.4 Caches and copy speed

- [O] Canon cache primitives (Thumb, 32-byte lines): invalidate range 0xE05D088C(addr, size), clean range
  0xE05D08F4, clean and invalidate range 0xE05D0958; whole-cache set/way loops 0xE05D08B8 / 0xE05D0920 /
  0xE05D0984 (A13-verify). Never use the whole-cache invalidate from a recorder: it discards other
  tasks' dirty lines.
- [O] The measured raw_vidx copy rate is about 5.7 MB/s: 0.492 s per 2.81 MB frame, mean of 20 intervals
  from 5 logged runs; the host log quantises time to 0.2 s, so only the mean is meaningful (A13-verify).
- [O] Cause, from the build and the logged addresses: the crop x offset of 200 pixels makes every source
  row start at an address that is 2 mod 4 (`event_pusher.c:421`, `worker.c:427-429`), and ML's `memcpy`
  (0xECBA8 in the build examined) copies the whole length byte by byte unless both pointers are
  word-aligned; both source and destination are uncached. [I] That this alignment accident, and not task
  pre-emption or logging, accounts for the time is unproven until the x = 192 experiment is run
  (A13-verify).
- [O] Canon's ROM `memcpy` 0xE0329D10 is a plain word loop without burst loads; it is not a fast path.
- [I] Proposed faster path, all rates being estimates: read the source through the cacheable alias
  (0x2BC61CC8) after an invalidate-by-range, copy with `memcpy64` (`src/stdio.c:153`), write the
  destination either uncached or cacheable followed by a clean-by-range before the card write. Whether
  cache maintenance reaches the other CPU core is [U] (A13).
- [O] Hardware copy engines exist (`Engine::XdmacWrap.c`: open 0xE0618F1C, start 0xE0618F8A / 0xE0618FEA;
  `StartMem2Mem` strings at 0xE0062434) but their argument semantics are not resolved [U] (A13).

---

## 8. The card write path and measured speed

### 8.1 Call chain ([O], A7)

`_FIO_WriteFile(fd, buf, size)` 0xE05B1A00 -> takes a per-drive semaphore (0xE05B1242) -> file-system
write wrapper 0xE00AA480 (retries once if the first call returns short) -> efat library dispatch
0xE03EF740 (not traced further) -> `FSUBlockWrite(dev, buf, startSector, nSectors)` 0xE008564E ->
buffer check 0xE00850F0 -> block-device ioctl 0x100C -> `sdIOControl` 0xE00E5C84 -> SD write 0xE00DE6D4
-> SD library 0xE01B4362 (`CMD25_WriteMultiBlock`).

### 8.2 Rules the ROM imposes

| Rule | Evidence |
|---|---|
| A buffer at or above 0xE0000000 is rejected ("buf is ROM Addr") | 0xE0085180 (A7-verify) |
| A buffer below 0x40000000 (cacheable) is accepted only if 32-byte aligned; otherwise "Please Designate Uncacheable Addr!!!!!!" | 0xE008518C..0xE0085194 (A7-verify) |
| A rejected buffer makes `FSUBlockWrite` log "NotPrepared" and skip the transfer, so the data is silently not written | A7 [O for the log lines in history; the skip path itself was not re-checked by A7-verify] |
| One block-device call is capped at 0xF800 sectors (31 MiB); larger writes loop in the calling task | 0xE00E5DCC; the cap applies when a state query at 0xE00DF332 does not return 1 [U which state] (A7-verify) |
| All file operations on a drive are serialised by one semaphore, held across the whole write | 0xE05B1242, 0xE05B12C4 (A7-verify) |
| There is no "set file size" or pre-allocate call | no such tag among the FIO tags in 0xE05B1000-0xE05B5000 (A7) |

Consequences: keep every buffer passed to `FIO_WriteFile` in the uncached view and aligned (the port's
gate in `fio-ml.c` does this; the community confirmed the same requirement on other DIGIC 8 bodies in
December 2025, B4); keep all other card I/O (logs, configuration saves) off the card while recording,
because it queues behind a multi-megabyte frame write and vice versa. Whether the SD driver flushes the
data cache for an accepted cacheable buffer is [U] (A7).

Canon's native pre-allocation mechanism is the SD Speed Class recording mode (`CreateSC` 0xE05B16FA,
`EntSC` 0xE05B4C2A, `RecordStart` 0xE05B46D0, `DelayFlush` 0xE05B2FF0; allocation unit 4 MB and recording
unit 512 KB on the logged card). Argument conventions are [U] (A7).

### 8.3 Bus mode and measured speed

- [O] The mode selection at 0xE00DE1CE-0xE00DE21A picks the highest of the card's access-mode bits 1, 2,
  3 and maps bit 3 (SDR104) to clock index 9; the clock-name table (0xE011C448) names index 9
  "1.8V 195.0MHz" (A7-verify). The hardware log has 9 boots with `mode:SDR104` at 800 mA and no
  `TryClkDown` line. [I] The bus ceiling is therefore about 93-97 MB/s and there is nothing to gain from
  an SD overclock on this body; the community reports the same for M50 mk1 (B3).
- [O] Measured on this camera (earlier hardware session, ML bench, 16 MB buffers, one 16 GB card):
  write 75.1 MB/s, read 90.4 and 93.9 MB/s (`tools/mlserial/run/uart.jsonl`, A7-verify). A second write
  figure of 75.7 MB/s is on a log line torn by another message.
- [I] Sustained budget for raw video: about 65-75 MB/s. This rests on one card and one benchmark run;
  M50 mk1 users report about 70 MB/s while recording (B3, B4). Real recording will be lower once
  file-system updates are mixed in.
- *Corrected (A7-verify):* A7 expected a short stall every 4 GiB from a periodic retune (byte counter at
  device+0xE8, ioctl 0x100E, handler 0xE00DF166). The handler only reaches the tuning call when a value
  that is probably the clock index is 4 or 5, so at SDR104 it is probably a no-op. Also corrected:
  Canon's debug benchmark procedures (`DebugSTG_GetSDWriteSpeed` 0xE00E6ECA and others) **are**
  registered at boot (0xE00E8362, called from `CSMGR_Initialize` at 0xE00AF47C) and are probably callable
  by name; and a "DDR50" string does exist in ROM (0xE015FB7C), although the selection code never picks
  that mode.
- [O] Canon stores the duration of the last block write near RAM 0x63FC (A7 gives 0x640C for write and
  0x6408 for read, in microseconds; the offsets and unit were not re-derived by A7-verify).

### 8.4 Data-rate arithmetic

| Mode | Bytes per frame | At 24 fps | Fits 70 MB/s? |
|---|---|---|---|
| 2096x1164, 14 bit | 4,269,552 | 102 MB/s | no (burst only) |
| 1792x896, 14 bit | 2,809,856 | 67 MB/s | marginal |
| 1920x818, 14 bit | about 2.75 MB | 66 MB/s | marginal (ran 15 minutes on a 200D, B3) |
| 1280x720, 14 bit | 1,612,800 | 39 MB/s | yes |

Lower bit depths and lossless compression are unsolved on every DIGIC 7/8 body (B2, B3); do not plan on
them.

---

## 9. Raw geometry, bit depth and timing constants

### 9.1 Geometry ([O] for the table data, A9-verify)

Canon's LV raw geometry records are in ROM0 at 0xE1359338, 8 records of 11 words (stride 0x2C): width,
height, 1, left, top, active width, active height, ...

| Record | Raw size | Left, top | Active area |
|---|---|---|---|
| 0xE13593E8 | 2096x1164 | 92, 38 | 2000x1126 |
| 0xE1359338, 0xE1359440 | 2096x838 | 88, 34 | 2008x804 |
| 0xE1359414 | 2096x714 | 88, 34 | 2008x680 |
| 0xE1359390 | 2096x608 | 88, 35 | 2008x572 |
| 0xE1359364 | 4368x2201 | 392, 35 | 3848x2166 |
| 0xE13593BC | 4288x1202 | 2672, 212 | 1208x806 |
| 0xE135946C | 4288x1202 | 2272, 50 | 2008x1130 |

- The port uses skip_left 88 / skip_top 34 for all modes (`src/raw.c:1069-1073`), copied from M50 mk1,
  where the developer derived an active area of 2008x1128 for 1080p by eye (B2, B4). The ROM record for
  2096x1164 says 92 / 38 and 2000x1126. [I] 92/38 is the better value for the 1164-line mode; whether
  these record fields are the true optical-black edge is [U]. Check on a decoded frame.
- The sensor *drive mode* (0..13; 0-5 stills, 6-13 LiveView, by reading of a map column) is at RAM
  0x10654, setter 0xE06CFE68, getter 0xE06CFE80 [O] (A9-verify, A10). Which drive mode each Canon video
  setting selects, and which geometry record it uses, is a **hypothesis**: A9's map (6 = 2096x1164
  1080p, 7 = 2096x838, 8 = 2096x714 high frame rate, 12 = 4K crop, 13 = 1:1 crop) rests on a ROM1 table
  for which no code reference was found (A9-verify) [U].
- 4K and zoom raw frames are 2.1 times wider than 1080p frames; a 4K frame would be about 16.8 MB at
  14 bit. Whether they appear on channel 3 at all is [U] (A9).

### 9.2 Bit depth and gain

- [O] Channel 3 line length is `width * 7 / 4` bytes, that is 14 bits per pixel (A1), matching the
  hardware read of 3668 bytes per line (A9).
- [O] The registers ML uses on older bodies for bit depth (`PACK32_MODE` 0xD0008094, `src/raw.c` ~2665)
  and digital gain (`SHAD_GAIN_REGISTER` 0xD0008030, `consts.h:92`) do not occur as literals in either
  ROM (A9-verify). [I] They are very likely wrong for this body; absence of a literal is not strict
  proof, because Thumb-2 code can build an address without one.
- [U] The real DIGIC 8 pack / bit-depth register. Packing goes through `LiveViewCore::LvComPack.c`:
  dispatcher 0xE071FB14 over a RAM handler table at 0x6DC60 (6 handlers, 6 arguments at +0x18), with the
  registration function at 0xE071FB42 as the static lead (A9-verify). Caution: A2 shows that slot 1 of the
  same table holds `evfLvComEvent` (registered through 0xE071FB9A), so this is a general LiveView
  "LvCom" callback table and its link to packing is itself only inferred from the file name. M50 mk1 experiments with 10 and
  12 bit produced every other frame stale, or a tilted image (B2, B3, B4). Stay at 14 bit.
- B5: the mk1 code returns early from `raw_lv_request_bpp` on DIGIC 8; the port's version falls through
  and can rewrite `raw_info.bits_per_pixel` and pitch. Adopt the early return.

### 9.3 Frame timing

- [O] Frame-timer registers: A = 0xD0406198 (0xD040619C is always written with the same value),
  B = 0xD04061A4 (0xD04061B8 gets the same value in the full mode tables) (A9-verify).
- [O] A per-drive-mode table of {register, value} pairs is indexed by code at 0xE06E97A6 as
  `base + 0x6F44 + mode * 0x58`, base taken from RAM `*(0x216D0+0x14)` = `*(0x216E4)`. A9 located a
  matching 14-entry table in ROM1 at 0xF001B858 (blob base 0xF0014914), with values:

| Drive mode | A (0xD0406198) | B (0xD04061A4) | 27e6 / ((A+1)(B+1)) |
|---|---|---|---|
| 6 | 0x2D1 | 0x270 | 59.83 |
| 7 | 0x2D1 | 0x26F | 59.93 |
| 8 | 0x215 | 0x1A4 | 120.1 |
| 12 | 0x1C5 | 0x94D | 24.97 |
| 13 | 0x2D1 | 0x270 | 59.83 |

- [I, high] The timer clock is 27 MHz, so `TG_FREQ_BASE` in `src/fps-engio.c:324` must be 27000000, not
  32000000 (the port currently reads frame rates 18.5 % high, B5). The argument is: the A values and the
  60p / 120p B values are identical to those measured on M50 mk1, whose port uses 27 MHz
  (`research/bilal_M50_rawvideo/src/fps-engio.c:326-338`), and only 27 MHz turns them into 59.94 and
  120 fps. It rests on the mk1 hardware measurement, not on this ROM alone (A9-verify).
- **Disagreement, resolved by D1 re-check.** A9 says the base pointer equals the ROM1 address
  0xF0014914. A10 says the tables must be in writable memory because Canon patches them in place. The
  ROM supports A10: at 0xE06E69F6..0xE06E6A02 Canon loads the engine-table base from `[0x216C4+4]`, adds
  0x6F44, and calls 0xE06E7E60, which searches the list for 0xD04061A4 and executes
  `str r2,[r0,#4]` (0xE06E7E68) into the list itself. A store into flash-mapped ROM would do nothing,
  so the table Canon uses at run time is in RAM [O for the store; I for the conclusion]. The two
  readings are reconciled if the ROM1 blob is the load image: 0xF001490C lies inside the FIX property
  area (0xF0010000-0xF01668EC, A15) and has a {type, size} header, so it is most likely a property that
  Canon copies to RAM at boot [I]. Practical consequence: A9's numbers are the *initial* values; read
  the live values and the pointers `*(0x216C4)`, `*(0x216C8)`, `*(0x216E0)`, `*(0x216E4)` on hardware
  (all zero at boot per `raminit.py`).
- [U] The role of `FPS_REGISTER_CONFIRM_CHANGES` 0xD0406190: it exists in Canon's tables, always written
  with 1, but early in a full mode-set table, which does not look like a commit (A9-verify).

### 9.4 Sensor and engine register writes

- [O] Engine registers are written by `engio_write` 0xE06CAEB2, a plain {register, value} list writer
  terminated by 0xFFFFFFFF with a 500-entry limit; there is no shadow memory on this body, so reading a
  register back is a plain MMIO read (A10; B2 for the same finding on other DIGIC 8 bodies). The port's
  `_engio_write` and `_EngDrvOut` are empty functions (`platform/M50mk2.101/function_overrides.c:148,167`).
- [O] Sensor registers go through one serial writer, `cmos_write` 0xE06C2F98
  (`ImageSequence::Sio0Driver.c`): a list of 32-bit words, register number in bits 31..22, value in bits
  21..0. There is no ADTG path in this ROM (A10).
- [O] `cmos_write` calls optional pre/post callbacks stored at RAM 0x194B4 / 0x194B8; the only function
  that sets them (0xE06C39D0) has no caller in ROM0 (A10). [I] They are probably free, which would let a
  module observe every sensor write without a ROM patch; untested.
- A10 has no independent verification section.

---

## 10. The movie-record state machine

### 10.1 What starts a Canon recording ([O], A6 and A6-verify)

1. `handleMovieButton` 0xE071FE72 (`DlgLiveView.c`; string `handleMovieButton RecState(%d),RTime(%d),
   TftSts(%d)`). It has no direct callers, so it is reached through a dialog dispatch table; that it runs
   in the GUI task is [I].
2. It reaches the gate only if 0xE03FCFD2() == 1, 0xE0599D68() == 1 and 0xE059A902() == 0
   (0xE071FEA8-0xE071FEBC); with RecState == 3 it goes to the stop path at 0xE071FFAE (A6-verify).
3. Gate: 0xE071FEC8 `bl IsEnableMovieStart` (0xE0547518); a return of 1 continues to the begin sequence;
   anything else stores 1 at `[0x15ABC+0xC]` and calls 0xE06D515A, whose user-interface effect is [U].
4. The recorder side is the state object "MovRecState" (descriptor at 0xE0BB79BC). *Corrected:*
   0xE024A8FC is a 0x4E-byte state-entry wrapper; the function that logs `mvrRecStart In` is
   **0xE024A068**, reached by `b.w` from 0xE024A946 (A6-verify).
5. Tasks: MovieRecorder (priority 0x11, created by `MVR_Initialize` 0xE072D556), MovWriter (0x15),
   MovieTSMTask (0x19) (A11). Which task executes `mvrRecStart` was not shown.
6. In photo modes the equivalent gate is `IsEnableAnyTimeMovieStart` 0xE05475DC (single caller
   0xE07BEEB8).

### 10.2 No card remount

- [O] The addresses that older project notes (docs/32) treated as `vcmMovieFormatMount` (0xE0202328) and
  `fmMountCard` (0xE02023C0) are text strings. The real `fmMountCard` is 0xE0202106 and `fmUnmountCard`
  is 0xE0202612; both are handlers in the FileMgr state table (0xE0B630A0, 0xE0B630C8) with no direct
  callers anywhere in ROM0. The places where a movie-format mount or unmount call would be are constant
  `movs r0,#0` (0xE0201F58, 0xE02026BC) (A6-verify).
- [I] Canon's record start does not remount or unmount the card on this firmware. No remount wait or
  mount-bit polling is justified by the ROM. ROM1 and RAM-copied code were not searched (A6-verify).
- [O] The `Not mounted ID=%d` messages compute ID as `path[0] - 'A'` (code following the string at
  0xE05B15AC), so ID=0 means a path starting with "A:". The SD card is drive 2 = "B:" in the logs
  (`fmPrepareShooting (Drive = 2)`). [U] Who issues the 862 logged A: accesses, and whether they are
  harmless; 486 lines show B: unmounted too (A6-verify).
- [U] What actually made file writes from another task fail during Canon H.264 recording in the older
  sessions. Candidates outside the mount theory: a cacheable buffer (section 8.2), the drive letter,
  memory pressure (A6).

### 10.3 Keeping Canon from recording H.264 when the key means "raw"

- [O] On this port ML only peeks Canon's key events (`src/gui.c:124`, `M50II_BYPASS_GUI_OVERRIDE 1`), so
  it sees the record key (`BGMT_REC 0x20`, `platform/M50mk2.101/gui.h:49`) but cannot swallow it; the
  reference ports rely on swallowing (A6-verify).
- Options, none tested: (a) trigger raw from a key Canon ignores in LiveView, from the ML menu, or over
  the serial harness; (b) patch `IsEnableMovieStart` to return 0 while raw is armed (bytes
  `00 20 70 47`), which also changes two other callers (0xE0443032, 0xE07090B8), has an unknown refusal
  side effect, and costs one of the two MMU remap pages (A6-verify: unverifiable); (c) let H.264 run
  alongside as a proxy, which worked by accident on EOS R and M50 mk1 (B4, other bodies).

### 10.4 The MVR structure

- [O] The MovieRecorder object pointer is at RAM `*(0x8B94)` (`mvrChangeAckCBR` 0xE0729EB4 loads 0x8B90
  and reads +4), not at 0x1ED8 as `platform/M50mk2.101/consts.h:183-198` says; that block is
  byte-identical junk carried over from M50 mk1 (A6-verify). Candidate fields: bytes written, 64 bit, at
  +0x2150; capacity at +0x2180; the frame-count offset is [U].
- [I] These are only needed for H.264 displays, not for raw recording.

---

## 11. Tables

### 11.1 ML assumptions that were wrong

| # | Old belief | What the ROM (or log) shows | Source |
|---|---|---|---|
| 1 | `lv_save_raw(1)` creates a "raw job" that must become ready; the MvRawPro asserts are a race in that job | It sets one flag (`[0x10238+0x40]`). The asserts are overflow checks of the normal capture pipeline's 2-entry completion rings and 6-slot job pool | A1 |
| 2 | `*(0x23800+0x30) == 1 && *(0x292B0+0x30) != 0` means "raw job ready" (`worker.c:496-510`) | A mode argument and an EDMAC channel number, constant while LV is up | A1, A1-verify |
| 3 | The repeating assert lines are Canon soft asserts | Canon's fatal assert, survived only because ML's handler returns; stock firmware would hang at the first one | A1-verify |
| 4 | `EVF_STATE = *(0x10108)`, input 6 / state 3 is the per-frame "readout done" vsync | 0x10108 is EvfDevState; (6, 3) is `evfdevUpdateParam`. The object ML's generic code expects is EvfState at `*(0x8E8C)`; readout done is its input 5 | A2, A2-verify, D1 re-check |
| 5 | "EvfDevState nullptr" asserts are NULL dereferences | Failed non-blocking posts to the 200-entry EvfDev queue; "nullptr" is the empty expression string | A2-verify |
| 6 | Crashes at pc = 0 with lr = 0xE05D1161 come from a NULL EDMAC callback slot in the table at 0x83F24 | A stack overflowing below 0xDF000000 while `DryosDebugMsg` is nested in `TryPostEvent`'s pool-empty path | A3, A3-verify |
| 7 | `edmac_select_boomer` = 0xE05B752E, `edmac_reset_boomer_vdkick` = 0xE05B7520, selector 0x373F0226 | Those are `Engine::Outer.c` port functions. The EDMAC select is 0xE05B802E and takes the reference selector 0x373F0126; VdKick per channel is 0xE05B7FB6 / 0xE05B7FDE / 0xE05B8006 | A5, A5-verify, B5 |
| 8 | 0xE05B9690 is "StartEDmac" for any channel | A read-channel-only helper; on write channel 13 it asserts `Edmac.c` 0x770 and overwrites the prepared state | A5-verify |
| 9 | mem2mem failed because of the channel pair, the cache, or the hardware | Explained by items 7 and 8; hardware not implicated (inference) | A5 |
| 10 | Holding resource lock `{0x100AD, 0x100BB}` starves Canon's DAF/EIS paths | No Canon path uses those IDs; ML's lock is private and inert | A12 |
| 11 | `LockEngineResources` = 0xE05BE8A6 | That is a never-waiting try-lock with no Canon callers; the blocking lock is 0xE05BE6A6 | A12 |
| 12 | Canon runs `vcmMovieFormatMount` @0xE0202328 / `fmMountCard` @0xE02023C0 at record start and unmounts the card | Both addresses are strings; the mount calls are constant 0; no mount code on the record path | A6, A6-verify |
| 13 | `MVR_190_STRUCT` is at 0x1ED8 with offsets 220 / 212 | Object pointer is `*(0x8B94)`; the old block is mk1 junk | A6-verify |
| 14 | SRM stubs 0xE056A5C6 / 0xE056DA22; "SRM alloc crashes on this body" | mk1 addresses, not code here; real pair 0xE057B7BA / 0xE057DA00; the crash observation was made with the wrong address | A8, A8-verify |
| 15 | The frame timer runs at 32 MHz (`TG_FREQ_BASE`) | 27 MHz (inferred with high confidence from tables identical to mk1) | A9, B5 |
| 16 | `PACK32_MODE` 0xD0008094 and `SHAD_GAIN_REGISTER` 0xD0008030 exist on this body | Neither literal occurs in ROM0 or ROM1 | A9-verify |
| 17 | skip_left 88 / skip_top 34 for 2096x1164 | Canon's record says 92 / 38, active 2000x1126 (88/34 belongs to the 838- and 714-line modes) | A9-verify |
| 18 | The copy takes 0.4-0.6 s per frame because uncached memory or the hardware is slow | Mean 0.49 s per 2.8 MB frame = 5.7 MB/s; the source rows are misaligned and the copy runs byte by byte | A13, A13-verify |
| 19 | An SD overclock (`sd_uhs`) could raise write speed | The card already runs SDR104 at the 195 MHz clock index; no headroom | A7, B3 |
| 20 | Raw-video pointers in RAM have meaningful static values | All are 0 at boot and filled by Canon init | A15 |
| 21 | There is an ADTG write function to hook, and engine registers have shadow memory | No ADTG in this ROM; sensor writes are 32-bit words through `cmos_write`; engine registers are plain MMIO | A10, B2 |
| 22 | raw_vidx `[RVX]` logging might be consuming Canon's message pool | It uses ML's direct UART writer | A3-verify |
| 23 | Channels 5 and 6 belong to MvRawProPath | Their call sites sit in the SantaPath.c code range (inferred from string pools) | D1 re-check of A12 vs A5 |
| 24 | The engine timing tables are read straight from ROM1 at 0xF0014914 | Canon patches the table in place (0xE06E7E68), so the run-time table is a RAM copy | D1 re-check of A9 vs A10 |

### 11.2 RAM addresses

Confidence: **H** = re-derived by an independent verification pass or by D1; **M** = single report,
static; **L** = inferred meaning. "Initial" is the boot value from `raminit.py`.

| Address | What | Conf. | Source |
|---|---|---|---|
| 0x8E8C | pointer to EvfState state object (17 x 8) | H | A2-verify, D1 |
| 0x10108 | pointer to EvfDevState state object (15 x 4); initial 0 | H | A2-verify, A15 |
| 0x9B48 | pointer to EvfDev object (+0x08 TaskClass, +0x0C state object, +0x14 priority) | H | A2 |
| 0x554C | pointer to EvfCap owner object | M | A2 |
| 0x10238 | Mem1Path context: +0x40 `lv_save_raw` flag, +0x44 `lv_set_mm`, +0x50 path set up, +0x58 current buffer (0x10290), +0x5C first buffer, +0x60 armed latch, +0x64/+0x68 channel-3 completion callback and argument (0x1029C / 0x102A0) | H | A1-verify |
| 0x19094 | `lv_save_raw` state (+4 argument) | H | A1-verify |
| 0x10220 | CapPath context | M | A1 |
| 0x1910C | MvRawProPath context (+0x10 variant argument) | H | A1-verify |
| 0x23800 | MvRawProDs context: +0x04 ring count, +0x18 expected mask, +0x20 accumulator, +0x30 variant, +0x34/+0x38 ring | H | A1-verify |
| 0x292B0 | RawPro context: +0x00 ring count, +0x04 mode, +0x28 accumulator, +0x30 / +0x34 EDMAC channels, +0x60 raw-write flag, +0x64/+0x68 ring; +0x04 initial 0x00120000 | H | A1-verify, A15 |
| 0x9A04 | LvJob context; 0x50AF8 + n*0x188 (n = 0..5) job slots, word 0 non-zero = free | H | A1-verify |
| 0x93A4 | ImagerState context (+0x0C save-raw mode, +0x1C semaphore) | H | A1-verify |
| 0x22D9C | soft-assert sink {function, argument, enable}; enable was 0 at run time | H | A1-verify |
| 0x4000 | assert handler pointer used by Canon's assert 0xE0674574 | H | A1-verify |
| 0x39B84 (lock 0x39B80), 0xBEE18 | TaskClass message-node free list head; pool of 1536 x 16 bytes | H | A2-verify, A3-verify |
| 0x5154, 0x5158 | DebugMsg context pointer; notify gate (initial 1) | H | A3-verify, A15 |
| 0x1008, 0x1014 | CPU id; top of the per-CPU stack region (0xDF001000 on CPU 0) | H | A3-verify |
| 0x83F24, 0x84184, 0x83B94 | EDMAC completion callback table, second callback table, hold masks (76 entries each) | H / M / M | A5 |
| 0xB0F0 | resource-lock manager state (+8 bucket array) | M | A12 |
| 0x5128 | RscWrap function table (10 slots; slot 0 = 0xE057CDDF) | H | A8-verify |
| 0x5280, 0x51F4 | RscMgr object pointer; AllocateMemory pool handle | M | A8 |
| 0x4FF4 | PropertyMgr object pointer; initial 0 | M | A15 |
| 0x10654 | sensor drive mode (0x1064C + 8), 0..13 | H | A9-verify, A10 |
| 0x216C4, 0x216C8, 0x216E0, 0x216E4 | sensor / engine table base pointers (stills pair, LV pair); initial 0; point to RAM at run time (inferred) | M | A10, D1 |
| 0x19494 | Sio0Driver state; 0x194B4 / 0x194B8 pre/post callbacks of `cmos_write` | M | A10 |
| 0x6DC60 | LvComPack handler table (6 handlers + 6 arguments); slot 1 also used for the EvfDev post callback per A2 | M | A9-verify, A2 |
| 0x8B94 | MovieRecorder object pointer | H | A6-verify |
| 0x63FC | base of block read/write timing (+0x10 last write, +0xC last read, microseconds) | M | A7 |
| 0x2DEE8 + 0x78 | per-drive FIO semaphore table | H | A7-verify |
| 0x5BD8, 0x5BDC | default task stack, priority ceiling (run-time values unknown) | M | A11 |
| 0x6BC61CC8 / 0x2BC61CC8 | Canon's LV raw buffer, uncached / cacheable view, as seen in one earlier 1080p hardware session; not a constant to hard-code | hardware log | docs/35:26 |

MMIO: 0xD0420200 channel 3 block (+0x50 size, +0xA0 address); 0xD0423254 channel 3 selector;
0xD0406198 / 0xD040619C timer A; 0xD04061A4 / 0xD04061B8 timer B; 0xD0406190 role unknown (A5, A9).

### 11.3 ROM functions

Names in quotes are Canon debug strings; other names are descriptive labels from the sprint.

| Address | Function | Conf. | Source |
|---|---|---|---|
| 0xE06C0D04 | `lv_save_raw` handler | H | A1-verify |
| 0xE0753D86 | `lv_set_mm` handler | H | A1-verify |
| 0xE0753DAA / 0xE0754236 / 0xE0754298 | Mem1Path Init / SetPath / SetParam | H | A1-verify, D1 |
| 0xE0753D9C | channel-3 completion trampoline | H | A1-verify |
| 0xE06AD51A | "imagerMem1PathWrDmacCompleteCBR" | H | A1-verify |
| 0xE06CFB10 / 0xE06CFBF2 / 0xE06CFC2C / 0xE06CFA70 | CapPath Init / Start / SetParam / CompleteCBR | M | A1 |
| 0xE076FDF2 | MvRawProPath Start | M | A1 |
| 0xE07B9B28 / 0xE07B9CB0 / 0xE07B9AD0 | MvRawProDs Create / PushFrame / SubCompleteCBR | H | A1-verify, D1 |
| 0xE0760A88 / 0xE0760E74 / 0xE0760A30 | RawPro Create / PushFrame / EDmacCompleteCBR | H | A1-verify |
| 0xE06AF4B2 | LvJob CreateJob | H | A1-verify |
| 0xE0674574 | Canon assert (jumps to `[0x4000]` if set) | H | A1-verify |
| 0xE05BB31A / 0xE05BB2B2 | CreateStateObject / state dispatcher | H | A2-verify |
| 0xE05B9E60 / 0xE05B9E20 | CreateTaskClass / TaskClass loop | H | A2-verify, A11 |
| 0xE05B9F9E / 0xE05B9F10 / 0xE05BA078 | TryPostEvent / PostEvent / pending count | H | A2-verify, A3-verify |
| 0xE069A670 / 0xE069A7E8 | "evfLvComEvent" (asserts line 0x174) / event-13 poster (line 0x21E) | H | A2-verify |
| 0xE06EF37A / 0xE06EF51E / 0xE06EF732 | "evfVdInterrupt" / "evfSetParamInterrupt" / "evfReadOutDoneInterrupt" | H | A2-verify |
| 0xE069B2EA / 0xE069AEDE | "evfdevUpdateParam" / "evfdevSendJob" | H | A2-verify |
| 0xE05D10C4 / 0xE005D89C / 0xE005DB9C | DryosDebugMsg / its small formatter / vsnprintf (neither guards NULL `%s`) | H | A3-verify |
| 0xE05B802E / 0xE05B80C2 | EDMAC write-channel input select / disconnect | H | A5-verify, D1 |
| 0xE05B7FB6 / 0xE05B7FDE / 0xE05B8006 | VdKick off / on / pulse per EDMAC channel | H (identity of the mk1 homolog: L) | A5-verify |
| 0xE05B848E / 0xE05B849A / 0xE05B88FC | EDMAC set address / set size / transfer mode | H / M / M | A5 |
| 0xE05B7B42 / 0xE05B7A6C / 0xE05B7B08 | EDMAC prepare / enable leaf / disable leaf | H | A5-verify |
| 0xE05B7C48 / 0xE05B7F68 | RegisterEDmacCompleteCBR / Unregister | H | A5-verify |
| 0xE05B8308 / 0xE05B83B2 | abort write / abort read channel | H | A5-verify |
| 0xE05B752E / 0xE05B7520 | `Engine::Outer.c` port select / port VdKick pulse - **do not use as EDMAC stubs** | H | A5-verify |
| 0xE05BE4BE / 0xE05BE6A6 / 0xE05BE740 / 0xE05BE5A2 / 0xE05BE8A6 | resource lock Create / blocking Lock / Unlock / Delete / try-lock | M | A12 |
| 0xE0608904 / 0xE0608932 / 0xE0608960 | shoot-memory thunks (slots 0 / 4 / 8 of the table at 0x5128) | H | A8-verify |
| 0xE057B7BA / 0xE057DA00 | SRM 1stJob allocate / free (identity from Canon's failure string) | H address, L usability | A8-verify, B4 |
| 0xE05BEE20 / 0xE05BECAA / 0xE05BEC7E | `_AllocateMemory` / GetMemoryInformation / GetSizeOfMaxRegion | M | A8 |
| 0xE04B3468 | "srmGetShootMemAreaSize" | H | A8-verify |
| 0xE05D088C / 0xE05D08F4 / 0xE05D0958 | data-cache invalidate / clean / clean+invalidate by range | H | A13-verify |
| 0xE0329D10 | ROM memcpy (word loop) | H | A13-verify |
| 0xE05B1A00 | `_FIO_WriteFile` | M | A7 |
| 0xE008564E / 0xE00850F0 | "FSUBlockWrite" / its buffer check | H (check), M | A7-verify |
| 0xE00E5C84 / 0xE00DE6D4 | "sdIOControl" / SD write | M | A7 |
| 0xE05B16FA / 0xE05B4C2A / 0xE05B46D0 / 0xE05B2FF0 | CreateSC / EntSC / RecordStart / DelayFlush (Speed Class mode) | M | A7 |
| 0xE00E6ECA | "DebugSTG_GetSDWriteSpeed" (registered at boot via 0xE00E8362) | H | A7-verify |
| 0xE071FE72 | "handleMovieButton" | H | A6-verify |
| 0xE0547518 / 0xE05475DC | "IsEnableMovieStart" / "IsEnableAnyTimeMovieStart" | H | A6-verify |
| 0xE024A8FC / 0xE024A068 | MovRecState entry wrapper / "mvrRecStart" body | H | A6-verify |
| 0xE0202106 / 0xE0202612 | "fmMountCard" / "fmUnmountCard" (FileMgr state handlers) | H | A6-verify |
| 0xE06CAEB2 / 0xE06CAF10 | `engio_write` / logging twin | M | A10 |
| 0xE06C2F98 | `cmos_write` (sensor serial writer) | M | A10 |
| 0xE06CFE68 / 0xE06CFE80 | drive mode setter / getter | H | A9-verify |
| 0xE06E7E60 | in-place patch of a {register, value} list entry | H | A10, D1 |
| 0xE071FB14 / 0xE071FB42 | LvComPack dispatch / handler registration | H | A9-verify |
| 0xE05F40BA / 0xE05F42D2 | `task_create` / `task_create_ex` (extra CPU argument, must be < 2) | M | A11 |
| 0xE0618F1C / 0xE0618F8A | XdmacWrap open / start (arguments unknown) | H address, L use | A13-verify |

Label proposals for Ghidra are in `docs/offline/labels/*.csv`.

### 11.4 Open disagreements and unverifiable claims a reader should not rely on

| Topic | Positions | Status |
|---|---|---|
| Which state object to hook | B5: keep 0x10108 (6, 3) because a state spy on hardware saw it tick; A2: it is the wrong object, use EvfState 5/5 | Identity settled for A2 (ROM). Which hook is *safe* is a hardware question |
| Why a hook-time copy storms | A1: copy starves EDMAC programming; A1-verify: could equally be a completion-mask mismatch stalling the exact-equality pop | Open; distinguishing probe in A1-verify (read ring, accumulator and count at the first assert) |
| Engine timing tables in ROM1 or RAM | A9: ROM1 0xF0014914; A10: writable | A10 right for run time (D1); A9's values are the load image (inferred) |
| Owner of channels 5 / 6 | A12: MvRawProPath; D1: SantaPath | Inferred for SantaPath |
| SRM 1stJob usable in LV | A8: only route above 63 MB, untested; B4: community says it returns nothing on DIGIC 8 | Open; needs the A8 hardware probe |
| Active area for 1080p | mk1 developer: 2008x1128 at 88/34; ROM record: 2000x1126 at 92/38 | Open; check a decoded frame |
| Copy priority | A11: 0x15 / 0x16; C9-recommended: start at 0x1B / 0x1A throttled | Design choice, to be settled by a hardware A/B |
| Drive mode -> video mode -> geometry map | A9 | Hypothesis (table without code reference) |
| mk1 `EVF_STATE` is EvfState; mk1 homolog of each EDMAC stub | A2, A5 | Unverifiable (no mk1 ROM on disk) |
| Stale mem2mem frames caused by the wrong stubs | A5 | Unverifiable statically |

---

## 12. Glossary

- **ADTG**: on older Canon bodies, the sensor timing chip and its write function. Absent on this body.
- **Alias (cacheable / uncached)**: the same DRAM seen at address x (through the CPU data cache) and at
  x | 0x40000000 (bypassing it). DMA hardware and the card driver need the uncached view or flushed caches.
- **Assert**: Canon's run-time check; on failure it calls 0xE0674574, which hangs unless a handler is
  installed at RAM 0x4000. ML installs one that logs and returns.
- **Boomer**: the multiplexer network selecting which module output feeds an EDMAC write channel. A
  *selector* value is `class bits << 16 | input-select type << 8 | mux value`.
- **CBR**: callback routine.
- **CapPath / Mem1Path / MvRawProPath / SantaPath / SapPath**: "path" modules of `LiveViewDrive` that
  each configure a chain of engine blocks and EDMAC channels for one data stream.
- **DAF**: dual-pixel autofocus. **EIS**: electronic image stabilisation.
- **DIGIC 8 (D8)**: the image processor generation of this camera, EOS R, RP, M50, SX740.
- **Drive mode**: Canon's index (0..13) of sensor readout configurations.
- **DryOS**: Canon's real-time operating system. Lower priority number = higher priority.
- **EDMAC**: Canon's image-data DMA engine; 76 channels on this body (0-38 write, 39-75 read).
- **engio**: Canon's list-based writer of engine MMIO registers.
- **Event procedure (eventproc)**: a Canon function registered by name, callable with `call("name")`.
- **EvfCap / EvfDev**: the two Canon tasks that run the LiveView capture (EvfState) and develop
  (EvfDevState) state machines.
- **FIO**: Canon's file I/O layer (`FIO_WriteFile` etc.). **FSU**: the block layer under it.
- **LV**: LiveView. **LvJob**: the per-frame job record of the LiveView core (6 slots).
- **mem2mem**: copying memory to memory with a read EDMAC channel feeding a write EDMAC channel.
- **MLV**: Magic Lantern's raw video container format.
- **MMIO**: memory-mapped hardware registers (0xC0000000-0xDEFFFFFF here).
- **MVR**: Canon's movie recorder (H.264).
- **Pitch**: bytes per image line (3668 for 2096 pixels at 14 bit).
- **PropMgr / property**: Canon's settings database and its task.
- **raw_vidx**: the project's experimental raw recorder module (`modules/raw_video/raw_vidx`).
  **mlv_lite**: ML's standard raw recorder module.
- **Ring (completion ring)**: the 2-entry queue of expected EDMAC completions in MvRawProDs and RawPro.
- **RscMgr / shoot memory / SRM**: Canon's resource manager and its large image-buffer pools.
- **State object**: Canon's table-driven state machine (matrix of {next state, action}).
- **Storm**: project slang for a burst of repeating Canon asserts or errors.
- **TaskClass**: Canon's task-with-message-queue wrapper. **TryPostEvent**: its non-blocking post.
- **Thumb**: the ARM instruction set used by all ROM code here; call addresses are odd.
- **VD / VdKick**: vertical drive, the per-frame sync; a VdKick starts an engine block on the next VD.
- **XIP**: execute in place (code runs directly from ROM).

---

## 13. What an ML recorder must respect (checklist)

Each item is a consequence of the sections above; none has been validated on the camera in this sprint.

1. Use only `lv_set_mm(1)` + `lv_save_raw(1)` to arm the tap. Do not redirect, re-register, reset or
   abort Canon's channels (3, 41, and the channel in `[0x292B0+0x30]`). (Sections 2.4, 3.)
2. The state-object hook does nothing but count or signal. No copy, no sleep, no allocation, no logging
   in EvfDev or EvfCap context. (Section 5.)
3. Copy in a task outside Canon's LiveView priority band (0x0D-0x14), and do not let it run
   back-to-back without yielding. (Sections 5.4, 6.)
4. Judge pipeline health by the ring counts and the LvJob slots, not by the old "ready" flags. Treat the
   first MvRawPro / LvJob assert as fatal for the session. (Section 2.4.)
5. Align the source row address to 4 bytes (crop x a multiple of 16 pixels) before measuring any copy
   rate. (Section 7.4.)
6. Buffers for `FIO_WriteFile` are uncached and aligned; no other card I/O during recording; write large
   multiples of 512 KB. (Section 8.)
7. Budget about 65-75 MB/s at most; full 2096x1164 14-bit is a burst mode. Stay at 14 bit. (Sections
   8.3, 8.4, 9.2.)
8. Do not take the whole shoot-memory pool; do not rely on SRM until it is probed. (Section 7.3.)
9. Do not wait for or poll a card remount. Decide deliberately what Canon does with the record key.
   (Section 10.)
10. Test every RAM pointer in table 11.2 for non-zero before use; they are filled at run time.
11. Take no engine resource lock for the CPU-copy path. (Section 4.5.)
12. Any change to stubs or constants in the ML core shifts symbols and has caused boot failures on this
    body (memory note `m50ii-symbol-shift-boot-crash.md`); prefer calling the addresses of section 11.3
    from the module through function pointers.

The first hardware checks that would confirm or refute the load-bearing inferences of this document are
listed at the end of each A-report ("Next hardware checks"). The most decisive few: A2 check 1 and 3
(EvfState identity and per-frame inputs), A1 checks 2-4 (Mem1Path fields and ring counts), A3 check 1
(pool free count during recording), A13 check 4 (crop x = 192), A5 check 2 (real select on channel 13),
A9 check 1 (timer registers per mode).
