/**
 * Generic (not model-specific) init code called during ML startup.
 * To be included in boot-*.c only.
 */

/** Hooks for calling user-level code after booting */

/* Called before Canon's init_task */
void boot_pre_init_task(void);

/* Called after Canon's init_task */
void boot_post_init_task(void);

/** The boot method may optionally define similar hooks (internal) */
/** To use them, define BOOT_USE_LOCAL_PRE_INIT_TASK / BOOT_USE_LOCAL_POST_INIT_TASK before including. */
static void local_pre_init_task(void);
static void local_post_init_task(void);

/** The boot method may also require calling a relocated init_task. */
/** Define BOOT_USE_INIT_TASK_PATCHED to use this. */
static init_task_func init_task_patched(void);

/* these are global and must be initialized by the boot method, otherwise it won't boot */
uint32_t ml_used_mem = 0;
uint32_t ml_reserved_mem = 0;

/* optionally used for error handling, if we cannot boot */
void WEAK_FUNC(ret_0) info_led_blink(int times, int delay_on, int delay_off);

/** Specified by the linker */
extern uint32_t _bss_start[], _bss_end[];
extern uint32_t _text_start[], _text_end[];

#ifdef M50II_TRACE_INIT_LADDER
static void m50ii_init_trace_wait(uint32_t n)
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

static void m50ii_init_trace_blink_once(uint32_t pulses)
{
#if defined(CARD_LED_ADDRESS) && defined(LEDON) && defined(LEDOFF)
    m50ii_init_trace_wait(2);
    for (uint32_t i = 0; i < pulses; i++)
    {
        MEM(CARD_LED_ADDRESS) = LEDON;
        m50ii_init_trace_wait(1);
        MEM(CARD_LED_ADDRESS) = LEDOFF;
        m50ii_init_trace_wait(1);
    }
    m50ii_init_trace_wait(3);
#else
    (void)pulses;
#endif
}

static void m50ii_init_trace_blink_forever(uint32_t pulses)
{
    while (1)
    {
        m50ii_init_trace_blink_once(pulses);
        m50ii_init_trace_wait(5);
    }
}

#define M50II_INIT_STAGE(n) m50ii_init_trace_blink_once(n)
#define M50II_INIT_STOP(n) m50ii_init_trace_blink_forever(n)
#else
#define M50II_INIT_STAGE(n) do { } while (0)
#define M50II_INIT_STOP(n) do { } while (0)
#endif

#ifdef M50II_TRACE_POST_INIT_FIO_PROBE
extern FILE* _FIO_CreateFile(const char* filename);
extern int _FIO_WriteFile(FILE* stream, const void* ptr, size_t count);
extern int _FIO_RemoveFile(const char * filename);
extern int FIO_Flush(const char * filename);

static int m50ii_post_init_fio_try(const char *filename, uint32_t remove_marker, uint32_t create_ok_marker, uint32_t write_ok_marker, uint32_t close_ok_marker, uint32_t flush_ok_marker)
{
    static const char msg[] =
        "M50II AUTOEXEC reached post-init FIO probe.\n"
        "Canon init_task returned and boot_post_init_task returned.\n";

    M50II_INIT_STAGE(remove_marker);
    _FIO_RemoveFile(filename);

    FILE *f = _FIO_CreateFile(filename);
    if (!f || f == PTR_INVALID)
    {
        return -1;
    }
    M50II_INIT_STAGE(create_ok_marker);

    int written = _FIO_WriteFile(f, msg, sizeof(msg) - 1);
    M50II_INIT_STAGE(write_ok_marker);

    FIO_CloseFile(f);
    M50II_INIT_STAGE(close_ok_marker);

    FIO_Flush(filename);
    M50II_INIT_STAGE(flush_ok_marker);

    return written;
}

static void m50ii_post_init_fio_probe(void)
{
    int written = m50ii_post_init_fio_try("A:/M50TRACE.TXT", 10, 11, 12, 13, 14);
    if (written > 0)
    {
        M50II_INIT_STOP(15);
    }

    M50II_INIT_STAGE(1);       /* A: create/write failed cleanly; try B: */
    written = m50ii_post_init_fio_try("B:/M50TRACE.TXT", 2, 3, 4, 5, 6);
    if (written > 0)
    {
        M50II_INIT_STAGE(7);
        M50II_INIT_STOP(15);
    }

    M50II_INIT_STOP(10);       /* both drive letters failed without hanging */
}
#endif

static int my_init_task(int a, int b, int c, int d)
{
    M50II_INIT_STAGE(1);
#ifdef ARMLIB_OVERFLOWING_BUFFER
    // An overflow in Canon code may write a zero right in the middle of ML code
    uint32_t * backup_address = 0;
    uint32_t backup_data = 0;
    uint32_t task_id = current_task->taskId;

    if(task_id > 0x68 && task_id < 0xFFFFFFFF)
    {
        uint32_t * some_table = (uint32_t *) ARMLIB_OVERFLOWING_BUFFER;
        backup_address = &some_table[task_id-1];
        backup_data = *backup_address;
        qprintf("[BOOT] expecting armlib to overwrite %X: %X (task id %x)\n", backup_address, backup_data, task_id);
        *backup_address = 0xbaaabaaa;
    }
#endif

    ml_used_mem = (uint32_t)&_bss_end - (uint32_t)&_text_start;
    qprintf("[BOOT] autoexec.bin loaded at %X - %X.\n", &_text_start, &_bss_end);
    M50II_INIT_STAGE(2);

    /* early initialization (before Canon's init_task) */
#ifdef BOOT_USE_LOCAL_PRE_INIT_TASK
    qprintf("[BOOT] calling local pre_init_task %X...\n", (uint32_t) &local_pre_init_task);
    local_pre_init_task();
#endif
    qprintf("[BOOT] calling pre_init_task %X...\n", (uint32_t) &boot_pre_init_task);
    M50II_INIT_STAGE(3);
#ifdef M50II_TRACE_SKIP_BOOT_PRE_INIT
    M50II_INIT_STAGE(14);
#else
    boot_pre_init_task();
#endif
    M50II_INIT_STAGE(4);

    // Prepare to call Canon's init_task
    init_task_func init_task_func = &init_task;

#ifdef BOOT_USE_INIT_TASK_PATCHED
    /* use a patched version of Canon's init_task */
    /* this call will also tell us how much memory we have reserved for autoexec.bin */
    init_task_func = init_task_patched();
#endif
    M50II_INIT_STAGE(5);

    qprintf("[BOOT] reserved %d bytes for ML (used %d)\n", ml_reserved_mem, ml_used_mem);

    /* ensure binary is not too large */
    if (ml_used_mem > ml_reserved_mem)
    {
        qprint("[BOOT] out of memory.\n");
        M50II_INIT_STOP(13);

        while(1)
        {
            info_led_blink(3, 500, 500);
            info_led_blink(3, 100, 500);
            msleep(1000);
        }
    }
    M50II_INIT_STAGE(6);

#ifdef M50II_TRACE_REENABLE_BOOT_IRQS_BEFORE_INIT
    M50II_INIT_STAGE(10);
    MEM(0xC1001100) = (1U << 31);
    MEM(0xC1001104) = (1U << 0) | (1U << 1) | (1U << 11);
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    M50II_INIT_STAGE(11);
#endif

#ifdef M50II_TRACE_REENABLE_IRQ32_33_43_BEFORE_INIT
    M50II_INIT_STAGE(10);
    MEM(0xC1001104) = (1U << 0);
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    M50II_INIT_STAGE(11);
    MEM(0xC1001104) = (1U << 1);
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    M50II_INIT_STAGE(12);
    MEM(0xC1001104) = (1U << 11);
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    M50II_INIT_STAGE(13);
#endif

#ifdef M50II_TRACE_CLEAR_ENABLE_IRQ32_BEFORE_INIT
    M50II_INIT_STAGE(10);
    MEM(0xC1001284) = (1U << 0);
    MEM(0xC1001384) = (1U << 0);
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    M50II_INIT_STAGE(11);
    MEM(0xC1001104) = (1U << 0);
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    M50II_INIT_STAGE(12);
#endif

#ifdef M50II_TRACE_CLEAR_ENABLE_IRQ33_43_BEFORE_INIT
    M50II_INIT_STAGE(10);
    MEM(0xC1001284) = (1U << 1);
    MEM(0xC1001384) = (1U << 1);
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    M50II_INIT_STAGE(11);
    MEM(0xC1001104) = (1U << 1);
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    M50II_INIT_STAGE(12);
    MEM(0xC1001284) = (1U << 11);
    MEM(0xC1001384) = (1U << 11);
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    M50II_INIT_STAGE(13);
    MEM(0xC1001104) = (1U << 11);
    __asm__ volatile("dmb sy\n dsb sy" ::: "memory");
    M50II_INIT_STAGE(14);
#endif

    // memory check OK, call Canon's init_task
    qprintf("[BOOT] starting init_task %X...\n", &init_task_func);
    M50II_INIT_STAGE(7);
    int ans = init_task_func(a,b,c,d);
    M50II_INIT_STAGE(8);


#ifdef ARMLIB_OVERFLOWING_BUFFER
    // Restore the overwritten value.
    // Refuse to boot if ARMLIB_OVERFLOWING_BUFFER is incorrect.
    qprintf("[BOOT] %X now contains %X, restoring %X.\n", backup_address, *backup_address, backup_data);
    while (backup_address == 0);
    while (*backup_address == 0xbaaabaaa);
    *backup_address = backup_data;
#endif

    /* regular initialization (usually just launching new tasks) */
    /* we can't do much from here, as the memory reserved for this task is very limited */
#ifdef BOOT_USE_LOCAL_POST_INIT_TASK
    qprintf("[BOOT] calling local post_init_task %X...\n", (uint32_t) &local_post_init_task);
    local_post_init_task();
#endif
    qprintf("[BOOT] calling post_init_task %X...\n", (uint32_t) &boot_post_init_task);
    boot_post_init_task();
    M50II_INIT_STAGE(9);
#ifdef M50II_TRACE_POST_INIT_FIO_PROBE
    m50ii_post_init_fio_probe();
#endif

    qprintf("[BOOT] my_init_task completed.\n");
    M50II_INIT_STOP(15);
    return ans;
}

/** Zeroes out bss */
/** To be called from copy_and_restart. */
static inline void
zero_bss( void )
{
    uint32_t *bss = _bss_start;
    while( bss < _bss_end )
        *(bss++) = 0;
}

/* Cannot use qprintf in boot-*.c for debugging (no snprintf). */
/* You may use qprint/qprintn instead. */
#define qprintf qprintf_not_available
