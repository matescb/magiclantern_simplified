#ifndef __PLATFORM_MMU_PATCHES_H__
#define __PLATFORM_MMU_PATCHES_H__

#include "patch.h"

#if CONFIG_FW_VERSION == 101 // ensure our hard-coded patch addresses are not broken
                             // by a FW upgrade (M50 Mark II 1.0.1)

// Data patches this early on Digic 8 don't seem to work.
// It looks like ROM1 isn't properly mapped or initialised
// at that time, so accesses fail.
struct patch early_data_patches[] =
{

};

struct patch normal_data_patches[] =
{

};


struct function_hook_patch early_code_patches[] =
{

};

/* ---------------------------------------------------------------------------
 * [M50II Phase 1] GUI input exclusivity
 *
 * Problem: the M50 II menu bridge is a passive, read-only peek of Canon's GUI
 * event ring (m50ii_input_task -> m50ii_handle_canon_gui_event). Canon's
 * gui_main_task ALSO dispatches every event via funcs[event->type](), so while
 * the ML overlay menu is open every button ALSO drives Canon's UI:
 *   funcs[0] @0xE021E460 -> gui_massive_event_loop @0xE03FD182
 *   (keycode -> GUICMD -> GuiEventPostToWinSys). This is the "leakage":
 *   MENU opens Canon config (#4), nav keys move Canon's AF/UI and desync the
 *   per-mode keycode map (#2/#3/#5). Every other ML port overrides gui_main_task
 *   and drains its queue, which consumes events for free; on the M50 II that
 *   override faulted (EVF cascade), so we add the narrowest possible consumption:
 *   a code hook of the keypress handler funcs[0] that DROPS the event whenever
 *   the ML menu is shown. The read-only bridge still sees the event and moves
 *   the ML highlight; Canon's translator never runs -> full input exclusivity.
 *
 * gui_main_task stack-copies the handler table once at task entry, so patching
 * the ROM table pointer (GMT_FUNCTABLE) after boot does nothing -> we must patch
 * funcs[0]'s code directly.
 *
 * Original funcs[0] prologue (8 bytes replaced by the ldr pc,[pc] hook):
 *   e021e460  01 46      mov  r1, r0
 *   e021e462  78 48      ldr  r0, [pc, #0x1e0]   ; literal @0xe021e644 = 0x5428
 *   e021e464  08 b5      push {r3, lr}
 *   e021e466  83 68      ldr  r3, [r0, #8]
 *   ...continue at 0xe021e468.
 * The ldr r0,[pc,#0x1e0] loads the CONSTANT 0x5428 (gui_main_struct base), so
 * the relocated replay is simply "ldr r0, =0x5428".
 *
 * gui_menu_shown() is declared in menu.h (pulled in via dryos.h -> gui-common.h);
 * the inline-asm "bl gui_menu_shown" is resolved by the linker.
 *
 * m50ii_exclusivity_on is a RUNTIME kill-switch (default 1). `wr &m50ii_exclusivity_on 0`
 * over the serial harness disables the drop live (funcs[0] then runs normally = Canon
 * behaves as stock even with the ML menu open), so exclusivity can be A/B-tested on HW
 * without reflashing -- e.g. to check whether dropping funcs[0] regresses the ML menu's
 * own pad navigation (which depends on Canon's input state machine still running).
 * ------------------------------------------------------------------------- */
int m50ii_exclusivity_on = 0;   /* [2026-07-18] DEFAULT OFF: the funcs[0] drop blocks Canon's
                                 * per-shooting-mode input activation -> breaks ML menu nav (Auto works,
                                 * Video/M do not). Hook stays installed but transparent (replay+continue
                                 * = stock funcs[0]); `wr &m50ii_exclusivity_on 1` re-enables for the
                                 * deep-fix work (SetGUIRequestMode floods EVF -- see docs/33). */

void __attribute__((noreturn, noinline, naked, aligned(4)))
hook_gui_funcs0_exclusivity(void)
{
    asm volatile(
        // Preserve caller-saved regs AND the original return addr (lr) across
        // the C call. r0 = the funcs[0] arg (the GUI event ptr); we need it,
        // r3 and lr intact to replay the original prologue below.
        "push   {r0-r3, r12, lr}       \n"
        "bl     gui_menu_shown          \n"   // r0 = is the ML menu shown?
        "ldr    r1, =m50ii_exclusivity_on\n"
        "ldr    r1, [r1]               \n"    // r1 = runtime kill-switch (0 = disabled)
        "cmp    r0, #0                 \n"     // menu shown?
        "it     ne                     \n"
        "cmpne  r1, #0                 \n"     // ...and enabled? (Z clear iff shown && enabled)
        "pop    {r0-r3, r12, lr}        \n"   // restore (LDM/POP preserves flags)
        "bne    1f                      \n"   // shown && enabled -> drop the event

        // ML menu closed: replay the overwritten prologue, then rejoin Canon's
        // handler at +8 so stock behaviour is completely unchanged.
        "mov    r1, r0                  \n"   // e021e460
        "ldr    r0, =0x5428             \n"   // e021e462 (relocated PC-rel load)
        "push   {r3, lr}                \n"   // e021e464
        "ldr    r3, [r0, #8]            \n"   // e021e466
        "ldr    pc, =0xe021e469         \n"   // continue at 0xe021e468 (Thumb)

        // Drop path: menu open -> consume the button. Return to gui_main_task
        // (lr restored above) with r0 = 0 (handled); the read-only ML bridge
        // has already processed this same event for the overlay.
    "1:                                 \n"
        "movs   r0, #0                  \n"
        "bx     lr                      \n"
    );
}

struct function_hook_patch normal_code_patches[] =
{
    /* [2026-07-19 BOOT-RELIABILITY] The GUI-input-exclusivity funcs[0] hook below is the ONLY
     * normal code patch. Applying ANY code patch at boot runs mmu_reclaim_init1_page() +
     * change_mmu_tables at big.4a2 -- an MMU-remap-vs-concurrent-FIO coherency race that faults
     * (pc=0xE02F281A lr=0xE05BB769 dfar=0xE0997D80) and the [AB] abort handler then HANGS the boot,
     * killing ~50% of cold boots (watchdog reset). The exclusivity feature is OFF by default
     * (m50ii_exclusivity_on=0) AND known-broken (it blocks Canon per-mode input activation ->
     * breaks Video/M menu nav, see comment above), so even when installed the hook runs
     * TRANSPARENTLY (replay + continue = stock funcs[0]) -- installing it changes NOTHING except
     * adding the racy boot-time MMU op. Empty patch set => COUNT()==0 => apply_normal_patches skips
     * the whole reclaim/change_mmu_tables block => boot clears big.4 reliably, with zero behaviour
     * change. Restore the entry (and set m50ii_exclusivity_on=1) if the exclusivity deep-fix resumes. */
    // {
    //     .patch_addr = 0xe021e460,       // Canon keypress handler funcs[0] (Thumb)
    //     .orig_content = {0x01, 0x46, 0x78, 0x48, 0x08, 0xb5, 0x83, 0x68},
    //     .target_function_addr = (uint32_t)hook_gui_funcs0_exclusivity,
    //     .description = "M50II: GUI input exclusivity (drop buttons while ML menu shown)"
    // },
};

#endif // M50 Mark II FW_VERSION 101

#endif // __PLATFORM_MMU_PATCHES_H__
