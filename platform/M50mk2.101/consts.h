/*
 *  EOS M50 1.1.0 consts
 */

#define CARD_LED_ADDRESS            0xD01300E4     /* maybe also 0xD01301A4 */
#define LEDON                       0xD0002
#define LEDOFF                      0xC0003

#define BR_ICACHE_INV_1     0xE0040072   /* first call to icache_invalidate, before cstart */
#define BR_DCACHE_CLN_1     0xE0040068   /* first call to dcache_clean, before cstart */
#define BR_DCACHE_CLN_2     0xE00400A0   /* second call to dcache_clean, before cstart */
#define BR_ICACHE_INV_2     0xE00400AA   /* second call to icache_invalidate, before cstart */
#define BR_BR_CSTART        0xE00400C0   // indirect branch to cstart; the first branch is
                                         // absolute to original, we must patch
                                         /* [M50II 1.0.1 F1-fix] was 0xE00400C6 (the `bx r0`); patching a
                                          * 4-byte branch there overwrites the copy-loop source literal at
                                          * 0xE00400C8 (=0xE1394358, read by preamble ldr@0xE004003E) and
                                          * corrupts relocation. 0xE00400C0 is the `ldr r0,[pc]->0xE0040035`
                                          * (dead once redirected), matches RP.160. NEEDS HW VERIFY. */
#define BR_BZERO32          0xE0040152   /* called from cstart */
#define BR_CREATE_ITASK     0xE00401B4   /* called from cstart */

// Used for copying and modifying ROM code before transferring control.
// Keep this deliberately wider than the exact patched branch range; cstart may
// read nearby literal pools/data before create_init_task is called.
#define FIRMWARE_ENTRY_LEN 0x800

#define CANON_ORIG_MMU_TABLE_ADDR 0xe0000000 // Yes, this is the rom start, yes, there is code there.
                                             // I assume ARM MMU alignment magic means this is okay,
                                             // presumably the tables themselves don't use the early part.
                                             // I don't have an exact ref in ARM manual.

#define DRYOS_SGI_HANDLERS_PTR 0x4008                    /* [M50II 1.0.1 OK] VBAR IRQ trace: handler table ptr at 0x4008, 8-byte entries; was 0x402c */

/* "Malloc Information" */
#define MALLOC_STRUCT_ADDR 0x2d160                       /* [M50II 1.0.1 OK] shared malloc/free pool (all 4 literal slots read 0x2d160); was M50.110 0x56a1c */
//#define MALLOC_FREE_MEMORY (MEM(MALLOC_STRUCT + 8) - MEM(MALLOC_STRUCT + 0x1C)) // "Total Size" - "Allocated Size"

/* high confidence */
#define DRYOS_ASSERT_HANDLER        0x4000               //from debug_asset function, hard to miss

#define CURRENT_GUI_MODE (*(int*)0x7EC0)                 /* [M50II 1.0.1 matched] state base 0x7E5C, [+0x64] compared in SetGUIRequestMode; was 0x7B44 - verify */

#define GUIMODE_PLAY 2
#define GUIMODE_MENU 3
// bindGUIEventFromGUICBR DNE on M50, however by educated guess from older generations:
// In SetGUIRequestMode, look at what code calls NotifyGUIEvent(9, something)
// skip RECORDING variant for now
#define GUIMODE_ML_MENU (lv ? 0x63 : GUIMODE_MENU)
//#define GUIMODE_ML_MENU (RECORDING ? 0 : lv ? 0x63 : GUIMODE_MENU)


#define CANON_SHUTTER_RATING 100000

#define GMT_FUNCTABLE               0xE099E958           /* [M50II 1.0.1 OK] from gui_main_task; was M50.110 0xE08FFB94 */
#define GMT_NFUNCS                  0x7                  //size of table above

/* [offline F7] WRONG, M50 mk1 values: the words 0x75094 and 0x8E660 occur nowhere in ROM0
 * (aligned literal scan of gang100.bin). Real bases not derived (the lvae_* handlers run from
 * RAM at 0x0224xxxx). Values are left in place so the build is unchanged; anything that uses
 * CONTROL_BV*, LVAE_DISP_GAIN, LVAE_MOV_M_CTRL or LVAE_ISO_MIN reads or writes unrelated RAM.
 * Keep exposure override / ExpSim features off until these are re-derived on hardware.
 * LVAE_ISO_MIN is also mis-parenthesised (dereferences, then adds 0xE). */
#define LVAE_STRUCT                 0x75094              // First value written in 0xe04ffb38
#define CONTROL_BV      (*(uint16_t*)(LVAE_STRUCT+0x28)) // via "lvae_sentcontrolbv"
#define CONTROL_BV_TV   (*(uint16_t*)(LVAE_STRUCT+0x2E)) // via "lvae_setcontrolaeparam"
#define CONTROL_BV_AV   (*(uint16_t*)(LVAE_STRUCT+0x30)) // via "lvae_setcontrolaeparam"
#define CONTROL_BV_ISO  (*(uint16_t*)(LVAE_STRUCT+0x32)) // via "lvae_setcontrolaeparam"
#define CONTROL_BV_ZERO (*(uint16_t*)(LVAE_STRUCT+0x34)) // via "lvae_setcontrolaccumh"
#define LVAE_DISP_GAIN  (*(uint16_t*)(LVAE_STRUCT+0x48)) // via "lvae_setdispgain"
#define LVAE_MOV_M_CTRL (*(uint8_t* )(LVAE_STRUCT+0x24)) // via "lvae_setmoviemanualcontrol"

/*
 * kitor: ISO related stuff is not in LVAE struct anymore?
 * iso-related stuff calls 0x02275de which returns pointer at 0x02276168 to 0x6b818
 */
#define LVAE_ISO_STRUCT 0x8e660
#define LVAE_ISO_MIN    (*(uint8_t* )LVAE_ISO_STRUCT + 0x0E ) // via string: ISOMin:%d

//#define LVAE_ISO_HIS    (*(uint8_t* )(LVAE_STRUCT+0xXX)) // no idea, not referenced in ./src?!
//#define LVAE_ISO_SPEED  (*(uint8_t* )(LVAE_STRUCT+0xXX))  //WRONG, not sure how to follow

/*
 * kitor: DIGIC 8 has no PROP_LV_OUTPUT_TYPE (PROP_HOUTPUT_TYPE in ML source)
 * I looked around LiveViewApp and found `LvInfoToggle_Update()` which updates
 * variable to represent currently display overlays. Look at R conts.h for more
 * details.
 */
#define LV_OVERLAYS_MODE MEM(0x13CC8)

/* [RAW-VIDEO R2, docs/31 + docs/30] DIGIC 8 raw LiveView registers.
 * RAW_LV_EDMAC_CHANNEL_ADDR: write-EDMAC channel MMIO Canon fills after lv_save_raw(1);
 *   raw.c reads raw_lv_edmac here to discover raw_info.buffer. Confirmed on 1.0.1 (docs/30),
 *   same value as M50 mk1. Without this, raw.c falls through to the wrong DIGIC-V default.
 * SHAD_GAIN_REGISTER: PLACEHOLDER carried from 200D/M50 mk1 (0 xrefs on our ROM); affects
 *   highlight gain only, tuned empirically on-target (docs/30 deferred). */
#define RAW_LV_EDMAC_CHANNEL_ADDR 0xd0420200
#define SHAD_GAIN_REGISTER        0xd0008030 // WRONG/placeholder from 200D — tune later

/* There's no `DispOperator_PropertyMasterSetDisplayTurnOffOn()` like on EOSR.
 * PropID 80040083 is also not referenced anywhere.
 * GUILockTask does DISP_MuteOnOSDLineVram() and DISP_MuteOnOSDPanelVram()
 * so maybe values stored by those two combined can be used instead. */
#define DISPLAY_IS_ON               1

/*
 * On M50 UYVY Image buffers are hardcoded to be (see VramRead e009ccd6):
 * for EVF   : 1024x768
 * for Panel : 736x480, 720x480 is active image area
 * for HDMI  : 736x480, 1280x720, 1920x1080, 3840x2160 depending on output.
 *             HDMI resolution is selected based on type field from DispDev
*              structure (8e00 + 0x18)
 * HDMI is referenced as "Line" in Canon functions.
 *
 * Buffers used for regular display are available in smemShowFix output.
 * IMG_VRAM1, IMG_VRAM2, IMG_VRAM3
 *
 * Different to R there's no Clean HDMI output. But DispVram looks similar to
 * R, so who knows...
 */

#define YUV422_LV_BUFFER_1   0x7F41C000
#define YUV422_LV_BUFFER_2   0x7F810800
#define YUV422_LV_BUFFER_3   0x7FC05000

// Offsets found via DISP_GetImageAddress() helper that prints
// "CurrentImgAddr : %#08x"
#define DISP_VRAM_STRUCT_PTR *(unsigned int *)0xAB10           /* [M50II 1.0.1 OK] DispVram struct (+0xA8 LINE/HDMI,+0xB0 PANEL,+0xB8 EVF); was 0x8e08 */
#define DV_DISP_TYPE  *((unsigned int *)(DISP_VRAM_STRUCT_PTR + 0xC))   // Display type mask
#define DV_VRAM_LINE  *((unsigned int *)(DISP_VRAM_STRUCT_PTR + 0xA8))  // Pointer to LV buffer for HDMI output
#define DV_VRAM_PANEL *((unsigned int *)(DISP_VRAM_STRUCT_PTR + 0xB0))  // Pointer to LV buffer for Panel output
#define DV_VRAM_EVF   *((unsigned int *)(DISP_VRAM_STRUCT_PTR + 0xB8))  // Pointer to LV buffer for EVF	 output

/* Hardcoded to Panel for now. It would be easier if we can replace this with a
 * function call that would be put into functon_overrides.c. Then we could just
 * define full structs there instead of playing with pointers */
#define YUV422_LV_BUFFER_DISPLAY_ADDR DV_VRAM_PANEL
#define YUV422_LV_PITCH               736       // depends on display type

#define YUV422_HD_BUFFER_DMA_ADDR 0x0 // it expects this to be shamem_read(some_DMA_ADDR)

/* WRONG! */
#define HALFSHUTTER_PRESSED         0
/* kitor: I was unable to find any related stuff from 200D
 * Working theory: since R is LV all-the-time, maybe it's not special anymore
 * and is handled by MPU now?
 */

//Replaced by CONFIG_NO_BFNT in internals.h
//#define BFNT_CHAR_CODES             0x00000000
//#define BFNT_BITMAP_OFFSET          0x00000000
//#define BFNT_BITMAP_DATA            0x00000000

#define NUM_PICSTYLES 11

#define AUDIO_MONITORING_HEADPHONES_CONNECTED 0
#define INFO_BTN_NAME               "INFO"
#define Q_BTN_NAME                  "Q/SET"

#define MIN_MSLEEP 11
#define PLAY_MODE (gui_state == GUISTATE_PLAYMENU && CURRENT_GUI_MODE == GUIMODE_PLAY)
#define MENU_MODE (gui_state == GUISTATE_PLAYMENU && CURRENT_GUI_MODE == GUIMODE_MENU)

/* WRONG: copied straight from 200d/50d */
// Definitely wrong / hacks / no testing at all:
    #define IMGPLAY_ZOOM_LEVEL_ADDR (0x2CBC) //wrong

extern int winsys_bmp_dirty_bit_neg;
#define WINSYS_BMP_DIRTY_BIT_NEG MEM(&winsys_bmp_dirty_bit_neg) // faked via function_overrides.c
#define FOCUS_CONFIRMATION 0 // wrong, but safe, no focus lock
    #define LV_BOTTOM_BAR_DISPLAYED 0x0 // wrong, fake bool
// this block all copied from 50D, and probably wrong, though likely safe
    #define FASTEST_SHUTTER_SPEED_RAW 160
    #define MAX_AE_EV 2
    #define FLASH_MAX_EV 3
    #define FLASH_MIN_EV -10
    #define COLOR_FG_NONLV 80
    #define AF_BTN_HALFSHUTTER 0
    #define AF_BTN_STAR 2
// another block copied from 50D
    #define GUIMODE_WB 5
    #define GUIMODE_FOCUS_MODE 9
    #define GUIMODE_DRIVE_MODE 8
    #define GUIMODE_PICTURE_STYLE 4
    #define GUIMODE_Q_UNAVI 0x18
    #define GUIMODE_FLASH_AE 0x22
    #define GUIMODE_PICQ 6

// all these MVR ones are junk, don't try and record video and they probably don't get used?
    #define MVR_190_STRUCT (*(void**)0x1ed8) // look in MVR_Initialize for AllocateMemory call;
                                         // decompile it and see where ret_AllocateMemory is stored.
#define div_maybe(a,b) ((a)/(b))
    // see mvrGetBufferUsage, which is not really safe to call => err70
    // macros copied from arm-console
    #define MVR_BUFFER_USAGE 70 /* obviously wrong, don't try and record video
           // div_maybe(-100*MEM(236 + MVR_190_STRUCT) - \
           // 100*MEM(244 + MVR_190_STRUCT) - 100*MEM(384 + MVR_190_STRUCT) - \
           // 100*MEM(392 + MVR_190_STRUCT) + 100*MEM(240 + MVR_190_STRUCT) + \
           // 100*MEM(248 + MVR_190_STRUCT), \
           // - MEM(236 + MVR_190_STRUCT) - MEM(244 + MVR_190_STRUCT) + \
           // MEM(240 + MVR_190_STRUCT) +  MEM(248 + MVR_190_STRUCT)) */
    #define MVR_FRAME_NUMBER (*(int*)(220 + MVR_190_STRUCT))
    //#define MVR_LAST_FRAME_SIZE (*(int*)(512 + MVR_752_STRUCT))
    #define MVR_BYTES_WRITTEN MEM((212 + MVR_190_STRUCT))

//address of XimrContext structure to redraw in FEATURE_VRAM_RGBA
#define XIMR_CONTEXT 0x9329C

// Pointer to the address of init1_task, used for the create_task_ex() call by
// cpu0 (init_task's helper FUN_e00402d0 @0xe00402d0 reads *(0xe00403c0) = 0xE0040229
// = init1_task entry, thumb). CONFIG_INIT1_HIJACK MMU-patches this so cpu1's init
// task routes through init1_task_wrapper (patch_mmu.c) and adopts ML's MMU tables.
// RE-verified from the 1.0.1 ROM (gang100.bin). M50 II analog of 200D's 0xe00403a8.
#define PTR_INIT1_TASK      0xe00403c0
