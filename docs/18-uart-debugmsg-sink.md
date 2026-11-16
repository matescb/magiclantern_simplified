# docs/18 — UART / DebugMsg Sink (WS-F6)

**Verified 2026-07-07** — F6 workflow agent; char→FIFO path checked via romdis.

## Char → UART FIFO (verified)
Physical writer **`uart_write_bytes` 0xE007D814**: writes each byte to **UART_TX 0xC0800000**,
polls **status 0xC0800014** bit1 (`UART_ST_TX_READY=0x02`) — `lsls status,#0x1e; bpl` (wait while
clear), acks by `str #2` to status. Second UART instance: UART_TX_CH1 0xC0810000 (same +0x00/+0x14).

Higher levels: `uart_console_sink` 0xE027A5A2 → writer; `uart_emit_callback` 0xE00A678A (fn-ptr
@[0x405C+4]); `uart_printf` 0xE01C7280 (varargs, existing); `DryosDebugMsg` 0xE05D10C4 (existing).
UART routing regs (docs/09): 0xD01202F0/F4 (route 0x0=ICU/our ARM core, 0x2=CCLIME/Xtensa).

## Proposed ML sink
Point ML's `qprintf`/uart putchar at `uart_write_bytes` (or replicate: poll 0xC0800014 bit1, str
byte to 0xC0800000). Ensure UART1 TX routed to ICU (write 0x0 to 0xD01202F4) at bring-up. This is
the observability layer for HW bring-up (LED markers → UART log).

---
## F6 agent detail
## UART / DebugMsg logging sink — char to FIFO path (M50 Mark II, DIGIC 8, fw 1.0.1)

### 1. The two UART subsystems (do not confuse them)

There are two independent UART peripherals in this ROM:

| Block | Base(s) | Status reg / bit | Config/routing | Role |
|-------|---------|------------------|----------------|------|
| Legacy debug UART (ICU/ARM) | `0xC0800000` (dev0), `0xC0810000` (dev1) | `base+0x14`, bit 1 (0x02) | fixed HW | DebugMsg / uart_printf output — our bring-up target |
| Configurable UART | `0xD0132000` (+ `0xD0132280`) | `+0xF4`, bit 16 | `D0130260/D0120260` block; routing `D01202F0/F4`,`D01302F0/F4` | MPU/CCLIME-shared serial; configured by `0xE000A870..0xE000A982` |

The session anchor `0xE000A900` ("UART writer") is mid-function (no prologue; ends `pop{r4-r7,pc}` @0xE000A982) and only writes the config/routing registers of the second block — it is NOT the DebugMsg character path.

### 2. Char to FIFO call chain (the real path)

```
uart_printf(fmt, ...)                 0xE01C7280
  push {r0-r3}                         ; spill varargs
  irqsave                              0xE021B8F0
  vcprintf_formatter(cb,ctx,fmt,args)  0xE021C432   ; %-engine, emits literal chunks
    emit_chunk_helper                  0xE021C726   ; ldrd r3,r4,[r0]; blx r3  -> cb(ctx,ptr,len)
      emit_callback(ctx,ptr,len)       0xE00A678A   ; if RAM [0x405C+4]!=0 forward, else drop
        console_sink(ptr,len)          0xE027A5A2   ; r0=dev 0; bl writer
          uart_write_bytes(dev,ptr,len,&out)  0xE007D814   ; PHYSICAL
            base = *(0xE0997E94 + dev*4)  = 0xC0800000
            for each byte:
              if ch==0x0a && dev.crlf_flag: emit CR first
              while (!(*(base+0x14) & 2)) ;   // wait TX-ready (bit1)
              *(base+0x14) = 2;               // ack / clear TX-ready
              *(base+0x00) = ch;              // write byte to DATA/FIFO
```

TX register: `0xC0800000` (write byte). TX-ready poll: `0xC0800014`, bit 1 (0x02); the driver also writes 0x02 back to ack. Cross-validated by sibling `0xE007D7C0` which uses the identical `+0x14`/`+0x00` layout.

The register base is taken from a ROM pointer table at `0xE0997E94` (dev0=`0xC0800000`, dev1=`0xC0810000`). The per-device struct lives in RAM at `0x2CEEC + dev*0x24`; `+0x04` is the CRLF-expansion flag.

### 3. How DryosDebugMsg relates

`DryosDebugMsg` (`0xE05D10C4`, `DebugMsg(class,level,fmt,...)`) is a separate path: it filters by a per-class table at `[0x5154]` (level `& 0xf` vs `[entry+0x38]`), formats, and dispatches to a registered debug device / ring buffer via `0xE05B9F9E` — it does NOT call `uart_printf`. `uart_printf` is the dedicated, always-formats, straight-to-UART0 printer that Canon uses for boot and error output (e.g. the SGI register dump at `0xE00D407C` loops `uart_printf` over 0xD entries). So for ML, `uart_printf` is the convenient formatted sink, while the raw `0xC0800000` poke is the OS-independent one.

### 4. Proposed ML UART bring-up sink

Two layers, pick per bring-up stage:

(a) Direct MMIO (earliest boot, zero OS deps — recommended first):
```c
/* M50 II (DIGIC 8) legacy debug UART */
#define UART_TX  0xC0800000   /* write byte to TX FIFO/data */
#define UART_ST  0xC0800014   /* status; bit1 (0x02) = TX ready */

static void uart_putc(char c) {
    while (!(MEM(UART_ST) & 2)) { }   /* wait TX-ready */
    MEM(UART_ST) = 2;                 /* ack TX-ready   */
    MEM(UART_TX) = (uint8_t)c;        /* emit           */
}
static void uart_puts(const char *s) {
    for (; *s; s++) { if (*s=='\n') uart_putc('\r'); uart_putc(*s); }
}
```
This mirrors Canon's own `0xE007D814` exactly and needs no RAM state.

(b) ROM `uart_printf` (formatted, once Canon boot has run):
Keep `THUMB_FN(0xE01C7280, uart_printf)` and enable `CONFIG_COPY_CONSOLE_TO_UART` (as 200D.101 does) so `printf`/console mirrors to UART. Note it only emits when the RAM gate `[0x405C+4]` is non-zero (Canon sets it during early boot — proven by the working SGI dump). ML can force-enable by writing that gate if needed.

Suggested consts.h additions: `UART_TX=0xC0800000`, `UART_ST=0xC0800014`, `UART_ST_TX_READY=0x02`.
Suggested stubs.S additions: confirm `uart_printf@0xE01C7280`; optionally add `uart_write_bytes@0xE007D814` (`uart_write(dev,buf,len,&out)`) for a low-level interpose.

### struct/register layouts
```

### struct uart_regbase_table_E0997E94
  +0x00   dev0_base                    uint32_t*  | =0xC0800000; [E0997E94]
  +0x04   dev1_base                    uint32_t*  | =0xC0810000; [E0997E98]

### struct uart_dev_struct_RAM_0x2CEEC
  +0x00   scratch_head                 uint32_t   | 0xE007D814 str r2,[r3] uses caller out-ptr; struct base = 0x2CEEC + dev*0x24 (r0
  +0x04   crlf_expand_flag             uint32_t   | [r5+4]==1 triggers CR-before-LF injection in 0xE007D814
  +0x08   return_status                uint32_t   | [dev+8] loaded into [sp] as return value at 0xE007D870

### struct uart_console_ctrl_RAM_0x405C
  +0x00   state                        uint32_t   | [0x405C+0] referenced by 0xE00A67EC/0xE00A6866 console-timer code
  +0x04   uart_output_gate             uint32_t   | emitter 0xE00A6790 cbz [0x405C+4] -- if 0, formatted chunks are dropped instead 
  +0x1c   handler_array                fnptr[]    | 0xE00A67EC/0xE00A6840 index [0x405C+0x1c + n*4] as callbacks

```
