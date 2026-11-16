# 29 — DIGIC 8 Raw-Video Reference Lineage (M50 Mark II port map)

Maps each M50mk2.101 raw-video gap to concrete reference code in three D8 clones.
Addresses are ROM-specific to the *reference* cameras (SX740 1.0.2 / EOS R 1.8.0 /
M6II 1.1.1) and must be re-resolved for M50mk2 — they are recorded only to locate
the functions. This file lives outside the git repo, so addresses are fine here.

Clones:
- EDMAC engine ...... /mnt/data/WORK/eos/research/kitor_edmac_sx740      (branch edmac_sx740, HEAD 347beff)
- Raw LV + mlv_lite .. /mnt/data/WORK/eos/research/kitor_raw_draft_v2     (branch raw_draft_v2, HEAD 2682246)
- Config prereqs ..... /mnt/data/WORK/eos/research/m6ii_raw_video         (branch m6ii_raw_video, HEAD f3d555d)

================================================================================
A. DIGIC 8 EDMAC ENGINE  (kitor_edmac_sx740)
================================================================================

## A.1 The D8 model (why our NOP stubs are actually fine for the mem2mem path)
On D8 the D45 EDMAC API is NOT used. In BOTH SX740 and R.180 the following remain
literal NOP stubs and that is intentional:
  SetEDmac / ConnectWriteEDmac / ConnectReadEDmac / StartEDmac / AbortEDmac
  (SX740 function_overrides.c:127-150 ; R.180 function_overrides.c:253-274)
They are the legacy API; the SLURP path would need them, but the mem2mem/raw path
used here does not. Real transfers go through native Canon "low-level" EDMAC stubs.

Key structural differences vs D5 (src/edmac.c, src/edmac-memcpy.c):
- No "device id"/"where" connection arg. `ConnectReadEDmac_maybe(channel)` takes
  ONLY the read channel; there is no ConnectWriteEDmac equivalent.
- Channel connections are established implicitly by "Boomer" (BoomerVdKick /
  BoomerSelect) and/or EDMAC config structs, not by an explicit connect call.
- Sub-chip power gating: `PwrMng_WakeSubChips({0,7})` MUST be called before ANY
  EDMAC MMIO read/write or the camera hard-locks (edmac-d8.c:33-41).
- No shadow memory: `shamem_read()` is a direct load from 0xDxxxxxxx.

## A.2 SX740 edmac-d8.c  (kitor_edmac_sx740/platform/SX740.102/edmac-d8.c, full file)
Guarded by `#ifndef CONFIG_EDMAC_MEMCPY_D8` (line 25); wired via ML_SRC_EXTRA_OBJS.
Functions defined:
  - mem2mem_CBR(arg)                         :93   completion CBR; ORs status, sets flag at ==3
  - mem2mem_emdac_copy_d8(src,dst,si,di)     :103  the core mem2mem copy
  - edmac_memcpy_res_lock()/_unlock()        :153/161  reslock + wake/suspend subchips
  - edmac_copy_rectangle_cbr_start(...)      :168  mlv_lite entry; builds edmac_info, calls copy
  - edmac_copy_rectangle_adv_cleanup()       :213  dummy
  - edmac_read_chan / edmac_write_chan = 0x6 :217-218  dummy, "don't use"
Constants (SX740): mem2mem_devices{0,7}:81 ; mem2mem_resources{0x100AD,0x100BB}:82 ;
  RD_CH 46, WR_CH 13 :83-84 ; BOOMER_SELECTOR 0x373f0126 :85 ; MODE 0x0 :86 ; WAIT 50ms.
Copy sequence (:106-150), done BY HAND on SX740 because MemToMemE1_* are unresolved:
  edmac_reset_channel(RD/WR); edmac_reset_boomer_vdkick(WR);
  edmac_select_boomer(WR,0x373f0126); CreateEventFlag_strictly;
  RegisterEDmacCompleteCBR(RD,cbr,1)/(WR,cbr,2); edmac_reset_packunpack_mode(RD/WR);
  edmac_set_address(WR,dst)/(RD,src); edmac_set_size(WR/RD);
  edmac_set_transfer_mode(WR/RD,0); StartEDmac_maybe(WR)/(RD);
  ConnectReadEDmac_maybe(RD); WaitForAnyEventFlag; ...; UnregisterEDmacCompleteCBR;
  edmac_stop_boomer_maybe(WR).

## A.3 R.180 edmac-d8.c  (kitor_raw_draft_v2/platform/R.180/edmac-d8.c, full file)
Guarded by `#ifndef CONFIG_EDMAC_MEMCPY_R` (line 25). Structurally identical but
CLEANER — it delegates the boomer/reset/packunpack work to higher-level wrappers:
  MemToMemE1_SetupEDMAC(channels[RD,WR]) :113
  MemToMemE1_RegisterCBR(cbr,NULL)       :114
  MemToMemE1_ResetCBR() / _ClearEdmacCBR():142-143
These come from Engine::MemoryToMemoryEsub1.c and use EDOMAIN_EDMAC_1_* (SubChipID 0).
(EDOMAIN_EDMAC_5_* / Esub5 was tried and never transferred — edmac-d8.c:43-51.)
Same RD_CH 46 / WR_CH 13 / devices{0,7} / resources{0x100AD,0x100BB}.
==> PREFER THE R.180 VARIANT for the M50mk2 port IF its MemToMemE1_* symbols can be
    located in the M50mk2 ROM; otherwise fall back to the SX740 hand-rolled variant.

## A.4 Real EDMAC/event/reslock symbols in stubs.S (D8 — re-resolve for M50mk2)
These are the functions our NOP stubs must be replaced/augmented by. On D8 the
"real bodies" are these Canon stubs, NOT reimplemented SetEDmac() etc.

SX740.102/stubs.S:
  277  PwrMng_WakeSubChips            0xe0805c62
  278  PwrMng_SuspendSubChips        0xe0805c90
  286  edmac_set_address             0xe051f192
  287  edmac_set_size                0xe051f19e
  288  edmac_set_transfer_mode       0xe051f694
  289  StartEDmac_maybe              0xe051efb0
  290  ConnectReadEDmac_maybe        0xe051e63a
  293  edmac_reset_channel           0xe051e674
  294  edmac_reset_boomer_vdkick     0xe051eb40
  295  edmac_select_boomer           0xe051ebbe
  296  RegisterEDmacCompleteCBR      0xe051e75e
  297  UnregisterEDmacCompleteCBR    0xe051eaf2
  298  edmac_reset_packunpack_mode   0xe051ec96
  299  edmac_stop_boomer_maybe       0xe051ec52
  301  CreateResLockEntry            0xe0536cca
  302  LockEngineResources           0xe0536eb2
  303  UnLockEngineResources         0xe0536f52
  305  CreateEventFlag_strictly      0xe0541702
  306  DeleteEventFlag               0xe053212a
  307  WaitForAnyEventFlag           0xe0532248
  308  SetEventFlag                  0xe0532444
  309  ClearEventFlag                0xe0532472
  (MemToMemE1_SetupEDMAC/RegisterCBR/ResetCBR/ClearEdmacCBR are COMMENTED OUT :283-292 — DNE on SX740)

R.180/stubs.S:
  348  PwrMng_WakeSubChips           0xe0671742
  349  PwrMng_SuspendSubChips        0xe0671770
  351  MemToMemE1_SetupEDMAC         0xe0712bac
  352  MemToMemE1_RegisterCBR        0xe0712ca0
  353  MemToMemE1_ResetCBR           0xe0712caa
  354  MemToMemE1_ClearEdmacCBR      0xe0712c86
  355  edmac_set_address             0xe05364b6
  356  edmac_set_size                0xe05364c2
  357  edmac_set_transfer_mode       0xe05369b8
  358  StartEDmac_maybe              0xe05362d4
  359  ConnectReadEDmac_maybe        0xe053595e
  361  CreateResLockEntry            0xe054761e
  362  LockEngineResources           0xe0547806
  363  UnLockEngineResources         0xe05478a6
  365  CreateEventFlag_strictly      0xe05546f2
  366  SetEventFlag                  0xe053a5b8
  367  WaitForAnyEventFlag           0xe053a3bc
  368  ClearEventFlag                0xe053a5e6
  369  DeleteEventFlag               0xe053a29e

NOTE: SX740 resolves RegisterEDmacCompleteCBR as a real stub (0xe051e75e) and
      calls it directly; R.180 NOPs RegisterEDmacCompleteCBR in function_overrides.c:278
      and instead uses MemToMemE1_RegisterCBR. Pick one path consistently.

## A.5 EDMAC MMIO base/offsets
- Channel MMIO region: 0xD042xxxx. Raw LV write channel struct base
  RAW_LV_EDMAC_CHANNEL_ADDR = 0xd0420200  (channel 3 = EDOMAIN_EDMAC_1_WR_M1)
  (R.180 consts.h:150).
- Accessed as `struct edmac_mmio *raw_lv_edmac` fields: ram_addr, yn_xn, yb_xb
  (src/raw.c:669,739-740). shamem_read(0xDxxxxxxx) == direct read.

## A.6 Build wiring
- platform/<CAM>/Makefile.platform.default:
    ML_SRC_EXTRA_OBJS += function_overrides.o test_features.o edmac-d8.o
  (R.180:16). This compiles the platform edmac-d8.c into the build.
- Gating macros in internals.h:  CONFIG_EDMAC_MEMCPY_R  (R.180:29)
                                 CONFIG_EDMAC_MEMCPY_D8 (SX740:26)
- The GENERIC mem2mem in src/edmac-memcpy.c is gated by plain `CONFIG_EDMAC_MEMCPY`
  (src/edmac-memcpy.c:8) which the D8 platforms do NOT define — so the platform
  edmac-d8.c supplies edmac_copy_rectangle_cbr_start / edmac_memcpy_res_lock /
  edmac_read_chan / edmac_write_chan instead (no double definition).
- CAVEAT: edmac-d8.c wraps its body in `#ifndef CONFIG_EDMAC_MEMCPY_R/_D8` (line 25),
  which reads inverted vs the internals.h `#define`. It works in kitor's builds
  (real MLV produced), implying platform CONFIG_* are not globally -D'd into this TU;
  verify guard semantics when porting rather than copying blindly.

================================================================================
B. D8 RAW LIVEVIEW + mlv_lite  (kitor_raw_draft_v2, R.180 / EOS R)
================================================================================

## B.1 Config that turns raw on (R.180)
internals.h (kitor_raw_draft_v2/platform/R.180/internals.h):
  CONFIG_DIGIC_VIII :6 ; CONFIG_NO_BFNT :9 ; CONFIG_PLATFORM_POST_INIT :12 ;
  CONFIG_MEMORY_SRM_NOT_WORKING :18 ; CONFIG_LIVEVIEW :21 ;
  CONFIG_STATE_OBJECT_HOOKS :23 ; CONFIG_EVF_STATE_SYNC :24 ;
  CONFIG_NO_DEDICATED_MOVIE_MODE :26 ; CONFIG_EDMAC_MEMCPY_R :29 ;
  CONFIG_MEM_2GB :34 ; CONFIG_NEW_TASK_STRUCTS :36 ;
  CONFIG_TASK_STRUCT_V2_SMP :37 ; CONFIG_TASK_ATTR_STRUCT_V5 :38.
features.h (R.180): FEATURE_PICSTYLE :33 + CONFIG_RAW_LIVEVIEW :34
  ("These two needed to enable mlv_lite"); also FEATURE_GLOBAL_DRAW, FEATURE_CROPMARKS.
  UNDEFs: CONFIG_CRASH_LOG, CONFIG_PROP_REQUEST_CHANGE, CONFIG_AUTOBACKUP_ROM (:39-41).

CRITICAL: R.180 does **NOT** define CONFIG_EDMAC_RAW_SLURP, CONFIG_RAW_PHOTO,
CONFIG_ALLOCATE_RAW_LV_BUFFER, or CONFIG_MALLOC_STRUCT_V2. (grep across the tree
confirms SLURP only on D4/D5 platforms.) It uses the "traditional" LV buffer path.
=> The premise that "R.180 uses CONFIG_EDMAC_RAW_SLURP" is FALSE for this
   branch. The working R.180 raw path is CONFIG_RAW_LIVEVIEW-only.

## B.2 How R.180 gets raw without SLURP (src/raw.c)
Enable (raw.c raw_lv_enable, :2332-2350):
  #ifndef CONFIG_EDMAC_RAW_SLURP / #ifdef CONFIG_DIGIC_VIII:
     call("lv_set_mm", 1);   // switch LV output to RAW (default is YUV) — D8 only
     // call("lv_set_raw_wp",0) noted optional (SAP::HEAD source point), left off
  call("lv_save_raw", 1);    // make Canon dump raw to its EDMAC buffer
Disable (raw.c raw_lv_disable, :2384-2392):  call("lv_save_raw", 0).

Buffer discovery (NON-slurp branch):
  raw_get_default_lv_buffer() :668-669
     return CACHEABLE(shamem_read(&raw_lv_edmac->ram_addr));   // read Canon's EDMAC dst
  raw_lv_get_resolution() :737-748
     lv_raw_height = shamem_read(&raw_lv_edmac->yn_xn);
     lv_raw_size   = shamem_read(&raw_lv_edmac->yb_xb);
     pitch = lv_raw_size & 0xFFFF; width = pitch*8/14;
     height = MAX((lv_raw_height&0xFFFF)+1, ((lv_raw_size>>16)&0xFFFF)+1);
  raw_lv_edmac = (struct edmac_mmio*)RAW_LV_EDMAC_CHANNEL_ADDR (raw.c:140).

Optional per-frame redirect (raw.c raw_lv_redirect_edmac, :1997-2004):
  #ifndef CONFIG_EDMAC_RAW_SLURP branch:  raw_lv_edmac->ram_addr = CACHEABLE(ptr);
  (i.e. point Canon's own write EDMAC at an ML buffer — no ML slurp needed).

## B.3 RAW_TYPE_REGISTER / SHAD_GAIN / PREFERRED_RAW_TYPE on D8
- raw.c only defines RAW_TYPE_REGISTER / PREFERRED_RAW_TYPE for CONFIG_DIGIC_V (0xC0F37014/0x10)
  and CONFIG_DIGIC_IV (0xC0F08114/0x5) — raw.c:178-188. There is **NO D8 branch**; on D8
  the RAW type is selected via `call("lv_set_mm",1)` instead of a register poke.
- lv_raw_type / lv_raw_gain and the EngDrvOut(RAW_TYPE_REGISTER,...) code live inside
  `#ifdef CONFIG_EDMAC_RAW_SLURP` (raw.c:191-193, 2006-2055) and are therefore compiled
  OUT on R.180.
- SHAD_GAIN_REGISTER for D8 is defined in the PLATFORM consts.h, not raw.c:
    R.180 consts.h:151  SHAD_GAIN_REGISTER = 0xd0008030  // marked "WRONG, from 200D"
- The whole raw_lv_vsync()/edmac_raw_slurp() machinery is SLURP-only and unused on R.180.

## B.4 raw_write_chan (src/edmac-memcpy.c) — not needed for the R.180 path
  edmac-memcpy.c:369-373  #ifndef CONFIG_EDMAC_RAW_SLURP: raw_write_chan = 0xffffffff;
     "mlv_lite requires the symbol to exist ... value makes edmac_start_spy() fail (good)"
  edmac-memcpy.c:377-381  #ifdef CONFIG_EDMAC_RAW_SLURP: 5D3=0x4 ; EVF_STATE(60D+/all D5)=0x12
  => Because R.180 has no SLURP, raw_write_chan stays the 0xffffffff sentinel and is
     never used. The raw LV write channel that MATTERS is channel 3 (0xd0420200),
     owned by Canon; ML does not drive it via raw_write_chan.
  Frame extraction for MLV instead goes through edmac_copy_rectangle_cbr_start
  (platform edmac-d8.c mem2mem, section A) copying Canon's buffer into MLV frames.

## B.5 Diff vs stock raw.c — the D8-relevant hunks (conceptual)
The only genuine D8 additions in raw.c are small:
  1. raw.c:2342-2348  `#ifdef CONFIG_DIGIC_VIII` inside raw_lv_enable's non-SLURP block:
        call("lv_set_mm",1);   (the RAW-vs-YUV switch)   <-- NEW for D8
  2. raw.c:231-233   BLACK_LEVEL 2047 under `#if defined(EVF_STATE)` (already covers D8)
  3. Everything else (buffer discovery via raw_lv_edmac, resolution via yn_xn/yb_xb,
     redirect via ram_addr) is the pre-existing non-SLURP EVF_STATE path, REUSED for D8.
  => For M50mk2, the raw.c change is essentially: ensure the non-SLURP + CONFIG_DIGIC_VIII
     `lv_set_mm` branch is reachable, and provide raw_lv_edmac consts. No new D8 register
     block is required.

## B.6 The vsync hook problem & the EvfCap crash  (src/state-object.c)
Mechanism (CONFIG_STATE_OBJECT_HOOKS):
  - platform/R.180/include/platform/state-object.h:  EVF_STATE = *(struct state_object**)0x77c4
  - state_init() (INIT_FUNC) :278-300 calls stateobj_start_spy(EVF_STATE, stateobj_lv_spy).
  - stateobj_start_spy() :258-276 saves the object's StateTransition_maybe and replaces it
    with the spy (matrix is in RAM, not ROM, so no patching needed).
  - stateobj_lv_spy() :133-244 runs the original transition, then on
       (self==EVF_STATE && input==5 && old_state==5)  [evfReadOutDoneInterrupt]
    under CONFIG_EVF_STATE_SYNC (:207-214) calls vsync_func().
  - vsync_func() :78-116 = per-frame hook: module_exec_cbr(CBR_VSYNC) [mlv_lite frame grab],
       digic_iso_step, image_effects_step; raw_lv_vsync() only under SLURP (:86-88, off on R).
EvfCap crash:
  - SX740.102/internals.h:28-29 and M6II.111/internals.h keep the comment
    "hooking EFV_STATE ends with EvfCap crashes, requires investigation".
    SX740 leaves CONFIG_STATE_OBJECT_HOOKS enabled at :20 but re-comments it at :29 (conflict);
    the crash is UNRESOLVED on non-R D8.
  - R.180 enables the hook cleanly and it works. The difference (EVF present, EvfCap
    behaviour) is not root-caused in these repos.
  - M50mk2.101/internals.h currently has `//#define CONFIG_STATE_OBJECT_HOOKS` (commented),
    matching M6II. Enabling it is the risky step for our port.

## B.7 mmu_patches / ROM patches for raw
  - R.180 has NO include/platform/mmu_patches.h at all (only mvr.h + state-object.h).
  - function_overrides.c NOPs patch_instruction (returns -1) and unpatch_memory (:239-248).
  => R.180 raw needs ZERO ROM/MMU patches. The buffer redirect is done by writing the
     live EDMAC channel register (raw_lv_edmac->ram_addr), not by patching ROM.
  - M6II DOES ship a mmu_patches.h but all four patch arrays are EMPTY
    (early_data/normal_data/early_code/normal_code) — M6II.111/include/platform/mmu_patches.h.
  => M50mk2's empty mmu_patches.h arrays are acceptable for a first raw bring-up.

## B.8 mlv_lite enablement
  - Makefile.user.default:225  CONFIG_MODULES = y
  - modules/Makefile.modules.default:  ML_MODULES default list includes mlv_lite (+ mlv_play,
    mlv_rec, mlv_snd, ...). R.180 ships NO modules.included/modules.hidden override, so it
    inherits that default list.
  - features.h FEATURE_PICSTYLE + CONFIG_RAW_LIVEVIEW are the switches that make mlv_lite build.
  - Platform "glue to make mlv_lite happy": edmac-d8.c edmac_read_chan/edmac_write_chan dummies
    (:213-214) and edmac_copy_rectangle_adv_cleanup (:209); function_overrides.c
    RegisterEDmac*CBR NOPs (:278-306); srm_malloc_suite returns 0 (SRM disabled, :221-230).

================================================================================
C. M6II CONFIG PREREQUISITES  (m6ii_raw_video, M6II.111)  — closest body-class analog
================================================================================

## C.1 internals.h (the D8 CONFIG baseline; very close to M50mk2)
  CONFIG_DIGIC_VIII ; CONFIG_RPC ; CONFIG_MMU ; CONFIG_NO_BFNT ;
  CONFIG_MEMORY_SRM_NOT_WORKING ; CONFIG_LIVEVIEW ; CONFIG_STATE_OBJECT_HOOKS (enabled,
  with the EvfCap-crash warning comment) ; CONFIG_MEM_2GB ; CONFIG_MALLOC_STRUCT_V2 ;
  CONFIG_TASK_STRUCT_V2_SMP ; CONFIG_TASK_ATTR_STRUCT_V5.
  M6II does NOT define CONFIG_RAW_LIVEVIEW / CONFIG_EDMAC_MEMCPY_* — no raw yet.
  M6II has NO edmac-d8.c and its stubs.S/consts.h have essentially NO edmac/raw symbols
  (grep found only dialog_redraw). ==> M6II is a CONFIG reference, NOT an EDMAC reference.

## C.2 debug.c "raw experiment"
  EXPECTATION CHECK: the hypothesised bespoke ~65-line raw experiment in run_test()/
  Don't-Click-Me is NOT present as M6II-specific code.
  - run_test() (debug.c:348-359) only does DryosDebugMsg("run_test fired") + call("dumpf")
    and a `#if 0 && defined(CONFIG_200D)` disabled scaffold.
  - The raw code that exists is the STOCK block in dump_img_task() (debug.c:219-292),
    guarded by `#ifdef CONFIG_RAW_LIVEVIEW`: raw_lv_request(); raw_update_params();
    wait_lv_frames(3); memcpy(buf, raw_info.buffer, raw_info.frame_size); save_dng(...).
    Since M6II does not define CONFIG_RAW_LIVEVIEW, this is INERT. It is a good template
    for a single-DNG capture test once raw_info.buffer is populated, but it is not a
    novel M6II contribution.

## C.3 fps-engio_per_cam
  - M6II.111/fps-engio_per_cam.c is REAL (20 lines): get_fps_register_a/b() wrap
    _get_fps_register_a/b(); get_fps_register_a_default() returns 1122<<16 ("see 200D").
  - R.180 ships NO per-cam fps file (uses generic fps-engio.c).
  => For M50mk2's stubbed fps-engio_per_cam.c, port M6II.111's version (needs
     _get_fps_register_a/b stubs resolved) or leave the default-returning shim.

## C.4 modules config
  - M6II.111/modules.included = { bench, file_man } only (no mlv_lite yet).
  - M6II.111 also ships modules.hidden. R.180 ships neither (uses global default → mlv_lite).

================================================================================
D. CONSOLIDATED M50mk2 GAP → REFERENCE MAP  (ranked by dependency)
================================================================================
Legend: reference file paths are inside the clones listed at top.

Order 1 — D8 EDMAC ENGINE (do first; everything else depends on mem2mem copy)
  Gap: EDMAC/engio/shamem funcs are NOP in M50mk2.101/function_overrides.c;
       raw_write_chan unset; no edmac-d8.c.
  Do:  (a) Add platform/M50mk2.101/edmac-d8.c ported from
           kitor_raw_draft_v2/platform/R.180/edmac-d8.c (preferred, MemToMemE1_* based)
           OR kitor_edmac_sx740/platform/SX740.102/edmac-d8.c (hand-rolled fallback).
       (b) Resolve in M50mk2 stubs.S the symbols from A.4 (edmac_set_address/size/
           transfer_mode, StartEDmac_maybe, ConnectReadEDmac_maybe, PwrMng_Wake/Suspend
           SubChips, CreateResLockEntry, LockEngineResources, UnLockEngineResources,
           CreateEventFlag_strictly, Set/Wait/Clear/DeleteEventFlag, and either
           MemToMemE1_* [R path] or edmac_reset_channel/select_boomer/reset_boomer_vdkick/
           reset_packunpack_mode/stop_boomer_maybe + RegisterEDmacCompleteCBR [SX740 path]).
       (c) internals.h: #define CONFIG_EDMAC_MEMCPY_R (or _D8); add edmac-d8.o to
           Makefile.platform.default ML_SRC_EXTRA_OBJS.
       (d) Leave SetEDmac/ConnectWriteEDmac/ConnectReadEDmac/StartEDmac/AbortEDmac as NOP
           (they are unused on the mem2mem path).
  Ref: R.180 edmac-d8.c (all) ; R.180 stubs.S:347-369 ; R.180 Makefile.platform.default:16.

Order 2 — RAW_INFO PARAMS / shamem / consts (needed before raw.c can read buffers)
  Gap: shamem_read is NOP; no RAW_LV_EDMAC_CHANNEL_ADDR / SHAD_GAIN for D8;
       CONFIG_PROP_REQUEST_CHANGE undef state.
  Do:  (a) Port shamem_read() direct-read body → function_overrides.c
           (R.180 function_overrides.c:204-219 : reject non-0xDxxxxxxx, else *(uintptr_t*)addr).
       (b) Add to M50mk2.101/consts.h:
             RAW_LV_EDMAC_CHANNEL_ADDR (find M50mk2 raw LV write channel; R uses 0xd0420200
                                        = ch3 EDOMAIN_EDMAC_1_WR_M1)
             SHAD_GAIN_REGISTER (R placeholder 0xd0008030; re-resolve)
       (c) Keep _EngDrvOut / _engio_write NOP for the non-SLURP path (R.180 keeps them NOP).
  Ref: R.180 consts.h:150-151 ; R.180 function_overrides.c:204-219 ; raw.c:140,668-669,737-748.

Order 3 — raw.c CONFIG_RAW_LIVEVIEW + D8 enable branch
  Gap: raw.c has no D8 enable path; M50mk2 lacks CONFIG_RAW_LIVEVIEW.
  Do:  (a) features.h: #define FEATURE_PICSTYLE + CONFIG_RAW_LIVEVIEW; undef CONFIG_PROP_REQUEST_CHANGE.
       (b) internals.h: add CONFIG_MEM_2GB (M50mk2 currently lacks it — R.180 & M6II both set it),
           CONFIG_NO_DEDICATED_MOVIE_MODE, CONFIG_PLATFORM_POST_INIT if needed.
       (c) raw.c: ensure the non-SLURP CONFIG_DIGIC_VIII branch (call "lv_set_mm",1 +
           "lv_save_raw",1) is active (raw.c:2342-2350). No RAW_TYPE_REGISTER D8 block needed.
  Ref: R.180 features.h:33-34 ; R.180 internals.h ; src/raw.c:2332-2350, 656-750.

Order 4 — EVF_STATE VSYNC HOOK  (per-frame; HIGH RISK: EvfCap crash on non-R D8)
  Gap: CONFIG_STATE_OBJECT_HOOKS commented out in M50mk2.101/internals.h.
  Do:  (a) Add platform/M50mk2.101/include/platform/state-object.h with EVF_STATE pointer
           (find M50mk2 EVF StateObject; R = *(struct state_object**)0x77c4).
       (b) internals.h: enable CONFIG_STATE_OBJECT_HOOKS + CONFIG_EVF_STATE_SYNC.
       (c) src/state-object.c is generic — stateobj_start_spy(EVF_STATE, stateobj_lv_spy)
           installs the spy; vsync_func() fires on input==5/old_state==5.
       RISK: SX740 & M6II both note this crashes in EvfCap; validate carefully. If it
             crashes, raw single-frame capture (Order 3 + debug.c dump_img_task) can still
             work without the per-frame hook; only continuous mlv_lite needs CBR_VSYNC.
  Ref: R.180 include/platform/state-object.h ; src/state-object.c:78-116,133-244,258-300 ;
       SX740 internals.h:20-29 (crash note).

Order 5 — mlv_lite / modules glue (last)
  Gap: M50mk2 has CONFIG_MODULES:=n and CONFIG_TCC:=n.
  Do:  (a) Makefile.user / platform: CONFIG_MODULES = y (+ CONFIG_TCC = y if modules built from source).
       (b) Rely on global ML_MODULES default (mlv_lite) OR add modules.included with mlv_lite.
       (c) Provide mlv_lite glue already covered by edmac-d8.c dummies + srm_malloc_suite NOP.
       (d) fps-engio_per_cam.c: port M6II.111 version (get_fps_register_a/b + default 1122<<16).
  Ref: Makefile.user.default:225 ; modules/Makefile.modules.default ;
       M6II.111/fps-engio_per_cam.c ; R.180 edmac-d8.c:208-214 ; R.180 function_overrides.c:221-306.

================================================================================
KEY CORRECTIONS TO PRIOR ASSUMPTIONS
================================================================================
1. R.180 does NOT use CONFIG_EDMAC_RAW_SLURP. Its working raw path is
   CONFIG_RAW_LIVEVIEW-only: Canon fills its own EDMAC buffer (lv_save_raw +
   lv_set_mm), ML reads it via raw_lv_edmac MMIO and mem2mem-copies frames. This is
   simpler than SLURP and is the recommended M50mk2 target.
2. Therefore raw_write_chan (0xffffffff sentinel) is IRRELEVANT to the R.180 port —
   do not chase the SLURP 0x12 value.
3. RAW_TYPE_REGISTER / PREFERRED_RAW_TYPE have NO D8 definition; D8 selects raw via
   call("lv_set_mm",1). Only SHAD_GAIN_REGISTER lives in platform consts.h.
4. R.180 needs NO ROM/MMU patches for raw (no mmu_patches.h; patch_instruction NOP'd).
5. The M6II debug.c raw code is stock/inert, not a bespoke experiment. M6II's value
   is its internals.h CONFIG baseline and the real fps-engio_per_cam.c.
6. The EvfCap crash when hooking EVF_STATE is real and UNRESOLVED on SX740/M6II; it
   works on R.180. This is the single biggest risk for the M50mk2 continuous-raw port.
   ==> SUPERSEDED by Section E: the M50 mk1 branch (same DIGIC 8 family) enabled the
       EVF_STATE hooks and mlv_lite works — the crash was likely a symptom of an
       incomplete pipeline, not the hook itself. Risk downgraded.

================================================================================
E. M50 mk1 — THE PRIMARY REFERENCE  (bilalfakhouri branch `M50_rawvideo`)
================================================================================
Clone: /mnt/data/WORK/eos/research/bilal_M50_rawvideo/  (branch M50_rawvideo, depth 120)
Platform: platform/M50.110/  — SAME DIGIC 8 family as M50 II; our M50mk2.101 port
STARTED from the M50.110 stubs, so this is the closest reference by far.
Working raw video + mlv_lite achieved Aug 2025. Key commits:
  32dbd9c "M50: added RAW video support, mlv_lite works"   (the payload)
  e4ad4dd "M50: Edmac memcpy"
  850c229 "M50: define FPS Timers and TG_FREQ_BASE"
  5312cd4 "M50: enable CONFIG_MMU_REMAP" ; dbc7594 "M50: Added MMU support"

## E.1 EDMAC — hand-rolled (SX740/M50 style), NOT R.180 MemToMemE1
platform/M50.110/edmac-d8.c (220 lines, e4ad4dd+32dbd9c), gated CONFIG_EDMAC_MEMCPY_D8.
M50 family has NO MemToMemE1_* symbols (same as SX740) → uses the hand-rolled boomer
sequence (edmac_reset_channel / reset_boomer_vdkick / select_boomer(0x373f0126) /
set_address/size/transfer_mode / StartEDmac_maybe / ConnectReadEDmac_maybe /
RegisterEDmacCompleteCBR — edmac-d8.c:106-150). Constants identical to SX740/R:
devices{0,7}, resources{0x100AD,0x100BB}, RD_CH 46, WR_CH 13, MODE 0, WAIT 50ms.
==> CORRECTION to §A.3/§D-Order1: for M50mk2 the SX740/M50 hand-rolled variant is the
    PRIMARY choice (not a fallback). Copy platform/M50.110/edmac-d8.c directly.
M50.110 stub addresses (stubs.S:220-253 — sibling ROM, re-resolve for 1.0.1):
  PwrMng_WakeSubChips 0xe0557ece ; PwrMng_SuspendSubChips 0xe0557efc
  edmac_set_address 0xe054a9ae ; edmac_set_size 0xe054a9ba ; edmac_set_transfer_mode 0xe054aeb0
  StartEDmac_maybe 0xe054a7cc ; ConnectReadEDmac_maybe 0xe0549e52 ; edmac_reset_channel 0xe0549e8c
  edmac_reset_boomer_vdkick 0xe054a35c ; edmac_select_boomer 0xe054a3da
  RegisterEDmacCompleteCBR 0xe0549f76 ; UnregisterEDmacCompleteCBR 0xe054a30e
  edmac_reset_packunpack_mode 0xe054a4b2 ; edmac_stop_boomer_maybe 0xe054a46e
  CreateResLockEntry 0xe055819e ; LockEngineResources 0xe0558386 ; UnLockEngineResources 0xe0558426
  CreateEventFlag_strictly 0xe057754a ; DeleteEventFlag 0xe05531be ; WaitForAnyEventFlag 0xe05532dc
  SetEventFlag 0xe05534d8 ; ClearEventFlag 0xe0553506
function_overrides.c: un-NOPs RegisterEDmacCompleteCBR/Unregister/UnLockEngineResources
(now real stubs); shamem_read = direct load with 0xDxxxxxxx guard (:126-139).

## E.2 Config (M50)
features.h:27-28  FEATURE_PICSTYLE + CONFIG_RAW_LIVEVIEW  ("needed to enable mlv_lite").
internals.h adds  CONFIG_STATE_OBJECT_HOOKS + CONFIG_EVF_STATE_SYNC +
  CONFIG_NO_DEDICATED_MOVIE_MODE + CONFIG_EDMAC_MEMCPY_D8 + CONFIG_MALLOC_STRUCT_V2.
UNDEF (kept): CONFIG_CRASH_LOG / CONFIG_PROP_REQUEST_CHANGE / CONFIG_AUTOBACKUP_ROM.
NOT defined on M50: CONFIG_EDMAC_RAW_SLURP, CONFIG_MEM_2GB, CONFIG_PLATFORM_POST_INIT.
srm_malloc_suite returns 0 (SRM disabled).
==> CORRECTION to §D-Order3: do NOT add CONFIG_MEM_2GB. M50 handles 2GB addressing
    INSIDE edmac-d8.c:196-217 — drops the `& 0x1FFFFFFF` mask on src/dst and inverts
    the UNCACHEABLE test ("inverted to make 2GB compatible"). Config flag not used.

## E.3 THE VSYNC / EvfCap RESOLUTION (key finding)
M50 simply enabled CONFIG_STATE_OBJECT_HOOKS + CONFIG_EVF_STATE_SYNC (internals.h:26-27,
32dbd9c). The old "EvfCap crash" comment + its //#define at internals.h:34-35 are STALE
dead text left behind. NO explicit root-cause fix exists in the commits — message is only
"mlv_lite works". Mechanism = generic src/state-object.c: state_init installs
stateobj_lv_spy on EVF_STATE (M50 = *(struct state_object**)0x6cec, state-object.h:4);
on self==EVF_STATE && input==5 && old_state==5 it calls vsync_func() → module_exec_cbr
(CBR_VSYNC) = mlv_lite frame grab. raw_lv_vsync() compiled out (no SLURP).
Inference: the SX740/M6II EvfCap crash was a SYMPTOM of an incomplete pipeline (shamem
returning 0, no EDMAC copy, SRM not stubbed); once M50 had EDMAC memcpy + direct
shamem_read + srm→0 + lv_set_mm all present, the hook completes cleanly.
==> Risk downgraded from HIGH to MEDIUM: proven-working precedent on the same chip
    family, but no documented root cause (residual risk).

## E.4 Buffer / raw.c (M50) — confirms non-SLURP
consts.h:122-123  RAW_LV_EDMAC_CHANNEL_ADDR 0xd0420200 (ch3, identical to R.180) ;
  SHAD_GAIN_REGISTER 0xd0008030 ("WRONG, from 200D"). MMIO — plausibly identical on M50 II.
raw.c:2454-2461 enable = call("lv_set_mm",1) + call("lv_save_raw",1) (R.180 D8 sequence).
raw_get_default_lv_buffer → CACHEABLE(shamem_read(&raw_lv_edmac->ram_addr)) (raw.c:683);
resolution via yn_xn/yb_xb (raw.c:753-754). M50-specific: skip_top=34/skip_left=88
(raw.c:1054, "FIXME 1080p only"); color matrix + DR from LibRaw/R; raw_lv_request_bpp
D8 shim = give_semaphore(raw_sem);return; (raw.c:2632, "idk how this works on D8").

## E.5 FPS timers (850c229)  &  MMU
TG_FREQ_BASE 27000000 (M50; vs 32MHz default). fps-engio_per_cam.h:
  FPS_REGISTER_A 0xD0406198 ; FPS_REGISTER_B 0xD04061A4 ; FPS_REGISTER_CONFIRM 0xD0406190
  (read via direct *(uintptr_t*) — no shamem on D8).
mmu_patches.h (dbc7594): shipped but ALL FOUR patch arrays EMPTY. Raw needs ZERO ROM
patches (confirms §B.7). MMU_REMAP still enabled (5312cd4) for the port's other needs.

## E.6 raw_vid module — NOT used by M50
modules/raw_video/raw_vid/ (names_are_hard) is a clean-room learning sandbox; still gets
vsync via MODULE_CBR(CBR_VSYNC,...) (event_pusher.c:433) — does NOT avoid the EVF_STATE
hook. M50's modules.included lists mlv_lite (stock), not raw_vid.

## E.7 Portability ranking  M50 mk1 → M50mk2.101
NEAR-DROP-IN (copy file, keep logic): edmac-d8.c ; fps-engio_per_cam.c/.h logic ;
  raw.c M50 hunks (skip/matrix/bpp shim/lv_set_mm branch) ; features.h+internals.h config ;
  empty mmu_patches.h ; state-object.c mechanism (generic).
SMALL ADJUST (sibling-ROM re-resolve, same symbol/structure): all stubs.S EDMAC/event/
  reslock addrs (the e0557ece…e0553506 block) ; PwrMng_* ; FPS_REGISTER_* (MMIO, maybe
  identical) ; RAW_LV_EDMAC_CHANNEL_ADDR 0xd0420200 + SHAD_GAIN 0xd0008030 (MMIO, plausibly
  identical) ; memcpy_dryos (bilal warns "might differ on other M50 roms") ; adtglog2 CMOS_write.
FULL RE (RAM/StateObject pointers, per-ROM): EVF_STATE 0x6cec ; SGI_HANDLERS_PTR 0x402c ;
  MALLOC_STRUCT_ADDR 0x56a1c ; LiveViewApp_dialog 0x139b8 ; RESTARTSTART 0xdc870 ; any DATA_PTR.
KNOWN-UNFINISHED (inherit the caveats): SHAD_GAIN/PACK32 "wrong, from 200D" ; no 4K skip
  values (1080p only) ; raw_lv_request_bpp D8 behavior not understood.
