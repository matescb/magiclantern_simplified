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
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/function_overrides.o

# [M50II 2026-07-11] Bring-up memory reclaim: disable the on-cam TCC compiler and the
# module system (~94KB text+bss). Frees enough of _bss_end to restore a 2-page MMU remap
# pool (MMU_MAX_REMAP_PAGES=2 in patch_mmu.c) so the GraphicsConvertPosType:9 NOP can be
# applied cleanly WITHOUT growing past the known-good _bss_end (0x14fa00) that starves
# Canon's DryOS pool. Neither is needed to reach big.8 (modules load after big.8; TCC only
# compiles them). Re-enable once a permanent memory layout is designed. Overrides the
# `?=` defaults in ../Makefile; must precede the include.
CONFIG_TCC     := n
CONFIG_MODULES := n

include ../Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete
