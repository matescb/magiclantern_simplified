/**
 * MagicLantern GuiMainTask override
 * This was previously camera-specific
 **/

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

#include <gui.h>

#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <config.h>
#include <consts.h>
#include <lens.h>
#include <config-defines.h>
#include <boot-hack.h>
#include <menu.h>

/**
 * Supported cameras [E] Means it's enabled
 * [E] 1100D: counter_0x0c <-> msg_queue_0x30
 * [E] 600D : counter_0x0c <-> msg_queue_0x30
 * [E] 60D  : counter_0x0c <-> msg_queue_0x30
 * [E] 650D : counter_0x0c <-> msg_queue_0x30
 * [E] EOSM : counter_0x0c <-> msg_queue_0x30
 * [E] 700D : counter_0x0c <-> msg_queue_0x30
 * [E] 100D : counter_0x0c <-> msg_queue_0x30
 * [E] 5D3  : counter_0x0c <-> msg_queue_0x30
 * [E] 6D   : counter_0x0c <-> msg_queue_0x30
 */

/**
 * Easy to support cameras
 * [E] 550D : counter_0x04 <-> msg_queue_0x38
 * [D] 7D   : counter_0x04 <-> msg_queue_0x38
 */

/**
 * Unsupported cameras for now
 * 5D2  : counter_0x04 <-> msg_queue_0x34
 * 50D  : counter_0x04 <-> msg_queue_0x34
 * 500D : counter_0x04 <-> msg_queue_0x34
 */

struct semaphore * gui_sem;

// return 0 if you want to block this event
static int handle_buttons(struct event * event)
{
    ASSERT(event->type == 0)

    if (event->type != 0) return 1; // only handle events with type=0 (buttons)
    if (handle_common_events_startup(event) == 0) return 0;
    extern int ml_started;
    if (!ml_started) return 1;

// SJE Want to log button presses?  Uncomment the following line
//    DryosDebugMsg(0,15,"event: %08x", event->param);
    if (handle_common_events_by_feature(event) == 0) return 0;

    return 1;
}

/** TODO: rename stuff in this structure, don't use camera names maybe?
 * Other possibility, move in-structure offset to platform dir and define
 * per camera. */
struct gui_main_struct {
  void *          obj;                     // off_0x00;
  uint32_t        counter_550d;
  uint32_t        off_0x08;
  uint32_t        counter;                 // off_0x0c;
  uint32_t        off_0x10;
  uint32_t        off_0x14;
  struct msg_queue *    msg_queue_m50;     // off_0x18;
  struct msg_queue *    msg_queue_eosr;    // off_0x1C;
  uint32_t        off_0x20;
  uint32_t        off_0x24;
  uint32_t        off_0x28;
  uint32_t        off_0x2c;
  struct msg_queue *    msg_queue;         // off_0x30;
  struct msg_queue *    off_0x34;          // off_0x34;
  struct msg_queue *    msg_queue_550d;    // off_0x38;
  uint32_t        off_0x3c;
};

extern struct gui_main_struct gui_main_struct;

/* [M50II Phase-2 2026-07-11] Root-cause levers for the GraphicsConvertPosType.c:9
 * assert (pos_type=11). ml_gui_main_task is a full TASK_OVERRIDE replacement of
 * Canon's gui_main_task and dispatches funcs[index](event) with a SINGLE argument.
 * On DIGIC 8 the GUI message grew to 5x32-bit (0x14) and Canon's real gui_main_task
 * forwards the extra field as a second dispatch parameter; a handler that reads a
 * stale register in place of that param can derive an out-of-range position type ->
 * the converter's default:Assert(...,9). Two gated levers (both default OFF so the
 * Phase-1 pool/NOP validation build is unaffected):
 *   M50II_BYPASS_GUI_OVERRIDE - skip ML's override entirely so Canon's own
 *       gui_main_task runs (bilal's R8 finding: removing the override let it boot).
 *       This is the escalation root-fix if Phase-1's NOP proves insufficient; note it
 *       also disables ML's button/menu hooks, so it is a bring-up A/B probe, not a
 *       permanent fix.
 *   M50II_GUI_TRACE - log the first N GUI events' type/param over UART so the event
 *       stream immediately preceding the assert can be identified on real hardware. */
#if defined(CONFIG_M50mk2)
#ifndef M50II_BYPASS_GUI_OVERRIDE
#define M50II_BYPASS_GUI_OVERRIDE 1   /* [2026-07-12 iter15] override OFF permanently -- PIVOT away from replacing
                                       * gui_main_task (6 override iters iter9-14 all failed: the swapped task
                                       * never runs ml_gui_main_task; Canon's GUI cascades to faults when disturbed).
                                       * New approach: keep Canon's gui_main_task running, PEEK the GUI event ring
                                       * (m50ii_gui_peek_task in init.c) to read button keycodes read-only. iter12: register the task-switch
                                       * callback (M50II_REGISTER_CB=1) but do NOT override gui_main_task, so
                                       * Canon's own gui_main_task runs. Diagnostic to split the iter9/10
                                       * Edet-flood cause: if this boots clean to big.8 + heartbeat, the
                                       * REGISTRATION is safe and the gui_main_task OVERRIDE is the culprit; if
                                       * it still floods Edet, the registration/m50ii_cb itself breaks Canon on
                                       * HW. Restore to 0 once the cause is known. Original note below:
                                       * [2026-07-11 FIXED->0] Re-enabled. The big.5 crash root was NOT
                                       * the dispatch convention: RE of Canon gui_main_task @0xE021DB7C
                                       * proves it calls funcs[event->type](event) with a SINGLE arg
                                       * (event in r0), exactly like ml_gui_main_task. The real defect
                                       * was the queue offset -- M50mk2 read events from +0x18 (DIGIC-7
                                       * msg_queue_m50) instead of the DIGIC-8 +0x1c (msg_queue_eosr),
                                       * fixed in the dispatch branch below. With the override on, button
                                       * events reach handle_buttons -> ML menus become interactive. */
#endif
#ifndef M50II_GUI_TRACE
#define M50II_GUI_TRACE 0   /* [2026-07-11] ON for the first override-on HW boot: log every received GUI
                             * event's type/param over UART to (a) confirm events flow through the
                             * re-enabled override without crashing and (b) capture real M50 II button
                             * keycodes for the gui.h placeholder fix. Set back to 0 once recorded. */
#endif
#ifndef M50II_GUI_TRANSPARENT
#define M50II_GUI_TRANSPARENT 1   /* [2026-07-11 iter10] TRANSPARENT dispatch: behave EXACTLY like Canon's
                                   * gui_main_task -- forward EVERY received event to funcs[type](event) with
                                   * NO ML interception (no handle_buttons/handle_other_events, no event
                                   * swallowing, no gui_init_end). iter9 proved the task override fires on HW,
                                   * but ML's swallowing of D8 GUI events broke Canon's Edet/scene setup
                                   * (Edet::EdetScene.c nullptr flood -> pool panic at the first yield). This
                                   * keeps Canon's GUI fully alive while still tracing every event (GUI_TRACE)
                                   * to capture the real M50 II keycodes. Once boot is clean + keycodes are
                                   * recorded, iter11 re-adds ML button interception for type-0 events ONLY,
                                   * forwarding everything else to Canon. */
#endif
#ifndef M50II_GUI_DIAG
#define M50II_GUI_DIAG 0   /* [2026-07-12 iter12] Instrument ml_gui_main_task to find why the (byte-correct)
                            * clone drains ZERO events: print the queue handle *(gui_main_struct+0x1c) at
                            * ENTER, then for the first 16 loops print msg_queue_receive's return code + the
                            * event ptr. Tells us whether the clone (a) runs at all, (b) reads a valid queue
                            * handle, (c) blocks forever (empty/wrong queue) or returns an error (bad handle).
                            * UART is SAFE here -- ml_gui_main_task is a normal task, not the scheduler
                            * critical section that constrained m50ii_cb. Set 0 once the cause is known. */
#endif
#ifndef M50II_MENU_INPUT
#define M50II_MENU_INPUT 1   /* [2026-07-12 Phase 0-3] Wire M50 II buttons -> ML menu on the iter15
                              * read-only ring peek. init.c's input task reads Canon's GUI event ring
                              * and calls m50ii_handle_canon_gui_event() (below) per event. FN opens the
                              * ML menu; nav keys drive it. 0 = feature off (bridge becomes a no-op). */
#endif
#ifndef M50II_MENU_INPUT_TRACE
#define M50II_MENU_INPUT_TRACE 0   /* UART echo of translated menu-input events during HW bring-up. */
#endif
#if M50II_GUI_TRACE || M50II_GUI_DIAG || M50II_MENU_INPUT_TRACE
extern void m50ii_uart_str(const char *);
static void m50ii_gui_hex(const char *label, uint32_t a, uint32_t b)
{
    static const char hx[] = "0123456789ABCDEF";
    char buf[20];
    int n = 0;
    for (int i = 0; i < 8; i++) buf[n++] = hx[(a >> (28 - i * 4)) & 0xF];
    buf[n++] = ' ';
    for (int i = 0; i < 8; i++) buf[n++] = hx[(b >> (28 - i * 4)) & 0xF];
    buf[n++] = '\n'; buf[n] = 0;
    m50ii_uart_str(label);
    m50ii_uart_str(buf);
}
#endif
#endif // CONFIG_M50mk2

#if defined(CONFIG_M50mk2)
/* ===== M50 II menu input bridge (Phase 0-3, 2026-07-12) ==========================================
 * Real M50 II hardware keycodes, from hw_iter15.log (double-press validated). For every event on
 * Canon's GUI ring, type==0 marks a button/dial event; p1 carries the code; p2 = 0 press / 1 repeat
 * / 2 release. The control dial arrives on its own channel (p1=0xB7) with a struct pointer in p2
 * (Phase 3). These are HARDWARE codes -- distinct from ML's internal BGMT_* tokens in gui.h; this
 * bridge is the ONLY place the two meet, so gui.h needs no functional change. */
/* [2026-07-12 HW pass 2/3, hw_menuinput.log] Codes RE-CAPTURED in the mode the camera boots into --
 * Canon keycodes are MODE-DEPENDENT, so these supersede the iter15 values (captured in a different
 * GUI state). Isolated from the camera's periodic background chatter (0x9F/0xA3/0xA4/0xA0/0xA6/0xB7,
 * which fires ~every 20s on its own) by matching the user's timed press bursts across two passes. */
#define M50II_KEY_MENU   0x0C   /* confirmed this mode */
#define M50II_KEY_INFO   0x0E   /* confirmed this mode */
#define M50II_KEY_SET    0x0A   /* was 0x12; SET press captured as 0x0A here */
#define M50II_KEY_LEFT   0x3E   /* HW: physical LEFT emits 0x3E (0x3C/0x3E were swapped on the first try) */
#define M50II_KEY_RIGHT  0x3C   /* HW: physical RIGHT emits 0x3C */
#define M50II_KEY_UP     0x40   /* HW: 4-way pad UP (ladder: RIGHT 0x3C/LEFT 0x3E/UP 0x40/DOWN 0x42) */
#define M50II_KEY_DOWN   0x42   /* HW: 4-way pad DOWN -- verify direction, swap with UP if inverted */
#define M50II_KEY_FN     0x22   /* was 0x4A; FN press captured as 0x22 here -> opens/closes ML menu */
#define M50II_KEY_DIAL   0xB7   /* control-dial rotation; direction in the struct *p2 (Phase 3 probe).
                                 * NOTE 0xB7 ALSO fires as background -- real up/down decode is deferred. */
/* 0x9F (old PLAY) intentionally DROPPED: it is background noise in this mode and would spam the menu. */

#define M50II_ST_PRESS   0
#define M50II_ST_REPEAT  1
#define M50II_ST_RELEASE 2

/* Feed one raw Canon GUI-ring event into ML. We are the SOLE producer of ML events -- Canon's
 * gui_main_task is NOT overridden (iter9-14 proved that path faults) -- so we drive ML's existing,
 * self-contained menu tasks directly:
 *   FN press   -> gui_open_menu()  : give_semaphore(gui_sem); menu_task (menu.c) toggles open/close,
 *                                    menu_redraw_task draws. Neither needs Canon's gui_main_task.
 *   nav keys   -> handle_buttons(&{type=0,param=BGMT_x}) : runs ML's full nav/menu chain in-place.
 * fake_simple_button()/GUI_Control() are deliberately NEVER used -- GUI_Control is an unresolved ROM
 * stub on this body (stubs.S:221), so they are no-ops here. Navigation is gated to gui_menu_shown():
 * while the ML menu is closed we touch nothing but the FN toggle, preserving the clean-boot,
 * Canon-undisturbed property that made the iter15 read-only peek work. */
void m50ii_handle_canon_gui_event(uint32_t type, uint32_t p1, uint32_t p2)
{
#if M50II_MENU_INPUT
    /* Do NOTHING until ML is fully started. The input task runs from post.0h -- long before big.8 --
     * so a button pressed DURING boot would otherwise reach here. Before ml_started, gui_sem/menu_task
     * don't exist yet, and calling gui_open_menu() (give_semaphore) that early asserts KerSem.c:162 and
     * cascades into a LiveViewDrive::EvfDevState flood that STALLS my_big_init_task (never reaches
     * big.8). Pressing FN before ML is up must be a safe no-op. [2026-07-12 HW: this was the "flaky
     * boot" -- FN pressed at big.3 hung the boot; not the keycode swap.] */
    extern int ml_started;
    if (!ml_started) return;

    if (type != 0) return;             /* only button/dial events (type 0) matter for menu input */

    /* FN toggles the ML menu on the press edge (p2==0); ignore its repeat (1) / release (2). */
    if (p1 == M50II_KEY_FN)
    {
        if (p2 == M50II_ST_PRESS)
        {
#if M50II_MENU_INPUT_TRACE
            m50ii_uart_str(gui_menu_shown()
                ? "[ML] menu-input: FN -> close ML menu\n"
                : "[ML] menu-input: FN -> open ML menu\n");
#endif
            gui_open_menu();           /* give_semaphore(gui_sem) -> menu_task toggles */
        }
        return;
    }

    /* Everything else only acts while the ML menu is open -> Canon left untouched otherwise. */
    if (!gui_menu_shown()) return;

    /* [Phase 3] Control-dial rotation: p1==0xB7, p2 -> a Canon heap struct holding the rotation
     * direction/delta. The struct offset isn't known yet, so PROBE it: on the HW test (open the ML
     * menu, turn the dial CW then CCW) dump the leading words over UART to reveal which field flips
     * sign between directions. BGMT_WHEEL_UP/DOWN then gets wired from that offset (a ~5-line follow
     * up). p2 is bounds-checked to the Canon heap window before deref so a stray pointer can't fault
     * the input task. This probe piggybacks the same boot -- no extra HW cold-boot for the data. */
    if (p1 == M50II_KEY_DIAL)
    {
#if M50II_MENU_INPUT_TRACE
        static int dial_probe_n = 0;
        if (dial_probe_n < 40 && p2 >= 0x00400000 && p2 < 0x02000000)
        {
            volatile uint32_t *s = (volatile uint32_t *)p2;
            dial_probe_n++;
            m50ii_gui_hex("[ML] dial p2/[0] ", p2,   s[0]);
            m50ii_gui_hex("[ML] dial [1]/[2] ", s[1], s[2]);
            m50ii_gui_hex("[ML] dial [3]/[4] ", s[3], s[4]);
            m50ii_gui_hex("[ML] dial [5]/[6] ", s[5], s[6]);
        }
#endif
        return;   /* -> BGMT_WHEEL_UP / BGMT_WHEEL_DOWN once the direction offset is known */
    }

    int bgmt = -1;
    switch (p1)
    {
        case M50II_KEY_MENU:  bgmt = BGMT_MENU;        break;
        case M50II_KEY_INFO:  bgmt = BGMT_INFO;        break;
        /* M50 II center button (Q/SET combined) -> BGMT_Q, which enters submenus
         * (menu_entry_select mode 2 -> menu_toggle_submenu) and opens single-item pickboxes.
         * BGMT_PRESS_SET (mode 3) only toggles/edits and does NOT enter submenus -- that showed the
         * item hint instead of descending, which read as "help". Values still change via LEFT/RIGHT. */
        case M50II_KEY_SET:   bgmt = BGMT_Q;           break;
        case M50II_KEY_LEFT:  bgmt = BGMT_PRESS_LEFT;  break;
        case M50II_KEY_RIGHT: bgmt = BGMT_PRESS_RIGHT; break;
        case M50II_KEY_UP:    bgmt = BGMT_PRESS_UP;    break;
        case M50II_KEY_DOWN:  bgmt = BGMT_PRESS_DOWN;  break;
        default: return;
    }

    /* Act on the initial press edge only (p2==0); repeats/releases would double-step the menu. */
    if (p2 != M50II_ST_PRESS) return;

#if M50II_MENU_INPUT_TRACE
    m50ii_gui_hex("[ML] menu-input: key/bgmt ", p1, (uint32_t)bgmt);
#endif

    struct event ev = { .type = 0, .param = bgmt, .obj = NULL, .arg = 0 };
    handle_buttons(&ev);
#else
    (void)type; (void)p1; (void)p2;   /* M50II_MENU_INPUT disabled -> bridge is a no-op */
#endif
}

/* [Phase 4 -- exclusivity: DESIGNED, deferred by intent] ------------------------------------------
 * This bridge is read-only on Canon's ring, so while the ML menu is open Canon ALSO still receives
 * FN/MENU/nav. In practice ML's menu_open() already calls SetGUIRequestMode(GUIMODE_MENU) (the
 * standard "piggyback" used on every ML camera): Canon enters its own menu mode and ML draws on
 * top, so the underlying Canon state changes are transient and hidden, and are undone when the ML
 * menu closes (SetGUIRequestMode(0)). That is usually sufficient -- true event consumption is only
 * needed if HW shows Canon visibly fighting the ML menu.
 *
 * If it IS needed, the ONLY safe mechanism is a ROM/cache patch of the leaf button handler
 * funcs[0] (0xE021E461) via ML's existing patch manager (apply_normal_patches is already wired),
 * detouring it to early-return when gui_menu_shown(). We deliberately do NOT patch it blind:
 * Canon's gui_main_task re-copies its function table from ROM every loop, and disturbing that path
 * is exactly what made the iter9-14 task-override attempts fault. Activate only after the Phase 1-3
 * HW boot proves it necessary. */
#endif // CONFIG_M50mk2

void ml_gui_main_task()
{
#if defined(CONFIG_M50mk2) && M50II_GUI_DIAG
    /* [iter13] ABSOLUTE-first-instruction trace: proves ml_gui_main_task executes at all + on WHICH core
     * (MPIDR). Must precede the funcs memcpy so a fault there can't hide it. With the iter13 CPU0-pin
     * (m50ii_cb sets cpu_requested=0) this should now print MPIDR=0x80000000 (CPU0), not CPU1. */
    {
        uint32_t mpidr;
        asm volatile ("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
        m50ii_uart_str("[ML] mlgui: ENTER (running)\n");
        m50ii_gui_hex("[ML] mlgui: MPIDR/base ", mpidr, (uint32_t)&gui_main_struct);
    }
#endif
    struct event * event = NULL;
    int index = 0;
    void* funcs[GMT_NFUNCS];
    memcpy(funcs, (void*)GMT_FUNCTABLE, 4*GMT_NFUNCS);

#if !(defined(CONFIG_M50mk2) && M50II_GUI_TRANSPARENT)
    gui_init_end(); // no params?  [M50II iter10: Canon gui_main_task @0xE021DB7C does NOT call this]
#endif

#if defined(CONFIG_M50mk2) && M50II_GUI_DIAG
    /* [iter13] queue handle the loop will block on (post-memcpy, so a memcpy fault shows as missing). */
    m50ii_gui_hex("[ML] mlgui: qhandle(+0x1c) ", (uint32_t)gui_main_struct.msg_queue_eosr, 0);
#endif

    while(1)
    {
        #if defined(CONFIG_550D) || defined(CONFIG_7D)
        msg_queue_receive(gui_main_struct.msg_queue_550d, &event, 0);
        gui_main_struct.counter_550d--;
        #elif defined(CONFIG_R) || defined(CONFIG_RP) || defined(CONFIG_850D) || defined (CONFIG_R5) || defined(CONFIG_SX70) || defined(CONFIG_M6II) || defined(CONFIG_M50mk2)
        /* [M50II 2026-07-11] M50mk2 is DIGIC 8: its Canon gui_main_task (@0xE021DB7C) receives GUI
         * events from gui_main_struct+0x1c (msg_queue_eosr) and decrements the counter at +0x4 --
         * VERIFIED by disassembly: `ldr r0,[r4,#0x1c]; bl msg_queue_receive` then `ldr r0,[r4,#4];
         * subs;str`. It was previously mis-grouped with the DIGIC-7 M50 (+0x18 msg_queue_m50), so
         * the override drained the WRONG queue -> real button events (queued at +0x1c) were never
         * consumed and malformed dispatches drove the big.5 GraphicsConvertPosType crash. */
        int m50ii_rcv_rc = msg_queue_receive(gui_main_struct.msg_queue_eosr, &event, 0);
        gui_main_struct.counter_550d--;
        (void)m50ii_rcv_rc;
#if defined(CONFIG_M50mk2) && M50II_GUI_DIAG
        /* [iter12] Show whether receive returns (blocked forever => no lines) + rc + event ptr. */
        { static int m50ii_rcv_n = 0; if (m50ii_rcv_n < 16) { m50ii_rcv_n++;
              m50ii_gui_hex("[ML] mlgui: rcv rc/ev ", (uint32_t)m50ii_rcv_rc, (uint32_t)event); } }
#endif
        #elif defined(CONFIG_M50) || defined(CONFIG_SX740)
        msg_queue_receive(gui_main_struct.msg_queue_m50, &event, 0);
        gui_main_struct.counter_550d--;
        #else
        msg_queue_receive(gui_main_struct.msg_queue, &event, 0);
        gui_main_struct.counter--;
        #endif

        if (event == NULL) {
            continue;
        }

        index = event->type;

#if defined(CONFIG_M50mk2) && M50II_GUI_TRACE
        /* [2026-07-11] Trace EVERY received GUI event (before ML consumes/blocks any), so button
         * events routed through handle_buttons are captured too. `type` is the dispatch index;
         * `param` carries the button keycode for type-0 (button) events -- this is how we read the
         * real M50 II keycodes to fix the gui.h placeholders. Bounded to avoid a UART flood. */
        {
            static int m50ii_gui_ev_n = 0;
            if (m50ii_gui_ev_n < 128)
            {
                m50ii_gui_ev_n++;
                m50ii_gui_hex("[ML] gui.ev type/param ", (uint32_t)event->type, (uint32_t)event->param);
            }
        }
#endif

#if defined(CONFIG_M50mk2) && M50II_GUI_TRANSPARENT
        /* [2026-07-11 iter10] Transparent dispatch: forward EVERY event straight to Canon's
         * funcs[type](event), exactly like Canon's gui_main_task -- NO ML interception (no
         * handle_buttons/handle_other_events, no swallowing). This keeps Canon's D8 GUI/Edet
         * subsystems alive (iter9's event-swallowing left Edet scene null -> pool panic). The
         * bound-check mirrors Canon's `assert type<=6` (GMT_NFUNCS=7). GUI_TRACE above still
         * logs every event so we capture the real M50 II button keycodes. */
        if (index >= 0 && index < GMT_NFUNCS)
        {
            void (*f)(struct event *) = funcs[index];
            if (f != NULL)
                f(event);
        }
        continue;
#endif

        if (!magic_is_off())
        {

            if (event->type == 0)
            {
                if (handle_buttons(event) == 0) { // ML button/event handler
                    continue;
                }
            }
            else
            {
                if (handle_other_events(event) == 0) {
                    continue;
                }
            }
        }

        if (IS_FAKE(event)) {
           event->arg = 0;      /* do not pass the "fake" flag to Canon code */
        }

        if (event->type == 0 && event->param < 0) {
            continue;           /* do not pass internal ML events to Canon code */
        }

        if ((index >= GMT_NFUNCS) || (index < 0)) {
            continue;
        }

        void(*f)(struct event *) = funcs[index];
        if (f != NULL)
            f(event);
    }
}

#if !(defined(CONFIG_M50mk2) && M50II_BYPASS_GUI_OVERRIDE)
TASK_OVERRIDE( gui_main_task, ml_gui_main_task);
#endif
