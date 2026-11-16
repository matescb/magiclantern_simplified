# M50 Mark II baseline port — progress & method

Target: baseline ML (boot + ML GUI + debug menus; NO raw yet) = "the M50's current level".
Reference platform: `magiclantern_simplified/platform/M50.110` (near-identical D8 hardware).
Our WIP platform: `magiclantern_simplified/platform/M50mk2.101`.

## Phase 0 — build chain VALIDATED (done)
`platform/M50.110` builds clean and produces `autoexec.bin` + `magiclantern.zip`.
Only fix needed: `pip install --break-system-packages docutils`. gcc 13.2.1 works fine.

## Phase 2 — skeleton (done)
`platform/M50mk2.101` = copy of M50.110. `Makefile`: `MODEL=M50mk2`, `FW_VERSION=101`.
Still M50 placeholders (TODO): `RESTARTSTART`, `FIRMWARE_ID` (model id).

## Phase 3 — stub/const finding (started)
Method: disassemble `gang100.bin` with **capstone** (Thumb), string-anchor + structurally diff vs M50.110.
No Ghidra needed.

### DONE: boot patch points (consts.h BR_* macros), found @ 0xe0040000
| macro | M50mk2 1.0.1 | mk1 was | target fn |
|---|---|---|---|
| BR_DCACHE_CLN_1 | 0xE0040068 | 0xE0040068 | dcache_clean 0xe05d08f4 |
| BR_ICACHE_INV_1 | 0xE0040072 | 0xE0040072 | icache_invalidate 0xe05d09c8 |
| BR_DCACHE_CLN_2 | 0xE00400A0 | 0xE004009E | dcache_clean |
| BR_ICACHE_INV_2 | 0xE00400AA | 0xE00400A8 | icache_invalidate |
| BR_BR_CSTART    | 0xE00400C6 | 0xE00400BE | (bx r0 -> cstart) |
| BR_BZERO32      | 0xE0040152 | 0xE004014A | bzero32 0xe05d1f40 |
| BR_CREATE_ITASK | 0xE00401B4 | 0xE00401AC | create_itask 0xe02e9d80 |
Written into `platform/M50mk2.101/consts.h`. Layout near-identical to mk1 (good sign).

## TODO (the multi-week bulk)
- `RESTARTSTART` / user_mem_start: from create_itask(0xe02e9d80) itask struct arg.
- `FIRMWARE_ID` (model id): M50 mk1 = 0x412; M50 II differs — extract from ROM/props.
- `stubs.S`: re-find ~90 stubs (malloc/free, DebugMsg, task_create + task structs, GUI event table,
  DMA/EDMAC, property funcs, FIO, semaphores...). Use capstone string-anchoring vs M50.110 comments.
- `consts.h`: CURRENT_GUI_MODE, GUIMODE_*, LVAE_STRUCT + offsets, MALLOC_STRUCT_ADDR, GMT_FUNCTABLE,
  DRYOS_ASSERT_HANDLER, SGI handler ptr, etc.
- Then: build M50mk2.101 autoexec.bin.

## After baseline (later stages)
- QEMU attempt (low expectation on D8).
- Hardware bring-up (BRICK RISK): enable bootflag via Canon Basic boot_en.m, run autoexec, iterate.
- Raw video: graft kitor `edmac_sx740` + bilal `M50_rawvideo` (mostly copy edmac-d8.c + find PACK32_MODE).
