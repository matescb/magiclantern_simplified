#define FEATURE_VRAM_RGBA

// Don't Click Me menu looks to be intended as a place
// for devs to put custom code in debug.c run_test(),
// and allowing triggering from a menu context.
#define FEATURE_DONT_CLICK_ME

#define FEATURE_SHOW_SHUTTER_COUNT

// working but incomplete, some allocators don't report
// anything yet as they're faked / not yet found
#define FEATURE_SHOW_FREE_MEMORY

#define CONFIG_TSKMON
#define FEATURE_SHOW_TASKS
#define FEATURE_SHOW_CPU_USAGE
#define FEATURE_SHOW_GUI_EVENTS

#define CONFIG_ADDITIONAL_VERSION
#define FEATURE_SCREENSHOT

// enable global draw
#define FEATURE_GLOBAL_DRAW
#define FEATURE_CROPMARKS

/* [RAW-VIDEO R2, docs/31] Raw LiveView. CONFIG_RAW_LIVEVIEW turns on the raw pipeline
 * (raw.c D8 branch, dump_img_task DNG capture, mlv_lite later).
 *
 * FEATURE_PICSTYLE (enabled on M50 mk1) is DISABLED on M50 II: picstyle.c registers
 * prop_register_slave handlers for ~11 picture-style properties (PROP_PICTURE_STYLE /
 * PROP_PICSTYLE_SETTINGS_*), which map to 0x0205xxxx ICU properties this body does NOT
 * expose. Canon's ICU property-delivery loop then fails to resolve them and floods the
 * UART with "[PROPAD] GetPropID(0)=0x02050072" + "[PROPST] PROPST_DeliverIcuProperty
 * 0x800000d3" on every menu/LV property broadcast (HW-observed 2026-07-13, ~55x/2min).
 * Not needed for raw video. Re-enable only after the picstyle prop map is fixed for K468. */
//#define FEATURE_PICSTYLE
#define CONFIG_RAW_LIVEVIEW

// Enable remapping ROM pages to RAM
#define CONFIG_SGI_HANDLERS
#define CONFIG_MMU_REMAP

// [M50II Phase 1 exclusivity] NOTE: the GUI-input exclusivity hook (funcs[0]
// @0xE021E460, mmu_patches.h) is a ROM CODE patch in region 0xE0200000 -- a DIFFERENT
// 1MB region than the CONFIG_INIT1_HIJACK patch (PTR_INIT1_TASK 0xe00403c0, region
// 0xE0000000) which claims the single default remap page/L2. A 2nd 1MB region would
// need MMU_MAX_REMAP_PAGES>=2, but that measured _bss_end 0x160000 (>0x15fe00, the
// Canon DryOS-pool starvation threshold) -> HW boot panic. There is no ML BSS to
// reclaim. Solution = post-boot L2 reclaim of the init1-hijack page (see patch_mmu.c
// mmu_reclaim_init1_page / apply_normal_patches, and docs/33). Kept at 1 page here.

//~kitor: add bootdisk menu for easier testing on foreign camera :)
#define FEATURE_BOOTFLAG_MENU

// We can't yet rely on image capture.  Cam crashes due to null pointer,
// I think?  If it fails to AF lock, for example.
#define CONFIG_IMAGE_CAPTURE_NOT_WORKING

#undef CONFIG_CRASH_LOG
#undef CONFIG_PROP_REQUEST_CHANGE
#undef CONFIG_AUTOBACKUP_ROM
