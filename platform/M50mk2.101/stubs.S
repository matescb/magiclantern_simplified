/** \file
 * Entry points into the firmware image.
 *
 * These are the functions that we can call from our tasks
 * in the Canon 1.1.0 firmware for the EOS M50.
 *
 */
/*
 * Copyright (C) 2021 Magic Lantern Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <stub.h>

/* All functions are Thumb. */


/** Startup **/
NSTUB(MAIN_FIRMWARE_ADDR, firmware_entry)
THUMB_FN(0xE0040104,  cstart)                               /* [M50II 1.0.1 OK] boot trampoline 0xE0040034 b.w here; reads MPIDR, calls bzero32 (0xE0040152) + create_init_task (0xE00401B4) */
THUMB_FN(0xE05D1F40,  bzero32)                              /* [M50II 1.0.1 OK] confirmed at cstart bl site 0xE0040152 */
THUMB_FN(0xE02E9D80,  create_init_task)                     /* [M50II 1.0.1 OK] confirmed at cstart bl site BR_CREATE_ITASK=0xE00401B4 */
THUMB_FN(0xE05D08F4,  dcache_clean)                         /* [M50II 1.0.1 OK] confirmed at bl site BR_DCACHE_CLN_1=0xE0040068 */
THUMB_FN(0xE00982EC,  dcache_clean_multicore)               /* [M50II 1.0.1 OK] bl right after dcache_clean at 0xE0072308 (same r0,r1); HW reg 0xC11007B0 (word@0xE0098334+0x34); leaf */
THUMB_FN(0xE05D09C8,  icache_invalidate)                    /* [M50II 1.0.1 OK] confirmed at bl site BR_ICACHE_INV_1=0xE0040072 */
THUMB_FN(0xE0040236,  init_task)                            /* [M50II 1.0.1 OK] r1 arg to create_init_task (word@0xE00401E4); push {r4,lr} prologue */

/** Tasks **/
DATA_PTR(    0x1010,  current_interrupt)                    /* from interrupt handler (VBAR + 0x18); where the interrupt ID is stored */
DATA_PTR(    0x1028,  current_task)                         /* from task_create; pointer to the current task structure */
THUMB_FN(0xE05BA37A,  msleep)                               /* [M50II 1.0.1 OK] r0=ms; [0x100c] interrupt-context assert; found via SleepTime(%d) caller */
THUMB_FN(0xE05F40BA,  task_create)                          /* [M50II 1.0.1 OK] 3 task-name anchors (GuiMainTask/PowerMgr/TaskMain) -> this bl; assert str "SystemIF::KerTask.c" */
THUMB_FN(0xE05F42D2,  task_create_ex)                       /* [M50II 1.0.1 OK] sibling of task_create (0xE05F40BA), +0x218; push{r4-r7,lr}; selects CPU for task */
NSTUB(    0x4108,  task_max)                             /* [M50II 1.0.1 OK, offline F7] was 0x40CC. get_task_by_id 0xE00A6BC6 bounds id by *(0x4108) (literal @0xE00A6F6C); enumerator 0xE014A65E walks *(0x40E8)+0x5C*n up to *(0x4108) */

/** File I/O **/
THUMB_FN(0xE05B163E, _FIO_CreateFile)                       /* [M50II 1.0.1 OK] 'Create' tag@0xE05B1A40; drive-letter parse; was 0xE05465E6 (mid-func, wrong) */
THUMB_FN(0xE05B13A4, _FIO_OpenFile)                         /* [M50II 1.0.1 OK] 'Open' tag@0xE05B15E8; push.w{r2-r8,lr}; Storage::Storage.c; was 0xE0546534 */
THUMB_FN(0xE05B1C1A,  FIO_CloseFile)                        /* [M50II 1.0.1 OK] 'Close' tag@0xE05B1F5C; fd validity check; was 0xE0546D56 */
THUMB_FN(0xE05B1848, _FIO_ReadFile)                         /* [M50II 1.0.1 OK] 'Read' tag; fmt 'fd=%d,pvBuffer=%#x,dwSize=%d'; was 0xE05469B6 */
THUMB_FN(0xE05B1A00, _FIO_WriteFile)                        /* [M50II 1.0.1 OK] 'Write' tag; fmt 'fd=%d, pvBuffer=%#x,dwSize=%d'; was 0xE0546B42 */
THUMB_FN(0xE05B1942,  FIO_SeekSkipFile)                     /* [M50II 1.0.1 OK] 'Search' tag@0xE05B1A90 (confirms M50.110 guess); was 0xE0546A82 */
THUMB_FN(0xE05B17B0, _FIO_RemoveFile)                       /* [M50II 1.0.1 OK] 'Remove' tag@0xE05B1A54; was 0xE054691C */
THUMB_FN(0xE05B27EE, _FIO_RenameFile)                       /* [M50II 1.0.1 OK] 'Rename' tag; psOldPath/psNewPath fmts; was 0xE0547926 */
THUMB_FN(0xE05B2C04, _FIO_CreateDirectory)                  /* [M50II 1.0.1 OK] 'CreateDir' tag@0xE05B2E58 (base, not CreateDirEx 0xE05B2CAA); was 0xE0547DB0 */
THUMB_FN(0xE05B30E6, _FIO_FindFirstEx)                      /* [M50II 1.0.1 OK] 'FirstEnt' tag@0xE05B32C0; drive-letter parse; was 0xE0548212 */
THUMB_FN(0xE05B31E6,  FIO_FindNextEx)                       /* [M50II 1.0.1 OK] 'NextEnt' tag@0xE05B32F8; hFindFile fmt; was 0xE0548318 */
THUMB_FN(0xE05B3348,  FIO_FindClose)                        /* [M50II 1.0.1 OK] 'CloseEnt' tag@0xE05B3748; was 0xE05483DA */
THUMB_FN(0xE05B2F3A,  FIO_Flush)                            /* [M50II 1.0.1 OK] 'Flush' tag@0xE05B3288; between CreateDir and FirstEnt; was 0xE0547F9A */
THUMB_FN(0xE05B1DB4, _FIO_GetFileSize64)                    /* [M50II 1.0.1 OK] 'AcqSize' tag; calls bzero32 0xE05D1F40; 64-bit size struct; was 0xE0546EF2 */

/** Bitmap **/
DATA_PTR(   0x11294,  bmp_vram_info)                        /* [M50II 1.0.1 maybe] used right after DISP_SetUpdateOSDVram (0xE05A1794); alt surface array 0x7F28C - verify on HW */

/** Debug messages **/
THUMB_FN(0xE05D10C4,  DryosDebugMsg)                        /* [M50II 1.0.1 OK] (class,level,fmt,...); push{r0-r3} va_list; found via SleepTime(%d) call site */

/** Dumper **/
THUMB_FN(0xE065FE58,  dump_file)                            /* [M50II 1.0.1 OK, offline F7] was 0xE00DB9E6 (mid-function). (name r0, addr r1, size r2); "A:/%s" @0xE065FF34; Remove/Create/Write/Close/Flush; 0x18-byte path buffer: keep names short */

/** Eventprocs (call by name) **/
THUMB_FN(0xE05CBF82,  call)                                 /* [M50II 1.0.1 matched] eventproc-by-name executor: table load -> name lookup bl 0xE02EAF36 -> blx execute; verify */

/** GUI timers **/
THUMB_FN(0xE0618BBE,  SetHPTimerNextTick)                   /* [M50II 1.0.1 OK] HPTimer cluster; sibling of SetHPTimerAfterNow */
THUMB_FN(0xE0618B38,  SetHPTimerAfterNow)                   /* [M50II 1.0.1 OK] from error message; HPTimer cluster */
THUMB_FN(0xE05C05B4,  SetTimerAfter)                        /* [M50II 1.0.1 OK] from error message */
THUMB_FN(0xE05C066A,  CancelTimer)                          /* [M50II 1.0.1 OK] adjacent to SetTimerAfter (+0xB6) */

/** Interrupts **/
/* [M50II 1.0.1 FIXED - Phase A] The inherited M50.110 address 0xE0121128 is MISATTRIBUTED
 * on M50 II 1.0.1: that function is a UHS2/SD card-init (writes slot-state array @0x140AC,
 * per-slot callbacks), NOT an SGI sender. Verified real GIC SGI primitive:
 *   0xE00D4034: r0 = sgi_id | (r1 << 16); str r0,[0xC1001F00 = GICD_SGIR]; dsb; bx lr  (leaf).
 * Args (r0=sgi_id, r1=cpu_target). SGI 10 = GIC_scheduling = CPU1 wake (docs/09, docs/10).
 * CAVEAT: confirm on HW whether ML's cpu_id (index 0/1) needs conversion to a GIC CPU
 * target *bitmask* by a wrapper before this primitive. */
THUMB_FN(0xE00D4034,  send_software_interrupt) // int 10 is used by DryOS to sleep/wake cpu1

/* [M50II 1.0.1 LIKELY, offline F7] was 0xDF002C32 = ROM 0xE14999D6 = second half of the 4-byte
 * `dmb sy` at 0xE14999D4. Blob copy: src 0xE14995A4 -> 0xDF002800..0xDF0033A0 ([0xE00400EC..F4]).
 * ROM 0xE14999EE: push {r4,r5,r6,lr}; irq-save; bl 0xE1499876(r0,r1); irq-restore. No string anchor. */
THUMB_FN(0xDF002C4A, _request_RPC) // allows CPU0 or CPU1 to pass a func pointer and param
                                   // to the other for RPC.
                                   // Passing a NULL func pointer, will suspend the other
                                   // CPU until it becomes non-NULL.
                                   // DryOS seems to use this to deliberately sleep a CPU
                                   // in some exceptional cases (e.g. asserts).
                                   //
                                   // Do not call this directly, because if you do,
                                   // and you forget to call clear_RPC_request(),
                                   // the target CPU will repeatedly call the func pointer
                                   // as fast as possible, forever.
                                   // Instead use request_RPC().

THUMB_FN(0xDF002C6A,  clear_RPC_request) /* was 0xDF002C52 (mid-body); ROM 0xE1499A0E: push {r4,lr}; irq-save; bl 0xE149999A; irq-restore; return 0 */ // reset the request to prevent calling
                                         // the function forever

// MMU related, required by CONFIG_MMU features
/* [M50II 1.0.1 Phase B - re-anchored, verified by romdis this session] inherited M50.110
 * addr 0xE05772D6 is WRONG (a higher-level fn, not the TTBR writer). Real leaf @0xE007D61A:
 * mcr p15 c2c0#0/#1 (TTBR0/TTBR1), TLBIALL (c8c7), BPIALL (c7c5#6), dsb/isb, bx lr. */
THUMB_FN(0xE007D61A, change_mmu_tables) // Updates TTBRs and does TLB maintenance
//THUMB_FN(0xe001eca4, copy_mmu_tables) // copies from one location to another,
                                      // fixing up addresses held in table start,
                                      // and doing cache maintenance

// slightly MMU related, used by CONFIG_MMU_REMAP
// [M50II 2026-07-11] FIXED mis-port: 0xe065e998 was blindly copied from M50 mk1 (M50.110), but on
// M50 II 1.0.1 that address is the MIDDLE of EfmErscUnLockResources (0xe065e98c), NOT memcpy. Calling
// it from copy_mmu_tables_ex()/apply_data_patch() jumped into EFM code -> NULL deref -> data abort at
// init_mmu_globals (captured on QEMU via our CPU0 abort handler: pc=0xE05BE752, DFAR=0, DFSR=7). Real
// memcpy found via DryOS name/ptr export table @0xe004eca0 ([ptr][name] pairs): memcpy=0xE0329D11
// (verified: ldrb[r1]->strb[r3] byte head + 16B ldr/str word blocks + byte tail; leaf; ROM; >0xE0040000).
THUMB_FN(0xe0329d10, memcpy_dryos) // real M50II memcpy (was 0xe065e998 = mk1 leftover, = EFM code here)


/** Memory allocation **/
THUMB_FN(0xE05BEE20, _AllocateMemory)                       /* [M50II 1.0.1 OK] name-table thunk [0xE004EF48]->0xE004E9C7 tail-calls here; '[MEM] NG AllocateMemory' */
THUMB_FN(0xE05BEFD8, _FreeMemory)                           /* [M50II 1.0.1 OK] name-table thunk [0xE004EF5C]->0xE004E9C1 tail-calls here; +0x1B8 from AllocateMemory */
THUMB_FN(0xE05BECAA,  GetMemoryInformation)                 /* [M50II 1.0.1 OK] bl target in AllocateMemory; '[MEM] ERROR GetMemoryInformation' */
THUMB_FN(0xE05BEC7E,  GetSizeOfMaxRegion)                   /* [M50II 1.0.1 OK] bl target in AllocateMemory; '[MEM] ERROR GetSizeOfMaxRegion' */
/* [M50II 2026-07-11 ROOT FIX] These two addresses are WRONG: 0xE055AAF4/0xE055AB28 land INSIDE a
 * Canon GRAPHICS draw function (FUN_e055aa98 "GUI_DrawHdrImage/GRAPHICS_DrawOn"), so every
 * fio_malloc()/DMA allocation jumped into graphics code -> GraphicsConvertPosType(garbage pos_type)
 * -> pc=0x2 prefetch abort. That was the big.5 _load_fonts crash (font_read's first fio_malloc).
 * Real DMA alloc = AllocateMemory + uncacheable-mirror bit, now provided as C wrappers in
 * function_overrides.c over the verified _AllocateMemory/_FreeMemory. */
// THUMB_FN(0xE055AAF4, _alloc_dma_memory)   // WRONG (graphics func) -> function_overrides.c
// THUMB_FN(0xE055AB28, _free_dma_memory)    // WRONG (graphics func) -> function_overrides.c
THUMB_FN(0xE03C5270, _malloc)                               /* [M50II 1.0.1 OK] push{r4,r5,r6,lr}; pool slot 0xE03C52D0=0x2d160; returns block+8 */
THUMB_FN(0xE03C52A8, _free)                                 /* [M50II 1.0.1 OK] push{r4,r5,r6,lr}; shares pool 0x2d160; reads [ptr-8] header; +0x38 from malloc */

/** Memory info **/
THUMB_FN(0xE039A3D8,  memmap_info)                          /* Exception vector, DRYOS system memory etc */
THUMB_FN(0xE013D480,  smemShowFix)                          /* Common Lower, Common Upper etc */
THUMB_FN(0xE03CAE30,  malloc_info)                          /* Malloc Information */
THUMB_FN(0xE03CAEC8,  sysmem_info)                          /* System Memory Information */

/** Misc **/
/* [M50II 1.0.1 Phase B - re-anchored, verified by romdis this session] inherited 0xE055A8B4
 * is WRONG. Real varargs UART printer @0xE01C7280 (push{r0-r3} arg-spill, irqsave 0xE021B8F0,
 * ldr pc,[sp],#0x14 tail); matches docs/06 candidate + used by boot SGI-dump 0xE00D407C.
 * CAVEAT: could be the panic-serialized printer vs generic uart_printf (ledger nuance). */
THUMB_FN(0xE01C7280,  uart_printf)
/* [M50II 1.0.1 Phase B - re-anchored, verified] inherited 0xE0096060 WRONG. Real @0xE005DB9C:
 * bounded format-copy loop (walks fmt r2, copies to buf r0..r0+r1, handles '%'). */
THUMB_FN(0xE005DB9C,  vsnprintf)                            /* called by LogFile_printf(..., "*Data Lost*\n", ...); the one called by DebugMsg only knows %s */

/** Startup **/
DATA_PTR(    0xE484,  additional_version)
/* kitor: Please note that this points at '\n' char in char* with FW version */

/** Tasks **/
DATA_PTR(    0x4144,  task_dispatch_hook)                   /* [M50II 1.0.1 OK] Scheduler FUN_e01b1d82 calls *(0x4144)(1,old,next) each context switch (literal DAT_e01b2128=0x4144). 0x4144 is Canon's OWN task-switch list-walker slot -- ML does NOT squat it (squatting panicked iters 6/7/8). ML registers a node into Canon's list instead (see register_task_switch_callback below). Symbol kept only for the boot diagnostic + tasks.h extern; never written by ML now. */
THUMB_FN(0xE0164044,  register_task_switch_callback)        /* [M50II 1.0.1 OK] Canon's ctx-switch list-insert: r0 = ML node {next@0,prev@4,fn@8}. Sets node->next/prev=0, locked head-insert into the list at *(DAT_e01642b0)=RAM 0x23e3c, (re)installs the walker 0xE0163FE4 at *0x4144, restores IRQ (msr cpsr_c). Walker then calls node->fn(1,old,next) each switch. Tail of FUN_e012c6f4; verified docs/27. */
THUMB_FN(0xE00A6C74,  task_trampoline)                      /* [M50II 1.0.1 OK] saved initial-context PC=0xE00A6C75; unlocks scheduler then BLX task->entry */
/*
 * kitor: task_create calls internal function (e05f5fb4) which set up basic
 * task structure, and part of that is call to task_trampoline_caller (e00e96ac)
 * (it is followed by check of caller return value).
 * task_trampoline_caller will pass task_trampoline+1 as parameter to another
 * function.
 */
// [M50II 1.0.1 OK] _get_task_info_by_id RE-corrected 2026-07-11 (was WRONG 0xe01c8378 = a string
// literal "...CrawImageSize(%#X)"). Real fn = FUN_e01d77c4: calls get_task_by_id(id) (0xE00A6BBA,
// itself verified) then fills the caller's task-attr struct from task-struct fields (+0x49 status,
// +0x08 name ptr, +0x0c/+0x10 stack, +0x1c/+0x20 ...); returns 0 ok / -1 fail; uses critsec. Verified
// via exception dumper FUN_e00d41ee which calls it right before printing "CORE"/"TASK Name" (RP.160 anchor).
THUMB_FN(0xe01d77c4, _get_task_info_by_id)
/*
 * kitor: via extask function. While on 5D3 it was called directly, now helper
 * is used. Find extask via string formats. Trace variables holding task id,
 * you will quickly find where it is verified (this is the mentioned helper).
 * In the helper code you will find call to our stub.
 */

/** Recursive locks **/
/* [M50II 1.0.1 Phase B - re-anchored] inherited 0xE0577598 WRONG. Real KerMisc.c wrapper
 * @0xE05CC64C (calls KerRLock primitive 0xE0587318, asserts-on-err via debug_assert). */
THUMB_FN(0xE05CC64C,  CreateRecursiveLock)                  /* SystemIF::KerMisc.c  */
/* [M50II 1.0.1 Phase B - re-anchored, verified] inherited 0xE06D77DC WRONG. Real @0xE058738C:
 * interrupt-context asserts [0x100C]/[0x102C], handle-bit0 validation (KerRLock.c). */
THUMB_FN(0xE058738C,  AcquireRecursiveLock)                 /* SystemIF::KerRLock.c */
/* [M50II 1.0.1 Phase B - re-anchored, verified] inherited 0xE06D78A6 WRONG. Real @0xE0587456:
 * paired with Acquire (+0xCA), same context-assert/handle shape (KerRLock.c). */
THUMB_FN(0xE0587456,  ReleaseRecursiveLock)                 /* SystemIF::KerRLock.c */

/** Semaphores **/
THUMB_FN(0xE05D0CAC,  create_named_semaphore)               /* [M50II 1.0.1 OK] arg2 in {0,1} (=CreateBinarySemaphore); counting variant 0xE05D0CFA; confirmed via CSMGR_Initialize */
THUMB_FN(0xE05D0E22,  give_semaphore)                       /* [M50II 1.0.1 OK] GiveSemaphore; push{r4,lr}, 1 arg (handle) */
THUMB_FN(0xE05D0DA8,  take_semaphore)                       /* [M50II 1.0.1 OK] TakeSemaphore; push{r2-r6,lr}, 2 args (handle,timeout) */
/* [M50II 1.0.1 Phase B - re-anchored, verified by romdis this session] inherited 0xE0577C2C
 * is WRONG. Real @0xE05D0D78: immediately precedes verified take_semaphore 0xE05D0DA8,
 * calls take-sem wait worker 0xE05BB4DA + debug_assert 0xE0674574, non-blocking (checks -2). */
THUMB_FN(0xE05D0D78,  take_semaphore_now)

/** GUI **/
DATA_PTR(   0x11144,  gui_task_list)                        /* [M50II 1.0.1 base OK, offline F7] was 0xFB38 (mk1 copy). CtrlMan.c 0xE07BCF68: [0x11144] = CreateRecursiveLock([0x11144+0x1C]); 0xE07BD3DC: AcquireRecursiveLock([0x11144]) then reads [+0xC]. NOTE: `current` is probably at +0xC here, gui-common.h:116 has it at +8 - layout NOT confirmed. */
THUMB_FN(0xE0599FBA,  SetGUIRequestMode)                    /* [M50II 1.0.1 OK] by debug message; writes gui-state field */
THUMB_FN(0xE021DB7C,  gui_main_task)                        /* [M50II 1.0.1 OK] entry ptr 0xE021DB7D in literal@0xE021DFD4 loaded by create_gui_main_task(0xE021DBC6) before task_create */
THUMB_FN(0xE065A4FC,  dialog_redraw)                        /* [M50II 1.0.1 STUBBED OUT, offline F7] was 0xE054F3CA = literal pool (not code). Real function not found; 0xE065A4FC is a bare `bx lr`, so the call is a no-op instead of a crash. Candidate for the real one: 0xE05A094C (unverified). */
THUMB_FN(0xE03FD182,  gui_massive_event_loop)               // various "GUICMD_"* strings
THUMB_FN(0xE021DCB2,  gui_enqueue_message)                  // via string "warning! QueueLength=" and "GUI::GUI.c" in one function
THUMB_FN(0xE065A4F2,  gui_init_end)                         /* [M50II 1.0.1 F7-fix] was 0xE065A4F4 (2 bytes into fn, at the `bne`); real entry 0xE065A4F2 = `cmp r0,#0`; verify_stubs.py flagged cond-branch-at-entry, via "EndGuiInit" string */
DATA_PTR(    0x5428,  gui_main_struct)

/*
 * kitor Similar to R180 I was unable to find GUI_Control either. If you compare
 * 200D.101 surrounding functions, it just seems to DNE on R and M50.
 */

/** GUI timers **/
THUMB_FN(0xE0329DB2,  StartDateTimer)                       // by debug message
THUMB_FN(0xE0329E64,  CancelDateTimer)                      // by debug message

/** MPU communication **/
/*
 * kitor: skipped for now. InitializeIntercom looks identical to R180?!
 * I thought M50 has no MPU, yet at least buffers are initialized.
 */

/** Queue **/
THUMB_FN(0xE05B53CC,  msg_queue_create)                     /* [M50II 1.0.1 OK] CreateMessageQueue; push{r4,r5,r6,lr} */
THUMB_FN(0xE05B5490,  msg_queue_receive)                    /* [M50II 1.0.1 OK] ReceiveMessageQueue; push.w{r2-r8,lr}; dequeue worker 0xE05BB6D0 */
THUMB_FN(0xE05B55EE,  msg_queue_post)                       /* [M50II 1.0.1 OK] non-blocking post; push{r4,lr}; enqueue worker 0xE05BB71C (blocking variant 0xE05B556C) */
/* [M50II 1.0.1 Phase B - re-anchored] inherited 0xE054437C WRONG. Real @0xE05B5624:
 * KerQueue.c, validates queue handle + debug_assert; role (count) likely, verify on HW. */
THUMB_FN(0xE05B5624,  msg_queue_count)                      // called before string "warning! QueueLength"

/** Memory **/
/* [M50II 1.0.1 OK] *MemoryResource cluster RE-corrected 2026-07-11 (were WRONG 0xe05c7f0e/f3c/fe2,
 * all inside FUN_e05c7edc, a "SetMem1Component" assert helper). Real cluster = three adjacent
 * System/RscWrap.c dispatch thunks over the resource-manager vtable (manager global @0xe0608ac4,
 * assert string "System/RscWrap.c" @0xe0608ac8): each caches its args, asserts its vtable slot is
 * non-null, then tail-dispatches (bx) to *(mgr+slot). Verified by disassembly + arg-count + reference
 * deltas (Cont=Alloc+0x2e, Free=Alloc+0x5c, identical to RP.160 0xe061d398/3c6/3f4, 200D, 850D):
 *   0xe0608904 slot 0, 4 args (size,cbr,ctx,type)  -> AllocateMemoryResource
 *   0xe0608932 slot 4, 4 args (size,cbr,ctx,type)  -> AllocateContinuousMemoryResource
 *              (confirmed: caller FUN_e02058aa logs "AllocateContinuousMemoryResource failed")
 *   0xe0608960 slot 8, 3 args (hSuite,cbr,ctx)     -> FreeMemoryResource */
THUMB_FN(0xe0608904,  AllocateMemoryResource)               // [M50II 1.0.1 OK] RscWrap slot0, 4-arg
THUMB_FN(0xe0608932,  AllocateContinuousMemoryResource)     // [M50II 1.0.1 OK] RscWrap slot4, 4-arg
THUMB_FN(0xe0608960,  FreeMemoryResource)                   // [M50II 1.0.1 OK] RscWrap slot8, 3-arg
THUMB_FN(0xE05BD70C,  AddMemoryChunk)                       // Same as in 200D (matched by offsets), may be wrong as multiple variants exists.
                                                            // Other options: e054c3da e054cf5a
THUMB_FN(0xE05BD790,  GetFirstChunkFromSuite)               // "IsSuiteSignature( hSuite )" + PackMem
THUMB_FN(0xE05BDFB0,  GetNextMemoryChunk)                   // "IsSuiteSignature( hSuite )" + PackMem
                                                            // Similar function is at e05547a0, but with just a few xrefs and different offset used
THUMB_FN(0xE05BD552,  GetMemoryAddressOfMemoryChunk)        // "IsChunkSignature( hChunk )" + PackMem - 200D uses "MemChunk" directly
THUMB_FN(0xE05BD95C,  DeleteMemorySuite)                    // String "DeleteMemorySuite 0x%x"
THUMB_FN(0xE05BDBE8,  CreateMemorySuite)                    // String "PackMem.c", calls malloc(0x18)
THUMB_FN(0xE05BD22E,  CreateMemoryChunk)                    // called from CreateMemorySuite
//kitor: disabled using CONFIG_MEMORY_SRM_NOT_WORKING
//THUMB_FN(0xe056a5c6,  SRM_AllocateMemoryResourceFor1stJob)  // "pMessage" and "pPriorMessage" with "RscMgr.c". Unsure - there seems to be multiple identical functions
                                                            // Matched like in R by structure params to 200D stub 
//THUMB_FN(0xe056da22,  SRM_FreeMemoryResourceFor1stJob)      // find refs to "pMessage", there are several similar looking functions using it,
                                                            // Matched to 200D stub, just buffer size differs (0x34 vs 0x30)

/** Uncategorized **/
DATA_PTR(   0x1405C,  sd_device)                            /* [M50II 1.0.1 OK, offline F7] was 0x6450 (other SD module, 0xE04E923C). Real pStgDev slot is 0x14060: SD_DeviceCreate 0xE00E5E14 does `ldr r6,=0x14060; str r4,[r6]` and fills pStgDev+0/4/8/0xC. bootflags.c:144/215 read sd_device[1], so the symbol is slot-4. */
// [M50II 2026-07-11 REMOVED] _LoadCalendarFromRTC (was 0xe0637210): WRONG address -- landed
// mid-instruction inside FUN_e0637136 (Canon "Arima" struct-copy), causing a NULL-store data
// abort whenever ML ran config_save after big.8 (HW-proven, see function_overrides.c). The
// caller LoadCalendarFromRTC() is now a self-contained sentinel-date fill (no stub call), so this
// line is dead. verify_stubs.py flagged it REVIEW (STRADDLES/MID-FUNC). TODO: RE the real RTC read.
THUMB_FN(0xE01DD8FC,  ErrForCamera_handler)                 // ERR70, ERR80 etc (DlgErrForCamera.c)

/** Properties **/
THUMB_FN(0xE05BFD48, _prop_cleanup)                         /* [M50II 1.0.1 matched] PropertyMgr cluster; verify */
THUMB_FN(0xE05BF79E,  prop_register_slave)                  /* [M50II 1.0.1 OK] PropertyMgr pRegisterSlave; push.w{r3-fp,lr} */
THUMB_FN(0xE05BF86C, _prop_request_change)                  /* [M50II 1.0.1 OK] PropertyMgr pRequestChange; push.w{r3-r8,sb,lr} */

/**
 * RGBA / Compositor functions and structures
 */

/** General FEATURE_VRAM_RGBA stubs **/
THUMB_FN(0xE018B9F4, XimrExe);                              /* [M50II 1.0.1 OK] RGBA compositor submit (mzrm->Zico); confirmed FUN_e018b9f4 (allocs msg, copies 0x31c-byte Ximr blob, sends+waits) */
DATA_PTR(  0x11298, winsys_sem);                            /* [M50II 1.0.1 OK] was 0xFC10 (blind mk1 copy). RE'd: refreshVrmsSurface (FUN_e035d134) takes the sem at *(VMIX_ctx+4), VMIX_ctx=DAT_e035d204=0x11294 -> winsys_sem @ 0x11298 */

/*
 * Pointer to first Canon RGBA buffer (GUI). On M50 mk1 this is @0xFC1C, set up by
 * VDrv_InitializeScreen. [M50II 2026-07-11] 0xFC1C was a blind mk1 copy — on M50 II 1.0.1 it holds
 * an MMIO value (0xD0483040, in the 0xD0xxxxxx device region, HW-confirmed), so rgb_vram_preinit()'s
 * deref of ->bitmap_data bus-HANGS the boot at post.0c (no fault; the looping abort handler stayed
 * silent). Real M50 II GUI surface: RENDERER_InitializeScreen (FUN_e02c9112) does CreateMARV
 * (FUN_e0186278, flags 0x05040100=RGBA) and stores the layer-0 MARV ptr into the surface array base
 * DAT_e02c9518 = 0x7F1B4 ([0]) (and caches it in RENDERER state @0x11254+0x10 = 0x11264). 0x7F1B4 is
 * RAM and NULL until that init runs -> rgb_vram_preinit safely returns NULL (waits) instead of
 * bus-hanging. Alt candidate: 0x11264. TODO-HW: confirm the surface populates (bitmap_data non-NULL).
 */
DATA_PTR(  0x7F1B4, _rgb_vram_info);   /* was 0xFC1C (mk1 copy = MMIO on M50 II -> bus-hang) */

/** Wrong on purpose **/
DATA_PTR(       0x0,  LCD_Palette)                          // D8 has no indexed RGB buffers.


/** App handlers **/
/**
 * Those can usually be found by looking at function address passed to
 * WINSYS_CreateDialogBox_wrapper / WINSYS_CreateDialogBox, just after some
 * debug message */
THUMB_FN(0xE0567A34,  PlayMain_handler)                     // msg: "StartPlayMain"
THUMB_FN(0xE06BCD84,  ShootOlcApp_handler)                  // msg: "StopShootOlcApp PopPalette(%d)"
THUMB_FN(0xE0444058,  LiveViewApp_handler)                  /* [M50II 1.0.1 OK, offline F7] was 0xE07A8C30 = StarShootingTime dialog handler (created by 0xE07A8D10, slot 0x14EB0). StartLiveViewApp 0xE0445864 passes literal [0xE0445C1C]=0xE0444059 to WINSYS_CreateDialogBox 0xE059F43C at 0xE044591C */
//THUMB_FN(0xe0338a10,  LiveViewShutterApp_handler)         // msg: "StartLiveViewShutterApp"
THUMB_FN(0xE08F993C,  PlayMovieGuideApp_handler)            // msg: "StartPlayMovieGuideApp"

// See R180 stubs.s for details.
//DATA_PTR(  0xe708,  PlayMain_dialog)                      // in StartPlayMain()
//DATA_PTR( 0x13fa4,  ShootOlcApp_dialog)                   // in StartShootOlcApp()
DATA_PTR(   0x139b8,  LiveViewApp_dialog)                   // in StartLiveViewApp()
//DATA_PTR( 0x1c434,  LiveViewShutterApp_dialog)            // in StartLiveViewShutterApp(), not sure if needed
//DATA_PTR( 0x14fa8,  PlayMovieGuideApp_dialog)             // in StartPlayMovieGuideApp()



/**
/** Campaign additions (M50 II 1.0.1 doc campaign; anchor+prologue verified) **/
THUMB_FN(0xE00A6BF4,  get_current_task)                     /* [M50II 1.0.1 OK] leaf: mov.w r0,#0x1000 -> reads current_task global */
THUMB_FN(0xE05BFC94,  prop_deliver)                         /* [M50II 1.0.1 OK] PropertyMgr deliver; sibling of prop_request_change cluster */
THUMB_FN(0xE004EA38,  WriteFileString)                      /* [M50II 1.0.1 OK] event proc (thumb ptr 0xE004EA39); registered in builder 0xE004EAC8 */
THUMB_FN(0xE05A1B70,  DISP_GetImageAddress)                 /* [M50II 1.0.1 OK] DISP module; DryosDebugMsg-logs its own name */
THUMB_FN(0xE05A1794,  DISP_SetUpdateOSDVram)                /* [M50II 1.0.1 OK] DISP module; precedes bmp_vram_info use */
//THUMB_FN(0xE03FF1D8, GUI_ChangeMode)                      /* [M50II 1.0.1 OK] ctrlman GUI_ChangeMode; enable if needed */

/**
 * Foundations RE sweep (F1-F6, 2026-07-07) — all entries independently romdis-verified
 * (valid prologue) AND adversarially verified; see docs/13/14/15. New/additive only.
 */

/** Interrupts / GIC (F4) — ARM GICv2 fabric **/
THUMB_FN(0xE018E17C,  isr_dispatch)                         /* IRQ dispatcher: r7=[0xC100010C]=GICC_IAR; id=r7&0x3FF; handler=[0x2AD1C+id*8]; EOI str [0xC1000110] */
THUMB_FN(0xE018E384,  register_interrupt)                   /* push{r4,r5,r6,lr}; installs handler into isr_table @0x2AD1C; refs boot_state_word 0x4004 */
THUMB_FN(0xE018E2A8,  enable_interrupt)                     /* push{r4,r5,r6,lr}; irqsave 0xE021B8F0; GICD_ISENABLER 0xC1001100 */
THUMB_FN(0xE018E2E4,  disable_interrupt)                    /* sibling of enable_interrupt (+0x3C); GICD_ICENABLER 0xC1001180 */
THUMB_FN(0xE00A69EE,  arm_gic_init)                         /* push{r3-r7,lr}; r5=0xC1001000=GICD_BASE; str 1,[GICD_CTLR]; masks ints 0-63 */
DATA_PTR(   0x400C,  pre_isr_hook)                          /* isr_dispatch: ldrd fp,[0x4004,#8]=[0x400C]; called before ISR if non-null */
DATA_PTR(   0x4010,  post_isr_hook)                         /* isr_dispatch: sl=[0x4004,#0xC]=[0x4010]; called after ISR */
DATA_PTR(  0x2AD1C,  isr_table_handler)                     /* isr_dispatch: handler = [0x2AD1C + id*8] */
DATA_PTR(  0x2AD20,  isr_table_param)                       /* isr_dispatch: param   = [0x2AD20 + id*8] */

/** Event flags + sync extras (F3) **/
THUMB_FN(0xE05D0CFA,  create_counting_semaphore)            /* counting variant adjacent to create_named_semaphore (0xE05D0CAC) */
THUMB_FN(0xE05B556C,  msg_queue_post_blocking)              /* blocking post; noted in msg_queue_post comment; push.w{r2-r8,lr} */
THUMB_FN(0xE05B0B68,  CreateEventFlag)                      /* push{r4,r5,r6,lr}; interrupt-ctx check mov.w r0,#0x1000; EVF class table 0x41C8 */
THUMB_FN(0xE05B0BA2,  DeleteEventFlag)                      /* +0x3A from CreateEventFlag; push{r4,lr} */
THUMB_FN(0xE05B0EBC,  SetEventFlag)                         /* push{r4,lr}; handle validate cbz/lsls #0x1f */
THUMB_FN(0xE05B0EEA,  ClearEventFlag)                       /* +0x2E from SetEventFlag; same handle-validate shape */
THUMB_FN(0xE05B0CC0,  WaitForAnyEventFlag)                  /* push{r1-r7,lr}; interrupt-ctx check; OR-wait */
THUMB_FN(0xE05B0D40,  WaitForAllEventFlag)                  /* +0x80 from WaitForAny; AND-wait */

/** DIGIC 8 EDMAC mem2mem engine (RAW-VIDEO R1, docs/30) — resolved+verified on gang100 1.0.1.
 *  Called by platform/edmac-d8.c. All Thumb. Addresses from the 61-agent RE workflow + closer. **/
THUMB_FN(0xe05b848e,  edmac_set_address)                    /* leaf: writes addr into chan desc +0xa0 (twin of set_size) */
/* [M50mk2 RAW-VIDEO set_size fix, docs/32] was 0xe05b8862 = the SCALAR byte-count SetEDmac wrapper
 * (2nd arg = byte count; align-check assert 0x45e, block-count assert 0x463). edmac-d8.c passes a
 * struct edmac_info* which that wrapper treated as a byte count -> fabricated nonzero geometry ->
 * inner writer FUN_e05b849a asserts Engine::Edmac.c:0x40B (x2). 0xe05b849a IS the real (channel,
 * struct edmac_info*) geometry writer = set_address(0xe05b848e) + 0xc; xn=yn=0 in our info makes the
 * capability gate false -> no assert. +0xc twin matches SX740 (f192->f19e) / M50mk1 (a9ae->a9ba). */
THUMB_FN(0xe05b849a,  edmac_set_size)                       /* leaf: 2-D geometry writer (channel, struct edmac_info*) */
THUMB_FN(0xe05b88fc,  edmac_set_transfer_mode)              /* set chan transfer mode */
THUMB_FN(0xe05b9690,  StartEDmac_maybe)                     /* start an EDMAC channel */
THUMB_FN(0xe05b95e4,  ConnectReadEDmac_maybe)               /* connect a read chan (single arg on D8) */
/* [M50mk2 RAW-VIDEO channel-prepare fix, docs/32] edmac-d8.c's mem2mem calls edmac_reset_channel(RD)+(WR)
 * as the first two calls -- in Canon's real mem2mem this slot is the per-channel PREPARE (kitor's EOS R
 * FUN_e0535998), which clears the per-channel status array and pokes the channel MMIO into its ready state
 * (field_0x1c 0x80000000->0, field_0x18=0, field_0xb4=2). Our old stub 0xe05b96b8 was Canon's "reset an
 * ALREADY-prepared channel" -> it asserts Engine::Edmac.c:0x77C on the un-prepared mem2mem channels, leaving
 * them un-armed so StartEDmac no-ops -> no completion IRQ -> WaitForAnyEventFlag 50ms timeout -> "Frame
 * incomplete" every frame. FUN_e05b7b42 IS the prepare (verified: clears status arr, field_0xb4=2, Port<0x4c
 * bounds check @Engine::Edmac.c:0x288; called by Canon Mem1Path/OrbbenPath/WbDetectionPath SetPath). */
THUMB_FN(0xe05b7b42,  edmac_reset_channel)                  /* Canon per-channel PREPARE (FUN_e0535998 analog; was 0xe05b96b8 = reset-prepared-channel, wrong) */
/* [M50mk2 RAW-VIDEO boomer fix, docs/32] These two were the WRONG boomer layer -- they pointed at the
 * low-level per-InputPort setters (Engine::BoomerVdKick.c / Engine::BoomerSelector.c, arg = packed
 * BoomerID>>16) instead of the Engine::Outer.c WRAPPERS that mem2mem (edmac-d8.c) calls with a RAW
 * channel. Result: boomer mis-configured for the mem2mem WR/RD channels -> StartEDmac assert
 * Engine::Edmac.c:0x770 -> crash on record. Correct wrappers take a raw channel, look up the per-channel
 * boomer config table (DAT_e05b767c, stride 0x14), and are adjacent (kitor's EOS R VdKick e0535e64 ->
 * Selector e0535ee2 pattern). Selector verified: bilal's 0x373f0126 passes its InSelType(0x100)/
 * InSelEdmacType(0x20000000) checks for ch13/46. edmac_stop_boomer_maybe(0xe05b80c2) was already right. */
THUMB_FN(0xe05b7520,  edmac_reset_boomer_vdkick)            /* Outer.c wrapper: raw chan -> low-level VdKick (was 0xe05b5720, wrong layer) */
THUMB_FN(0xe05b752e,  edmac_select_boomer)                  /* Outer.c wrapper: (raw chan, bsel=0x373f0126) (was 0xe0683ca0, wrong layer) */
THUMB_FN(0xe05b7c48,  RegisterEDmacCompleteCBR)             /* register EDMAC completion CBR (was NOP in fn_overrides) */
THUMB_FN(0xe05b7f68,  UnregisterEDmacCompleteCBR)           /* unregister EDMAC completion CBR */
/* [M50mk2 RAW-VIDEO packunpack fix, docs/32] was 0xe05b8f38 = a 3-arg pack/unpack MODE SETTER
 * f(channel, mode{1,2,3}, order{0,1}) writing desc+0xd8; edmac-d8.c calls it with ONE arg -> r1/r2
 * garbage -> asserts Engine::Edmac.c:0x5EF (mode) x2 and 0x5DE (order) x1. 0xe05b8106 is the true
 * 1-arg RESET leaf (zeroes desc+0x18/+0x1c/+0x20/+0x24 enable flags) = edmac_stop_boomer_maybe
 * (0xe05b80c2) + 0x44 -- the exact reference delta on SX740 (ec52+0x44) and M50mk1 (a46e+0x44).
 * (Its inverse sibling 0xe05b8146 sets those flags = the ARM step; NOT used, per the working ports.) */
THUMB_FN(0xe05b8106,  edmac_reset_packunpack_mode)          /* leaf: 1-arg reset of pack/unpack + enable flags */
THUMB_FN(0xe05b80c2,  edmac_stop_boomer_maybe)              /* stop Boomer for a write channel */
THUMB_FN(0xe06b934e,  PwrMng_WakeSubChips)                  /* power on EDMAC sub-chips {0,7}; MUST precede EDMAC MMIO */
THUMB_FN(0xe06b937c,  PwrMng_SuspendSubChips)               /* power off EDMAC sub-chips */
THUMB_FN(0xE05BE4BE,  CreateResLockEntry)                   /* create engine resource lock entry */
THUMB_FN(0xe05be8a6,  LockEngineResources)                  /* lock engine resources */
THUMB_FN(0xe05be740,  UnLockEngineResources)                /* unlock engine resources (was NOP in fn_overrides) */
THUMB_FN(0xe05cc5fe,  CreateEventFlag_strictly)             /* create DryOS event flag (mem2mem completion sync) */

/** UART sink (F6) — char -> FIFO (see docs/15 UART section) **/
THUMB_FN(0xE007D814,  uart_write_bytes)                     /* physical UART writer: UART_TX 0xC0800000, poll status 0xC0800014 bit1 (TX ready) */
THUMB_FN(0xE027A5A2,  uart_console_sink)                    /* console-level sink; push{r3,lr}; forwards to writer */
THUMB_FN(0xE00A678A,  uart_emit_callback)                  /* emit dispatcher: loads fn-ptr @[0x405C+4], calls if set */

/** MMU support (F5) — for CONFIG_MMU / CONFIG_MMU_REMAP; names descriptive, wire-up TBD **/
THUMB_FN(0xE004F15C,  mmu_setup)                            /* push.w{r4-r8,lr}; MMU table RAM base 0xBFF20000 (literal @E004F368) */
THUMB_FN(0xE00722BC,  mmu_table_init_copy)                  /* copies 0x4900-byte MMU table from 0xE0000000 source */
THUMB_FN(0xE0072310,  set_ttbr_checked)                     /* push{r4,lr}; validates then writes TTBR */
THUMB_FN(0xE007233C,  set_ttbr_per_core)                    /* push{r4,r5,r6,lr}; reads MPIDR, per-core TTBR write */
THUMB_FN(0xE007232E,  restore_orig_mmu)                     /* push{r4,lr}; r0=0xE0000000; tail-calls set_ttbr_per_core */

/** Tasks (F2) **/
THUMB_FN(0xE00A6BBA,  get_task_by_id)                       /* id==-1 -> get_current_task (0xE00A6BF4); else validate uxth id; task_table @0x40E8 */
DATA_PTR(   0x40E8,  task_table)                            /* task-array base ptr; loaded by get_task_by_id / build_task (literal @0xE00A6F68) */

// STUFF BELOW IS WRONG!

THUMB_FN(0xdf00d365,  fsuDecodePartitionTable)             // inlined like in EOS R. See R180 function_overrides.c for WIP alternative
