MODEL = M50mk2
FW_VERSION = 101

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120

#CANON_FIR  = CCF19110.FIR
#UPDATE_NAME_FIR = D_M50110.FIR
FIRMWARE_ID     = 0x80000468     # [M50II 1.0.1] model word K468 (7x in ROM); 0x412 was M50 mk1, absent here
MAIN_FIRMWARE_ADDR = 0xE0040000

# user_mem_start from cstart struct, aligned up to ELF_ALIGNMENT (0x10).
# [M50II 1.0.1] cstart sets user_mem_start = r5 + 0x800, where r5 = bss_end = 0xC70D8
# (bic r5,0xC70DF,#7). raw 0xC78D8 -> aligned 0xC78E0. Verified vs M50 (bss_end+0x800=0xdc870).
RESTARTSTART = 0xC78E0

# Cortex A9, binaries loaded as Thumb
CFLAGS_PLATFORM = -mthumb -mlong-calls
PLATFORM_ARCH = armv7-a

ML_BOOT_OBJ = boot-d678.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/function_overrides.o edmac-d8.o

# [M50II 2026-07-12] RAW-VIDEO R0 (docs/31): re-enable the module system + on-cam TCC linker.
# mlv_lite is a module, and CONFIG_TCC is required by the module loader to relocate/link .mo
# files at load time, so raw video cannot load without both. This grows _bss_end by ~94KB from
# the modules-off known-good 0x14fa00 (toward ~0x167000), between the known-good 2-page value
# (0x15fe00, Canon ~490KB left) and the pages=4 panic value (0x180700, DryOS pool starved).
# => MEMORY GATE: on cold-boot watch for a DryOS pool panic (LED-13, no pre.0). If it panics,
#    reduce ml_reserved_mem (boot-d678.c) or drop MMU_MAX_REMAP_PAGES to 1 (patch_mmu.c) before
#    adding mlv_lite. modules.included is kept minimal (bench, file_man) to measure headroom
#    first. The module_* NOP stubs in function_overrides.c self-exclude via #if !CONFIG_MODULES.
# Overrides the `?=` defaults in ../Makefile; must precede the include.
CONFIG_TCC     := y
CONFIG_MODULES := y

include ../Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete
