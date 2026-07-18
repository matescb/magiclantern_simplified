/* Duplex UART serial debug harness for the M50 Mark II (DIGIC 8, fw 1.0.1).
 *
 * TX: m50ii_uart_str (boot-d678.c), mirroring uart_write_bytes @0xE007D814
 *     (TX data 0xC0800000, status 0xC0800014, TX-ready = bit1).
 * RX: recovered from Canon's polled reader FUN_e007d878 / RX ISR FUN_e007da46:
 *     RX data 0xC0800004, status 0xC0800014 bit0 = RX ready (ack = write 1),
 *     bits 0x38 = RX errors (ack = write 0x38). Phase 0 confirmed on HW that the
 *     duplex link works and Canon's RX ISR is dormant, so poll mode is safe.
 *
 * This is the camera half of a two-sided harness: a host daemon (tools/mlserial)
 * owns the port and issues commands so the operator never hand-types. To make
 * host parsing deterministic:
 *   - a command line may start with "@<seq>" (echoed back in the terminator);
 *   - every command prints its body then a terminator "=ok @<seq>" or
 *     "=err @<seq> <msg>";
 *   - asynchronous log lines keep the "[ML] ..." prefix, so the host routes them
 *     to the log rather than into a command response.
 *
 * Commands:
 *   help                     list commands
 *   ver                      build / camera identity
 *   md <addr> [nwords]       dump 32-bit words (default 0x10, max 0x100)
 *   rd <addr>                read one 32-bit word
 *   dump <addr> <len>        bulk read -> hex "D" lines + "=crc" (host verifies)
 *   edmac                    dump the raw-LV EDMAC block (0xd0420200)
 *   watch <addr> [ms] [n]    sample a word over time, report changes
 *   key <code>               inject a GUI keycode (live menu-nav debugging)
 *   wr <addr> <val>          write a 32-bit word   (REFUSED for addr >= ROM)
 *   call <addr> [a b c d]    call a function pointer, print return
 *   load <hexbytes>          stage a code/data blob into the RAM scratch buffer
 *   exec [a b c d]           cache-sync + call the scratch blob (thumb), print ret
 *
 * Safety: the only hard rail is "never write to ROM" -- wr/load refuse any target
 * at or above 0xE0000000 (rom0/rom1 and their mirrors). Everything else is
 * permitted; a bad RAM/MMIO write or call faults at worst, recoverable by a cold
 * boot. dump/md/rd read anything (reading ROM is useful). call may target ROM
 * (executing ROM code is the point of RE); it does not write.
 */

#include "dryos.h"

#ifdef CONFIG_M50mk2

extern void m50ii_handle_canon_gui_event(uint32_t type, uint32_t p1, uint32_t p2);  /* gui.c */
extern void sync_caches(void);                                                      /* arm-mcr.h */

#ifndef RAW_LV_EDMAC_CHANNEL_ADDR
#define RAW_LV_EDMAC_CHANNEL_ADDR 0xd0420200
#endif

/* --- UART channel-0 MMIO --- */
#define M50II_UART_TX      0xC0800000
#define M50II_UART_RX      0xC0800004
#define M50II_UART_STATUS  0xC0800014
#define M50II_UART_RX_RDY  0x1
#define M50II_UART_TX_RDY  0x2
#define M50II_UART_RX_ERR  0x38

/* the one hard safety rail: refuse writes into ROM (rom0/rom1 + mirrors) */
#define M50II_ROM_BASE     0xE0000000u
#define IS_ROM(a)          ((uint32_t)(a) >= M50II_ROM_BASE)

#define SH_LINE_MAX    160
#define SH_MAX_ARGS    10
#define SH_MD_DEFAULT  0x10
#define SH_MD_MAX      0x100
#define SH_DUMP_MAX    0x100000     /* 1 MiB cap on a single serial dump */
#define SH_WATCH_MAX   0x400        /* max samples */
#define SH_SCRATCH_LEN 256          /* code/data staging buffer (RAM) */
#define SH_SEQ_MAX     16

static uint8_t  sh_scratch[SH_SCRATCH_LEN] __attribute__((aligned(8)));
static uint32_t sh_scratch_len = 0;
static char     sh_seq[SH_SEQ_MAX];         /* current command's @seq (may be "") */

/* ---- low-level UART ---- */
static void sh_putc(char ch)
{
    volatile uint32_t *st = (volatile uint32_t *)M50II_UART_STATUS;
    volatile uint32_t *tx = (volatile uint32_t *)M50II_UART_TX;
    int guard = 2000000;
    while (!(*st & M50II_UART_TX_RDY) && --guard) { }
    *st = M50II_UART_TX_RDY;
    *tx = (uint32_t)(unsigned char)ch;
}

/* RX must be read from Canon's ring buffer, not the raw register: Canon's UART
 * RX ISR (FUN_e007da46) drains 0xC0800004 into a RAM ring and signals a sema, so
 * the register is always empty by the time we poll it. FUN_e007d922 is Canon's
 * own ring consumer (ch, buf, nbytes, *actual) -- it takes the ring lock and the
 * RX sema, so it is safe against the ISR and the other CPU core. We only call it
 * when the ring count is > 0, so the sema is already signaled (no blocking).
 * Ring struct layout (ints, base = *(0xe007db48) + ch*0x24):
 *   [0]=lock [2]=RXsema [4]=count [5]=wptr [6]=rptr [7]=start [8]=end. */
typedef int (*uart_ring_read_fn)(int ch, void *buf, int n, int *actual);
#define UART_RING_READ       ((uart_ring_read_fn)(0xe007d922u | 1u))  /* thumb entry */
#define UART_RING_ARRAY_PTR  0xe007db48u   /* *(this) = RAM ring-array base */

static uint32_t sh_ring_count(void)
{
    uint32_t ring0 = *(volatile uint32_t *)UART_RING_ARRAY_PTR;   /* ch-0 struct */
    return *(volatile uint32_t *)(ring0 + 0x10);                  /* [4] = count */
}

/* per-path receive counters, so the diagnostic shows WHERE bytes actually land */
static uint32_t sh_reg_rx = 0;    /* bytes taken from the raw RX register */
static uint32_t sh_ring_rx = 0;   /* bytes taken by tailing Canon's RX ring */
static uint8_t *sh_rx_cursor = 0; /* our private read position in the ring buffer */

/* ch0 ring metadata pointers (base = *(0xe007db48), fields as ints):
 *   [4]=count [5]=wptr [6]=rptr [7]=start [8]=end */
static volatile uint32_t *sh_ring_meta(void)
{
    return (volatile uint32_t *)(*(volatile uint32_t *)UART_RING_ARRAY_PTR);
}

/* Read one received byte. Canon's debug console owns ch0 RX: its ISR copies each
 * byte into the ring at [7] and advances the write pointer [5], and its console
 * reader drains the ring (count/read-pointer) before our task can poll -- which
 * is why reading the register or the ring COUNT finds nothing. So instead we TAIL
 * the ring by its write pointer with our own cursor, never touching count/rptr:
 * the ISR advances wptr on every byte regardless of who consumes, so we see them
 * all without fighting Canon. Path A (raw register) still handles the case where
 * no RX ISR is active (e.g. very early boot). */
static int sh_getc(void)
{
    /* path A: raw RX register (works when no RX ISR is armed) */
    volatile uint32_t *st = (volatile uint32_t *)M50II_UART_STATUS;
    uint32_t s = *st;
    if (s & M50II_UART_RX_ERR) *st = M50II_UART_RX_ERR;
    if (s & M50II_UART_RX_RDY)
    {
        int ch = (int)(unsigned char)*(volatile uint32_t *)M50II_UART_RX;
        *st = M50II_UART_RX_RDY;      /* ack */
        sh_reg_rx++;
        return ch;
    }

    /* path B: tail Canon's ch0 RX ring by its write pointer */
    volatile uint32_t *meta = sh_ring_meta();
    uint8_t *wptr  = (uint8_t *)meta[5];
    uint8_t *start = (uint8_t *)meta[7];
    uint8_t *end   = (uint8_t *)meta[8];
    if (start == 0 || end <= start) return -1;            /* ring not initialized */
    if (sh_rx_cursor == 0 || sh_rx_cursor < start || sh_rx_cursor >= end)
        sh_rx_cursor = wptr;                              /* (re)sync to live wptr */
    if (sh_rx_cursor == wptr) return -1;                  /* no new bytes */
    int ch = (int)(unsigned char)*sh_rx_cursor;
    sh_rx_cursor++;
    if (sh_rx_cursor >= end) sh_rx_cursor = start;        /* wrap */
    sh_ring_rx++;
    return ch;
}

static void sh_puts(const char *s)
{
    for (; *s; s++) { if (*s == '\n') sh_putc('\r'); sh_putc(*s); }
}

static const char SH_HX[] = "0123456789ABCDEF";
static void sh_puthex(uint32_t v)   /* 0x........ */
{
    sh_putc('0'); sh_putc('x');
    for (int i = 28; i >= 0; i -= 4) sh_putc(SH_HX[(v >> i) & 0xF]);
}
static void sh_puthex2(uint8_t b)   /* two nibbles, no prefix */
{
    sh_putc(SH_HX[(b >> 4) & 0xF]); sh_putc(SH_HX[b & 0xF]);
}

static void sh_prompt(void) { sh_puts("ml> "); }

/* terminators carry the @seq so the host can correlate replies */
static void sh_end_ok(void)
{
    sh_puts("=ok");
    if (sh_seq[0]) { sh_puts(" @"); sh_puts(sh_seq); }
    sh_puts("\n");
}
static void sh_end_err(const char *msg)
{
    sh_puts("=err");
    if (sh_seq[0]) { sh_puts(" @"); sh_puts(sh_seq); }
    sh_puts(" "); sh_puts(msg); sh_puts("\n");
}

/* ---- helpers ---- */
static uint32_t sh_parse_num(const char *s, int *ok)
{
    uint32_t v = 0;
    *ok = 0;
    if (!s || !*s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        s += 2;
        if (!*s) return 0;
        for (; *s; s++)
        {
            char c = *s; uint32_t d;
            if      (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            v = (v << 4) | d;
        }
    }
    else
    {
        for (; *s; s++) { if (*s < '0' || *s > '9') return 0; v = v * 10 + (*s - '0'); }
    }
    *ok = 1;
    return v;
}

static int sh_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int sh_tokenize(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < SH_MAX_ARGS)
    {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

static int sh_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static uint32_t sh_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
    crc = ~crc;
    while (n--)
    {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int)(crc & 1)));
    }
    return ~crc;
}

/* ---- commands ---- */
static void cmd_help(void)
{
    sh_puts("md <a> [n] | rd <a> | dump <a> <len> | edmac | watch <a> [ms] [n]\n");
    sh_puts("key <code> | wr <a> <v> | call <a> [args] | load <hex> | exec [args] | ver\n");
    sh_puts("writes to ROM (>=0xE0000000) are refused; everything else is live.\n");
    sh_end_ok();
}

static void cmd_ver(void)
{
    sh_puts("ML M50mk2 1.0.1");
#ifdef VERSION
    sh_puts(" "); sh_puts(VERSION);
#endif
    sh_puts("\n");
    sh_end_ok();
}

static void sh_dump_words(uint32_t addr, uint32_t n)
{
    addr &= ~3u;
    if (n == 0) n = 1;
    if (n > SH_MD_MAX) n = SH_MD_MAX;
    for (uint32_t i = 0; i < n; i++)
    {
        if ((i & 3) == 0) { if (i) sh_puts("\n"); sh_puthex(addr + i * 4); sh_puts(":"); }
        sh_puts(" ");
        sh_puthex(*(volatile uint32_t *)(addr + i * 4));
    }
    sh_puts("\n");
}

static void cmd_md(int argc, char **argv)
{
    if (argc < 2) { sh_end_err("usage: md <addr> [nwords]"); return; }
    int ok = 0;
    uint32_t addr = sh_parse_num(argv[1], &ok);
    if (!ok) { sh_end_err("bad addr"); return; }
    uint32_t n = SH_MD_DEFAULT;
    if (argc >= 3) { n = sh_parse_num(argv[2], &ok); if (!ok) { sh_end_err("bad nwords"); return; } }
    sh_dump_words(addr, n);
    sh_end_ok();
}

static void cmd_rd(int argc, char **argv)
{
    if (argc < 2) { sh_end_err("usage: rd <addr>"); return; }
    int ok = 0;
    uint32_t addr = sh_parse_num(argv[1], &ok);
    if (!ok) { sh_end_err("bad addr"); return; }
    sh_dump_words(addr, 1);
    sh_end_ok();
}

/* bulk read as little-endian bytes, 32 bytes per "D <addr> <hex>" line, then "=crc" */
static void cmd_dump(int argc, char **argv)
{
    if (argc < 3) { sh_end_err("usage: dump <addr> <len>"); return; }
    int ok1 = 0, ok2 = 0;
    uint32_t addr = sh_parse_num(argv[1], &ok1) & ~3u;
    uint32_t len  = sh_parse_num(argv[2], &ok2);
    if (!ok1 || !ok2) { sh_end_err("bad args"); return; }
    if (len > SH_DUMP_MAX) { sh_end_err("len too big (max 0x100000)"); return; }
    uint32_t nwords = (len + 3) / 4;
    uint32_t crc = 0;
    uint32_t bytes_on_line = 0;
    for (uint32_t i = 0; i < nwords; i++)
    {
        if (bytes_on_line == 0) { sh_puts("D "); sh_puthex(addr + i * 4); sh_puts(" "); }
        uint32_t w = *(volatile uint32_t *)(addr + i * 4);
        uint8_t b[4] = { (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24) };
        for (int k = 0; k < 4; k++) sh_puthex2(b[k]);
        crc = sh_crc32(crc, b, 4);
        bytes_on_line += 4;
        if (bytes_on_line >= 32) { sh_puts("\n"); bytes_on_line = 0; }
    }
    if (bytes_on_line) sh_puts("\n");
    sh_puts("=crc "); sh_puthex(crc); sh_puts("\n");
    sh_end_ok();
}

static void cmd_edmac(void)
{
    sh_puts("raw-LV EDMAC @"); sh_puthex(RAW_LV_EDMAC_CHANNEL_ADDR);
    sh_puts(" (0=inactive/wrong channel):\n");
    sh_dump_words(RAW_LV_EDMAC_CHANNEL_ADDR, 0x10);
    sh_end_ok();
}

static void cmd_watch(int argc, char **argv)
{
    if (argc < 2) { sh_end_err("usage: watch <addr> [ms] [n]"); return; }
    int ok = 0;
    uint32_t addr = sh_parse_num(argv[1], &ok) & ~3u;
    if (!ok) { sh_end_err("bad addr"); return; }
    uint32_t ms = 200, n = 20;
    int k;
    if (argc >= 3) { ms = sh_parse_num(argv[2], &k); if (!k) ms = 200; }
    if (argc >= 4) { n  = sh_parse_num(argv[3], &k); if (!k) n = 20; }
    if (n > SH_WATCH_MAX) n = SH_WATCH_MAX;
    if (ms < 5) ms = 5;
    uint32_t last = ~*(volatile uint32_t *)addr;   /* force first print */
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t v = *(volatile uint32_t *)addr;
        if (v != last) { sh_puts("W "); sh_puthex(i); sh_puts(" "); sh_puthex(v); sh_puts("\n"); last = v; }
        msleep(ms);
    }
    sh_puts("W final "); sh_puthex(*(volatile uint32_t *)addr); sh_puts("\n");
    sh_end_ok();
}

static void cmd_key(int argc, char **argv)
{
    if (argc < 2) { sh_end_err("usage: key <code> [state]"); return; }
    int ok = 0;
    uint32_t code = sh_parse_num(argv[1], &ok);
    if (!ok) { sh_end_err("bad code"); return; }
    uint32_t state = 0;
    if (argc >= 3) { int k; state = sh_parse_num(argv[2], &k); if (!k) state = 0; }
    /* ML-BRIDGE-ONLY injection: calls the ML bridge directly, bypassing Canon's queue.
     * Use this to test ML's menu/nav logic IN ISOLATION (the #2/#3 isolation test). */
    m50ii_handle_canon_gui_event(0, code, state);
    sh_puts("injected key "); sh_puthex(code); sh_puts(" state "); sh_puthex(state); sh_puts("\n");
    sh_end_ok();
}

/* [M50II] inject a FULL synthetic button press via Canon's gui_enqueue_message @0xE021DCB2:
 * it writes the event to the ring AND posts to Canon's queue, so BOTH Canon's gui_main_task
 * and ML's ring-peek process it -- a realistic hardware press. Use `key` for ML-bridge-only.
 * `btn <keycode> [state]`  (state 0=press, 1=repeat, 2=release). */
static void cmd_btn(int argc, char **argv)
{
    if (argc < 2) { sh_end_err("usage: btn <keycode> [state]"); return; }
    int ok = 0;
    uint32_t code = sh_parse_num(argv[1], &ok);
    if (!ok) { sh_end_err("bad code"); return; }
    uint32_t state = 0;
    if (argc >= 3) { int k; state = sh_parse_num(argv[2], &k); if (!k) state = 0; }
    void (*gui_enqueue)(uint32_t, uint32_t, uint32_t, uint32_t) =
        (void (*)(uint32_t, uint32_t, uint32_t, uint32_t))(0xE021DCB2u | 1u);  /* thumb */
    gui_enqueue(0, code, state, 0);
    sh_puts("btn enqueued code "); sh_puthex(code); sh_puts(" state "); sh_puthex(state); sh_puts("\n");
    sh_end_ok();
}

static void cmd_wr(int argc, char **argv)
{
    if (argc < 3) { sh_end_err("usage: wr <addr> <val>"); return; }
    int ok1 = 0, ok2 = 0;
    uint32_t addr = sh_parse_num(argv[1], &ok1) & ~3u;
    uint32_t val  = sh_parse_num(argv[2], &ok2);
    if (!ok1 || !ok2) { sh_end_err("bad args"); return; }
    if (IS_ROM(addr)) { sh_end_err("write to ROM blocked"); return; }
    *(volatile uint32_t *)addr = val;
    sh_puts("wrote "); sh_puthex(val); sh_puts(" -> "); sh_puthex(addr); sh_puts("\n");
    sh_end_ok();
}

static void cmd_call(int argc, char **argv)
{
    if (argc < 2) { sh_end_err("usage: call <addr> [a b c d]"); return; }
    int ok = 0;
    uint32_t addr = sh_parse_num(argv[1], &ok);
    if (!ok) { sh_end_err("bad addr"); return; }
    uint32_t a[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4 && (i + 2) < argc; i++) { int k; a[i] = sh_parse_num(argv[i + 2], &k); if (!k) a[i] = 0; }
    uint32_t (*fn)(uint32_t, uint32_t, uint32_t, uint32_t) =
        (uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))addr;
    uint32_t r = fn(a[0], a[1], a[2], a[3]);
    sh_puts("ret "); sh_puthex(r); sh_puts("\n");
    sh_end_ok();
}

/* stage a blob of hex bytes into the RAM scratch buffer (never ROM) */
static void cmd_load(int argc, char **argv)
{
    if (argc < 2) { sh_end_err("usage: load <hexbytes>"); return; }
    const char *h = argv[1];
    uint32_t n = 0;
    while (h[0] && h[1])
    {
        int hi = sh_hexval(h[0]), lo = sh_hexval(h[1]);
        if (hi < 0 || lo < 0) { sh_end_err("bad hex"); return; }
        if (n >= SH_SCRATCH_LEN) { sh_end_err("blob too big"); return; }
        sh_scratch[n++] = (uint8_t)((hi << 4) | lo);
        h += 2;
    }
    if (h[0]) { sh_end_err("odd hex length"); return; }
    sh_scratch_len = n;
    sh_puts("loaded "); sh_puthex(n); sh_puts(" bytes @"); sh_puthex((uint32_t)(uintptr_t)sh_scratch); sh_puts("\n");
    sh_end_ok();
}

/* cache-sync then call the staged blob as a thumb function */
static void cmd_exec(int argc, char **argv)
{
    if (sh_scratch_len == 0) { sh_end_err("no blob loaded"); return; }
    uint32_t a[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4 && (i + 1) < argc; i++) { int k; a[i] = sh_parse_num(argv[i + 1], &k); if (!k) a[i] = 0; }
    sync_caches();
    uint32_t (*fn)(uint32_t, uint32_t, uint32_t, uint32_t) =
        (uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))((uintptr_t)sh_scratch | 1u);  /* thumb */
    uint32_t r = fn(a[0], a[1], a[2], a[3]);
    sh_puts("ret "); sh_puthex(r); sh_puts("\n");
    sh_end_ok();
}

/* --- state-object transition spy (mlv_lite EVF_STATE hunt) ---
 * Canon state_object: +0x0c = StateTransition(self,x,input,z,t), +0x1c = current_state.
 * We replace a target object's transition ptr with sh_state_spy, which records the
 * transition (count + last input/old/new) into globals then calls the original.
 * NO UART I/O in the spy (it runs in Canon's transition/interrupt context) -- poll the
 * record with "statespy" (no arg). Lets us find which state object fires once per LV
 * frame (count climbs ~30/s) and its exact (input,old_state) for the D8 vsync branch. */
typedef int (*sh_state_fn)(void *self, int x, int input, int z, int t);
static void      *g_spy_obj  = 0;      /* hooked state_object (0 = none) */
static sh_state_fn g_spy_orig = 0;     /* its original transition fn */
static volatile uint32_t g_spy_count = 0;
static volatile uint32_t g_spy_rec[3] = {0, 0, 0};   /* last: input, old_state, new_state */

static int sh_state_spy(void *self, int x, int input, int z, int t)
{
    uint32_t old = *(volatile uint32_t *)((char *)self + 0x1c);
    int r = g_spy_orig(self, x, input, z, t);
    g_spy_rec[0] = (uint32_t)input;
    g_spy_rec[1] = old;
    g_spy_rec[2] = *(volatile uint32_t *)((char *)self + 0x1c);
    g_spy_count++;
    return r;
}

static void cmd_statespy(int argc, char **argv)
{
    if (argc < 2)   /* report */
    {
        sh_puts("spy obj=");  sh_puthex((uint32_t)(uintptr_t)g_spy_obj);
        sh_puts(" count=");   sh_puthex(g_spy_count);
        sh_puts(" last in="); sh_puthex(g_spy_rec[0]);
        sh_puts(" ");         sh_puthex(g_spy_rec[1]); sh_puts("->"); sh_puthex(g_spy_rec[2]);
        sh_puts("\n"); sh_end_ok(); return;
    }
    if (sh_streq(argv[1], "off"))
    {
        if (g_spy_obj && g_spy_orig)
            *(volatile void **)((char *)g_spy_obj + 0x0c) = (void *)g_spy_orig;
        g_spy_obj = 0; g_spy_orig = 0;
        sh_puts("spy off\n"); sh_end_ok(); return;
    }
    int ok = 0;
    uint32_t obj = sh_parse_num(argv[1], &ok);
    if (!ok || obj < 0x1000 || IS_ROM(obj)) { sh_end_err("bad objptr"); return; }
    if (g_spy_obj) { sh_end_err("spy busy; 'statespy off' first"); return; }
    volatile void **tptr = (volatile void **)((char *)(uintptr_t)obj + 0x0c);   /* StateTransition */
    g_spy_orig  = (sh_state_fn)(*tptr);
    g_spy_obj   = (void *)(uintptr_t)obj;
    g_spy_count = 0;
    g_spy_rec[0] = g_spy_rec[1] = g_spy_rec[2] = 0;
    *tptr = (void *)((uintptr_t)&sh_state_spy);   /* &fn already has the thumb bit */
    sh_puts("spy on obj="); sh_puthex(obj);
    sh_puts(" orig=");      sh_puthex((uint32_t)(uintptr_t)g_spy_orig);
    sh_puts("\n"); sh_end_ok();
}

static void sh_dispatch(char *line)
{
    char *argv[SH_MAX_ARGS];
    int argc = sh_tokenize(line, argv);
    char **av = argv;

    /* optional leading "@<seq>" for host reply correlation */
    sh_seq[0] = '\0';
    if (argc > 0 && av[0][0] == '@')
    {
        const char *s = av[0] + 1;
        int i = 0;
        while (s[i] && i < SH_SEQ_MAX - 1) { sh_seq[i] = s[i]; i++; }
        sh_seq[i] = '\0';
        av++; argc--;
    }
    if (argc == 0) return;   /* blank line: no terminator, just re-prompt */

    /* Own the UART TX for the duration of the reply so async ML loggers
     * (ml_hb/ml_in via m50ii_uart_str) can't shred it mid-line. */
    extern volatile int g_ml_uart_quiet;
    g_ml_uart_quiet = 1;

    const char *cmd = av[0];
    if      (sh_streq(cmd, "help") || sh_streq(cmd, "?")) cmd_help();
    else if (sh_streq(cmd, "ver"))                        cmd_ver();
    else if (sh_streq(cmd, "md"))                         cmd_md(argc, av);
    else if (sh_streq(cmd, "rd"))                         cmd_rd(argc, av);
    else if (sh_streq(cmd, "dump"))                       cmd_dump(argc, av);
    else if (sh_streq(cmd, "edmac"))                      cmd_edmac();
    else if (sh_streq(cmd, "watch"))                      cmd_watch(argc, av);
    else if (sh_streq(cmd, "key"))                        cmd_key(argc, av);
    else if (sh_streq(cmd, "btn"))                        cmd_btn(argc, av);
    else if (sh_streq(cmd, "wr"))                         cmd_wr(argc, av);
    else if (sh_streq(cmd, "call"))                       cmd_call(argc, av);
    else if (sh_streq(cmd, "load"))                       cmd_load(argc, av);
    else if (sh_streq(cmd, "exec"))                       cmd_exec(argc, av);
    else if (sh_streq(cmd, "statespy"))                   cmd_statespy(argc, av);
    else                                                  sh_end_err("unknown cmd");

    g_ml_uart_quiet = 0;   /* release the TX; async logging resumes */
}

/* ---- the task: line reader + dispatch ---- */
static void m50ii_serial_shell_task(void)
{
    char line[SH_LINE_MAX];
    int len = 0;
    int last_cr = 0;
    int diagc = 0;

    sh_puts("\n[ML] serial harness ready (v4, dual-path rx). type 'help'.\n");

    while (1)
    {
        int got = 0;
        int ch;
        /* Drain ALL available RX bytes this cycle. NO per-character echo: the
         * old echo did a TX spin-wait per byte, which blocked the RX poll long
         * enough for the FIFO-less RX register to overrun and drop the rest of
         * the command. Reading straight through fixes that. */
        while ((ch = sh_getc()) >= 0)
        {
            got = 1;
            if (ch == '\r' || ch == '\n')
            {
                if (ch == '\n' && last_cr) { last_cr = 0; continue; }  /* swallow LF of CRLF */
                last_cr = (ch == '\r');
                line[len] = '\0';
                sh_dispatch(line);
                len = 0;
                continue;
            }
            last_cr = 0;
            if (ch == 0x08 || ch == 0x7F) { if (len > 0) len--; continue; }
            if (ch >= 0x20 && ch < 0x7F && len < SH_LINE_MAX - 1) line[len++] = (char)ch;
        }

        /* RX diagnostic every ~3s: reception counter + live status/mask regs.
         * Tells the host whether bytes reach our poll (rxcount climbing) and
         * whether Canon's RX interrupt source is masked (msk @0xC0800018). */
        if (++diagc >= 1500)   /* 1500 * 2ms = ~3s */
        {
            diagc = 0;
            {
                volatile uint32_t *meta = sh_ring_meta();
                sh_puts("[ML] serrx reg="); sh_puthex(sh_reg_rx);   /* bytes via raw register */
                sh_puts(" ring=");          sh_puthex(sh_ring_rx);  /* bytes via wptr-tail */
                sh_puts(" wptr=");          sh_puthex(meta[5]);     /* ISR advances on every RX */
                sh_puts(" rptr=");          sh_puthex(meta[6]);     /* Canon console consume */
                sh_puts(" cnt=");           sh_puthex(meta[4]);
                sh_puts("\n");
            }
        }

        if (!got) msleep(2);
    }
}

/* Started from init.c (boot_post_init_task) under the M50II_SERIAL_SHELL gate. */
void m50ii_serial_shell_start(void)
{
    task_create("ml_serial", 0x1f, 0x1000, m50ii_serial_shell_task, 0);
}

#endif /* CONFIG_M50mk2 */
