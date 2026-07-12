/** \file
 * Startup code for DIGIC 7 & 8
 */

#include "dryos.h"
#include "boot.h"
#include "boot-d678.h"

// [M50II 1.0.1 HW-TEST — docs/23] Enable the create-ladder LED trace for the on-camera
// wrap-and-swap test. Blinks the card LED through my_create_init_task stages 1..10, then
// 15-forever if create_init_task returns. Works together with the .data-copy fix already
// in cstart(). NOTE: do NOT also define M50II_TRACE_LADDER — it halts early (blink-15) at
// boot-d678.c:909, before the wrap-and-swap. REMOVE this line for a clean/production build.
#define M50II_TRACE_CREATE_LADDER

// [M50II STEP 2, 2026-07-10] SWITCHED OFF: hand straight to the REAL full ML my_init_task
// (boot.h) instead of the my_minimal_init_task beacon. The beacon proved the boot chain to
// K468 READY / deep success (and the task_dispatch_hook fix); now run actual ML init:
// my_init_task -> boot_pre_init_task -> Canon init_task -> boot_post_init_task -> my_big_init_task.
// qprintf traces are QEMU-only; boot_pre/post_init_task m50ii_uart_str checkpoints show on HW.
// Re-enable this line to fall back to the beacon.
// #define M50II_MINIMAL_INIT_TASK

#ifdef M50II_TRACE_KEEP_SOURCE_ORDER
#pragma GCC optimize ("no-toplevel-reorder")
#endif

#if !defined(CONFIG_DIGIC_678X)
    #error "Expected D678"
#endif

/** These are called when new tasks are created */
static int my_init_task(int a, int b, int c, int d);

// This reserves space for some early Canon code to be copied into,
// edited by us, then executed.  The build locates this in bss of
// our binary, so the location is known.
//
// See reboot.c for the prior part in the process.
//
// Note that some instructions in the copied region may load
// constants or call code located nearby.  If so, you should
// ensure the LEN constants mean these are also copied.
//
// Some cams have the cstart region far from the firmware_entry region.
// If firmware_entry and cstart are close, make FIRMWARE_ENTRY_LEN
// enough to cover both.
//
// If they're far apart, define CSTART_LEN, as well as FIRMWARE_ENTRY_LEN,
// each for their own region.  The starts are already defined in stubs.S.
// If in doubt about sizes, it's safe to make the regions larger than needed,
// you lose some space for ML / DryOS.
//
// We then copy both regions so they are adjacent in our buffer,
// compacting them and correcting the relevant calls
// so the middle can be skipped.
//
// Values must be 4 aligned.
#ifdef CSTART_LEN
    static uint32_t _reloc[(FIRMWARE_ENTRY_LEN + CSTART_LEN)/ 4];
#else
    static uint32_t _reloc[FIRMWARE_ENTRY_LEN / 4];
#endif
#define RELOCADDR ((uintptr_t)_reloc)

static uint32_t reloc_addr(uint32_t addr)
{
    // converts an address from "normal" cam range
    // to its address within our copied code in reloc buffer
#ifdef CSTART_LEN
    if (addr >= ((uint32_t)cstart & 0xfffffffe))
    {
        return RELOCADDR + (addr - ((uint32_t)cstart & 0xfffffffe)) + FIRMWARE_ENTRY_LEN;
    }
#endif
    return RELOCADDR + addr - MAIN_FIRMWARE_ADDR;
}

static void patch_thumb_branch(uint32_t pc, uint32_t dest)
{
    // Converts the pc address to within the reloc buffer range,
    // and patches the reloc copy to branch to dest.
    //
    // pc should be an address in "normal" cam range for code,
    // dest should be an address within ML address range.
    //
    // pc should be part of the rom region that has been copied
    // to reloc buffer.  The instruction at pc should be b.w, bx or blx.
    //
    // pc should be a Thumb instruction.
    //
    // Type of branch to patch in is detected from dest,
    // Thumb targets should be specified with LSb set.

    qprint("[BOOT] orig pc: "); qprintn(pc); qprint("\n");
    pc = reloc_addr(pc);
    qprint("[BOOT] fixing up branch at "); qprintn(pc);
    qprint(" (ROM: "); qprintn(pc); qprint(") to "); qprintn(dest); qprint("\n");

    // See ARMv7-A / ARMv7-R Ref Manual for encodings

    if (dest % 2) // Thumb target, use BL, firmware is Thumb
    {
        uint32_t opcode = 0xd000f000; // BL fixed bits
        uint32_t offset = dest - (pc + 4);
        uint32_t s = (offset >> 24) & 1;
        uint32_t i1 = (offset >> 23) & 1;
        uint32_t i2 = (offset >> 22) & 1;
        uint32_t imm10 = (offset >> 12) & 0x3ff;
        uint32_t imm11 = (offset >> 1) & 0x7ff;
        uint32_t j1 = (!(i1 ^ s)) & 0x1;
        uint32_t j2 = (!(i2 ^ s)) & 0x1;

        *(uint32_t *)pc = opcode | (s << 10) | imm10 | (j1 << 29) | (j2 << 27) | (imm11 << 16);
    }
    else // ARM target, use BLX
    {
        uint32_t opcode = 0xc000f000; // BLX fixed bits
        uint32_t offset = dest - (pc + 4);
        uint32_t s = (offset >> 24) & 1;
        uint32_t i1 = (offset >> 23) & 1;
        uint32_t i2 = (offset >> 22) & 1;
        uint32_t imm10H = (offset >> 12) & 0x3ff;
        uint32_t imm10L = (offset >> 2) & 0x3ff;
        uint32_t j1 = (!(i1 ^ s)) & 0x1;
        uint32_t j2 = (!(i2 ^ s)) & 0x1;

        *(uint32_t *)pc = opcode | (s << 10) | imm10H | (j1 << 29) | (j2 << 27) | (imm10L << 17);
    }
}

#if defined(M50II_TRACE_STOP_BEFORE_RELOC) || defined(M50II_TRACE_STOP_AT_BZERO) || defined(M50II_TRACE_STOP_AT_CREATE_ITASK) || defined(M50II_TRACE_LADDER) || defined(M50II_TRACE_CREATE_LADDER) || defined(M50II_TRACE_CREATE_PASSTHRU) || defined(M50II_TRACE_CREATE_TAIL_PASSTHRU) || defined(M50II_TRACE_CREATE_ROM_LR_TAIL) || defined(M50II_TRACE_SKIP_CREATE_PATCH) || defined(M50II_TRACE_CREATE_INTERNALS) || defined(M50II_TRACE_CREATE_FIRST_INTERNALS) || defined(M50II_TRACE_CREATE_REG_HELPER) || defined(M50II_TRACE_CREATE_REG_FORCE) || defined(M50II_TRACE_CREATE_DEEP_HANDOFF) || defined(M50II_TRACE_CREATE_DEEP_HANDOFF_QUIET) || defined(M50II_TRACE_SCHED_HANDOFF_MIRROR) || defined(M50II_TRACE_SCHED_HANDOFF_STEP_SCAN) || defined(M50II_TRACE_SCHED_CONTEXT_SCAN) || defined(M50II_TRACE_RESTORE_PROBE) || defined(M50II_TRACE_BOOTSTRAP_WRAPPER) || defined(M50II_TRACE_BOOTSTRAP_INLINE) || defined(M50II_TRACE_INIT_HELPER_DIRECT_RESTORE) || defined(M50II_TRACE_LOCK_ORIGIN) || defined(M50II_TRACE_LOCK_INITIAL) || defined(M50II_TRACE_LOCK_EARLY) || defined(M50II_TRACE_LOCK_BEFORE_CSTART) || defined(M50II_TRACE_LOCK_BEFORE_CSTART_CLEAR)
static void trace_busy_wait(uint32_t n)
{
    volatile uint32_t i;
    while (n--)
    {
#ifdef M50II_TRACE_FAST_BLINK
        for (i = 0; i < 0x400000; i++)
#else
        for (i = 0; i < 0x800000; i++)
#endif
        {
        }
    }
}

static void __attribute__((used,noinline)) trace_blink_raw_once(uint32_t pulses)
{
#if defined(CARD_LED_ADDRESS) && defined(LEDON) && defined(LEDOFF)
    trace_busy_wait(2);
    for (uint32_t i = 0; i < pulses; i++)
    {
        MEM(CARD_LED_ADDRESS) = LEDON;
        trace_busy_wait(1);
        MEM(CARD_LED_ADDRESS) = LEDOFF;
        trace_busy_wait(1);
    }
    trace_busy_wait(3);
#else
    (void)pulses;
#endif
}

#ifdef M50II_TRACE_QUIET_UNTIL_HELPER_TAIL
static volatile uint32_t m50ii_trace_gate = 0;

static void trace_enable(void)
{
    m50ii_trace_gate = 1;
}
#else
static void trace_enable(void)
{
}
#endif

static void __attribute__((used,noinline)) trace_blink_once(uint32_t pulses)
{
    /* LED step-ladder disabled — UART trace supersedes it, no more waiting on 1..10
     * blinks (matescb request 2026-07-10). trace_blink_forever (halt states) still active. */
    (void)pulses;
}

static void trace_blink_forever(uint32_t pulses)
{
    while (1)
    {
        trace_blink_raw_once(pulses);
        trace_busy_wait(5);
    }
}
#endif

/* Global direct-MMIO UART writer (HW debug), callable from init.c during bring-up.
 * Mirrors Canon uart_write_bytes (0xE007D814): TX=0xC0800000, status=0xC0800014, ready=bit1. */
void m50ii_uart_str(const char *s)
{
    volatile uint32_t *st = (volatile uint32_t *)0xC0800014;
    volatile uint32_t *tx = (volatile uint32_t *)0xC0800000;
    for (; *s; s++)
    {
        if (*s == '\n') { int g = 2000000; while (!(*st & 2) && --g) {} *st = 2; *tx = '\r'; }
        { int g = 2000000; while (!(*st & 2) && --g) {} *st = 2; *tx = (uint32_t)(unsigned char)*s; }
    }
}

#ifdef M50II_MINIMAL_INIT_TASK
/* [M50II 1.0.1 HW-TEST Phase 2b — direct-MMIO UART] uart_printf/DebugMsg did NOT reach the
 * console on HW (uart_printf @0xE01C7280 is likely the panic printer; DebugMsg needs the log
 * task, which our early hijack path hasn't started). So mirror Canon's own uart_write_bytes
 * (0xE007D814), decoded from ROM: TX=0xC0800000, status=0xC0800014, TX-ready=bit1. Isolate
 * the test: banner + triple-blink (LED cross-check that we arrived, independent of UART) +
 * halt (no full ML yet). If the UART shows the banner, we have a reliable ML->UART primitive
 * to trace real init. Remove M50II_MINIMAL_INIT_TASK to hand straight to the real my_init_task. */
static void m50ii_uart_putc(char ch)
{
    volatile uint32_t *st = (volatile uint32_t *)0xC0800014;  /* UART status  */
    volatile uint32_t *tx = (volatile uint32_t *)0xC0800000;  /* UART TX data */
    int guard = 2000000;
    while (!(*st & 2) && --guard) { }   /* wait TX-ready (bit1); bounded so a wrong bit can't hang */
    *st = 2;                            /* ack, mirroring Canon's routine */
    *tx = (uint32_t)(unsigned char)ch;
}
static void m50ii_uart_puts(const char *s)
{
    for (; *s; s++) { if (*s == '\n') m50ii_uart_putc('\r'); m50ii_uart_putc(*s); }
}
static void m50ii_uart_puthex(uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    m50ii_uart_putc('0'); m50ii_uart_putc('x');
    for (int i = 28; i >= 0; i -= 4) m50ii_uart_putc(hx[(v >> i) & 0xF]);
}
/* print "<label> = 0x........\n" for the word at addr */
static void m50ii_uart_kv(const char *label, uint32_t addr)
{
    m50ii_uart_puts(label);
    m50ii_uart_putc(' '); m50ii_uart_putc('='); m50ii_uart_putc(' ');
    m50ii_uart_puthex(*(volatile uint32_t *)addr);
    m50ii_uart_putc('\n');
}
static int my_minimal_init_task(int a, int b, int c, int d)
{
    extern void boot_pre_init_task(void);
    m50ii_uart_puts("\n\n[ML] ===== M50II 1.0.1 init trace (fat dump + step ladder) =====\n");

    /* --- foundation dump: read actual HW kernel globals (validates the .data fix + layout) --- */
    m50ii_uart_kv("percpu   0x1000", 0x1000);
    m50ii_uart_kv("sgiptrA  0x4008", 0x4008);
    m50ii_uart_kv("isr_lock 0x4004", 0x4004);
    m50ii_uart_kv("pre_isr  0x400C", 0x400C);
    m50ii_uart_kv("post_isr 0x4010", 0x4010);
    m50ii_uart_kv("resched  0x40C8", 0x40C8);
    m50ii_uart_kv("sgiptrB  0x402C", 0x402C);
    m50ii_uart_kv("task_tbl 0x40E8", 0x40E8);
    m50ii_uart_kv("initslot 0x417C", 0x417C);

    /* --- step ladder: reconstruct ML init one call at a time; last line = failing step --- */
    m50ii_uart_puts("[ML] L1: calling boot_pre_init_task ...\n");
    boot_pre_init_task();
    m50ii_uart_puts("[ML] L2: boot_pre_init_task SURVIVED\n");

    /* [M50II 2026-07-10] Read-only check of 0x4108 (Canon task-count global). The real bug
     * was NOT a bad task count: FUN_e013a214 correctly writes dryos->task_max (=199, 0xC7)
     * here. It was boot_pre_init_task installing my_task_dispatch_hook into the MIS-PORTED
     * task_dispatch_hook stub (0x4108), overwriting 199 with a function pointer -> stdioSetup
     * alloc blew up. Now that the install is disabled (see init.c), 0x4108 must read 199.
     * QEMU write-watchpoint confirmed the three writers. No store here anymore. */
    m50ii_uart_kv("taskcnt  0x4108", 0x4108);   /* expect 0xC7 (199); 0x100 or garbage => regression */

    m50ii_uart_puts("[ML] L3: calling Canon init_task 0xE0040237 (Canon DebugMsg should follow) ...\n");
    ((void (*)(int,int,int,int))(0xE0040237u | 1u))(a, b, c, d);
    m50ii_uart_puts("[ML] L4: Canon init_task returned (deep success)\n");

    trace_blink_forever(4);   /* if we ever reach here, full init survived */
    __builtin_unreachable();
}
#endif

#ifdef M50II_TRACE_RESTORE_PROBE
static void __attribute__((noreturn,noinline)) m50ii_after_context_restore_probe(void);
#endif

#ifdef M50II_TRACE_BOOTSTRAP_WRAPPER
static uint32_t m50ii_probe_task_entry;
static void __attribute__((noreturn,noinline)) m50ii_after_context_bootstrap_wrapper(void);
#endif

#ifdef M50II_TRACE_INIT_HELPER_DIRECT_RESTORE
static void __attribute__((noreturn,noinline)) m50ii_after_context_init_helper_direct_restore(void);
#ifdef M50II_TRACE_INIT_HELPER_CONTEXT_PC_PROBE
static void __attribute__((noreturn,noinline)) m50ii_init_helper_context_pc_probe(void);
#endif
#ifdef M50II_TRACE_INIT_HELPER_DIRECT_ENTRY
static void __attribute__((noreturn,noinline)) m50ii_init_helper_direct_entry(void);
#endif
#endif

#ifdef M50II_TRACE_RESTORE_PROBE
static void __attribute__((noreturn,noinline)) m50ii_after_context_restore_probe(void)
{
    trace_blink_forever(9);
    __builtin_unreachable();
}
#endif

#if defined(M50II_TRACE_INIT_HELPER_DIRECT_RESTORE) && defined(M50II_TRACE_INIT_HELPER_CONTEXT_PC_PROBE)
static void __attribute__((noreturn,noinline)) m50ii_init_helper_context_pc_probe(void)
{
    trace_blink_forever(13);
    __builtin_unreachable();
}
#endif

#if defined(M50II_TRACE_INIT_HELPER_DIRECT_RESTORE) && defined(M50II_TRACE_INIT_HELPER_DIRECT_ENTRY)
static void __attribute__((noreturn,noinline)) m50ii_init_helper_direct_entry(void)
{
#ifdef M50II_TRACE_INIT_HELPER_DIRECT_ENTRY_UNLOCK
    typedef void (*canon_void0)(void);
#endif
    trace_blink_once(13);      /* direct-entry wrapper reached after second context restore */
#ifdef M50II_TRACE_INIT_HELPER_DIRECT_ENTRY_UNLOCK
    ((canon_void0)0xE01B2237)();
    trace_blink_once(14);      /* scheduler unlock returned in the second-task context */
#elif defined(M50II_TRACE_INIT_HELPER_DIRECT_ENTRY_MANUAL_UNLOCK)
    volatile uint32_t * const sched_lock = (volatile uint32_t *)0x4024;
    sched_lock[4] = 0;
    __asm__ volatile("dmb sy" ::: "memory");
    sched_lock[0] = 0;
#ifdef M50II_TRACE_INIT_HELPER_DIRECT_ENTRY_MASK_IRQS
    MEM(0xC1001180) = (1U << 31);
    MEM(0xC1001184) = (1U << 0) | (1U << 1) | (1U << 11);
#endif
    __asm__ volatile("dsb sy" ::: "memory");
    trace_blink_once(14);      /* minimal manual scheduler unlock applied */
    __asm__ volatile("dsb sy\n cpsie i" ::: "memory");
#ifdef M50II_TRACE_INIT_HELPER_DIRECT_ENTRY_MASK_IRQS
    trace_blink_once(15);      /* IRQ enable survived with selected lines masked */
#endif
#endif
    (void)my_init_task(0, 0, 0, 0);
    trace_blink_forever(14);
    __builtin_unreachable();
}
#endif

#ifdef M50II_TRACE_BOOTSTRAP_INLINE
static void __attribute__((noreturn,noinline)) m50ii_after_context_bootstrap_inline(void)
{
    typedef void (*canon_void0)(void);
    typedef void (*canon_fn1v)(uint32_t);
    typedef void (*canon_fn1pv)(uint32_t *);
    typedef uint32_t (*canon_fn1)(uint32_t *);
    typedef void (*canon_fn2v)(uint32_t, uint32_t);

    volatile uint32_t * const sched_lock = (volatile uint32_t *)0x4024;
    if (sched_lock[4] != 0)
    {
        sched_lock[4] = 0;
        ((canon_void0)0xE01B1FC1)();
    }

    __asm__ volatile("dmb sy" ::: "memory");
    sched_lock[0] = 0;

    MEM(0xC1001180) = (1U << 31);              /* mask IRQ31 */
    MEM(0xC1001184) = (1U << 0) | (1U << 1) | (1U << 11); /* mask IRQ32, IRQ33, IRQ43 */
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    trace_blink_once(7);               /* selected bad IRQ lines masked */

    uint32_t iar = MEM(0xC100010C);
    uint32_t irq = iar & 0x3FF;
    if (irq == 0x3FF)
    {
        trace_blink_once(4);           /* no forwarded IRQ before cpsie */
    }
    else
    {
        trace_blink_once(5);           /* unexpected forwarded IRQ */
        trace_blink_once((irq >> 5) + 1);
        trace_blink_once(((irq & 31) >> 2) + 1);
        trace_blink_once((irq & 3) + 1);
        MEM(0xC1000110) = iar;
        trace_blink_once(6);
    }

    __asm__ volatile("dsb sy\n cpsie i" ::: "memory");
    trace_blink_once(8);               /* IRQ enable survived with selected mask */

    volatile uint32_t * const boot_once = (volatile uint32_t *)0x4178;
    if (boot_once[0] != 0)
    {
        trace_blink_forever(15);
    }
    boot_once[0] = 1;
    trace_blink_once(9);

    uint32_t saved_cpsr = 0;
    ((canon_fn1pv)0xE01B21CD)(&saved_cpsr);
    trace_blink_once(10);

    uint32_t spec[8] = {0};
    spec[0] = 0xFFFFFFFF;
    spec[1] = boot_once[1];            /* init entry saved by E0166A5C */
    spec[2] = 0;
    spec[3] = MEM(0x4140);
    spec[4] = MEM(0x4138);
    spec[5] = 0xE0166B60;              /* "init" */
    ((volatile uint8_t *)spec)[0x18] = *(volatile uint8_t *)0x1008;

    uint32_t task = ((canon_fn1)0xE00A6C95)(spec);
    trace_blink_once(11);

    ((canon_fn2v)0xE01B1BE7)(task, 1);
    trace_blink_once(12);

    ((canon_fn1v)0xE01B21F3)(saved_cpsr);
    trace_blink_once(13);
    trace_blink_once(14);
    ((canon_void0)0xE00A679F)();       /* Canon idle/WFI loop */
    __builtin_unreachable();
}
#endif

#if defined(M50II_TRACE_LOCK_BEFORE_CSTART) || defined(M50II_TRACE_LOCK_BEFORE_CSTART_CLEAR)
static uint32_t m50ii_reloc_cstart_addr;

static void __attribute__((noreturn,noinline)) my_cstart_lock_probe(void)
{
#ifdef M50II_TRACE_LOCK_BEFORE_CSTART_CLEAR
    if (MEM(0x4004) != 0)
    {
        trace_blink_once(2);
        MEM(0x4004) = 0;
        __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    }
    else
    {
        trace_blink_once(1);
    }
#else
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(1);
    }

    trace_blink_once(1);
#endif
    ((thunk)m50ii_reloc_cstart_addr)();
    trace_blink_forever(15);
    __builtin_unreachable();
}
#endif

#ifdef M50II_TRACE_LADDER
#define M50II_STAGE(n) trace_blink_once(n)
#else
#define M50II_STAGE(n) do { } while (0)
#endif

#ifdef M50II_TRACE_CREATE_LADDER
#define M50II_CREATE_STAGE(n) trace_blink_once(n)
#define M50II_CREATE_STOP(n) trace_blink_forever(n)
#else
#define M50II_CREATE_STAGE(n) do { } while (0)
#define M50II_CREATE_STOP(n) do { } while (0)
#endif

#ifdef M50II_TRACE_CREATE_DEEP_HANDOFF_QUIET
#define M50II_DEEP_EARLY_STAGE(n) do { } while (0)
#else
#define M50II_DEEP_EARLY_STAGE(n) trace_blink_once(n)
#endif

#ifdef M50II_TRACE_SCHED_HANDOFF_MIRROR
static void __attribute__((noreturn,noinline)) m50ii_sched_handoff_mirror(void);
#endif

#ifdef M50II_TRACE_CREATE_DEEP_HANDOFF
static void __attribute__((noreturn,noinline)) m50ii_create_deep_handoff(struct dryos_init_info *dryos, uint32_t init_task, uint32_t c)
{
    typedef int (*canon_fn0)(void);
    typedef int (*canon_fn1)(uint32_t);
    typedef int (*canon_fn2)(uint32_t, uint32_t);
    typedef int (*canon_fn3)(struct dryos_init_info *, uint32_t, uint32_t);
    typedef void (*canon_fn1v)(uint32_t);
    typedef void (*canon_fn3v)(uint32_t, uint32_t, uint32_t);
    typedef void (*canon_void0)(void);

    M50II_DEEP_EARLY_STAGE(1);  /* entered deep mirror after ML memory adjustment */

    volatile uint32_t * const irq_base = (volatile uint32_t *)0xC1001000;
    volatile uint8_t * const irq_flags = (volatile uint8_t *)0xC1002014;
    irq_base[0x180 / 4] = 0xFFFFFFFF;
    irq_base[(0x180 / 4) + 1] = 0xFFFFFFFF;
    irq_base[0] = 1;
    M50II_DEEP_EARLY_STAGE(2);

    ((canon_void0)0xE00A69A9)();
    M50II_DEEP_EARLY_STAGE(3);

    int rc = ((canon_fn2)0xE00A6B39)(32, 0);
    M50II_DEEP_EARLY_STAGE(4);
    if (rc < 0)
    {
        trace_blink_forever(4);
    }

    irq_flags[0] = 16;
    rc = ((canon_fn1)0xE00A69BB)(32);
    M50II_DEEP_EARLY_STAGE(5);
    if (rc < 0)
    {
        trace_blink_forever(5);
    }

    rc = ((canon_fn2)0xE00A6B39)(33, 1);
    M50II_DEEP_EARLY_STAGE(6);
    if (rc < 0)
    {
        trace_blink_forever(6);
    }

    irq_flags[1] = 16;
    rc = ((canon_fn1)0xE00A69BB)(33);
    M50II_DEEP_EARLY_STAGE(7);
    if (rc < 0)
    {
        trace_blink_forever(7);
    }

    rc = ((canon_fn0)0xE00D45F5)();
    M50II_DEEP_EARLY_STAGE(8);
    if (rc < 0)
    {
        trace_blink_forever(8);
    }

    rc = ((canon_fn0)0xE027A571)();
    M50II_DEEP_EARLY_STAGE(9);
    if (rc < 0)
    {
        trace_blink_forever(9);
    }

    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(10);
    }
    ((canon_fn3v)0xE018E385)(0x20A, 0xE00A6749, 0);
    M50II_DEEP_EARLY_STAGE(10);

    ((canon_void0)0xE04F8B54)();
    M50II_DEEP_EARLY_STAGE(11);

    ((canon_void0)0xE04F8B70)();
    M50II_DEEP_EARLY_STAGE(12);

    rc = ((canon_fn0)0xE007D6E5)();
    M50II_DEEP_EARLY_STAGE(13);
    if (rc < 0)
    {
        trace_blink_forever(13);
    }

    ((canon_fn1v)0xE0104E47)(0xE00D4375);
    M50II_DEEP_EARLY_STAGE(14);

    M50II_DEEP_EARLY_STAGE(15); /* finished mirrored 0xE00A674A */

#ifdef M50II_TRACE_CLEAR_TASK_LOCK_BEFORE_E01B2138
    if (MEM(0x4024) != 0)
    {
        MEM(0x4024) = 0;
        __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    }
#endif

    ((canon_void0)0xE01B2139)();

    rc = ((canon_fn3)0xE0166A5D)(dryos, init_task, c);
    if (rc == -1)
    {
        trace_blink_forever(12);
    }

    rc = ((canon_fn0)0xE01669D5)();
    if (rc == -1)
    {
        trace_blink_forever(13);
    }

    ((canon_void0)0xE0166AD5)();

#ifdef M50II_TRACE_SCHED_HANDOFF_MIRROR
    m50ii_sched_handoff_mirror();
#else
    ((canon_void0)0xE01B207F)();
#endif
    trace_blink_forever(15);
    __builtin_unreachable();
}
#endif

static void my_bzero32(void *buf, size_t len)
{
#ifdef M50II_TRACE_STOP_AT_BZERO
    trace_blink_forever(4);
#endif
#ifdef M50II_TRACE_LOCK_EARLY
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(2);
    }
    trace_blink_once(2);
#endif
    M50II_STAGE(14);
    bzero32(buf, len);
#ifdef M50II_TRACE_LOCK_EARLY
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(3);
    }
    trace_blink_once(3);
#endif
}

#ifdef M50II_TRACE_LOCK_EARLY
static void __attribute__((noreturn,noinline)) my_create_init_task(struct dryos_init_info *dryos, uint32_t init_task, uint32_t c)
{
    (void)dryos;
    (void)init_task;
    (void)c;

    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(4);
    }
    trace_blink_once(4);
    trace_blink_forever(15);
    __builtin_unreachable();
}
#elif defined(M50II_TRACE_LOCK_INITIAL)
static void __attribute__((noreturn,noinline)) my_create_init_task(struct dryos_init_info *dryos, uint32_t init_task, uint32_t c)
{
    (void)dryos;
    (void)init_task;
    (void)c;

    volatile uint32_t * const irq_base = (volatile uint32_t *)0xC1001000;

    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(1);
    }
    trace_blink_once(1);

    irq_base[0x180 / 4] = 0xFFFFFFFF;
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(2);
    }
    trace_blink_once(2);

    irq_base[0x184 / 4] = 0xFFFFFFFF;
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(3);
    }
    trace_blink_once(3);

    irq_base[0] = 1;
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(4);
    }
    trace_blink_once(4);

    trace_blink_forever(15);
    __builtin_unreachable();
}
#elif defined(M50II_TRACE_CREATE_FIRST_INTERNALS) || defined(M50II_TRACE_CREATE_REG_HELPER) || defined(M50II_TRACE_CREATE_REG_FORCE) || defined(M50II_TRACE_LOCK_ORIGIN)
static void __attribute__((noreturn,noinline)) my_create_init_task(struct dryos_init_info *dryos, uint32_t init_task, uint32_t c)
{
    typedef int (*canon_fn0)(void);
    typedef int (*canon_fn1)(uint32_t);
    typedef int (*canon_fn2)(uint32_t, uint32_t);
#if !defined(M50II_TRACE_CREATE_REG_HELPER) && !defined(M50II_TRACE_CREATE_REG_FORCE)
    typedef void (*canon_fn3v)(uint32_t, uint32_t, uint32_t);
    typedef void (*canon_fn1v)(uint32_t);
#endif
    typedef void (*canon_void0)(void);
#if defined(M50II_TRACE_CREATE_REG_HELPER) || defined(M50II_TRACE_CREATE_REG_FORCE)
    typedef uint32_t (*canon_lock_fn)(volatile uint32_t *);
#endif

    (void)dryos;
    (void)init_task;
    (void)c;

    trace_blink_once(1);  /* entered replacement for create_init_task */

    volatile uint32_t * const irq_base = (volatile uint32_t *)0xC1001000;
    volatile uint8_t * const irq_flags = (volatile uint8_t *)0xC1002014;
    irq_base[0x180 / 4] = 0xFFFFFFFF;
    irq_base[(0x180 / 4) + 1] = 0xFFFFFFFF;
    irq_base[0] = 1;
    trace_blink_once(2);
#ifdef M50II_TRACE_LOCK_ORIGIN
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(2);
    }
#endif

    ((canon_void0)0xE00A69A9)();
    trace_blink_once(3);
#ifdef M50II_TRACE_LOCK_ORIGIN
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(3);
    }
#endif

    int rc = ((canon_fn2)0xE00A6B39)(32, 0);
    trace_blink_once(4);
    if (rc < 0)
    {
        trace_blink_forever(4);
    }
#ifdef M50II_TRACE_LOCK_ORIGIN
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(4);
    }
#endif

    irq_flags[0] = 16;
    rc = ((canon_fn1)0xE00A69BB)(32);
    trace_blink_once(5);
    if (rc < 0)
    {
        trace_blink_forever(5);
    }
#ifdef M50II_TRACE_LOCK_ORIGIN
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(5);
    }
#endif

    rc = ((canon_fn2)0xE00A6B39)(33, 1);
    trace_blink_once(6);
    if (rc < 0)
    {
        trace_blink_forever(6);
    }
#ifdef M50II_TRACE_LOCK_ORIGIN
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(6);
    }
#endif

    irq_flags[1] = 16;
    rc = ((canon_fn1)0xE00A69BB)(33);
    trace_blink_once(7);
    if (rc < 0)
    {
        trace_blink_forever(7);
    }
#ifdef M50II_TRACE_LOCK_ORIGIN
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(7);
    }
#endif

    rc = ((canon_fn0)0xE00D45F5)();
    trace_blink_once(8);
    if (rc < 0)
    {
        trace_blink_forever(8);
    }
#ifdef M50II_TRACE_LOCK_ORIGIN
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(8);
    }

    trace_blink_forever(15);
    __builtin_unreachable();
#endif

#ifdef M50II_TRACE_CREATE_REG_FORCE
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(8);
    }
#endif

    rc = ((canon_fn0)0xE027A571)();
    trace_blink_once(9);
    if (rc < 0)
    {
        trace_blink_forever(9);
    }

#if defined(M50II_TRACE_CREATE_REG_HELPER) || defined(M50II_TRACE_CREATE_REG_FORCE)
    trace_blink_once(10);
#ifdef M50II_TRACE_CREATE_REG_FORCE
    if (MEM(0x4004) != 0)
    {
        MEM(0x4004) = 0;
        trace_blink_once(11);
    }
#else
    if (MEM(0x4004) != 0)
    {
        trace_blink_forever(10);
    }
#endif

    uint32_t old_cpsr = ((canon_lock_fn)0xE021B8F1)((volatile uint32_t *)0x4004);
    trace_blink_once(12);

    volatile uint32_t * const vector_table = (volatile uint32_t *)0x2AD1C;
    vector_table[0x20A * 2] = 0xE00A6749;
    vector_table[0x20A * 2 + 1] = 0;
    trace_blink_once(13);

    __asm__ volatile("dmb sy" ::: "memory");
    MEM(0x4004) = 0;
    __asm__ volatile(
        "dsb sy\n"
        "msr CPSR_c, %0\n"
        :: "r"(old_cpsr) : "memory"
    );
    trace_blink_once(14);

    ((canon_void0)0xE04F8B54)();
    trace_blink_once(15);

    ((canon_void0)0xE04F8B70)();
    trace_blink_forever(15);
#else
    ((canon_fn3v)0xE018E385)(0x20A, 0xE00A6749, 0);
    trace_blink_once(10);

    ((canon_void0)0xE04F8B54)();
    trace_blink_once(11);

    ((canon_void0)0xE04F8B70)();
    trace_blink_once(12);

    rc = ((canon_fn0)0xE007D6E5)();
    trace_blink_once(13);
    if (rc < 0)
    {
        trace_blink_forever(13);
    }

    ((canon_fn1v)0xE0104E47)(0xE00D4375);
    trace_blink_once(14);

    trace_blink_forever(15);
#endif
    __builtin_unreachable();
}
#elif defined(M50II_TRACE_CREATE_INTERNALS)
static void __attribute__((noreturn,noinline)) my_create_init_task(struct dryos_init_info *dryos, uint32_t init_task, uint32_t c)
{
    typedef int (*canon_fn0)(void);
    typedef int (*canon_fn3)(struct dryos_init_info *, uint32_t, uint32_t);
    typedef void (*canon_void0)(void);

    trace_blink_once(1);  /* entered replacement for create_init_task */

    int rc = ((canon_fn0)0xE00A674B)();
    trace_blink_once(2);
    if (rc == -1)
    {
        trace_blink_forever(11);
    }

#ifdef M50II_TRACE_CREATE_CLEAR_TASK_LOCK_BEFORE_E01B2138
    if (MEM(0x4024) != 0)
    {
        trace_blink_once(8);
        MEM(0x4024) = 0;
        __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    }
    else
    {
        trace_blink_once(9);
    }
#endif

    ((canon_void0)0xE01B2139)();
    trace_blink_once(3);

#ifdef M50II_TRACE_CREATE_USE_ML_INIT_TASK
    if (!(dryos->sys_objs_start < dryos->sys_mem_start
       && dryos->user_mem_start < dryos->sys_objs_start))
    {
        trace_blink_forever(14);
    }

    init_task = (uint32_t)my_init_task;

    if (dryos->user_mem_start % ELF_ALIGNMENT != 0)
    {
        uint32_t align_diff = dryos->user_mem_start % ELF_ALIGNMENT;
        dryos->user_mem_start += align_diff;
        dryos->user_mem_len -= align_diff;
    }

    if (RESTARTSTART != dryos->user_mem_start)
    {
        trace_blink_forever(14);
    }

    ml_reserved_mem = (uintptr_t)_bss_end - dryos->user_mem_start;
    if (ml_reserved_mem >= dryos->user_mem_len)
    {
        trace_blink_forever(14);
    }

    dryos->user_mem_start += ml_reserved_mem;
    dryos->user_mem_len -= ml_reserved_mem;
#if defined(CONFIG_INCREASE_MAX_TASKS) && CONFIG_INCREASE_MAX_TASKS > 0
    dryos->user_mem_len -= sizeof(struct task) * CONFIG_INCREASE_MAX_TASKS;
    dryos->sys_objs_start -= sizeof(struct task) * CONFIG_INCREASE_MAX_TASKS;
    dryos->task_max += CONFIG_INCREASE_MAX_TASKS;
#endif

    if ((dryos->user_mem_len < MINIMUM_USER_MEM_LEFT) ||
        (dryos->user_mem_start + dryos->user_mem_len > dryos->sys_objs_start) ||
        (RESTARTSTART + ml_reserved_mem > dryos->user_mem_start) ||
        (RESTARTSTART + ml_reserved_mem > dryos->sys_objs_start))
    {
        trace_blink_forever(14);
    }

    trace_blink_once(10); /* ML init_task substituted and memory reserved */
#endif

    rc = ((canon_fn3)0xE0166A5D)(dryos, init_task, c);
    trace_blink_once(4);
    if (rc == -1)
    {
        trace_blink_forever(12);
    }

    rc = ((canon_fn0)0xE01669D5)();
    trace_blink_once(5);
    if (rc == -1)
    {
        trace_blink_forever(13);
    }

    ((canon_void0)0xE0166AD5)();
    trace_blink_once(6);

    trace_blink_once(7);  /* about to enter Canon scheduler handoff */
#ifdef M50II_TRACE_SCHED_HANDOFF_MIRROR
    m50ii_sched_handoff_mirror();
#else
    ((canon_void0)0xE01B207F)();
#endif
    trace_blink_forever(15);
    __builtin_unreachable();
}
#elif defined(M50II_TRACE_CREATE_TAIL_PASSTHRU) || defined(M50II_TRACE_CREATE_ROM_LR_TAIL)
static void __attribute__((naked,noinline)) my_create_init_task(struct dryos_init_info *dryos, uint32_t init_task, uint32_t c)
{
    __asm__ volatile(
        ".syntax unified\n"
        "stmdb sp!, {r0-r12, lr}\n"
        "movs r0, #1\n"
        "bl trace_blink_once\n"
        "ldmia sp!, {r0-r12, lr}\n"
#ifdef M50II_TRACE_CREATE_ROM_LR_TAIL
        "ldr r12, =0xE00401B9\n"
        "mov lr, r12\n"
#endif
        "ldr r12, =create_init_task\n"
        "bx r12\n"
    );
}
#else
static void my_create_init_task(struct dryos_init_info *dryos, uint32_t init_task, uint32_t c)
{
#ifdef M50II_TRACE_CREATE_PASSTHRU
    trace_blink_once(1);
    create_init_task(dryos, init_task, c);
    trace_blink_forever(15);
    return;
#endif
    M50II_CREATE_STAGE(1);
#ifdef M50II_TRACE_STOP_AT_CREATE_ITASK
    trace_blink_forever(5);
#endif
#ifdef M50II_TRACE_LADDER
    trace_blink_forever(15);
#endif
#ifdef CONFIG_FIXUP_BOOT_MEMORY
    // DIGIC X (and 8?) re-use the same memory range for coprocessors and autoexec.bin
    // This leads into unexpected behaviour (camera crashes), as described below.
    //
    // DIGIC has multiple secondary cores, those depends on Digic generation.
    // There are some cores related to networking, graphic acceleration, etc.
    //
    // There's also a group of cores related to lens communication, IBIS, etc.
    // Canon names: Shirahama, Arima, Kutatsu (DX only). They refer to those as CamIF.
    //
    // On Digic 6 and 7 models, CamIF cores (Shirahama, Arima, Kutatsu) were initialized
    // by DryOS, like other cores. On D8 and newer models this initialization was moved
    // before DryOS loader ( firmware_entry() ) is even executed or autoexec.bin loaded.
    //
    // Secondary cores seems to use memory range close to 0x4100_0000 - 0x4300_0000
    // as a shared memory with main core where we run on.
    //
    // =====
    //
    // Now here's what we believe to be a non-intentional Canon bug:
    //
    // Bootloader loads autoexec.bin at 0x4080_0000. Up to, and including early
    // Digic 8 it just loaded the file and jump code execution there.
    //
    // Somewhere near 250D/850D bootloader got updated. Now there's a maximum
    // file size check in place. And additionally, whole buffer starting from
    // 0x4080_0000 to max file size is erased when autoexec.bin is loaded.
    //
    // This is not an issue on Digic 8 models as buffer size doesn't interfere
    // with anything. However on Digic X buffer size increased, conflicting with
    // CamIF core regions described before.
    // Thus, if you run autoexec.bin - CamIF region gets wiped just after it was
    // initialized.
    //
    // We have hard evidence that DX models require this re-init. Without this, camera
    // crashes early in DryOS (after we transfer to Canon init task). Crash comes from
    // CamIF subsystem - Shirahama and/or Kutatsu cores "die".
    // However on EOS R (D8) calling this function results in a hard lock.
    //
    // =====
    //
    // This call executes the function that initializes CamIF cores.
    // Call is required here (and not in reboot.c) for safety reasons - reboot.c
    // runs still from buffer in question.
    //
    // Here we already run from relocated code, so it is safe to reinitialize memory.
    fixup_boot_memory();
#endif
    M50II_CREATE_STAGE(2);

    // We wrap Canon's create_init_task, allowing us to modify the
    // struct that it takes, which holds a bunch of OS info.
    // We adjust sizes of memory regions to reserve space for ML.
    //
    // We reserve space before DryOS user_mem, this means the start
    // address is fixed per cam, by RESTARTSTART in
    // platform/99D/Makefile

    // basic tests on layout of regions
    if(dryos->sys_objs_start < dryos->sys_mem_start
       && dryos->user_mem_start < dryos->sys_objs_start)
    { // good
    }
    else
    { // bad: we make assumptions that assume the ordering
      // is user_mem, sys_objs, sys_mem
        M50II_CREATE_STOP(11);
        goto fail;
    }
    M50II_CREATE_STAGE(3);

    // replace Canon's init_task with ours
#ifdef M50II_MINIMAL_INIT_TASK
    (void)my_init_task;                            /* keep referenced when swapped out */
    init_task = (uint32_t)my_minimal_init_task;    /* Phase-1 HW test: LED beacon, not full ML */
#else
    init_task = (uint32_t)my_init_task;
#endif
    M50II_CREATE_STAGE(4);

    // user_mem_start must have the same alignment as the code section
    // of the ELF used during building ML binaries, or things like relocs don't work.
    if(dryos->user_mem_start % ELF_ALIGNMENT != 0)
    {
        qprint("[BOOT] user_mem_start is not aligned to "); qprintn(ELF_ALIGNMENT); qprint(", adjusting\n");
        qprint("       before: user_mem_start = "); qprintn(dryos->user_mem_start); qprint("\n");
        qprint("       before: user_mem_size  = "); qprintn(dryos->user_mem_len); qprint("\n");
        uint32_t align_diff = dryos->user_mem_start % ELF_ALIGNMENT;
        dryos->user_mem_start += align_diff;
        dryos->user_mem_len -= align_diff;
        qprint("        after: user_mem_start = "); qprintn(dryos->user_mem_start); qprint("\n");
        qprint("        after: user_mem_size  = "); qprintn(dryos->user_mem_len); qprint("\n\n");
    }
    M50II_CREATE_STAGE(5);

    // check if RESTARTSTART is correct
    // note - this will fail to print due to... misalignment. Catch 22.
    if (RESTARTSTART != dryos->user_mem_start)
    {
        qprint("[BOOT] Wrong or unaligned RESTARTSTART address!\n\n");
        qprint("[BOOT] RESTARTSTART: "); qprintn(RESTARTSTART); qprint("\n");
        qprint("[BOOT]     expected: "); qprintn(dryos->user_mem_start); qprint("\n");
        M50II_CREATE_STOP(12);
        goto fail;
    }
    M50II_CREATE_STAGE(6);

    // steal memory from user_mem
    ml_reserved_mem = (uintptr_t)_bss_end - dryos->user_mem_start;
    qprint("[BOOT] reserving memory: "); qprintn(ml_reserved_mem); qprint("\n");
    qprint("       before: user_mem_start = "); qprintn(dryos->user_mem_start); qprint("\n");
    qprint("       before: user_mem_size  = "); qprintn(dryos->user_mem_len); qprint("\n");
    M50II_CREATE_STAGE(7);

    // shrink user_mem
    dryos->user_mem_start += ml_reserved_mem;
    dryos->user_mem_len -= ml_reserved_mem;
#if defined(CONFIG_INCREASE_MAX_TASKS) && CONFIG_INCREASE_MAX_TASKS > 0
// Some cams are very close to task_max, and additional ML tasks
// can cause asserts.  Steal more space to increase the task limit.
// We want to increase size of sys objs pool, tasks live there.
// Later DryOS code does various size checks of the pools, see
// df003570(dryos_init_info *param_1) on 200D 1.0.1
    dryos->user_mem_len -= sizeof(struct task) * CONFIG_INCREASE_MAX_TASKS;
    dryos->sys_objs_start -= sizeof(struct task) * CONFIG_INCREASE_MAX_TASKS;
    dryos->task_max += CONFIG_INCREASE_MAX_TASKS;
#endif
    qprint("        after: user_mem_start = "); qprintn(dryos->user_mem_start); qprint("\n");
    qprint("        after: user_mem_size  = "); qprintn(dryos->user_mem_len); qprint("\n");
    M50II_CREATE_STAGE(8);

    if (dryos->user_mem_len < MINIMUM_USER_MEM_LEFT)
    {
        qprint("[BOOT] Not enough user mem is left, aborting!");
        qprint("[BOOT] "); qprintn(dryos->user_mem_len);
        qprint(" >  "); qprintn(MINIMUM_USER_MEM_LEFT); qprint(")\n");
        M50II_CREATE_STOP(13);
        goto fail;
    }
    M50II_CREATE_STAGE(9);

    if ( (dryos->user_mem_start + dryos->user_mem_len > dryos->sys_objs_start) ||
         (RESTARTSTART + ml_reserved_mem > dryos->user_mem_start) ||
         (RESTARTSTART + ml_reserved_mem > dryos->sys_objs_start) )
    {
        qprint("[BOOT] User mem math gone wrong, aborting!");
        M50II_CREATE_STOP(14);
        goto fail;
    }
    M50II_CREATE_STAGE(10);

#ifdef M50II_TRACE_CREATE_DEEP_HANDOFF
    m50ii_create_deep_handoff(dryos, init_task, c);
#else
    create_init_task(dryos, init_task, c);
#endif
    M50II_CREATE_STOP(15);

    return;

fail:
    while(1); // SJE FIXME kitor wants to do cool stuff here
}
#endif

/* M50: Canon code calling these cache functions expects R3 to be preserved after the call */
/* trick to prevent our C compiler for overwriting R3: two unused parameters */
static void my_dcache_clean(uint32_t addr, uint32_t size, uint32_t keep1, uint32_t keep2)
{
    extern void dcache_clean(uint32_t, uint32_t, uint32_t, uint32_t);
    dcache_clean(addr, size, keep1, keep2);
}

static void my_icache_invalidate(uint32_t addr, uint32_t size, uint32_t keep1, uint32_t keep2)
{
    extern void icache_invalidate(uint32_t, uint32_t, uint32_t, uint32_t);
    icache_invalidate(addr, size, keep1, keep2);
}

#if defined CONFIG_DIGIC_VI // seen on 5D4, 750D, 7D2, 80D
static void my_pre_cstart_func(void)
{
    extern void pre_cstart_func(void);
    pre_cstart_func();
}
#endif

void
__attribute__((noreturn,noinline,naked))
copy_and_restart(int offset)
{
    // This function runs very early, before Canon initialisation code.
    // We want to edit their code to take control, but it's in ROM.
    // We copy the early code into a RWX region; the _reloc buffer
    // defined above.  We will later run this instead of the ROM code,
    // but before doing so we make our edits.
    //
    // [M50II 2026-07-12] NB: cpu1 does NOT reach here — reboot.c's _start MPIDR gate
    // bounces non-cpu0 cores to Canon firmware_entry before cstart/copy_and_restart,
    // and on M50 II cpu1 is released straight into Canon via the 1st-stage mailbox
    // (see reboot.c: single-pointer 0xBFE01FC4 fix). So this stays cpu0-only by design.

    M50II_STAGE(8);
    zero_bss();
    M50II_STAGE(9);

    // Copy the firmware to somewhere safe in memory
    const uint8_t *const firmware_start = (void *)MAIN_FIRMWARE_ADDR;
    const uint32_t firmware_len = FIRMWARE_ENTRY_LEN;
    uint8_t *const new_image = (void *)RELOCADDR;

    blob_memcpy(new_image, firmware_start, firmware_start + firmware_len);
#if defined(CSTART_LEN)
    const uint8_t *const cstart_start = (uint8_t *)((uint32_t)cstart & 0xfffffffe);
    blob_memcpy(new_image + firmware_len,
                cstart_start,
                cstart_start + CSTART_LEN);
#endif
    M50II_STAGE(10);

#if defined(BR_CSTART) && defined(BR_BR_CSTART)
    #error "Defining both BR_CSTART AND BR_BR_CSTART doesn't make sense"
#endif

#ifdef CONFIG_DIGIC_78X
    // Fix cache maintenance calls before cstart
    patch_thumb_branch(BR_DCACHE_CLN_1, (uint32_t)my_dcache_clean);
    patch_thumb_branch(BR_DCACHE_CLN_2, (uint32_t)my_dcache_clean);
    patch_thumb_branch(BR_ICACHE_INV_1, (uint32_t)my_icache_invalidate);
    patch_thumb_branch(BR_ICACHE_INV_2, (uint32_t)my_icache_invalidate);
    #if defined(CONFIG_XF605) // possibly all DV?
    patch_thumb_branch(BR_DCACHE_CLN_3, (uint32_t)my_dcache_clean);
    patch_thumb_branch(BR_ICACHE_INV_3, (uint32_t)my_icache_invalidate);
    #endif

    // On D78X, there's an indirect branch to branch to cstart,
    // the first branch goes to absolute cstart original address.
    // Patch the setup for that into a relative branch to our reloc'd cstart.
    #if defined(BR_BR_CSTART)
    #if defined(M50II_TRACE_LOCK_BEFORE_CSTART) || defined(M50II_TRACE_LOCK_BEFORE_CSTART_CLEAR)
    m50ii_reloc_cstart_addr = reloc_addr((uint32_t)cstart) | 1;
    patch_thumb_branch(BR_BR_CSTART, (uint32_t)my_cstart_lock_probe);
    #else
    patch_thumb_branch(BR_BR_CSTART, reloc_addr((uint32_t)cstart));
    #endif
    #endif

    /* there are two more functions in cstart that don't require patching */
    /* the first one is within the relocated code; it initializes the per-CPU data structure at VA 0x1000 */
    /* the second one is called only when running on CPU1; assuming our code only runs on CPU0 */
#endif
    M50II_STAGE(11);

#if defined(CSTART_LEN)
    // if we're compacting firmware_entry and cstart,
    // we need to patch the jump
    uint32_t reloc_cstart = reloc_addr((uint32_t)cstart_start);
    #if defined(BR_CSTART)
    patch_thumb_branch(BR_CSTART, reloc_cstart | 0x1);
    #endif
#endif

    // if firmware_entry calls code in the cstart reloc'd region,
    // we also need to patch that
#if defined CONFIG_DIGIC_VI // seen on 750D, 5D4, 7D2, 80D
    patch_thumb_branch(BR_PRE_CSTART, (uint32_t)my_pre_cstart_func);
#endif

    // Fix the calls to bzero32() and create_init_task() in cstart.
    //
    // Our my_create_init_task wraps Canon create_init_task
    // and modifies OS memory layout to make room for ML
    patch_thumb_branch(BR_BZERO32, (uint32_t)my_bzero32);
#ifndef M50II_TRACE_SKIP_CREATE_PATCH
    patch_thumb_branch(BR_CREATE_ITASK, (uint32_t)my_create_init_task);
#endif
    M50II_STAGE(12);

#if defined(BR_BR_CSTART)
    // [M50II 1.0.1 fix - qemu root-caused 2026-07-07, docs/22]
    // Our branch-patching around firmware_entry offset 0xC0 collaterally corrupts the
    // kernel .data-copy SOURCE literal at reloc offset 0xC8 (ROM 0xE00400C8 =
    // 0xE1394358): it gets overwritten with a garbage/misaligned pointer. Left corrupt,
    // the relocated firmware_entry copies the whole DryOS kernel-globals region
    // (0x4000..0x2AD1C) from a bad source, so the lock at 0x4004 comes up non-zero;
    // register_interrupt() then takes its contended path and does blx MEM[0x40C8]
    // (== NULL) -> data abort on address 0x40 -> boot hangs in the fault handler.
    // Restore the correct source literal before the relocated code runs its copy.
    *(volatile uint32_t *)(RELOCADDR + 0xC8) = 0xE1394358;
#endif

    // Make sure that our self-modifying code clears the cache
    sync_caches();
    M50II_STAGE(13);
#ifdef M50II_TRACE_SKIP_CREATE_PATCH
    trace_blink_once(2);
#endif

    // Jump to copied firmware code in our modified buffer.
#ifdef CONFIG_DIGIC_VI
    // In D6 this code starts in ARM.  This function is Thumb.
    // The first few instructions do nothing apart from switch mode to Thumb,
    // so we can instead skip them.
    thunk __attribute__((long_call)) reloc_entry = (thunk)(RELOCADDR + 0xc + 1);
#elif defined(CONFIG_DIGIC_78X)
    thunk __attribute__((long_call)) reloc_entry = (thunk)(RELOCADDR + 1);
#endif
    qprint("[BOOT] jumping to relocated startup code at "); qprintn((uint32_t)reloc_entry); qprint("\n");
#ifdef M50II_TRACE_STOP_BEFORE_RELOC
    trace_blink_forever(3);
#endif
    reloc_entry();

    // Unreachable
    while(1)
        ;
}

#ifdef M50II_TRACE_SCHED_HANDOFF_MIRROR
static void __attribute__((noreturn,noinline)) m50ii_sched_handoff_mirror(void)
{
    typedef uint32_t (*canon_fn1)(uint32_t);
#if !defined(M50II_TRACE_SCHED_HANDOFF_STEP_SCAN) && !defined(M50II_TRACE_SCHED_CONTEXT_SCAN)
    typedef void (*canon_fn1v)(uint32_t);
#endif

    uint32_t current_slot = MEM(0x1008);
    volatile uint32_t * const task_table = (volatile uint32_t *)0x2C054;
    volatile uint32_t * const task_ptr_table = (volatile uint32_t *)(0x4024 + 0x14);

    task_table[current_slot * 4] = 0;
    uint32_t task = ((canon_fn1)0xE01B19B7)(current_slot);

    MEM(0x1028) = task;
    task_ptr_table[current_slot] = task;
    *(volatile uint8_t *)(task + 0x51) = current_slot;
#ifdef M50II_TRACE_SCHED_HANDOFF_STEP_SCAN
    trace_blink_once(8);       /* mirrored task slot/current-task writes */
    if (task < 0x1000 || task >= 0x1000000)
    {
        trace_blink_forever(13);
    }
    trace_blink_once(9);       /* task pointer is in low RAM */

    uint32_t ctx_sp = MEM(task + 0x54);
    if (ctx_sp < 0x1000 || ctx_sp >= 0x1000000)
    {
        trace_blink_forever(14);
    }
    trace_blink_once(10);      /* context pointer is in low RAM */

    uint32_t ctx_pc = MEM(ctx_sp + 0x38);
    uint32_t ctx_cpsr = MEM(ctx_sp + 0x3C);
    if (ctx_cpsr != 0xB3)
    {
        trace_blink_forever(15);
    }

    if ((ctx_pc & ~1U) == (0xE00A6C75 & ~1U))
    {
        trace_blink_forever(11); /* saved PC is Canon task trampoline */
    }

    trace_blink_forever(12);     /* context exists, but saved PC is unexpected */
#elif defined(M50II_TRACE_SCHED_CONTEXT_SCAN)
    uint32_t ctx_sp = MEM(task + 0x54);
    if (task < 0x1000 || task >= 0x1000000 || ctx_sp < 0x1000 || ctx_sp >= 0x1000000)
    {
        trace_blink_forever(11);
    }
    trace_blink_once(8);       /* task and context pointers are in low RAM */

    uint32_t my_init = (uint32_t)my_init_task;
    uint32_t task_entry = MEM(task + 0x0C);
    uint32_t ctx_pc = MEM(ctx_sp + 0x38);
    uint32_t ctx_cpsr = MEM(ctx_sp + 0x3C);
    uint32_t saved_init = MEM(0x417C);

    if ((saved_init & ~1U) != (my_init & ~1U))
    {
        trace_blink_once(13);  /* Canon saved-init slot is not ML init */
    }
    else
    {
        trace_blink_once(9);   /* Canon saved-init slot is ML init */
    }

    if (ctx_cpsr != 0xB3)
    {
        trace_blink_forever(15);
    }
    trace_blink_once(10);      /* saved CPSR is the expected new-task mode */

    if ((ctx_pc & ~1U) != (0xE00A6C75 & ~1U))
    {
        trace_blink_forever(12);
    }
    trace_blink_once(11);      /* saved PC is Canon task trampoline */

    if ((task_entry & ~1U) == (my_init & ~1U))
    {
        trace_blink_forever(1); /* task entry is ML my_init_task */
    }

    if ((task_entry & ~1U) == (0xE0040237 & ~1U))
    {
        trace_blink_forever(2); /* task entry is Canon init_task */
    }

    if ((task_entry & ~1U) == (0xE0166A77 & ~1U))
    {
        trace_blink_forever(3); /* task entry is Canon trampoline helper */
    }

    trace_blink_forever(14);
#else
#if defined(M50II_TRACE_RESTORE_PROBE) || defined(M50II_TRACE_BOOTSTRAP_WRAPPER) || defined(M50II_TRACE_BOOTSTRAP_INLINE) || defined(M50II_TRACE_INIT_HELPER_DIRECT_RESTORE)
    uint32_t ctx_sp = MEM(task + 0x54);
    if (ctx_sp < 0x1000 || ctx_sp >= 0x1000000)
    {
        trace_blink_forever(11);
    }

#ifdef M50II_TRACE_BOOTSTRAP_INLINE
    MEM(ctx_sp + 0x38) = ((uint32_t)m50ii_after_context_bootstrap_inline) | 1U;
#elif defined(M50II_TRACE_INIT_HELPER_DIRECT_RESTORE)
#ifdef M50II_TRACE_INIT_HELPER_USE_TRAMPOLINE
    MEM(task + 0x0C) = ((uint32_t)m50ii_after_context_init_helper_direct_restore) | 1U;
#else
    MEM(ctx_sp + 0x38) = ((uint32_t)m50ii_after_context_init_helper_direct_restore) | 1U;
#endif
#elif defined(M50II_TRACE_BOOTSTRAP_WRAPPER)
    m50ii_probe_task_entry = MEM(task + 0x0C);
    MEM(ctx_sp + 0x38) = ((uint32_t)m50ii_after_context_bootstrap_wrapper) | 1U;
#else
    MEM(ctx_sp + 0x38) = ((uint32_t)m50ii_after_context_restore_probe) | 1U;
#endif
    MEM(ctx_sp + 0x3C) = 0xB3;
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
#ifdef M50II_TRACE_INIT_HELPER_ENTRY_SPLIT
    trace_blink_once(8);       /* about to restore into direct-helper wrapper */
#endif
#endif
    ((canon_fn1v)0xE03C51C3)(task + 0x54);
    trace_blink_forever(15);
#endif
    __builtin_unreachable();
}
#endif

#ifdef M50II_TRACE_BOOTSTRAP_WRAPPER
static void __attribute__((noreturn,noinline)) m50ii_after_context_bootstrap_wrapper(void)
{
    typedef void (*canon_void0)(void);

    ((canon_void0)0xE01B2237)();
    trace_blink_once(9);
    ((canon_void0)(m50ii_probe_task_entry | 1U))();
    trace_blink_forever(10);
    __builtin_unreachable();
}
#endif

#ifdef M50II_TRACE_INIT_HELPER_DIRECT_RESTORE
static void __attribute__((noreturn,noinline)) m50ii_after_context_init_helper_direct_restore(void)
{
#if !defined(M50II_TRACE_INIT_HELPER_USE_TRAMPOLINE) && !defined(M50II_TRACE_INIT_HELPER_SKIP_UNLOCK)
    typedef void (*canon_void0)(void);
#endif
#ifndef M50II_TRACE_INIT_HELPER_SKIP_LOCKS
    typedef void (*canon_lock_save)(uint32_t *);
#endif
    typedef uint32_t (*canon_task_create)(void *);
#ifndef M50II_TRACE_INIT_HELPER_SKIP_READY
    typedef void (*canon_task_ready)(uint32_t, uint32_t);
#endif
#ifndef M50II_TRACE_INIT_HELPER_SKIP_LOCKS
    typedef void (*canon_lock_restore)(uint32_t);
#endif
    typedef void (*canon_context_restore)(uint32_t);

    volatile uint32_t * const helper_state = (volatile uint32_t *)0x4178;
    volatile uint32_t desc[8];

    trace_blink_once(1);       /* entered direct-helper wrapper */
#if defined(M50II_TRACE_INIT_HELPER_USE_TRAMPOLINE)
    trace_blink_once(2);       /* Canon task trampoline already returned into wrapper */
#elif defined(M50II_TRACE_INIT_HELPER_SKIP_UNLOCK)
    trace_blink_once(2);       /* skipping unsafe task-trampoline unlock in direct-restore mode */
#else
    ((canon_void0)0xE01B2237)();
    trace_blink_once(2);       /* task-trampoline scheduler unlock returned */
#endif

#ifdef M50II_TRACE_INIT_HELPER_RESET_STATE
    helper_state[0] = 0;
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
#endif

    if (helper_state[0] != 0)
    {
        trace_blink_forever(12);
    }
    helper_state[0] = 1;

#ifdef M50II_TRACE_INIT_HELPER_SKIP_LOCKS
    trace_blink_once(3);       /* skipping unsafe helper critical-section entry */
#else
    uint32_t lock_token = 0;
    ((canon_lock_save)0xE01B21CD)(&lock_token);
    trace_blink_once(3);       /* helper critical section entered */
#endif

    desc[0] = 0xFFFFFFFF;
    desc[1] = MEM(0x417C);     /* init task entry, expected to be my_init_task */
    desc[2] = 0;
    desc[3] = MEM(0x4140);
    desc[4] = MEM(0x4138);
    desc[5] = (uint32_t)"init";
    desc[6] = 0;
    desc[7] = 0;
    ((volatile uint8_t *)desc)[24] = *(volatile uint8_t *)(0x1000 + 8);
    ((volatile uint8_t *)desc)[25] = 0;
    ((volatile uint8_t *)desc)[26] = 0;
    ((volatile uint8_t *)desc)[27] = 0;

    uint32_t new_task = ((canon_task_create)0xE00A6C95)((void *)desc);
    if (new_task < 0x1000 || new_task >= 0x1000000)
    {
        trace_blink_forever(13);
    }
    trace_blink_once(4);       /* second init task object created */

    if ((MEM(new_task + 0x0C) & ~1U) != (((uint32_t)my_init_task) & ~1U))
    {
        trace_blink_forever(14);
    }
    trace_blink_once(5);       /* new task entry is ML my_init_task */

    uint32_t new_ctx_sp = MEM(new_task + 0x54);
    if (new_ctx_sp < 0x1000 || new_ctx_sp >= 0x1000000)
    {
        trace_blink_forever(15);
    }
    trace_blink_once(6);       /* new task context pointer is valid */

    if ((MEM(new_ctx_sp + 0x38) & ~1U) != (0xE00A6C75 & ~1U))
    {
#ifdef M50II_TRACE_INIT_HELPER_COMPACT_TAIL
        trace_enable();
        trace_blink_forever(10);
#else
        trace_blink_forever(12);
#endif
    }
#ifdef M50II_TRACE_INIT_HELPER_COMPACT_TAIL
    trace_enable();
    trace_blink_once(1);       /* compact tail: new task context is valid */
#else
    trace_blink_once(7);       /* new task starts at Canon task trampoline */
#endif

#ifdef M50II_TRACE_INIT_HELPER_SKIP_READY
#ifdef M50II_TRACE_INIT_HELPER_COMPACT_TAIL
    trace_blink_once(2);       /* compact tail: skipping helper task-ready call */
#else
    trace_blink_once(8);       /* skipping unsafe helper task-ready call */
#endif
#else
    ((canon_task_ready)0xE01B1BE7)(new_task, 1);
    trace_blink_once(8);       /* helper task-ready call returned */
#endif

#ifdef M50II_TRACE_INIT_HELPER_SKIP_LOCKS
#ifdef M50II_TRACE_INIT_HELPER_COMPACT_TAIL
    trace_blink_once(3);       /* compact tail: skipping helper critical-section restore */
#else
    trace_blink_once(9);       /* skipping helper critical-section restore */
#endif
#else
    ((canon_lock_restore)0xE01B21F3)(lock_token);
    trace_blink_once(9);       /* helper critical section restored */
#endif

    uint32_t current_slot = MEM(0x1008);
    volatile uint32_t * const task_table = (volatile uint32_t *)0x2C054;
    volatile uint32_t * const task_ptr_table = (volatile uint32_t *)(0x4024 + 0x14);

#ifdef M50II_TRACE_INIT_HELPER_COMPACT_TAIL
    trace_blink_once(4);       /* compact tail: current scheduler slot was readable */
#else
#ifdef M50II_TRACE_INIT_HELPER_CURRENT_SWITCH_SPLIT
    trace_blink_once(11);      /* current scheduler slot was readable */
#endif
#endif
    task_table[current_slot * 4] = 0;
#ifdef M50II_TRACE_INIT_HELPER_COMPACT_TAIL
    trace_blink_once(5);       /* compact tail: task table slot cleared */
#else
#ifdef M50II_TRACE_INIT_HELPER_CURRENT_SWITCH_SPLIT
    trace_blink_once(12);      /* task table slot cleared */
#endif
#endif
    MEM(0x1028) = new_task;
#ifdef M50II_TRACE_INIT_HELPER_COMPACT_TAIL
    trace_blink_once(6);       /* compact tail: current task pointer replaced */
#else
#ifdef M50II_TRACE_INIT_HELPER_CURRENT_SWITCH_SPLIT
    trace_blink_once(13);      /* current task pointer replaced */
#endif
#endif
    task_ptr_table[current_slot] = new_task;
#ifdef M50II_TRACE_INIT_HELPER_COMPACT_TAIL
    trace_blink_once(7);       /* compact tail: per-slot task pointer replaced */
#else
#ifdef M50II_TRACE_INIT_HELPER_CURRENT_SWITCH_SPLIT
    trace_blink_once(14);      /* per-slot task pointer replaced */
#endif
#endif
    *(volatile uint8_t *)(new_task + 0x51) = current_slot;
#ifdef M50II_TRACE_INIT_HELPER_COMPACT_TAIL
    trace_blink_once(8);       /* compact tail: new task slot byte patched */
#else
#ifdef M50II_TRACE_INIT_HELPER_CURRENT_SWITCH_SPLIT
    trace_blink_once(15);      /* new task slot byte patched */
#endif
#endif
#ifdef M50II_TRACE_INIT_HELPER_CONTEXT_PC_PROBE
    MEM(new_ctx_sp + 0x38) = ((uint32_t)m50ii_init_helper_context_pc_probe) | 1U;
#elif defined(M50II_TRACE_INIT_HELPER_DIRECT_ENTRY)
    MEM(new_ctx_sp + 0x38) = ((uint32_t)m50ii_init_helper_direct_entry) | 1U;
#endif
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
#ifdef M50II_TRACE_INIT_HELPER_COMPACT_TAIL
    trace_blink_once(9);       /* compact tail: about to restore new init task */
#else
    trace_blink_once(10);      /* forcing direct restore of new init task */
#endif

    ((canon_context_restore)0xE03C51C3)(new_task + 0x54);
    trace_blink_forever(11);
    __builtin_unreachable();
}
#endif
