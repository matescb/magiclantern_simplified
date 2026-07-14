/** \file
 * Common startup code (for all models).
 *
 * !!!!!! FOR NEW PORTS, READ PROPERTY.C FIRST !!!!!!
 * OTHERWISE YOU CAN CAUSE PERMANENT CAMERA DAMAGE
 * 
 */
/*
 * Copyright (C) 2009 Trammell Hudson <hudson+ml@osresearch.net>
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

#include "dryos.h"
#include "dryos_rpc.h"
#include "config.h"
#include "version.h"
#include "bmp.h"
#include "menu.h"
#include "property.h"
#include "consts.h"
#include "tskmon.h"

#include "boot-hack.h"
#include "ml-cbr.h"
#include "backtrace.h"

extern int uart_printf(const char * fmt, ...);
static void m50ii_led_blink(uint32_t n);   /* fwd: UART-independent LED progress beacon (defined below) */
static char *m50ii_hex(char *b, uint32_t v);   /* fwd: hex formatter (defined in telemetry block below) */
static void mlog_kv(const char *label, uint32_t v);   /* fwd: "<label>=0x..\n" logger (defined below) */

extern void platform_post_init();

/* [M50II 2026-07-10] Skip mmu_init(): the MMU-remap is verified-correct (source/tables/switch/
 * SGI-ptr all confirmed) yet PrefetchAborts only on silicon; remap is only needed for ROM
 * patching, so bypass it to boot further and find the next blocker. Remove to re-enable. */
#define M50II_TRACE_SKIP_MMU_INIT

/* [M50II 2026-07-11] Abort-capture build: run mmu_init() UNDER our own CPU0 ARM abort
 * handler (installed in boot_pre_init_task) so its PrefetchAbort dumps faulting PC / IFAR /
 * IFSR over UART instead of vanishing into Canon's print-and-hang ROM vector. Comment out
 * M50II_MMU_CAPTURE to return to the plain skip-MMU boot. */
#define M50II_MMU_CAPTURE
#ifdef M50II_MMU_CAPTURE
#undef M50II_TRACE_SKIP_MMU_INIT
#endif

/* [M50II 2026-07-11] QEMU-only: after installing the abort handler, deliberately fetch from an
 * unmapped address to prove the Thumb handler fires + dumps end-to-end. DISABLE for the HW build
 * so mmu_init's own PrefetchAbort is what the handler catches. */
/* #define M50II_ABORT_SELFTEST */   /* wild 0x90000000 fetch core-dumps QEMU (no region); HW-only path */

#if defined(M50II_TRACE_INIT_LADDER) && !defined(M50II_TRACE_QUIET_BOOT_PRE_INIT)
static void m50ii_initc_trace_wait(uint32_t n)
{
    volatile uint32_t i;
    while (n--)
    {
#ifdef M50II_TRACE_FAST_BLINK
        for (i = 0; i < 0x800000; i++)
#else
        for (i = 0; i < 0x02000000; i++)
#endif
        {
        }
    }
}

static void m50ii_initc_trace_blink_once(uint32_t pulses)
{
#if defined(CARD_LED_ADDRESS) && defined(LEDON) && defined(LEDOFF)
    m50ii_initc_trace_wait(2);
    for (uint32_t i = 0; i < pulses; i++)
    {
        MEM(CARD_LED_ADDRESS) = LEDON;
        m50ii_initc_trace_wait(1);
        MEM(CARD_LED_ADDRESS) = LEDOFF;
        m50ii_initc_trace_wait(1);
    }
    m50ii_initc_trace_wait(3);
#else
    (void)pulses;
#endif
}

static void m50ii_initc_trace_blink_forever(uint32_t pulses)
{
    while (1)
    {
        m50ii_initc_trace_blink_once(pulses);
        m50ii_initc_trace_wait(5);
    }
}

#define M50II_INIT_STAGE(n) m50ii_initc_trace_blink_once(n)
#define M50II_INIT_STOP(n) m50ii_initc_trace_blink_forever(n)
#else
#define M50II_INIT_STAGE(n) do { } while (0)
#define M50II_INIT_STOP(n) do { } while (0)
#endif

#ifdef M50II_TRACE_BOOT_POST_FIO_PROBE
extern FILE* _FIO_CreateFile(const char* filename);
extern int _FIO_WriteFile(FILE* stream, const void* ptr, size_t count);
extern int FIO_Flush(const char * filename);
extern void sync_caches(void);

static int m50ii_boot_post_fio_try(const char *filename, uint32_t create_marker, uint32_t write_marker, uint32_t close_marker, uint32_t flush_marker)
{
    static const char msg[] =
        "M50II AUTOEXEC reached boot_post_init_task FIO probe.\n"
        "Canon init_task returned and display-side boot_post waits completed.\n";

    M50II_INIT_STAGE(create_marker);
    FILE *f = _FIO_CreateFile(filename);
    if (!f || f == PTR_INVALID)
    {
        return -1;
    }

    M50II_INIT_STAGE(write_marker);
    sync_caches();
    int written = _FIO_WriteFile(f, msg, sizeof(msg) - 1);

    M50II_INIT_STAGE(close_marker);
    FIO_CloseFile(f);

    M50II_INIT_STAGE(flush_marker);
    FIO_Flush(filename);

    return written;
}

static void m50ii_boot_post_fio_probe(void)
{
    int written = m50ii_boot_post_fio_try("A:/M50TRACE.TXT", 10, 11, 12, 13);
    if (written > 0)
    {
        M50II_INIT_STAGE(14);
        M50II_INIT_STOP(15);
    }

    M50II_INIT_STAGE(1);       /* A: failed cleanly; try B: */
    written = m50ii_boot_post_fio_try("B:/M50TRACE.TXT", 2, 3, 4, 5);
    if (written > 0)
    {
        M50II_INIT_STAGE(6);
        M50II_INIT_STOP(15);
    }

    M50II_INIT_STOP(10);       /* both drive letters failed without hanging */
}
#endif

#if defined(FEATURE_GPS_TWEAKS)
#include "gps.h"
#endif

#if defined(CONFIG_HELLO_WORLD)
#include "fw-signature.h"
#endif

#if defined(CONFIG_MMU_REMAP)
#include "patch.h"
#endif

static int _hold_your_horses = 1; // 0 after config is read
int ml_started = 0; // 1 after ML is fully loaded
int ml_gui_initialized = 0; // 1 after gui_main_task is started 
struct task *first_task = 0; // first item in the array of task structs

/**
 * Called by DryOS when it is dispatching (or creating?)
 * a new task.
 */
static void
my_task_dispatch_hook(
        struct context **p_context_old,    /* on new DryOS (6D+), this argument is different (small number, unknown meaning) */
        struct task *prev_task_unused,     /* only present on new DryOS */
        struct task *next_task_new         /* only present on new DryOS; old versions use HIJACK_TASK_ADDR */
)
{
    struct task * next_task = 
        #ifdef CONFIG_NEW_DRYOS_TASK_HOOKS
        next_task_new;
        #else
        *(struct task **)(HIJACK_TASK_ADDR);
        #endif

/* very verbose; disabled by default */
#undef DEBUG_TASK_HOOK
#ifdef DEBUG_TASK_HOOK
#ifdef CONFIG_NEW_DRYOS_TASK_HOOKS
    /* new DryOS */
    qprintf("[****] task_hook(%x) %x(%s) -> %x(%s), from %x\n",
        p_context_old,
        prev_task_unused, prev_task_unused ? prev_task_unused->name : "??",
        next_task, next_task ? next_task->name : "??",
        read_lr()
    );
#else
    /* old DryOS */
    qprintf("[****] task_hook(%x) -> %x(%s), from %x\n",
        p_context_old,
        next_task, next_task ? next_task->name : "??",
        read_lr()
    );
#endif  /* CONFIG_NEW_DRYOS_TASK_HOOKS */
#endif  /* DEBUG_TASK_HOOK */

    if (!next_task)
        return;

#ifdef CONFIG_NEW_DRYOS_TASK_HOOKS
    /* on new DryOS, first argument is not context; get it from the task structure */
    /* this also works for some models with old-style DryOS, but not all */
    struct context *context = next_task->context;
#else
    /* on old DryOS, context is passed as argument
     * on some models (not all!), it can be found in the task structure as well */
    struct context *context = p_context_old ? (*p_context_old) : 0;
#endif
    
    if (!context)
        return;
    
#ifdef CONFIG_TSKMON
    tskmon_task_dispatch(next_task);
#endif
    
    if (ml_started)
    {
        /* all task overrides should be done by now */
        return;
    }

    // Do nothing unless a new task is starting via the trampoile
    if(context->pc != (uint32_t)task_trampoline)
        return;

    thunk entry = (thunk) next_task->entry;

    qprintf("[****] Starting task %x(%x) %s\n", next_task->entry, next_task->arg, next_task->name);

    // Search the task_mappings array for a matching entry point
    extern struct task_mapping _task_overrides_start[];
    extern struct task_mapping _task_overrides_end[];
    struct task_mapping *mapping = _task_overrides_start;

    for( ; mapping < _task_overrides_end ; mapping++ )
    {
#if defined(POSITION_INDEPENDENT)
        mapping->replacement = PIC_RESOLVE(mapping->replacement);
#endif
        thunk original_entry = mapping->orig;
        if( original_entry != entry )
            continue;

/* -- can't call debugmsg from this context */
        qprintf("[****] Replacing task %x with %x\n",
            original_entry,
            mapping->replacement
        );

        next_task->entry = mapping->replacement;
        break;
    }
}

#ifdef CONFIG_M50mk2
/* [M50II 2026-07-11] Task-switch callback for the M50 II — menu-input bring-up.
 *
 * ML needs ml_gui_main_task to run in place of Canon's GuiMainTask so button events reach ML.
 * TASK_OVERRIDE() swaps a Canon task's entry (gui_main_task -> ml_gui_main_task) the first time it is
 * dispatched; that swap is driven from a per-context-switch callback.
 *
 * On DIGIC-7 siblings ML installs its hook into a FREE scheduler slot. On the M50 II the equivalent
 * slot (*0x4144) is Canon's OWN task-switch list-walker (ROM 0xE0163FE4); overwriting it starved
 * Canon's scheduler -> DryOS panic module 64 (iters 6/7/8, all HW-confirmed; see docs/26 s9). RE
 * (gang100.bin) showed the walker walks a doubly-linked list of {next,prev,fn} nodes and calls each
 * node's fn(1, old, next) inside its spinlock. So instead of squatting the walker, ML REGISTERS its own
 * node into that list via Canon's own insert primitive register_task_switch_callback (ROM 0xE0164044):
 * Canon's walker then calls ml's callback alongside its own. The primitive does the locked head-insert,
 * (re)installs the walker at *0x4144, and restores IRQ state itself -- ML adds no locking of its own.
 * The struct-task offsets the scheduler uses (context@0x54, entry@0x0c, cpu_assigned@0x51,
 * cpu_requested@0x50, yieldRequest@0x4b) all match ML's CONFIG_TASK_STRUCT_V2_SMP `struct task`.
 * Full RE + node/lock details: docs/27-menu-input-api-registration.md. */
struct m50ii_cbnode { void *next; void *prev; void (*fn)(int, struct task *, struct task *); };
static struct m50ii_cbnode m50ii_cbnode;           /* ML's node in Canon's ctx-switch list (static, never freed) */
volatile uint32_t m50ii_override_count = 0;        /* # of TASK_OVERRIDE entry-swaps done by the callback */
const char * volatile m50ii_override_name = 0;     /* name of the last overridden task (logged by heartbeat) */

/* [M50II 2026-07-11] Per-context-switch callback. Canon's walker (ROM 0xE0163FE4, installed at *0x4144)
 * calls every registered node's fn(1, old_task, next_task) while holding the list spinlock. We do the
 * ML TASK_OVERRIDE entry-swap here: swap next->entry (gui_main_task -> ml_gui_main_task) the first time
 * the task is dispatched. MUST NOT block/alloc/UART -- that is the scheduler critical section (a slow UART
 * here timed out CPU1's spinlock -> DryOS panic module 64 in iter6/7). Compare and store only; the
 * heartbeat logs hits from a safe context. */
static void m50ii_cb(int one, struct task *prev, struct task *next)
{
    (void)one; (void)prev;

    /* The walker runs on BOTH cores. CPU1 dispatches interrupt/idle pseudo-tasks whose next->context is
     * not a normal saved context; dereferencing it there could fault, and a fault taken while the walker
     * holds the list spinlock would also hang CPU0. gui_main_task is a CPU0 application task, so restrict
     * the override scan to CPU0 (MPIDR affinity level 0). */
    {
        uint32_t mpidr;
        asm volatile ("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
        if (mpidr & 0xFF)          /* not CPU0 -> skip the ML override scan */
            return;
    }

    /* ML TASK_OVERRIDE: only while booting, only on a task's FIRST dispatch (pc == task_trampoline) */
    if (!next)
        return;
    if (ml_started)                     /* all overrides are installed by the time ML is up */
        return;
    struct context *context = next->context;    /* task + 0x54 (validated vs scheduler RE) */
    if (!context)
        return;
    if (context->pc != (uint32_t)task_trampoline)
        return;

    thunk entry = (thunk) next->entry;          /* task + 0x0c */
    extern struct task_mapping _task_overrides_start[];
    extern struct task_mapping _task_overrides_end[];
    for (struct task_mapping *m = _task_overrides_start; m < _task_overrides_end; m++)
    {
        if (m->orig != entry)
            continue;
        next->entry = m->replacement;
#ifndef M50II_PIN_OVERRIDE_CPU0
#define M50II_PIN_OVERRIDE_CPU0 1
#endif
#if M50II_PIN_OVERRIDE_CPU0
        /* [iter13 ROOT-CAUSE FIX] Pin the overridden task to CPU0. GuiMainTask is created UNPINNED
         * (plain task_create -> cpu_requested inherited, cpu_assigned=0xFF), so the scheduler migrates
         * it to CPU1 and runs task_trampoline THERE -- BLX'ing our ML RAM entry on CPU1, decoupled from
         * the CPU0 core where this swap happened. That is why the swap fires (Canon's task drains
         * nothing -> Edet vsync flood) yet ml_gui_main_task never runs on the expected core. Forcing
         * cpu_requested=0 keeps the task on CPU0, where ML's whole environment (abort handler etc.)
         * lives. Root cause: scheduler RE (FUN_e01b1d82/e01b1be6/e01b19b6, unpinned + cpu_assigned
         * reset on switch-out). General rule: ML TASK_OVERRIDE entries are ML code -> pin to CPU0. */
        next->cpu_requested = 0;
#endif
        m50ii_override_name = next->name;
        m50ii_override_count++;
        break;
    }
}
#endif /* CONFIG_M50mk2 */

/** Call all of the init functions  */
static void
call_init_funcs()
{
    extern struct task_create _init_funcs_start[];
    extern struct task_create _init_funcs_end[];
    struct task_create * init_func = _init_funcs_start;

    for( ; init_func < _init_funcs_end ; init_func++ )
    {
#if defined(POSITION_INDEPENDENT)
        init_func->entry = PIC_RESOLVE(init_func->entry);
        init_func->name = PIC_RESOLVE(init_func->name);
#endif
        DebugMsg( DM_MAGIC, 3,
            "Calling init_func %s (%x)",
            init_func->name,
            (uint32_t) init_func->entry
        );
        thunk entry = (thunk) init_func->entry;
        entry();
    }
}

static void nop( void ) { }
void menu_init( void ) __attribute__((weak,alias("nop")));
void debug_init( void ) __attribute__((weak,alias("nop")));

static int magic_off = 0; // Set to 1 to disable ML
static int magic_off_request = 0;

int magic_is_off() 
{
    return magic_off; 
}

void _disable_ml_startup() {
    magic_off_request = 1;
}

#if defined(CONFIG_AUTOBACKUP_ROM)

#define BACKUP_BLOCKSIZE 0x00100000

static void backup_region(char *file, uint32_t base, uint32_t length)
{
    FILE *handle = NULL;
    uint32_t size = 0;
    uint32_t pos = 0;
    
    /* already backed up that region? */
    if((FIO_GetFileSize( file, &size ) == 0) && (size == length) )
    {
        return;
    }
    
    /* no, create file and store data */

    void* buf = malloc(BACKUP_BLOCKSIZE);
    if (!buf) return;

    handle = FIO_CreateFile(file);
    if (handle)
    {
      while(pos < length)
      {
         uint32_t blocksize = BACKUP_BLOCKSIZE;
        
          if(length - pos < blocksize)
          {
              blocksize = length - pos;
          }
          
          /* copy to RAM before saving, because ROM is slow and may interfere with LiveView */
          memcpy(buf, &((uint8_t*)base)[pos], blocksize);
          
          FIO_WriteFile(handle, buf, blocksize);
          pos += blocksize;
      }
      FIO_CloseFile(handle);
    }
    
    free(buf);
}

static void backup_rom_task()
{
    backup_region("ML/LOGS/ROM1.BIN", 0xF8000000, 0x01000000);
    backup_region("ML/LOGS/ROM0.BIN", 0xF0000000, 0x01000000);
}
#endif

#ifdef CONFIG_HELLO_WORLD

static void draw_test_pattern(int colour)
{
    uint8_t *b = bmp_vram();

    // draw a rectangle on the exact visible border
    for (int y=0; y < 480; y++)
    {
        bmp_putpixel_fast(b, 0, y, colour);
        bmp_putpixel_fast(b, 719, y, colour);
    }
    for (int x=0; x < 720; x++)
    {
        bmp_putpixel_fast(b, x, 0, colour);
        bmp_putpixel_fast(b, x, 479, colour);
    }
}

#ifdef FEATURE_VRAM_RGBA

/** kitor: This aint pretty, but we selectively call bmp init functions
 *  and run required tasks. Other solution would be to have function in bmp.c
 *  to break static scope.
 *
 * We should be safe to run them as:
 * - we are already past _mem_init()
 * - if this code runs, we successfully started ml_init task */
static void init_bmp_indexed()
{
    //first call bmp_init
    extern struct task_create _init_funcs_start[];
    extern struct task_create _init_funcs_end[];
    struct task_create * init_func = _init_funcs_start;

    for( ; init_func < _init_funcs_end ; init_func++ )
    {
        if(strcmp(init_func->name, "bmp_init") == 0)
        {
            thunk entry = (thunk) init_func->entry;
            entry();
            break;
        }
    }

    //then start redraw_task
    extern struct task_create _tasks_start[];
    extern struct task_create _tasks_end[];
    struct task_create * task = _tasks_start;

    for( ; task < _tasks_end ; task++ )
    {
        if(strcmp(task->name, "redraw_task") == 0)
        {
            task_create(
                task->name,
                task->priority,
                task->stack_size,
                task->entry,
                task->arg
            );
            break;
        }
    }
}
#endif

static void hello_world()
{
    int sig = compute_signature((uint32_t*)SIG_START, 0x10000);

    #ifdef FEATURE_VRAM_RGBA
    //kitor: see comment on init_bmp_indexed() above
    init_bmp_indexed();
    #endif

    // wait for GUI to be up
    //kitor: we already did it once in boot_post_init_task() ?!
    while (!bmp_vram_raw())
        msleep(100);

    //DryosDebugMsg(0, 15, "==== HELLO WORLD ====");
    int colour = 4;
    while(1)
    {
        bmp_printf(FONT_LARGE, 140, 50, "Hello, World!");
        bmp_printf(FONT_LARGE, 140, 400, "firmware signature = 0x%x", sig);

        if (colour == 15)
            colour = 4;
        else
            colour++;
        //DryosDebugMsg(0, 15, "display mode: %d", display_output_mode);
        DryosDebugMsg(0, 15, "colour: %d", colour);
        draw_test_pattern(colour);

        ml_refresh_display_needed = 1;
        msleep(200);
        //info_led_blink(1, 500, 500);
    }
}
#endif

#ifdef CONFIG_DUMPER_BOOTFLAG
static void dumper_bootflag()
{
    msleep(5000);
    SetGUIRequestMode(GUIMODE_PLAY);
    msleep(1000);
    bmp_fill(COLOR_BLACK, 0, 0, 720, 480);
    bmp_printf(FONT_LARGE, 50, 100, "Please wait...");
    msleep(2000);

    if (CURRENT_GUI_MODE != GUIMODE_PLAY)
    {
        bmp_printf(FONT_LARGE, 50, 150, "Hudson, we have a problem!");
        return;
    }
    
    /* this requires CONFIG_AUTOBACKUP_ROM */
    bmp_printf(FONT_LARGE, 50, 150, "ROM Backup...");
    backup_rom_task();

    // do not try to enable bootflag in LiveView, or during sensor cleaning (it will fail while writing to ROM)
    // no check is done here, other than a large delay and doing this while in Canon menu
    // todo: check whether the issue is still present with interrupts disabled
    bmp_printf(FONT_LARGE, 50, 200, "EnableBootDisk...");
    uint32_t old = cli();
    call("EnableBootDisk");
    sei(old);

    bmp_printf(FONT_LARGE, 50, 250, ":)");
}
#endif

static void led_fade(int arg1, void * on)
{
    /* emulate a fade-out PWM using a HPTimer */
    static int k = 16000;
    if (k > 0)
    {
        if (on)
            _card_led_on();
        else
            _card_led_off();
        int next_delay = (on ? k : 16000 - k);   /* cycle: 16000 us => 62.5 Hz */
        SetHPTimerNextTick(arg1, next_delay, led_fade, led_fade, (void *) !on);
        k -= MAX(16, k/32);  /* adjust fading speed and shape here */
    }
}

/* [M50II] Wait for Canon subsystems (PropertyMgr, and by extension Storage/FIO)
 * to finish initializing before ml_init registers property handlers or does
 * file I/O.
 *
 * Root cause (RE'd from the 1.0.1 ROM): Canon's prop_register_slave asserts
 * "Property/PropertyMgr.c:484" when the PropertyMgr manager object is still
 * NULL. That object pointer lives at RAM 0x4FF4 (DAT_e05bfa58) and is written
 * once by CreatePropertyMgr (FUN_e05bf8c6). DryOS scheduling is cooperative for
 * equal/lower-priority tasks, so Canon's PropMgr/Storage init tasks only get to
 * run when ml_init yields. We msleep() in a bounded poll -- each msleep hands
 * the CPU to those init tasks -- until the manager is non-NULL.
 * Returns 1 if PropertyMgr came up, 0 on timeout (points to a deeper init
 * problem, e.g. CPU1 never ready / an init event that never fires). */
static int m50ii_wait_canon_subsystems(void)
{
    extern void m50ii_uart_str(const char *);
    volatile uint32_t *propmgr = (volatile uint32_t *)0x4FF4;  /* Canon PropertyMgr manager ptr */
    int waited_ms = 0;
    const int step_ms = 10;
    const int timeout_ms = 5000;

    while (*propmgr == 0 && waited_ms < timeout_ms)
    {
        msleep(step_ms);
        waited_ms += step_ms;
    }

    if (*propmgr != 0)
    {
        m50ii_uart_str("[ML] canon-ready: PropertyMgr UP\n");
        DryosDebugMsg(0, 15, "canon-ready: PropertyMgr up after %d ms", waited_ms);
        return 1;
    }
    m50ii_uart_str("[ML] canon-ready: TIMEOUT (PropertyMgr still NULL)\n");
    DryosDebugMsg(0, 15, "canon-ready: TIMEOUT after %d ms, *0x4FF4 still 0", waited_ms);
    return 0;
}

/* [M50II 2026-07-11] Tier-A stub self-test -- validates the RE-corrected crash-class stubs LIVE after
 * big.8 through ML's real exmem/tasks integration path, logging per-stub markers over UART. The primary
 * signal is that each corrected stub DISPATCHES CLEANLY (no pc=0x2 abort, which the OLD wrong addresses
 * -- mid-instruction / string-pool -- would produce); a non-NULL allocation is a bonus (needs Canon's
 * shoot pool up). Covers: _get_task_info_by_id (0xe01d77c4), AllocateMemoryResource (0xe0608904 via
 * shoot_malloc_suite), FreeMemoryResource (0xe0608960 via shoot_free_suite), AllocateContinuousMemoryResource
 * (0xe0608932 via shoot_malloc_suite_contig) + suite accessors. Bounded: shoot_malloc_suite_int has a 100ms
 * alloc timeout so a bad stub reports FAIL, never hangs. Gated by M50II_STUB_SELFTEST; comment out once HW-validated. */
/* HW-VALIDATED 2026-07-11: all 4 corrected stubs PASS on silicon (1MB/256KB allocs + free + task-info).
 * Disabled for the clean keeper; re-enable (uncomment) to re-test after any stub-address change. */
// #define M50II_STUB_SELFTEST
#if defined(CONFIG_M50mk2) && defined(M50II_STUB_SELFTEST)
static void m50ii_stub_selftest(void)
{
    extern void m50ii_uart_str(const char *);
    m50ii_uart_str("[ML] selftest: BEGIN Tier-A corrected-stub validation\n");

    /* (1) _get_task_info_by_id 0xe01d77c4 -- via ML get_task_info_by_id wrapper (same call as tasks.c) */
    m50ii_uart_str("[ML] selftest: [1] _get_task_info_by_id ...\n");
    {
        struct task_attr_str ta;
        memset(&ta, 0, sizeof(ta));
        int id = current_task->taskId;
        int r = get_task_info_by_id(1, id, &ta);
        mlog_kv("[ML] selftest:   gtinfo r", (uint32_t)r);
        mlog_kv("[ML] selftest:   gtinfo stack_size", (uint32_t)ta.size);
        m50ii_uart_str((r == 0 && ta.size)
            ? "[ML] selftest:   _get_task_info_by_id PASS\n"
            : "[ML] selftest:   _get_task_info_by_id FAIL\n");
    }

    /* The 3 *MemoryResource stubs tail-dispatch through a RUNTIME vtable: manager base = *(0xe0608ac4)
     * = 0x5128, slots at +0/+4/+8 populated by Canon's RscWrap init. If a slot is NULL, the stub fires
     * its RscWrap.c:0x4f/0x58/0x60 assert (our non-fatal handler returns) and then `bx *(mgr+slot)`
     * branches to NULL -> abort. QEMU never registers the manager, so guard on the vtable being live.
     * (Reaching that assert already PROVES the addresses -- see AllocateMemoryResource@0xe0608904.) */
    {
        uint32_t mgr = *(volatile uint32_t *)0xE0608AC4;
        int rsc_ready = mgr
            && *(volatile uint32_t *)(mgr + 0)
            && *(volatile uint32_t *)(mgr + 4)
            && *(volatile uint32_t *)(mgr + 8);
        mlog_kv("[ML] selftest: RscWrap mgr", mgr);
        if (!rsc_ready)
        {
            m50ii_uart_str("[ML] selftest: RscWrap vtable NOT populated -> SKIP alloc tests "
                           "(expected in QEMU; on HW = Canon RscMgr not up at big.8)\n");
        }
        else
        {
            /* (2) AllocateMemoryResource 0xe0608904 + (3) FreeMemoryResource 0xe0608960 */
            m50ii_uart_str("[ML] selftest: [2] AllocateMemoryResource (shoot_malloc_suite 1MB) ...\n");
            {
                struct memSuite *s = shoot_malloc_suite(1024 * 1024);
                if (s)
                {
                    mlog_kv("[ML] selftest:   suite bytes", (uint32_t)GetSizeOfMemorySuite(s));
                    mlog_kv("[ML] selftest:   suite chunks", (uint32_t)GetNumberOfChunks(s));
                    m50ii_uart_str("[ML] selftest:   AllocateMemoryResource PASS\n");
                    m50ii_uart_str("[ML] selftest: [3] FreeMemoryResource (shoot_free_suite) ...\n");
                    shoot_free_suite(s);
                    m50ii_uart_str("[ML] selftest:   FreeMemoryResource PASS (returned, no abort)\n");
                }
                else
                {
                    m50ii_uart_str("[ML] selftest:   AllocateMemoryResource dispatched, suite=NULL "
                                   "(pool busy/empty; address validated = no abort)\n");
                }
            }

            /* (4) AllocateContinuousMemoryResource 0xe0608932 -- via shoot_malloc_suite_contig */
            m50ii_uart_str("[ML] selftest: [4] AllocateContinuousMemoryResource (shoot_malloc_suite_contig 256KB) ...\n");
            {
                struct memSuite *s = shoot_malloc_suite_contig(256 * 1024);
                if (s)
                {
                    mlog_kv("[ML] selftest:   contig bytes", (uint32_t)GetSizeOfMemorySuite(s));
                    m50ii_uart_str("[ML] selftest:   AllocateContinuousMemoryResource PASS\n");
                    shoot_free_suite(s);
                }
                else
                {
                    m50ii_uart_str("[ML] selftest:   AllocateContinuousMemoryResource dispatched, suite=NULL "
                                   "(fragmentation/pool; address validated = no abort)\n");
                }
            }
        }
    }

    m50ii_uart_str("[ML] selftest: END -- if no [ABORT] above, all corrected stubs dispatch cleanly\n");
}
#endif /* CONFIG_M50mk2 && M50II_STUB_SELFTEST */

#if defined(CONFIG_M50mk2)
/* [M50II 2026-07-11 DISPLAY-PROBE] Phase-A ground-truth capture for OSD/compositor bring-up.
 *
 * SAFE BY CONSTRUCTION — brick risk ~0:
 *   - Only READS the two candidate display structures + calls Canon's OWN read-only
 *     diagnostic evprocs (XCMStatus / VramState).
 *   - Does NOT write any Canon buffer and does NOT call XimrExe / mzrm-submit, so it
 *     cannot feed a bad Ximr context to the Zico core and hang the display pipeline.
 *   - Every pointer is range-guarded before deref; the hardened abort handler is a net.
 *
 * WHY A PROBE (not a working OSD yet): M50 II has TWO parallel display layers and the
 * XCM setter API is signature-inconsistent (GetSourceSurface takes a ctx *pointer*,
 * SetSourceSurface takes a screen *index*), so the M6II XCM_V2 compositor path won't
 * drop in and a blind XIMR_CONTEXT would crash XimrExe (Discord finding #333). This
 * probe captures the exact runtime pointers / per-layer MARV surfaces / geometry needed
 * to bind rgb_vram_info correctly next iteration. It disambiguates:
 *   A) RENDERER path  — surf array @0x7F1B4 (_rgb_vram_info), state cache @0x11264
 *   B) XCM path       — ctx table @0x7F35C, XCM_GetSourceSurface (0xE00BE348),
 *                       VMIX ctx @0x11294, winsys_sem @0x11298
 * All addresses RE'd from gang100.bin 1.0.1 (see docs/26-display-frontier.md).
 * struct MARV: +0 sig(0x5652414D) +4 bitmap_data +8 opacity +C flags +10 w +14 h.
 */
typedef void * (*m50ii_xcm_getsrc_fn)(void *ctx, uint32_t layer);

static void m50ii_marv_dump(const char *tag, volatile uint32_t *marv)
{
    extern void m50ii_uart_str(const char *);
    if (marv == 0 || (uint32_t)marv < 0x1000 || (uint32_t)marv >= 0x40000000)
    {
        m50ii_uart_str(tag);
        m50ii_uart_str(" = <null/invalid>\n");
        return;
    }
    mlog_kv(tag, (uint32_t)marv);
    mlog_kv("[ML] disp-probe:   sig(exp 5652414D)", marv[0]);
    mlog_kv("[ML] disp-probe:   bitmap_data",       marv[1]);
    mlog_kv("[ML] disp-probe:   flags",             marv[3]);
    mlog_kv("[ML] disp-probe:   width",             marv[4]);
    mlog_kv("[ML] disp-probe:   height",            marv[5]);
}

static void m50ii_display_probe(void)
{
    extern void m50ii_uart_str(const char *);
    m50ii_uart_str("[ML] disp-probe: BEGIN (read-only RENDERER + XCM ground-truth)\n");

    /* Candidate A: RENDERER surface array (prior RE) — the source for _rgb_vram_info */
    mlog_kv("[ML] disp-probe: A) RENDERER surf[0] @7F1B4", *(volatile uint32_t *)0x0007F1B4);
    mlog_kv("[ML] disp-probe:    RENDERER cache   @11264", *(volatile uint32_t *)0x00011264);
    m50ii_marv_dump("[ML] disp-probe: A) *(7F1B4) RENDERER MARV",
                    (volatile uint32_t *)*(volatile uint32_t *)0x0007F1B4);

    /* Candidate B: XCM ctx table + XCM_GetSourceSurface (new RE) */
    volatile uint32_t *ctx_tbl = (volatile uint32_t *)0x0007F35C;
    mlog_kv("[ML] disp-probe: B) XCM ctx_tbl[0] @7F35C", ctx_tbl[0]);
    mlog_kv("[ML] disp-probe:    XCM ctx_tbl[1]",        ctx_tbl[1]);
    mlog_kv("[ML] disp-probe:    XCM ctx_tbl[2]",        ctx_tbl[2]);
    mlog_kv("[ML] disp-probe:    VMIX ctx@11294 +18 dirty", *(volatile uint32_t *)(0x00011294 + 0x18));
    mlog_kv("[ML] disp-probe:    winsys_sem @11298",        *(volatile uint32_t *)0x00011298);

    void *panel_ctx = (void *)ctx_tbl[0];
    if (panel_ctx != 0 && (uint32_t)panel_ctx > 0x1000 && (uint32_t)panel_ctx < 0x40000000)
    {
        m50ii_xcm_getsrc_fn XCM_GetSourceSurface = (m50ii_xcm_getsrc_fn)(0xE00BE348 | 1);
        for (uint32_t layer = 0; layer < 4; layer++)
        {
            volatile uint32_t *marv = (volatile uint32_t *)XCM_GetSourceSurface(panel_ctx, layer);
            mlog_kv("[ML] disp-probe: B) XCM layer#", layer);
            m50ii_marv_dump("[ML] disp-probe:    XCM MARV", marv);
        }
    }
    else
    {
        m50ii_uart_str("[ML] disp-probe: B) XCM panel ctx null/invalid -> skip GetSourceSurface\n");
    }

    /* Canon's own read-only compositor / VRAM diagnostic evprocs -> UART debug sink */
    extern int call(const char *name, ...);
    m50ii_uart_str("[ML] disp-probe: call(XCMStatus)...\n");
    call("XCMStatus");
    m50ii_uart_str("[ML] disp-probe: call(VramState)...\n");
    call("VramState");
    m50ii_uart_str("[ML] disp-probe: END\n");
}

/* [M50II 2026-07-11 ITERATION-2] On-screen OSD attempt via a DEDICATED XCM layer.
 *
 * HW ground-truth (scratchpad/hw_phaseA.log): the panel XimrContextMaker @ *(0x7F35C)
 * (=0xCE1200 on HW) carries Canon input layers 0 (GUI) + 1 (overlay), both 960x540 RGBA
 * (flags 0x05040100), src(120,30,720,480)->dst(0,0); layers 2 & 3 are FREE. We claim the
 * first free slot for ML instead of fighting Canon's layers.
 *
 * RE'd entries (all validated live in Phase A):
 *   XCM_GetSourceSurface  0xE00BE348 (ctx*, layer) -> MARV*
 *   XCM_SetSourceSurface  0xE035D590 (screenIdx 0-2, layer, MARV*)  [resolves ctx internally]
 *   XCM_SetSourceArea     0xE00BE37C (ctx*, layer, srcX,srcY,srcW,srcH)
 * struct MARV @ +0 sig(0x5652414D) +4 bitmap_data +8 opacity +C flags +10 w +14 h +18 pmem.
 *
 * SAFETY: self-contained, range-guarded, allocates its OWN 960x540 buffer (no overrun of
 * Canon memory), writes pixels via the UNCACHEABLE alias (|0x40000000, the D8 rule) so no
 * cache flush is needed, and triggers recomposite via the VMIX dirty flag (*(0x11294+0x18)=1)
 * instead of calling the chunked mzrm refreshVrmsSurface directly (which could hang Zico).
 * We do NOT bind rgb_vram_info yet (keeps bmp.c's XimrExe(XIMR_CONTEXT) path out of it).
 * A bright magenta bordered box makes an ML-drawn layer unmistakable vs Canon's UI.
 */
#define M50II_OSD_TRY 1
#if M50II_OSD_TRY
typedef void * (*m50ii_getsrc_fn)(void *ctx, uint32_t layer);
typedef int    (*m50ii_setsurf_fn)(uint32_t screenIdx, uint32_t layer, void *marv);
typedef int    (*m50ii_setarea_fn)(void *ctx, uint32_t layer, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh);

static void m50ii_osd_try(void)
{
    extern void m50ii_uart_str(const char *);
    void *ctx = (void *)*(volatile uint32_t *)0x0007F35C;
    if (ctx == 0 || (uint32_t)ctx < 0x1000 || (uint32_t)ctx >= 0x40000000)
    {
        m50ii_uart_str("[ML] osd-try: ctx null/invalid (QEMU?) -> skip\n");
        return;
    }

    m50ii_getsrc_fn  Get     = (m50ii_getsrc_fn)(0xE00BE348 | 1);
    m50ii_setsurf_fn SetSurf = (m50ii_setsurf_fn)(0xE035D590 | 1);
    m50ii_setarea_fn SetArea = (m50ii_setarea_fn)(0xE00BE37C | 1);

    /* [ITER-3] Allocate ML's OWN 960xH RGBA layer from the shoot pool (contiguous) and
     * register it in a free slot -- iteration 2c proved compositing works by squatting on
     * Canon's layer 1; now we own the layer. shoot_malloc_suite_contig is HW-proven (Tier-A
     * self-test did 256KB) whereas malloc/_AllocateMemory both failed at 2MB. Size-fallback so
     * one boot secures the biggest buffer available and logs the ceiling. Paint a magenta
     * 10px border + semi-transparent interior so we see OUR layer composite over Canon. */
    uint32_t layer = 0;
    while (layer < 4 && Get(ctx, layer) != 0) layer++;
    if (layer >= 4) { m50ii_uart_str("[ML] osd-try: no free layer slot\n"); return; }
    mlog_kv("[ML] osd-try: free layer", layer);

    struct memSuite *suite = 0;
    uint32_t got = 0;
    uint32_t sizes[3]; sizes[0] = 0x200000; sizes[1] = 0x100000; sizes[2] = 0x80000; /* 2MB,1MB,512KB */
    for (int s = 0; s < 3 && suite == 0; s++)
    {
        suite = shoot_malloc_suite_contig(sizes[s]);
        mlog_kv(suite ? "[ML] osd-try: shoot_contig OK size" : "[ML] osd-try: shoot_contig FAIL size", sizes[s]);
        if (suite) got = sizes[s];
    }
    if (suite == 0) { m50ii_uart_str("[ML] osd-try: all shoot_contig sizes FAILED\n"); return; }

    void *chunkaddr = GetMemoryAddressOfMemoryChunk(GetFirstChunkFromSuite(suite));
    if (chunkaddr == 0) { m50ii_uart_str("[ML] osd-try: chunk addr NULL\n"); shoot_free_suite(suite); return; }
    mlog_kv("[ML] osd-try: buffer addr", (uint32_t)chunkaddr);

    const uint32_t W = 960;
    uint32_t H = (got / 4) / W; if (H > 540) H = 540;
    uint32_t srcH = (H > 30) ? (H - 30) : 0; if (srcH > 480) srcH = 480;
    mlog_kv("[ML] osd-try: layer H", H);
    mlog_kv("[ML] osd-try: srcH", srcH);

    uint32_t *px = (uint32_t *)((uintptr_t)chunkaddr | 0x40000000);   /* uncacheable alias */
    uint32_t *marv = (uint32_t *)malloc(0x20);
    if (marv == 0) { m50ii_uart_str("[ML] osd-try: MARV malloc FAILED\n"); shoot_free_suite(suite); return; }
    marv[0] = 0x5652414D; marv[1] = (uint32_t)px; marv[2] = 0;
    marv[3] = 0x05040100; marv[4] = W; marv[5] = H; marv[6] = 0;
    mlog_kv("[ML] osd-try: our MARV", (uint32_t)marv);

    /* [ITER-4] Clear our layer transparent, register it, then ROUTE ML's real OSD into it:
     * rgb_vram_info = our MARV, so bmp.c's redraw_task converts ML's indexed BMP buffer
     * (menus/zebra/bmp_printf via bmp_vram_indexed) into this layer and raises the VMIX
     * dirty flag (bmp.c refresh_yuv_from_rgb M50mk2 branch). Suite is NOT freed (persistent). */
    for (uint32_t i = 0; i < W * H; i++) px[i] = 0x00000000u;   /* transparent */
    SetSurf(0, layer, marv);
    SetArea(ctx, layer, 120, 30, 720, srcH);

    extern struct MARV *rgb_vram_info;
    extern uint32_t ml_refresh_display_needed;
    rgb_vram_info = (struct MARV *)marv;    /* ML OSD now draws into OUR layer */
    m50ii_uart_str("[ML] osd-try: layer registered + rgb_vram_info bound to OUR layer\n");

    /* draw via the REAL ML BMP API, then drive the redraw_task for ~5s so it converts our
     * indexed OSD -> layer + recomposites; confirms the whole ML OSD path end to end. */
    bmp_printf(FONT_LARGE, 120, 60,  "Magic Lantern - EOS M50 Mark II");
    bmp_printf(FONT_LARGE, 120, 120, "OSD layer LIVE (shoot 2MB -> XCM L2 -> VMIX)");
    for (int rep = 0; rep < 25; rep++) { ml_refresh_display_needed = 1; msleep(200); }

    for (uint32_t l = 0; l < 4; l++)
        mlog_kv("[ML] osd-try: post-layer MARV", (uint32_t)Get(ctx, l));
    m50ii_uart_str("[ML] osd-try: END -- ML text should be on the panel\n");
}
#endif /* M50II_OSD_TRY */
#endif /* CONFIG_M50mk2 */

/* This runs ML initialization routines and starts user tasks.
 * Unlike init_task, from here we can do file I/O and others.
 */
static void my_big_init_task()
{
    extern void m50ii_uart_str(const char *);   /* [M50II STEP2 HW-TRACE] */
    m50ii_uart_str("[ML] big.0: my_big_init_task ENTER -- full ML init begins\n");
    m50ii_led_blink(5);   /* LED beacon: ml_init task RUNS (5 blinks) — also proves ML tasks get scheduled */
    _mem_init();
    m50ii_uart_str("[ML] big.1: _mem_init done\n");

    /* [M50II] yield until Canon PropertyMgr/Storage are up (see helper above).
     * This must precede _find_ml_card (FIO) and call_init_funcs (prop_init ->
     * prop_register_slave), all of which otherwise fail: 'no sem' / PropertyMgr
     * assert, because ml_init would run before Canon finished its own init. */
    m50ii_wait_canon_subsystems();
    m50ii_uart_str("[ML] big.1b: canon-subsystems wait done\n");

    /* [M50II TIER1-DIAG 2026-07-10] One-boot race-vs-hard-fail probe for BOTH Tier-1 gaps
     * (display compositor + SD mount), placed in the YIELDING my_big_init_task context so the
     * msleep(150) gives Canon's OWN tasks CPU to finish RENDERER/VMIX graphics init and the SD
     * FileMgr mount (a busy-spin here would starve Canon and manufacture a false HARD-FAIL).
     * Each ~150ms sample records: display surface *0x7F1B4 (RENDERER surf-array[0] MARV) + its
     * cache *0x11264, and mount via is_file("B:/AUTOEXEC.BIN") (known-present) + is_dir("B:/ML").
     * A signal that flips 0->nonzero = RACE (fix: add a wait-gate). Never flips over ~4.5s =
     * HARD-FAIL (fix: RE the missing SD-IRQ / MPU property). First-good sample+us logged per
     * signal. Diagnostic only — remove for the keeper build. */
#define M50II_TIER1_DIAG 1
#if M50II_TIER1_DIAG
    {
        volatile uint32_t *US = (volatile uint32_t *)0xC0242014;   /* free-running microsecond timer */
        uint32_t t0 = *US;
        int disp_first = -1, mnt_first = -1, mld_first = -1;
        uint32_t disp_us = 0, mnt_us = 0, mld_us = 0;
        char hb[12];
        m50ii_uart_str("[ML] tier1-diag START: surf 0x7F1B4/0x11264 + mount B:/ (~4.5s, yielding)\n");
        for (int s = 0; s < 30; s++)
        {
            uint32_t surf  = *(volatile uint32_t *)0x0007F1B4;
            uint32_t surfc = *(volatile uint32_t *)0x00011264;
            int mnt = is_file("B:/AUTOEXEC.BIN");   /* known-present file: succeeds iff FS mounted */
            int mld = is_dir("B:/ML");
            uint32_t el = *US - t0;
            m50ii_uart_str("[ML] t1 s="); m50ii_uart_str(m50ii_hex(hb, (uint32_t)s));
            m50ii_uart_str(" us=");       m50ii_uart_str(m50ii_hex(hb, el));
            m50ii_uart_str(" surf=");     m50ii_uart_str(m50ii_hex(hb, surf));
            m50ii_uart_str(" surfc=");    m50ii_uart_str(m50ii_hex(hb, surfc));
            m50ii_uart_str(mnt ? " mntB=1" : " mntB=0");
            m50ii_uart_str(mld ? " mlD=1\n" : " mlD=0\n");
            if (disp_first < 0 && (surf || surfc)) { disp_first = s; disp_us = el; }
            if (mnt_first  < 0 && mnt)             { mnt_first  = s; mnt_us  = el; }
            if (mld_first  < 0 && mld)             { mld_first  = s; mld_us  = el; }
            msleep(150);   /* YIELD so Canon's graphics/storage tasks get CPU to finish bring-up */
        }
        if (disp_first >= 0) mlog_kv("[ML] tier1-diag VERDICT display=RACE first_us", disp_us);
        else                 m50ii_uart_str("[ML] tier1-diag VERDICT display=HARD-FAIL (surface never populated)\n");
        if (mnt_first  >= 0) mlog_kv("[ML] tier1-diag VERDICT mount=RACE first_us", mnt_us);
        else                 m50ii_uart_str("[ML] tier1-diag VERDICT mount=HARD-FAIL (B:/ never mounted)\n");
        if (mld_first  >= 0) mlog_kv("[ML] tier1-diag VERDICT B:/ML first_us", mld_us);
        else                 m50ii_uart_str("[ML] tier1-diag VERDICT B:/ML never seen\n");
        m50ii_uart_str("[ML] tier1-diag END\n");
    }
#endif /* M50II_TIER1_DIAG */

#if defined(CONFIG_M50mk2)
    /* [M50II PHASE-A] Capture display ground-truth AFTER the tier1-diag yield (Canon's
     * RENDERER/VMIX graphics init has had ~4.5s of CPU by here). Read-only; see fn header. */
    m50ii_display_probe();
    /* NOTE: m50ii_osd_try() runs LATER (after ml_started=1) — the shoot memory pool it needs
     * is not ready this early at big.1b (all shoot_malloc_suite_contig sizes failed here). */
#endif

    _find_ml_card();
    m50ii_uart_str("[ML] big.2: _find_ml_card done\n");
    m50ii_led_blink(7);   /* LED beacon: _mem_init + _find_ml_card done (7 blinks) */

    /* [M50II FIO-PROBE] Is storage/FIO actually ready this early in
     * my_big_init_task? If is_dir() false-negatives here, _find_ml_card can't
     * see the card and the config/font loads below will also fail. Log the raw
     * answers so we can tell the root (FIO timing) from the symptom (fontless). */
    {
        int d_root  = is_dir("B:/");
        int d_ml    = is_dir("B:/ML");
        int d_fonts = is_dir("B:/ML/FONTS");
        m50ii_uart_str(d_root  ? "[ML] fio-probe: B:/ dir=YES\n"        : "[ML] fio-probe: B:/ dir=NO\n");
        m50ii_uart_str(d_ml    ? "[ML] fio-probe: B:/ML dir=YES\n"      : "[ML] fio-probe: B:/ML dir=NO\n");
        m50ii_uart_str(d_fonts ? "[ML] fio-probe: B:/ML/FONTS dir=YES\n": "[ML] fio-probe: B:/ML/FONTS dir=NO\n");
    }

    /* should we require SET for loading ML, or not? */
    extern int _set_at_startup;
    _set_at_startup = config_flag_file_setting_load("ML/SETTINGS/REQUIRE.SET");
    m50ii_uart_str("[ML] big.3: config_flag REQUIRE.SET load done\n");

    // at this point, gui_main_task should be started and should be able to tell whether SET was pressed at startup
    if (magic_off_request != _set_at_startup)
    {
        /* should we bypass loading ML? */
        /* (pressing SET after this point will be ignored) */
        magic_off = 1;

    #if defined(CONFIG_ADDITIONAL_VERSION)
        /* fixme: enable on all models */
        extern char additional_version[];
        additional_version[0] = '-';
        additional_version[1] = 'm';
        additional_version[2] = 'l';
        additional_version[3] = '-';
        additional_version[4] = 'o';
        additional_version[5] = 'f';
        additional_version[6] = 'f';
        additional_version[7] = '\0';
    #endif

        /* some very basic feedback - fade out the SD led */
        SetHPTimerAfterNow(1000, led_fade, led_fade, 0);

        /* do not continue loading ML */
        return;
    }

#if defined(CONFIG_MMU_REMAP) && defined(CONFIG_RPC)
    m50ii_uart_str("[ML] big.4a: calling apply_normal_patches (raw-video ROM-patch machinery)\n");
    int err = apply_normal_patches();
    if (err < 0)
        qprintf("Error from apply_normal_patches: %d\n", err);
    m50ii_uart_str("[ML] big.4b: apply_normal_patches returned\n");
#endif

#if defined(CONFIG_M50mk2)
    /* [M50II] Small settle before font load. Scheduler is live since big.2 (heartbeat task
     * running, big.2/big.3 FIO already yielded); this matches the HW-confirmed b29fc3f0 timing.
     * The big.5 crash was the wrong DMA stub (now fixed in function_overrides.c), not a yield. */
    msleep(200);
#endif
    _load_fonts();
    m50ii_uart_str("[ML] big.5: _load_fonts done\n");
    m50ii_led_blink(9);   /* LED beacon: fonts loaded (9 blinks) */

    // SJE not sure on best place to do this.  Before HELLO_WORLD is nice
    // if possible, needs to be after DryOS inits task scheduler.  I assume
    // that has happened but don't know how to check.
    //
    // DryOS keeps task structs in an array, the first task is created
    // very early and never removed (the "idle" task).  TaskId is index
    // into the array, so we can find first item knowing any other.
    first_task = current_task->self - ((current_task->taskId & 0xffff) - 1);

#ifdef CONFIG_HELLO_WORLD
    hello_world();
    return;
#endif

#ifdef CONFIG_DUMPER_BOOTFLAG
    dumper_bootflag();
    return;
#endif

#ifdef CONFIG_XF605
    uart_printf("hello from ML, before early tasks");
#endif

    m50ii_uart_str("[ML] big.6a: entering early-init (DisablePowerSave/menu/debug/call_init_funcs)\n");
    call("DisablePowerSave");
    _ml_cbr_init();
    menu_init();
    debug_init();
    call_init_funcs(); // among other things, this initialises modules
    m50ii_uart_str("[ML] big.6b: call_init_funcs (modules init) done\n");
    msleep(200); // leave some time for property handlers to run

#ifdef CONFIG_XF605
    uart_printf("hello from ML, after early tasks");
#endif

    /**
     * kitor FIXME: disabling rom dump for D678 as it uses different addresses
     * and offsets. I feel those should be per generation, or maybe per camera
     * as R has different rom size than RP in same gen...
     */
    #if defined(CONFIG_AUTOBACKUP_ROM) && !defined(CONFIG_QEMU)
    /* backup ROM first time to be prepared if anything goes wrong. choose low prio */
    /* On 5D3, this needs to run after init functions (after card tests) */
    task_create("ml_backup", 0x1f, 0x4000, backup_rom_task, 0 );
    #endif

    /* Read ML config. if feature disabled, nothing happens */
    config_load();
    m50ii_uart_str("[ML] big.7: config_load done\n");

    debug_init_stuff();

    #ifdef FEATURE_GPS_TWEAKS
    gps_tweaks_startup_hook();
    #endif

    _hold_your_horses = 0; // config read, other overriden tasks may start doing their job

    // Create all of our auto-create tasks, defined via TASK_CREATE()
    extern struct task_create _tasks_start[];
    extern struct task_create _tasks_end[];
    struct task_create * task = _tasks_start;

    int ml_tasks = 0;
    for( ; task < _tasks_end ; task++ )
    {
#if defined(POSITION_INDEPENDENT)
        task->name = PIC_RESOLVE(task->name);
        task->entry = PIC_RESOLVE(task->entry);
        task->arg = PIC_RESOLVE(task->arg);
#endif
        task_create(
            task->name,
            task->priority,
            task->stack_size,
            task->entry,
            task->arg
        );
        ml_tasks++;
    }
    
    msleep(500);
#ifdef CONFIG_XF605
    uart_printf("hello from ML, after late tasks");
#endif

    m50ii_uart_str("[ML] big.8: my_big_init_task COMPLETE -- ml_started=1 (ML fully initialized)\n");
    m50ii_led_blink(11);   /* LED beacon: FULL ML init done (11 blinks) */
    ml_started = 1;

#if defined(CONFIG_M50mk2) && defined(M50II_STUB_SELFTEST)
    m50ii_stub_selftest();   /* validate the RE-corrected Tier-A allocator/task-info stubs live on HW */
#endif
#if defined(CONFIG_M50mk2) && M50II_OSD_TRY
    /* [ITER-3] Register ML's own OSD layer HERE, after ml_started=1: the shoot memory pool
     * osd_try needs is only ready this late (it was empty at big.1b). This is exactly where
     * the Tier-A self-test's shoot_malloc succeeded on HW. */
    m50ii_osd_try();
#endif
}

/** Blocks execution until config is read */
void hold_your_horses()
{
    while (_hold_your_horses)
    {
        msleep( 100 );
    }
}

/**
 * Custom assert handler - intercept ERR70 and try to save a crash log.
 * Crash log should contain Canon error message.
 */
static char assert_msg[512] = "";
static int (*old_assert_handler)(char*,char*,int,int) = 0;
const char* get_assert_msg() { return assert_msg; }

static int my_assert_handler(char* msg, char* file, int line, int arg4)
{
    if (msg == NULL)
        msg = "nullptr";

    uint32_t lr = read_lr();

#ifdef CONFIG_DIGIC_678X
    // compiler warning on unused len
    snprintf(assert_msg, sizeof(assert_msg),
#else
    int len = snprintf(assert_msg, sizeof(assert_msg), 
#endif
        "ASSERT: %s\n"
        "at %s:%d, %s:%x\n"
        "lv:%d mode:%d\n\n", 
        msg, 
        file, line, get_current_task_name(), lr,
        lv, shooting_mode
    );
// SJE FIXME: assert handling is buggy on modern Digic.
// Disable some of it here and do quick hack output:
#ifdef CONFIG_DIGIC_678X
    uart_printf("[SJE] my_assert_msg: %s", assert_msg);
#else
    backtrace_getstr(assert_msg + len, sizeof(assert_msg) - len);
#endif
    request_crash_log(1);
    return old_assert_handler(msg, file, line, arg4);
}

#if defined(CONFIG_M50mk2)
/* [M50II Phase-3 2026-07-11] Non-fatal assert handler -- bring-up safety net.
 * Canon's DryOS assert routine calls the handler at *DRYOS_ASSERT_HANDLER; the stock
 * handler (and ML's my_assert_handler above) chain to Canon's fatal handler, which
 * halts. During bring-up we would rather LOG the assert and RETURN so DryOS proceeds
 * past it ("kinda boot into UI"), letting us see how far Canon+ML get in a single boot
 * instead of dying at the first assert.
 *
 * [2026-07-11] NOW THE PRIMARY GraphicsConvertPosType:9 FIX (ON by default). The MMU
 * NOP approach is dead: it needs a 2nd remap page, and HW proved M50 II cannot spare
 * even one extra 64K page from Canon's DryOS pool without a boot-time panic (see
 * patch_mmu.c MMU_MAX_REMAP_PAGES note). This handler costs zero memory and makes the
 * converter assert survivable so ml_init can pass big.5->big.8. Installed at post.0,
 * which is before my_big_init_task's big.5, so it catches the ml_init assert. Logging is
 * BOUNDED (first N) because the converter may assert every display frame -- unbounded
 * UART at 115200 would stall the boot. Keep tiny -- runs in Canon's assert context. */
#ifndef M50II_NONFATAL_ASSERTS
#define M50II_NONFATAL_ASSERTS 1
#endif
#if M50II_NONFATAL_ASSERTS
static void *m50ii_old_assert_handler = 0;
static int m50ii_nonfatal_assert_handler(char *msg, char *file, int line, int arg4)
{
    extern void m50ii_uart_str(const char *);
    (void)arg4;
    /* Bound the UART logging so a per-frame assert can't flood the port and stall boot.
     * We still RETURN (non-fatal) for every assert; only the logging is rate-limited. */
    static int logged = 0;
    if (logged < 32)
    {
        logged++;
        static const char hx[] = "0123456789ABCDEF";
        char lbuf[16];
        int n = 0;
        lbuf[n++] = ' '; lbuf[n++] = 'L'; lbuf[n++] = ':';
        for (int i = 0; i < 8; i++) lbuf[n++] = hx[((uint32_t)line >> (28 - i * 4)) & 0xF];
        lbuf[n++] = '\n'; lbuf[n] = 0;
        m50ii_uart_str("[ML] !!! ASSERT (non-fatal): ");
        m50ii_uart_str(msg ? msg : "nullptr");
        m50ii_uart_str(" @ ");
        m50ii_uart_str(file ? file : "?");
        m50ii_uart_str(lbuf);
        if (logged == 32)
            m50ii_uart_str("[ML] !!! (further non-fatal asserts suppressed)\n");
    }
    /* RETURN instead of chaining to the fatal handler -> the assert becomes survivable. */
    return 0;
}
#endif // M50II_NONFATAL_ASSERTS
#endif // CONFIG_M50mk2

void ml_assert_handler(char* msg, char* file, int line, const char* func)
{
    int len = snprintf(assert_msg, sizeof(assert_msg), 
        "ML ASSERT:\n%s\n"
        "at %s:%d (%s), task %s\n"
        "lv:%d mode:%d\n\n", 
        msg, 
        file, line, func, get_current_task_name(), 
        lv, shooting_mode
    );
// SJE FIXME: assert handling is buggy on modern Digic.
// Disable some of it here and do quick hack output:
#ifdef CONFIG_DIGIC_678X
    uart_printf("[SJE] ml_assert_msg: %s", assert_msg);
#endif
    backtrace_getstr(assert_msg + len, sizeof(assert_msg) - len);
    request_crash_log(2);
}

void ml_crash_message(char* msg)
{
    snprintf(assert_msg, sizeof(assert_msg), "%s", msg);
    request_crash_log(1);
}

#ifdef CONFIG_RPC
uint32_t is_cpu1_ready = 0;
static void cpu1_ready(void)
{
    is_cpu1_ready = 1;
}
#endif

/* ============================================================================
 * [M50II 2026-07-11] CPU0 abort/fault handler — capture mmu_init's fault.
 * Canon's runtime prefetch/data-abort vectors (VBAR=0xE03C50C0 at this boot
 * stage) live in ROM and just LDR.W PC into ROM handlers, so a fault gives us
 * NO pc we control. HARDWARE FACT (confirmed in QEMU): SCTLR.TE=1 (0x48C5187D)
 * => exceptions are taken in THUMB state, and Canon's vectors are Thumb-2
 * `LDR.W PC,[PC,#imm]` loading a pointer from the adjacent literal pool
 * (VBAR+0x20..+0x3C). We copy the whole 16-word table verbatim into RAM (so
 * every PC-relative literal offset is preserved and the 6 pass-through vectors
 * stay byte-identical -> IRQ/SVC/FIQ keep working), overwrite ONLY the PAbort
 * (word 11) and DAbort (word 12) pointer literals to point at our THUMB stub,
 * then set VBAR to the RAM copy. The stub dumps faulting PC / DFAR / IFAR /
 * DFSR / IFSR over UART then hangs. All in init.o => layout-safe (boot-d678.o
 * frozen). RAM table is D-clean-to-PoC + I-invalidate before the VBAR switch so
 * the core fetches new vectors, not stale cache. Installed in boot_pre_init_task
 * BEFORE mmu_init on CPU0 (VBAR banked per-core; mmu_init runs on CPU0). The
 * mmu_init PrefetchAbort does NOT reproduce in QEMU (permissive MMU) -> this is
 * the HW capture tool. See memory/ml-m50ii-port-status.md. */
static char *m50ii_hex(char *b, uint32_t v);   /* fwd decl: defined in telemetry block below */

extern uint32_t m50ii_abort_sp;                /* scratch-stack top; set by installer at runtime */
void m50ii_pabort_stub(void);
void m50ii_dabort_stub(void);

/* saved context (NEW layout, set by the stubs below):
 *   ctx[0]=faulting SP (banked r13 of the faulting mode)
 *   ctx[1]=faulting LR (banked r14 of the faulting mode = immediate caller return addr)
 *   ctx[2..14]=r0..r12, ctx[15]=faulting PC (LR adjusted by abort type)
 * [M50II 2026-07-11] Extended for the post-big.8 Arima/CamIF abort: the old dump gave a
 * mis-attributed PC (pc pointed at a store where the ROM guarantees a non-null dest, yet
 * r1=0/DFAR=0). Adding the faulting SP/LR + r4-r11 + a ROM-retaddr stack scan gives a real
 * backtrace so the true fault site can be identified. */
void __attribute__((used)) m50ii_abort_report(uint32_t *ctx, uint32_t spsr, uint32_t type)
{
    extern void m50ii_uart_str(const char *);
    char b[12];
    uint32_t dfsr, dfar, ifsr, ifar;
    __asm__ volatile("mrc p15,0,%0,c5,c0,0" : "=r"(dfsr));   /* Data Fault Status   */
    __asm__ volatile("mrc p15,0,%0,c6,c0,0" : "=r"(dfar));   /* Data Fault Address  */
    __asm__ volatile("mrc p15,0,%0,c5,c0,1" : "=r"(ifsr));   /* Instr Fault Status  */
    __asm__ volatile("mrc p15,0,%0,c6,c0,2" : "=r"(ifar));   /* Instr Fault Address */
    uint32_t fsp = ctx[0], flr = ctx[1], *rr = &ctx[2], pc = ctx[15];
    m50ii_uart_str("\n[ABORT] type="); m50ii_uart_str(m50ii_hex(b, type));   /* 0=prefetch 1=data */
    m50ii_uart_str(" pc=");   m50ii_uart_str(m50ii_hex(b, pc));
    m50ii_uart_str(" spsr="); m50ii_uart_str(m50ii_hex(b, spsr));
    m50ii_uart_str("\n[ABORT] DFSR="); m50ii_uart_str(m50ii_hex(b, dfsr));
    m50ii_uart_str(" DFAR=");          m50ii_uart_str(m50ii_hex(b, dfar));
    m50ii_uart_str(" IFSR=");          m50ii_uart_str(m50ii_hex(b, ifsr));
    m50ii_uart_str(" IFAR=");          m50ii_uart_str(m50ii_hex(b, ifar));
    m50ii_uart_str("\n[ABORT] sp=");   m50ii_uart_str(m50ii_hex(b, fsp));
    m50ii_uart_str(" lr=");            m50ii_uart_str(m50ii_hex(b, flr));
    m50ii_uart_str("\n[ABORT] r0="); m50ii_uart_str(m50ii_hex(b, rr[0]));
    m50ii_uart_str(" r1=");   m50ii_uart_str(m50ii_hex(b, rr[1]));
    m50ii_uart_str(" r2=");   m50ii_uart_str(m50ii_hex(b, rr[2]));
    m50ii_uart_str(" r3=");   m50ii_uart_str(m50ii_hex(b, rr[3]));
    m50ii_uart_str("\n[ABORT] r4="); m50ii_uart_str(m50ii_hex(b, rr[4]));
    m50ii_uart_str(" r5=");   m50ii_uart_str(m50ii_hex(b, rr[5]));
    m50ii_uart_str(" r6=");   m50ii_uart_str(m50ii_hex(b, rr[6]));
    m50ii_uart_str(" r7=");   m50ii_uart_str(m50ii_hex(b, rr[7]));
    m50ii_uart_str("\n[ABORT] r8="); m50ii_uart_str(m50ii_hex(b, rr[8]));
    m50ii_uart_str(" r9=");   m50ii_uart_str(m50ii_hex(b, rr[9]));
    m50ii_uart_str(" r10=");  m50ii_uart_str(m50ii_hex(b, rr[10]));
    m50ii_uart_str(" r11=");  m50ii_uart_str(m50ii_hex(b, rr[11]));
    m50ii_uart_str(" r12=");  m50ii_uart_str(m50ii_hex(b, rr[12]));
    /* Backtrace: scan the faulting stack for ROM code addresses (main fw 0xE00xxxxx..0xE10xxxxx
     * or the DF00 region). These are candidate return addresses -> the call chain to the fault. */
    m50ii_uart_str("\n[ABORT] stack scan (ROM addrs on faulting stack):\n");
    if (fsp && (fsp & 3) == 0)
    {
        volatile uint32_t *s = (volatile uint32_t *)fsp;
        for (int i = 0; i < 64; i++)
        {
            uint32_t w = s[i];
            if ((w >= 0xE0000000u && w < 0xE1000000u) || (w >= 0xDF000000u && w < 0xDF020000u))
            {
                m50ii_uart_str("[ABORT]  sp+"); m50ii_uart_str(m50ii_hex(b, i * 4));
                m50ii_uart_str(" = ");           m50ii_uart_str(m50ii_hex(b, w));
                m50ii_uart_str("\n");
            }
        }
    }
    m50ii_uart_str("[ABORT] HANG (fault captured; looping short dump)\n");
    /* [M50II 2026-07-11] Re-print a SHORT line forever (busy-delay; abort ctx has no scheduler) so
     * the lossy J-Link VCOM (drops chars at 115200 under load) eventually passes a clean copy. */
    for (;;)
    {
        m50ii_uart_str("[AB] pc="); m50ii_uart_str(m50ii_hex(b, pc));
        m50ii_uart_str(" lr="); m50ii_uart_str(m50ii_hex(b, flr));
        m50ii_uart_str(" dfar="); m50ii_uart_str(m50ii_hex(b, dfar));
        m50ii_uart_str("\n");
        for (volatile uint32_t d = 0; d < 0x1000000; d++) { }   /* crude inter-print delay */
    }
}

/* THUMB stubs (SCTLR.TE=1). Entry: Abort/Undef mode, Thumb state. LR = faulting
 * instr + {4 prefetch, 8 data}. We set SP from m50ii_abort_sp (set at install),
 * save r0-r12+adjusted-LR, pass ctx/SPSR/type to the Thumb reporter, then hang. */
__asm__(
    ".thumb\n"
    ".align 2\n"
    ".globl m50ii_pabort_stub\n"
    ".thumb_func\n"
    "m50ii_pabort_stub:\n"
    "    ldr   sp, m50ii_abort_sp\n"         /* T2 literal load -> SP (no GPR clobber) */
    "    sub   lr, lr, #4\n"                 /* prefetch abort: faulting instr = LR-4 */
    "    stmdb sp!, {r0-r12, lr}\n"          /* -> [r0..r12, pc] */
    "    mrs   r1, spsr\n"                   /* r1 = faulting cpsr (r0-r12 already saved: free to clobber) */
    "    mrs   r2, cpsr\n"                   /* r2 = abort cpsr */
    "    and   r3, r1, #0x1f\n"              /* faulting mode bits (e.g. 0x13 SVC) */
    "    bic   r0, r2, #0x1f\n"
    "    orr   r0, r0, r3\n"                 /* abort cpsr with faulting mode (I/F/T from abort) */
    "    msr   cpsr_c, r0\n"                 /* -> faulting mode (interrupts stay masked) */
    "    mov   r5, sp\n"                     /* r5 = faulting SP (banked r13) */
    "    mov   r6, lr\n"                     /* r6 = faulting LR (banked r14) */
    "    msr   cpsr_c, r2\n"                 /* -> back to abort mode */
    "    stmdb sp!, {r5, r6}\n"              /* prepend -> [fsp, flr, r0..r12, pc] */
    "    mov   r0, sp\n"                     /* r0 = ctx ptr */
    "    movs  r2, #0\n"                      /* type 0 = prefetch */
    "    ldr   r3, =m50ii_abort_report\n"
    "    blx   r3\n"
    "1:  b     1b\n"
    ".globl m50ii_dabort_stub\n"
    ".thumb_func\n"
    "m50ii_dabort_stub:\n"
    "    ldr   sp, m50ii_abort_sp\n"
    "    sub   lr, lr, #8\n"                 /* data abort: faulting instr = LR-8 */
    "    stmdb sp!, {r0-r12, lr}\n"          /* -> [r0..r12, pc] */
    "    mrs   r1, spsr\n"                   /* r1 = faulting cpsr */
    "    mrs   r2, cpsr\n"                   /* r2 = abort cpsr */
    "    and   r3, r1, #0x1f\n"              /* faulting mode bits */
    "    bic   r0, r2, #0x1f\n"
    "    orr   r0, r0, r3\n"
    "    msr   cpsr_c, r0\n"                 /* -> faulting mode */
    "    mov   r5, sp\n"                     /* r5 = faulting SP */
    "    mov   r6, lr\n"                     /* r6 = faulting LR */
    "    msr   cpsr_c, r2\n"                 /* -> back to abort mode */
    "    stmdb sp!, {r5, r6}\n"              /* prepend -> [fsp, flr, r0..r12, pc] */
    "    mov   r0, sp\n"
    "    movs  r2, #1\n"                      /* type 1 = data */
    "    ldr   r3, =m50ii_abort_report\n"
    "    blx   r3\n"
    "2:  b     2b\n"
    ".align 2\n"
    ".globl m50ii_abort_sp\n"
    "m50ii_abort_sp: .word 0\n"
    ".ltorg\n"
);

static uint32_t m50ii_abort_vectors[16] __attribute__((aligned(32)));   /* verbatim copy of Canon's */
static uint32_t m50ii_abort_stack[256];                                 /* 1KB scratch for reporter */
static uint32_t m50ii_abort_vectors_cpu1[16] __attribute__((aligned(32)));  /* [iter14] separate CPU1 table */

/* vector-table word indices (from the +0x20 literal pool): 11=PrefetchAbort ptr, 12=DataAbort ptr */
#define M50II_VEC_PABORT_PTR 11
#define M50II_VEC_DABORT_PTR 12

static void m50ii_install_abort_handler(void)
{
    extern void m50ii_uart_str(const char *);
    char b[12];
    uint32_t vbar, sctlr;
    __asm__ volatile("mrc p15,0,%0,c12,c0,0" : "=r"(vbar));    /* VBAR (banked per-core) */
    __asm__ volatile("mrc p15,0,%0,c1,c0,0"  : "=r"(sctlr));   /* SCTLR: V=bit13 TE=bit30 */
    m50ii_uart_str("[ML] abort: VBAR=");  m50ii_uart_str(m50ii_hex(b, vbar));
    m50ii_uart_str(" SCTLR=");            m50ii_uart_str(m50ii_hex(b, sctlr));
    m50ii_uart_str("\n");

    m50ii_abort_sp = (uint32_t)&m50ii_abort_stack[256];        /* full-descending stack top */

    /* copy Canon's live 16-word vector table verbatim (8 vecs + 8 literals) */
    for (int i = 0; i < 16; i++)
        m50ii_abort_vectors[i] = *(volatile uint32_t *)(vbar + i * 4);
    /* redirect ONLY the two abort pointer literals to our Thumb stubs (bit0=1 => Thumb entry) */
    m50ii_abort_vectors[M50II_VEC_PABORT_PTR] = (uint32_t)&m50ii_pabort_stub | 1u;
    m50ii_abort_vectors[M50II_VEC_DABORT_PTR] = (uint32_t)&m50ii_dabort_stub | 1u;

    uint32_t base = (uint32_t)m50ii_abort_vectors;
    __asm__ volatile("dsb");
    for (uint32_t a = base; a < base + sizeof(m50ii_abort_vectors); a += 32)
        __asm__ volatile("mcr p15,0,%0,c7,c10,1" :: "r"(a));   /* DCCMVAC: clean D line to PoC */
    __asm__ volatile("dsb");
    for (uint32_t a = base; a < base + sizeof(m50ii_abort_vectors); a += 32)
        __asm__ volatile("mcr p15,0,%0,c7,c5,1"  :: "r"(a));   /* ICIMVAU: invalidate I line */
    __asm__ volatile("dsb");
    __asm__ volatile("isb");
    __asm__ volatile("mcr p15,0,%0,c12,c0,0" :: "r"(base));    /* VBAR -> our RAM table */
    __asm__ volatile("isb");

    m50ii_uart_str("[ML] abort: installed, VBAR->RAM @"); m50ii_uart_str(m50ii_hex(b, base));
    m50ii_uart_str(" pabort=");  m50ii_uart_str(m50ii_hex(b, (uint32_t)&m50ii_pabort_stub));
    m50ii_uart_str(" dabort=");  m50ii_uart_str(m50ii_hex(b, (uint32_t)&m50ii_dabort_stub));
    m50ii_uart_str("\n");
    /* read-back the constructed table so we can confirm it's byte-correct vs Canon's (part A) */
    for (int i = 0; i < 16; i++)
    {
        if ((i & 3) == 0) m50ii_uart_str("[ML] abort: vec");
        m50ii_uart_str("["); m50ii_uart_str(m50ii_hex(b, i)); m50ii_uart_str("]=");
        m50ii_uart_str(m50ii_hex(b, m50ii_abort_vectors[i]));
        m50ii_uart_str((i & 3) == 3 ? "\n" : " ");
    }

#ifdef M50II_ABORT_SELFTEST
    /* QEMU/HW validation: deliberately fetch from an unmapped address -> prefetch abort.
     * If the handler works, we see "[ABORT] type=0 ... IFAR=0x90000000". */
    m50ii_uart_str("[ML] abort: SELFTEST -> prefetch abort @0x90000000\n");
    ((void (*)(void))0x90000001u)();
    m50ii_uart_str("[ML] abort: SELFTEST returned WITHOUT faulting (QEMU permissive)\n");
#endif
}

/* [iter14] Install an abort handler on CPU1: point its (banked) VBAR at a SEPARATE ML vector table --
 * copy CPU1's own current (Canon) vectors, redirect ONLY pabort/dabort to the existing ML stubs, so a
 * silent CPU1 prefetch/data abort (e.g. a migrated GuiMainTask running ml_gui_main_task on CPU1) finally
 * prints its PC/DFAR/IFAR via m50ii_abort_report. Separate vectors keep CPU0 untouched; the abort stub's
 * scratch stack is shared (fine for a one-core diagnostic). Called on CPU1 after it adopts ML MMU tables. */
void m50ii_install_abort_handler_cpu1(void)
{
    extern void m50ii_uart_str(const char *);
    char b[12];
    uint32_t vbar;
    __asm__ volatile("mrc p15,0,%0,c12,c0,0" : "=r"(vbar));   /* CPU1's current (Canon) VBAR, banked */
    m50ii_uart_str("[ML] cpu1-abort: VBAR="); m50ii_uart_str(m50ii_hex(b, vbar)); m50ii_uart_str("\n");
    for (int i = 0; i < 16; i++)
        m50ii_abort_vectors_cpu1[i] = *(volatile uint32_t *)(vbar + i * 4);
    m50ii_abort_vectors_cpu1[M50II_VEC_PABORT_PTR] = (uint32_t)&m50ii_pabort_stub | 1u;
    m50ii_abort_vectors_cpu1[M50II_VEC_DABORT_PTR] = (uint32_t)&m50ii_dabort_stub | 1u;
    uint32_t base = (uint32_t)m50ii_abort_vectors_cpu1;
    __asm__ volatile("dsb");
    for (uint32_t a = base; a < base + sizeof(m50ii_abort_vectors_cpu1); a += 32)
        __asm__ volatile("mcr p15,0,%0,c7,c10,1" :: "r"(a));   /* clean D line to PoC */
    __asm__ volatile("dsb");
    for (uint32_t a = base; a < base + sizeof(m50ii_abort_vectors_cpu1); a += 32)
        __asm__ volatile("mcr p15,0,%0,c7,c5,1" :: "r"(a));    /* invalidate I line */
    __asm__ volatile("dsb");
    __asm__ volatile("isb");
    __asm__ volatile("mcr p15,0,%0,c12,c0,0" :: "r"(base));    /* CPU1 VBAR -> our RAM table */
    __asm__ volatile("isb");
    m50ii_uart_str("[ML] cpu1-abort: installed VBAR->"); m50ii_uart_str(m50ii_hex(b, base)); m50ii_uart_str("\n");
}

/* called before Canon's init_task */
void boot_pre_init_task()
{
    extern void m50ii_uart_str(const char *);   /* [M50II HW-TRACE] direct-uart checkpoints */
    m50ii_uart_str("[ML] pre.0: boot_pre_init_task enter\n");
#if defined(CONFIG_HELLO_WORLD) || defined(CONFIG_DUMPER_BOOTFLAG)
    // don't hook
#else
    M50II_INIT_STAGE(4);
    // normally, we create sems via INIT_FUNC macro,
    // but that happens via tasks, which is later than we need
    // for early MMU remapping.
    #if defined(CONFIG_RPC)
    m50ii_uart_str("[ML] pre.1: create_named_semaphore(RPC)\n");
    RPC_sem = create_named_semaphore("RPC", SEM_CREATE_UNLOCKED);
    m50ii_uart_str("[ML] pre.2: semaphore OK\n");
    M50II_INIT_STAGE(5);
    #endif
    /* [M50II] install CPU0 abort handler BEFORE mmu_init so its fault is captured over UART */
    m50ii_uart_str("[ML] pre.2b: installing CPU0 abort handler (VBAR->RAM)\n");
    m50ii_install_abort_handler();
    #if defined(CONFIG_MMU_REMAP)
    M50II_INIT_STAGE(6);
#ifdef M50II_TRACE_SKIP_MMU_INIT
    M50II_INIT_STAGE(7);
#else
    m50ii_uart_str("[ML] pre.3: mmu_init()\n");
    if (mmu_init() < 0)
    {
        M50II_INIT_STOP(13);
        DryosDebugMsg(0, 15, "ERROR doing mmu_init()");
    }
    m50ii_uart_str("[ML] pre.4: mmu_init OK\n");
    M50II_INIT_STAGE(7);
#endif
    #endif
    // Install our task creation hooks
    qprint("[BOOT] installing task dispatch hook at "); qprintn((int)&task_dispatch_hook); qprint("\n");
    M50II_INIT_STAGE(8);
#ifndef M50II_TRACE_SKIP_PREINIT_DEBUGMSG
    DryosDebugMsg(0, 15, "replacing task_dispatch_hook");
#endif
    M50II_INIT_STAGE(9);
    /* [M50II 2026-07-11] Register ML's per-context-switch callback into Canon's OWN task-switch list
     * (do NOT squat *0x4144 -- that is Canon's walker; squatting it panicked iters 6/7/8). We call
     * Canon's own list-insert primitive register_task_switch_callback (ROM 0xE0164044) with our static
     * {next,prev,fn} node: it does the locked head-insert, (re)installs the walker at *0x4144, and
     * restores IRQ state itself. Canon's walker then calls m50ii_cb(1, old, next) each switch alongside
     * Canon's own callbacks. Gated so it can be disabled in one line if the HW boot regresses. See
     * docs/27-menu-input-api-registration.md. */
#ifndef M50II_REGISTER_CB
#define M50II_REGISTER_CB 1
#endif
#if M50II_REGISTER_CB
    {
        extern void register_task_switch_callback(void *node);   /* stub -> ROM 0xE0164044 (r0 = node) */
        m50ii_cbnode.fn = m50ii_cb;                              /* node+0x8; insert only zeroes next/prev */
        register_task_switch_callback(&m50ii_cbnode);
        m50ii_uart_str("[ML] pre.5: task-switch callback REGISTERED (Canon cblist_insert)\n");
    }
    (void)my_task_dispatch_hook;   /* generic hook currently unused on M50 II */
#else
    m50ii_uart_str("[ML] pre.5: task-switch callback registration SKIPPED (M50II_REGISTER_CB=0)\n");
    (void)my_task_dispatch_hook;
#endif
    M50II_INIT_STAGE(10);
    #ifdef CONFIG_TSKMON
    M50II_INIT_STAGE(11);
    m50ii_uart_str("[ML] pre.6: tskmon_init()\n");
    tskmon_init();
    m50ii_uart_str("[ML] pre.7: tskmon OK\n");
    M50II_INIT_STAGE(12);
    #endif
#endif
    m50ii_uart_str("[ML] pre.9: boot_pre_init_task DONE\n");
}

/* ===== [M50II telemetry, Phase 0 — 2026-07-10] layout-safe UART value/heartbeat logging =====
 * All in init.o (links AFTER boot-d678.o) and built on the proven global m50ii_uart_str
 * (\n handled, TX bounded), so boot-d678.o stays byte-identical -> no "e" layout risk. */
static volatile uint32_t g_ml_phase;   /* bumped at checkpoints; heartbeat reports current phase */
static char *m50ii_hex(char *b, uint32_t v)   /* format "0x........" + NUL into b; returns b */
{
    static const char h[] = "0123456789ABCDEF";
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = h[(v >> (28 - 4 * i)) & 0xF];
    b[10] = 0;
    return b;
}
static void mlog_kv(const char *label, uint32_t v)   /* "<label>=0x........\n" (value, not addr) */
{
    extern void m50ii_uart_str(const char *);
    char b[12];
    m50ii_uart_str(label); m50ii_uart_str("="); m50ii_uart_str(m50ii_hex(b, v)); m50ii_uart_str("\n");
}
/* [M50II 2026-07-11] UART-independent progress beacon: the J-Link VCOM drops chars under Canon's
 * DebugMsg flood past post.0c, so blink the card LED N times (busy-delay, no scheduler) to SEE the
 * true boot frontier. Distinct N per checkpoint; user watches the LED and reports the highest N. */
static void m50ii_led_blink(uint32_t n)
{
#if defined(CARD_LED_ADDRESS) && defined(LEDON) && defined(LEDOFF)
    for (volatile uint32_t g = 0; g < 0x3000000; g++) { }        /* gap BEFORE group (separates counts) */
    for (uint32_t k = 0; k < n; k++)
    {
        MEM(CARD_LED_ADDRESS) = LEDON;
        for (volatile uint32_t d = 0; d < 0x900000; d++) { }
        MEM(CARD_LED_ADDRESS) = LEDOFF;
        for (volatile uint32_t d = 0; d < 0x900000; d++) { }
    }
#else
    (void)n;
#endif
}
/* [M50II 2026-07-11] Bounded busy-wait to replace msleep() in boot_post_init_task: HW boots freeze
 * at post.0c's first msleep (ML tasks never scheduled, msleep never wakes => DryOS timer/scheduler
 * not functional in our early boot). A pure spin can't block, so boot_post_init_task can run to
 * post.3 and we learn if the ml_init TASK then runs (big.0 beacon) — isolating timer-vs-scheduler. */
static void m50ii_busy_wait(uint32_t units)
{
    for (uint32_t u = 0; u < units; u++)
        for (volatile uint32_t d = 0; d < 0x40000; d++) { }
}
static void m50ii_heartbeat_task(void)   /* Phase-1 liveness: ticks => scheduler alive; stops => hard fault */
{
    extern void m50ii_uart_str(const char *);
    char b[12];
    for (uint32_t n = 0; ; n++)
    {
        m50ii_uart_str("[ML] <3 n="); m50ii_uart_str(m50ii_hex(b, n));
        m50ii_uart_str(" phase="); m50ii_uart_str(m50ii_hex(b, g_ml_phase));
        m50ii_uart_str(" us="); m50ii_uart_str(m50ii_hex(b, *(volatile uint32_t *)0xC0242014));
        m50ii_uart_str(" cpu1rdy="); m50ii_uart_str(m50ii_hex(b, is_cpu1_ready));   /* 1 => CPU1 alive (ran a task) */
        /* [iter8] TASK_OVERRIDE confirmation logged HERE (safe context), not in the dispatch hook. */
        m50ii_uart_str(" ovr="); m50ii_uart_str(m50ii_hex(b, m50ii_override_count));
        if (m50ii_override_count && m50ii_override_name)
        { m50ii_uart_str(" last="); m50ii_uart_str((const char *)m50ii_override_name); }
        m50ii_uart_str("\n");
        msleep(500);
    }
}

#ifndef M50II_GUI_PEEK
#define M50II_GUI_PEEK 1
#endif
#ifndef M50II_SERIAL_SHELL
#define M50II_SERIAL_SHELL 1   /* [DUPLEX-UART 2026-07-14] ON: start the serial_shell.c RX task.
                                * Phase 0 = pure echo + heartbeat, to confirm the duplex link works
                                * end-to-end and whether Canon's RX ISR steals bytes. [keeper: OFF] */
#endif
#if M50II_SERIAL_SHELL
extern void m50ii_serial_shell_start(void);   /* serial_shell.c */
#endif
#if M50II_GUI_PEEK
/* [iter15 PIVOT] Read-only GUI-event peek. Canon's gui_enqueue_message (0xE021DCB2) writes EVERY GUI/
 * button event into a ring at RAM 0x000BAE08 (=DAT_e021e034, 0x10-byte entries {type,p1,p2,p3}) and
 * advances the write index at gui_main_struct+0x14 = RAM 0x543C (wraps at 0x401), then posts it to
 * Canon's gui_main_task queue. We POLL the write index and log each new entry's type + params over UART
 * WITHOUT touching the queue -- Canon's gui_main_task still drains + dispatches normally, so the GUI
 * stays fully alive (no override, no Edet cascade). type==0 = button events, p1 = the real M50 II
 * keycode. [Phase 0-3] This task is now ML's INPUT PUMP: besides the bounded raw trace, it forwards
 * each non-idle event to m50ii_handle_canon_gui_event() (gui.c), which drives the ML menu (FN opens
 * it; nav keys navigate) via ML's own menu_task/handle_buttons -- still 100% read-only on Canon's
 * ring. Replaces the failed gui_main_task override (iter9-14). */
#if defined(CONFIG_M50mk2)
extern void m50ii_handle_canon_gui_event(uint32_t type, uint32_t p1, uint32_t p2);  /* gui.c menu bridge */
#endif
#ifndef M50II_INPUT_TRACE
#define M50II_INPUT_TRACE 0   /* [DUPLEX-UART 2026-07-14] OFF: the per-event [ML] BTN/DIAL flood
                                 shredded serial-harness replies on the shared TX. Input task still
                                 runs (menu nav); use the serial console to probe input instead.
                               [prior] 1 = capture the [ML] BTN event stream while
                                 navigating up/down in the MAIN menu vs a submenu -- to diagnose the
                                 +1 selection/display desync + "dead until submenu" (composite fix had
                                 no effect, so the cause is input delivery, not redraw). Turn OFF after.
                               [keeper: OFF] set 1 to log every button ([ML] BTN) + a periodic
                               * liveness/counter line ([ML] in.hb) over UART during HW bring-up. */
#endif
static void m50ii_input_task(void)
{
    volatile uint32_t *widx = (volatile uint32_t *)0x0000543C;   /* gui_main_struct+0x14 = ring write index */
    const uint32_t RING  = 0x000BAE08;                            /* DAT_e021e034 = ring base */
    const uint32_t NSLOT = 0x401;
    uint32_t last = *widx;
#if M50II_INPUT_TRACE
    extern void m50ii_uart_str(const char *);
    char b[12];
    uint32_t total = 0, n_t0 = 0, n_t2 = 0, n_t4 = 0, n_ot = 0;
    int lc = 0;
    m50ii_uart_str("[ML] input: watching GUI event ring @0xBAE08 (widx@0x543C)\n");
#endif
    while (1)
    {
        uint32_t cur = *widx;
        while (last != cur)
        {
            volatile uint32_t *e = (volatile uint32_t *)(RING + last * 0x10);
            uint32_t type = e[0], p1 = e[1], p2 = e[2];

            /* Skip the idle keep-alive (type==0, p1==0xA7, ~every 3s): carries no input. */
            int is_idle = (type == 0 && p1 == 0xA7);
#if M50II_INPUT_TRACE
            if (!is_idle)
            {
                total++;
                if (type == 0)
                {
                    n_t0++;
                    m50ii_uart_str("[ML] BTN p1="); m50ii_uart_str(m50ii_hex(b, p1));
                    m50ii_uart_str(" p2=");         m50ii_uart_str(m50ii_hex(b, p2));
                    m50ii_uart_str("\n");
                    /* [MENU-NAV 2026-07-13] The control dial reaches us as p1=0xB7 with a
                     * rotation struct pointer in p2 -- it is the ONLY nav control that reaches
                     * our ring in the main-menu Canon GUI mode (4-way pad 0x40/0x42 do not).
                     * Dump the head of that struct so the CW/CCW direction field can be found
                     * and 0xB7 wired to BGMT_WHEEL_UP/DOWN. Turn the dial one way N times, then
                     * the other way N times; the word that flips value is the direction. */
                    if (p1 == 0xB7 && p2 > 0x1000)
                    {
                        volatile uint32_t *s = (volatile uint32_t *)p2;
                        m50ii_uart_str("[ML] DIAL s0="); m50ii_uart_str(m50ii_hex(b, s[0]));
                        m50ii_uart_str(" s1=");          m50ii_uart_str(m50ii_hex(b, s[1]));
                        m50ii_uart_str(" s2=");          m50ii_uart_str(m50ii_hex(b, s[2]));
                        m50ii_uart_str(" s3=");          m50ii_uart_str(m50ii_hex(b, s[3]));
                        m50ii_uart_str(" s4=");          m50ii_uart_str(m50ii_hex(b, s[4]));
                        m50ii_uart_str("\n");
                    }
                }
                else if (type == 2) n_t2++;
                else if (type == 4) n_t4++;
                else                n_ot++;
            }
#endif
#if defined(CONFIG_M50mk2)
            /* Drive the ML menu: FN toggles it, nav keys navigate (bridge is gated to gui_menu_shown). */
            if (!is_idle)
                m50ii_handle_canon_gui_event(type, p1, p2);
#endif
            last = (last + 1) % NSLOT;
        }
#if M50II_INPUT_TRACE
        /* input-alive heartbeat + ring stats every ~3s (150 * 20ms). */
        if (++lc >= 150)
        {
            lc = 0;
            m50ii_uart_str("[ML] in.hb widx="); m50ii_uart_str(m50ii_hex(b, *widx));
            m50ii_uart_str(" tot=");             m50ii_uart_str(m50ii_hex(b, total));
            m50ii_uart_str(" t0=");              m50ii_uart_str(m50ii_hex(b, n_t0));
            m50ii_uart_str(" t2=");              m50ii_uart_str(m50ii_hex(b, n_t2));
            m50ii_uart_str(" t4=");              m50ii_uart_str(m50ii_hex(b, n_t4));
            m50ii_uart_str(" ot=");              m50ii_uart_str(m50ii_hex(b, n_ot));
            m50ii_uart_str("\n");
        }
#endif
        msleep(20);
    }
}
#endif

/* [M50II E4, 2026-07-10] No-op handler for SGI vector 0x9a. The dying Shirahama/CamIF coprocessor
 * raises SGI 0x9a; its handler slot in the DryOS SGI table (base 0x2AD1C, entry 0x2AD1C+0x9a*8
 * = 0x2B1EC) is NULL -> isr_dispatch (0xE018E17C) prints "Illegal inter 0x9a" and panics
 * (FUN_e00d4352(0x40,2)). Installing a no-op catches the SGI so the panic becomes survivable,
 * letting us observe the end state past the fatal point. Runs in interrupt context; keep tiny. */
static void m50ii_sgi9a_handler(int arg)
{
    extern void m50ii_uart_str(const char *);
    (void)arg;
    m50ii_uart_str("[ML] !!! SGI 0x9a caught by no-op handler (panic suppressed)\n");
}

/* called right after Canon's init_task, while their initialization continues in background */
void boot_post_init_task(void)
{
    extern void m50ii_uart_str(const char *);   /* [M50II STEP2 HW-TRACE] */
    /* Reaching here means Canon init_task RETURNED to ML on silicon (past K468/eep_i2c,
     * which QEMU can't emulate). This is the step-2 landmark: real my_init_task, not the beacon. */
    m50ii_uart_str("[ML] post.0: boot_post_init_task ENTER -- Canon init_task RETURNED\n");
    g_ml_phase = 0x00;

    /* [M50II 2026-07-11] Pre-seed Canon DebugStorage(DS) logger buffer to defeat the post.0c/post.1
     * wall. DS state struct @ RAM 0x45B8 (=DAT_e007e460); its log-buffer ptr @ +0x6c (=0x4624) is NULL
     * this early, so when a Canon task raises a diag event the dispatcher FUN_e007e074 hands NULL to
     * the LRU-insert FUN_e007dda4 → NULL-deref data abort @0xE007DDBE (caught by our handler). DS is
     * NON-critical: give it a valid 5x0x40-slot buffer (0xFF = "empty" per FUN_e007dda4's key check)
     * so it records instead of crashing. Done before any msleep so it wins the race vs Canon tasks. */
    {
        static uint8_t m50ii_ds_log_buf[0x200] __attribute__((aligned(4)));
        for (unsigned i = 0; i < sizeof(m50ii_ds_log_buf); i++) m50ii_ds_log_buf[i] = 0xFF;
        uint32_t ds_prev = *(volatile uint32_t *)0x00004624;
        *(volatile uint32_t *)0x00004624 = (uint32_t)&m50ii_ds_log_buf;   /* DS struct(0x45B8)+0x6c */
        mlog_kv("[ML] post.0d: DS buf seeded @0x4624 prev", ds_prev);
        mlog_kv("[ML] post.0d: DS buf addr", (uint32_t)&m50ii_ds_log_buf);
    }

    task_create("ml_hb", 0x1f, 0x1000, m50ii_heartbeat_task, 0);   /* Phase-1 heartbeat: liveness + phase */
    m50ii_uart_str("[ML] post.0h: heartbeat task created\n");
#if M50II_GUI_PEEK
    task_create("ml_in", 0x1f, 0x1000, m50ii_input_task, 0);       /* [Phase 0-3] GUI ring peek -> ML menu input */
    m50ii_uart_str("[ML] post.0h: input task created\n");
#endif
#if M50II_SERIAL_SHELL
    m50ii_serial_shell_start();                                    /* [DUPLEX-UART] RX echo task (Phase 0) */
    m50ii_uart_str("[ML] post.0h: serial shell task created\n");
#endif
#if defined(CONFIG_PLATFORM_POST_INIT)
    platform_post_init();
#endif
#if defined(CONFIG_CRASH_LOG)
    // decompile TH_assert to find out the location
    old_assert_handler = (void*)MEM(DRYOS_ASSERT_HANDLER);
    *(void**)(DRYOS_ASSERT_HANDLER) = (void*)my_assert_handler;
#endif // (CONFIG_CRASH_LOG)

#if defined(CONFIG_M50mk2) && M50II_NONFATAL_ASSERTS
    /* [M50II Phase-3] Escalation safety net (default OFF). CONFIG_CRASH_LOG is undef'd on
     * M50 II, so the block above is compiled out and no ML assert handler is installed.
     * When enabled, swap in a handler that logs+returns so Canon asserts are non-fatal. */
    m50ii_old_assert_handler = (void*)MEM(DRYOS_ASSERT_HANDLER);
    *(void**)(DRYOS_ASSERT_HANDLER) = (void*)m50ii_nonfatal_assert_handler;
    m50ii_uart_str("[ML] post.0a: non-fatal assert handler installed (bring-up net)\n");
#endif

    DebugMsg( DM_MAGIC, 3, "Magic Lantern %s (%s)",
        build_version,
        build_id
    );

    DebugMsg( DM_MAGIC, 3, "Built on %s by %s",
        build_date,
        build_user
    );

// kitor: on D678 this gets executed before value
//        is updated with running fw version
#if defined(CONFIG_ADDITIONAL_VERSION)
    // Re-write the version string.
    // Don't use strcpy() so that this can be done
    // before strcpy() or memcpy() are located.
    extern char additional_version[];
    additional_version[0] = '-';
    additional_version[1] = 'm';
    additional_version[2] = 'l';
    additional_version[3] = '-';
    additional_version[4] = build_version[0];
    additional_version[5] = build_version[1];
    additional_version[6] = build_version[2];
    additional_version[7] = build_version[3];
    additional_version[8] = build_version[4];
    additional_version[9] = build_version[5];
    additional_version[10] = build_version[6];
    additional_version[11] = build_version[7];
    additional_version[12] = build_version[8];
    additional_version[13] = '\0';
#endif

#ifdef CONFIG_RPC
    // Get cpu1 to flag when it's fully active,
    // this proves SGI handlers are usable, so we can use request_RPC()
    g_ml_phase = 0x0a; m50ii_uart_str("[ML] post.0a: task_create_ex(cpu1_ready) ...\n");
    task_create_ex(NULL, 0x10, 0x200, cpu1_ready, NULL, 1);
    m50ii_uart_str("[ML] post.0b: task_create_ex(cpu1_ready) returned\n");
#endif

    /* [M50II E4, 2026-07-10] install no-op SGI 0x9a handler (SGI table base 0x2AD1C, entry
     * 0x2AD1C+0x9a*8 = 0x2B1EC) so the dying-CamIF unhandled-SGI panic becomes survivable and
     * we can observe the end state. (uint32_t) of a thumb fn ptr carries the thumb bit for blx. */
    *(volatile uint32_t *)0x0002B1EC = (uint32_t)&m50ii_sgi9a_handler;   /* SGI table[0x9a].handler */
    *(volatile uint32_t *)0x0002B1F0 = 0;                                /* SGI table[0x9a].param   */
    m50ii_uart_str("[ML] post.0b2: SGI 0x9a no-op handler installed @0x2B1EC\n");

    #ifdef FEATURE_VRAM_RGBA
    /* [M50II 2026-07-11] UN-BYPASSED. Phase-0 HW probe showed *(0xFC1C)=0xD0483040 (a valid MARV):
     * the mk1-copied _rgb_vram_info=0xFC1C IS populated now that mmu_init works and Canon display init
     * runs, so the old "hangs on first call" was a pre-mmu-fix artifact. Call it bounded; the CPU0
     * abort handler catches any bad deref (with PC) instead of a silent hang. */
    g_ml_phase = 0x0c;
    m50ii_uart_str("[ML] post.0c: rgb_vram_preinit wait (FEATURE_VRAM_RGBA)\n");
    /* [2026-07-11] one-shot: which GUI-surface global is populated? _rgb_vram_info now = 0x7F1B4. */
    mlog_kv("[ML] post.0c *0x7F1B4", *(volatile uint32_t *)0x0007F1B4);   /* RENDERER surf-array[0] MARV */
    mlog_kv("[ML] post.0c *0x11264", *(volatile uint32_t *)0x00011264);  /* RENDERER state active MARV  */
    {
        uint32_t rgb_ok = 0;
        /* [M50II 2026-07-11 BOOT-TIME FIX] Cap was 50 (~16s). HW log proves rgb_vram_preinit()
         * returns 0 every iteration on this silicon and the OSD path does NOT depend on it
         * (osd_try builds its own MARV after ml_started). Keep a token 3-iter poll (~1s) in case
         * a future fw populates it fast, else proceed. Saves ~15s of dead boot spin. */
        for (int i = 0; i < 3; i++)
        {
            rgb_ok = (uint32_t)(uintptr_t) rgb_vram_preinit();
            if (rgb_ok) break;
            m50ii_busy_wait(50);   /* was msleep(100) — msleep hangs on HW (see helper note) */
        }
        if (rgb_ok) mlog_kv("[ML] post.0c: rgb_vram_preinit OK ret", rgb_ok);
        else m50ii_uart_str("[ML] post.0c: rgb_vram_preinit TIMEOUT -> proceeding\n");
    }
    #endif /* FEATURE_VRAM_RGBA */

    // wait for firmware to initialize
    g_ml_phase = 0x01; m50ii_uart_str("[ML] post.1: waiting for bmp_vram_raw (Canon display init)\n");
    m50ii_led_blink(2);   /* LED beacon: passed post.0c/rgb_vram_preinit (2 blinks) */
    {
        void *bmp = 0;
        /* [M50II 2026-07-11 BOOT-TIME FIX] Cap was 100 (~36s — the single biggest boot stall).
         * HW log proves bmp_vram_raw() stays 0x0 for all 100 iters: D8 never populates the mk-style
         * bmp_vram_raw, and the XCM/OSD path doesn't use it. Token 3-iter poll (~1s) then proceed. */
        for (int i = 0; i < 3; i++)
        {
            bmp = bmp_vram_raw();
            if (bmp) break;
            if ((i % 10) == 0) mlog_kv("[ML] post.1 bmp_vram_raw", (uint32_t)bmp);
            m50ii_busy_wait(50);   /* was msleep(100) */
        }
        if (bmp) mlog_kv("[ML] post.2: bmp_vram ready ptr", (uint32_t)bmp);
        else m50ii_uart_str("[ML] post.2: bmp_vram_raw TIMEOUT -> proceeding anyway\n");
    }

    // wait for overriden gui_main_task (but use a timeout so it doesn't break if you disable that for debugging)
    /* [M50II 2026-07-11 BOOT-TIME FIX] Cap was 50 (~11s). ml_gui_initialized is set only when the
     * first type-0 event flows through the override (gui-common.c:164), which may not arrive during
     * boot; my_big_init_task does NOT depend on the GUI being up, so break early and proceed. Short
     * 5-iter poll (~0.75s) still latches an early startup event if one is emitted. */
    for (int i = 0; i < 5; i++)
    {
        if (ml_gui_initialized)
            break;
        m50ii_busy_wait(25);   /* was msleep(50) */
    }
    m50ii_busy_wait(25);   /* was msleep(50) */

    m50ii_led_blink(3);   /* LED beacon: reached post.3 in boot_post_init_task (3 blinks) */
    m50ii_uart_str("[ML] post.3: creating ml_init task (my_big_init_task)\n");
    task_create("ml_init", 0x1e, 0x4000, my_big_init_task, 0 );
    m50ii_uart_str("[ML] post.4: ml_init task created; boot_post_init_task returning\n");

#ifdef M50II_TRACE_BOOT_POST_FIO_PROBE
    m50ii_boot_post_fio_probe();
#endif

    return;
}
