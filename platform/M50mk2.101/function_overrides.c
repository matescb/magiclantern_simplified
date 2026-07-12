/** \file
 * Function overrides needed for M50 1.1.0
 */
/*
 * Copyright (C) 2021 Magic Lantern Team
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
 
#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <config.h>
#include <consts.h>
#include <lens.h>
#include <edmac.h>
#include <mem_defs.h>   /* UNCACHEABLE()/CACHEABLE() + size_t for the DMA-memory wrappers below */

/** GUI **/

//Not sure if all args are uint.
extern void gui_enqueue_message(uint32_t, uint32_t, uint32_t, uint32_t);
void GUI_Control(int bgmt_code, int obj, int arg, int unknown){
   gui_enqueue_message(0, bgmt_code, obj, arg);
}

// fake WINSYS_BMP_DIRTY_BIT_NEG
int winsys_bmp_dirty_bit_neg = 0;

void LoadCalendarFromRTC(struct tm *tm)
{
   /* [M50II 2026-07-11 ROOT FIX] The _LoadCalendarFromRTC stub (0xe0637210) was WRONG: it points
    * mid-instruction INTO FUN_e0637136 (a Canon "Arima" struct-copy). Calling it as
    * _LoadCalendarFromRTC(tm,0,0,16) landed on `str r0,[r1]` with r1=0 (the 2nd arg) -> NULL-store
    * DATA ABORT (DFAR=0). Fired INTERMITTENTLY after big.8 whenever ML ran config_save (and the
    * menu/flexinfo/lens clocks), hanging the camera -- this was the elusive post-big.8 crash.
    * HW-proven via the hardened abort handler: lr=0x000D42A6 = config_save_file (config.c:207),
    * regs r0=tm r1=0 r2=0 r3=0x10 exactly match _LoadCalendarFromRTC(tm,0,0,16).
    * The correct RTC-read address is not yet RE'd; these callers only use the time cosmetically,
    * so fill a fixed sentinel date instead of calling the bad stub. TODO: RE the real function. */
   if (!tm) return;
   tm->tm_sec  = 0;
   tm->tm_min  = 0;
   tm->tm_hour = 0;
   tm->tm_mday = 1;
   tm->tm_mon  = 0;     /* January */
   tm->tm_year = 126;   /* 2026 (years since 1900) */
   tm->tm_wday = 0;
   tm->tm_yday = 0;
   tm->tm_isdst = 0;
}

int get_task_info_by_id(int unknown_flag, int task_id, void *task_attr)
{
    // task_id is something like two u16s concatenated.  The flag argument,
    // present on D45 but not on D678 allows controlling if the task info request
    // uses the whole thing, or only the low half.
    //
    // ML calls with this set to 1, meaning task_id is used as is,
    // if 0, the high half is masked out first.
    //
    // D678 doesn't have the 1 option, we use the low half as index
    // to find the full value.
    struct task *task = first_task + (task_id & 0xffff);
    return _get_task_info_by_id(task->taskId, task_attr);
}

/** File I/O **/

/**
* _FIO_GetFileSize returns now 64bit size in form of struct.
* This probably should be integrated into fio-ml for CONFIG_DIGIC_VIII
*/
extern int _FIO_GetFileSize64(const char *, void *);
int _FIO_GetFileSize(const char * filename, uint32_t * size){
   uint32_t size64[2];
   int code = _FIO_GetFileSize64(filename, &size64);
   *size = size64[0]; //return "lower" part
   return code;
}

/** WRONG: temporary overrides to get CONFIG_HELLO_WORLD working **/

void SetEDmac(unsigned int channel, void *address, struct edmac_info *ptr, int flags)
{
   return;
}

void ConnectWriteEDmac(unsigned int channel, unsigned int where)
{
   return;
}

void ConnectReadEDmac(unsigned int channel, unsigned int where)
{
   return;
}

void StartEDmac(unsigned int channel, int flags)
{
   return;
}

void AbortEDmac(unsigned int channel)
{
   return;
}

void RegisterEDmacCompleteCBR(int channel, void (*cbr)(void*), void* cbr_ctx)
{
   return;
}

void UnregisterEDmacCompleteCBR(int channel)
{
   return;
}

void RegisterEDmacAbortCBR(int channel, void (*cbr)(void*), void* cbr_ctx)
{
   return;
}

void UnregisterEDmacAbortCBR(int channel)
{
   return;
}

void RegisterEDmacPopCBR(int channel, void (*cbr)(void*), void* cbr_ctx)
{
   return;
}

void UnregisterEDmacPopCBR(int channel)
{
   return;
}

void _EngDrvOut(uint32_t reg, uint32_t value)
{
   return;
}

uint32_t shamem_read(uint32_t addr)
{
   return 0;
}

void _engio_write(uint32_t* reg_list)
{
   return;
}

unsigned int UnLockEngineResources(struct LockEntry *lockEntry)
{
   return 0;
}

/* Canon's PropertyMgr manager object pointer lives at RAM 0x4FF4 (RE'd from the
 * prop_register_slave assert "Property/PropertyMgr.c:484", which fires when it is
 * still NULL). It is written once by CreatePropertyMgr (FUN_e05bf8c6). ml_init can
 * reach property registration before Canon finished bringing PropertyMgr up (its
 * bring-up is HW-dependent: on QEMU it never completes, on silicon it does), so we
 * gate ML's prop_register_slave on this being non-NULL. Overrides the weak default
 * in src/property.c. */
int ml_canon_propmgr_ready(void)
{
   return *(volatile uint32_t *)0x4FF4 != 0;
}

/* [M50II 2026-07-11] Module-system stubs for the CONFIG_MODULES=n bring-up build.
 * Core files (config.c, shoot.c, hdr.c, tasks.c, raw.c) call a handful of module_*
 * functions UNCONDITIONALLY (no CONFIG_MODULES guard), so disabling the module system to
 * reclaim ~95KB of _bss_end (which lets the 2-page MMU pool fit under the known-good
 * watermark) leaves these undefined at link time. Provide no-op stubs. Runtime effect:
 * no ML modules load (expected; they aren't needed to reach big.8). Compiled ONLY when
 * CONFIG_MODULES is off; when re-enabled, module.o provides the real ones. */
#if !defined(CONFIG_MODULES)
int module_exec_cbr(unsigned int type) { (void)type; return 0; }
void module_save_configs(void) { }
void module_shutdown(void) { }
struct config_var * module_get_config_var(const char * name) { (void)name; return 0; }
struct config_var * module_config_var_lookup(int * ptr) { (void)ptr; return 0; }
#endif // !CONFIG_MODULES

/* [M50II 2026-07-11] DMA-memory allocator ROOT FIX -- see stubs.S. The stubbed _alloc_dma_memory
 * (0xE055AAF4) / _free_dma_memory (0xE055AB28) were MISIDENTIFIED and pointed INTO a Canon graphics
 * draw routine (FUN_e055aa98 "GUI_DrawHdrImage/GRAPHICS_DrawOn"), so fio_malloc() -- used by
 * rbf_font_load, raw video, EDMAC, beep, bootflags, etc. -- branched into GraphicsConvertPosType
 * with a garbage pos_type and aborted at pc=0x2 (r0=r3=0x8000000D). That was the big.5 font-load
 * crash. Canon's AllocateUncacheableMemory is simply AllocateMemory + the uncacheable-mirror bit,
 * which is exactly ML's UNCACHEABLE() macro, so reimplement the pair over the verified
 * _AllocateMemory / _FreeMemory stubs (0xE05BEE20 / 0xE05BEFD8, both [M50II 1.0.1 OK]). */
extern void *_AllocateMemory(size_t size);
extern void  _FreeMemory(void *ptr);
void *_alloc_dma_memory(size_t size)
{
    void *p = _AllocateMemory(size);
    return p ? UNCACHEABLE(p) : (void *)0;
}
void _free_dma_memory(void *ptr)
{
    if (ptr) _FreeMemory(CACHEABLE(ptr));
}
