/**
 * Camera internals for EOS M50 1.1.0
 */

/** This camera has a DIGIC VIII chip */
#define CONFIG_DIGIC_VIII

// has inter-core RPC (so far this has always been dependent on SGI, 0xc)
#define CONFIG_RPC

// Cam has MMU (by itself, does nothing, see CONFIG_MMU_REMAP)
#define CONFIG_MMU

// Cam can wrap init1, allowing control of cpu1 before tasks are started.
// Needed on M50 II: Canon's 1st-stage starts CPU1 correctly (single-ptr mailbox
// 0xBFE01FC4), but without this hijack CPU1 never adopts ML's remapped MMU tables,
// so it can't run ML tasks (cpu1rdy stayed 0) and Canon subsystems that assume
// CPU1 (MechaControl) assert. Consumed by patch_mmu.c (needs PTR_INIT1_TASK in
// consts.h). Mirrors the working 200D dual-core port.
#define CONFIG_INIT1_HIJACK

/** Digic 8 does not have bitmap font in ROM, try to load it from card **/
#define CONFIG_NO_BFNT

/** disable SRM for now
 * in current state SRM_AllocateMemoryResourceFor1stJob makes camera crash
 * even if just one buffer is requrested.
 */
#define CONFIG_MEMORY_SRM_NOT_WORKING

/* has LV */
#define CONFIG_LIVEVIEW

/* hooking EFV_STATE ends with EvfCap crashes, requires investigation */
//#define CONFIG_STATE_OBJECT_HOOKS

#define CONFIG_MALLOC_STRUCT_V2

#define CONFIG_TASK_STRUCT_V2_SMP
#define CONFIG_TASK_ATTR_STRUCT_V5
