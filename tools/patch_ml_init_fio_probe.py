#!/usr/bin/env python3
import argparse
import os
import struct
import subprocess
import tempfile


ML_BASE = 0x000C78E0
BLOB_OFF = 0x148
TASK_CREATE_THUMB = 0xE05F40BB


ASM_FIO_TEMPLATE = r"""
.syntax unified
.thumb
.section .text
.global _start
.thumb_func
_start:
    movs    r0, #10
    bl      blink_once
    ldr     r0, =path_a
    ldr     r3, =0xE05B163F
    blx     r3
    cmp     r0, #0
    beq     try_b
    adds    r3, r0, #1
    beq     try_b
    mov     r4, r0
    movs    r0, #11
    bl      blink_once
    mov     r0, r4
    ldr     r1, =msg
    movs    r2, #(msg_end - msg)
    ldr     r3, =0xE05B1A01
    blx     r3
    movs    r0, #12
    bl      blink_once
    mov     r0, r4
    ldr     r3, =0xE05B1C1B
    blx     r3
    movs    r0, #13
    bl      blink_once
    ldr     r0, =path_a
    ldr     r3, =0xE05B2F3B
    blx     r3
    movs    r0, #14
    bl      blink_once
    movs    r0, #15
    bl      blink_forever

try_b:
    movs    r0, #1
    bl      blink_once
    ldr     r0, =path_b
    ldr     r3, =0xE05B163F
    blx     r3
    cmp     r0, #0
    beq     failed
    adds    r3, r0, #1
    beq     failed
    mov     r4, r0
    movs    r0, #2
    bl      blink_once
    mov     r0, r4
    ldr     r1, =msg
    movs    r2, #(msg_end - msg)
    ldr     r3, =0xE05B1A01
    blx     r3
    movs    r0, #3
    bl      blink_once
    mov     r0, r4
    ldr     r3, =0xE05B1C1B
    blx     r3
    movs    r0, #4
    bl      blink_once
    ldr     r0, =path_b
    ldr     r3, =0xE05B2F3B
    blx     r3
    movs    r0, #5
    bl      blink_once
    movs    r0, #6
    bl      blink_once
    movs    r0, #15
    bl      blink_forever

failed:
    movs    r0, #10
    bl      blink_forever

blink_forever:
    mov     r4, r0
1:
    mov     r0, r4
    bl      blink_once
    movs    r0, #5
    bl      wait
    b       1b

blink_once:
    push    {r4, r5, r6, r7, lr}
    mov     r4, r0
    movs    r0, #2
    bl      wait
    ldr     r5, =0xD01300E4
    ldr     r6, =0x000D0002
    ldr     r7, =0x000C0003
2:
    str     r6, [r5]
    movs    r0, #1
    bl      wait
    str     r7, [r5]
    movs    r0, #1
    bl      wait
    subs    r4, r4, #1
    bne     2b
    movs    r0, #3
    bl      wait
    pop     {r4, r5, r6, r7, pc}

wait:
    push    {r1, r2, lr}
3:
    ldr     r1, =0x00300000
4:
    subs    r1, r1, #1
    bne     4b
    subs    r0, r0, #1
    bne     3b
    pop     {r1, r2, pc}

.balign 4
path_a:
    .asciz "A:/M50TRACE.TXT"
path_b:
    .asciz "B:/M50TRACE.TXT"
msg:
    .ascii "M50II ml_init binary patch wrote this file.\n"
msg_end:
.balign 4
"""

ASM_MARKER_TEMPLATE = r"""
.syntax unified
.thumb
.section .text
.global _start
.thumb_func
_start:
1:
    movs    r0, #2
    bl      blink_once
    b       1b

blink_once:
    push    {r4, r5, r6, r7, lr}
    mov     r4, r0
    movs    r0, #2
    bl      wait
    ldr     r5, =0xD01300E4
    ldr     r6, =0x000D0002
    ldr     r7, =0x000C0003
2:
    str     r6, [r5]
    movs    r0, #1
    bl      wait
    str     r7, [r5]
    movs    r0, #1
    bl      wait
    subs    r4, r4, #1
    bne     2b
    movs    r0, #3
    bl      wait
    pop     {r4, r5, r6, r7, pc}

wait:
    push    {r1, r2, lr}
3:
    ldr     r1, =0x02000000
4:
    subs    r1, r1, #1
    bne     4b
    subs    r0, r0, #1
    bne     3b
    pop     {r1, r2, pc}
.balign 4
"""


def read_u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def write_u32(data, off, value):
    struct.pack_into("<I", data, off, value)


def vma_to_off(vma):
    return BLOB_OFF + (vma - ML_BASE)


def find_ml_init_entry(data):
    candidates = []
    for off in range(BLOB_OFF, len(data) - 12, 4):
        if read_u32(data, off) != TASK_CREATE_THUMB:
            continue
        entry_ptr = read_u32(data, off + 4)
        name_ptr = read_u32(data, off + 8)
        if not (entry_ptr & 1):
            continue
        if not (ML_BASE <= (entry_ptr & ~1) < ML_BASE + len(data)):
            continue
        if not (ML_BASE <= name_ptr < ML_BASE + len(data)):
            continue
        name_off = vma_to_off(name_ptr)
        if 0 <= name_off < len(data) and data[name_off:name_off + 8] == b"ml_init\x00":
            candidates.append((off, entry_ptr & ~1, name_ptr))
    if len(candidates) != 1:
        raise SystemExit(f"expected one ml_init task_create literal, found {len(candidates)}: {candidates!r}")
    return candidates[0]


def assemble_stub(address, mode):
    template = ASM_MARKER_TEMPLATE if mode == "marker" else ASM_FIO_TEMPLATE
    with tempfile.TemporaryDirectory() as td:
        asm = os.path.join(td, "stub.S")
        elf = os.path.join(td, "stub.elf")
        bin_path = os.path.join(td, "stub.bin")
        with open(asm, "w", encoding="ascii") as f:
            f.write(template)
        subprocess.check_call([
            "arm-none-eabi-gcc",
            "-nostdlib",
            "-Wl,-Ttext=0x%X" % address,
            "-Wl,--oformat=elf32-littlearm",
            "-mthumb",
            "-march=armv7-a",
            asm,
            "-o",
            elf,
        ])
        subprocess.check_call([
            "arm-none-eabi-objcopy",
            "-O",
            "binary",
            elf,
            bin_path,
        ])
        with open(bin_path, "rb") as f:
            return f.read()


def recompute_autoexec_checksum(data):
    if read_u32(data, len(data) - 8) != 0xE12FFF13:
        raise SystemExit("footer magic missing")
    write_u32(data, len(data) - 4, 0xCCCCCCCC)
    checksum = 0
    for off in range(0, len(data), 4):
        checksum ^= read_u32(data, off)
    write_u32(data, len(data) - 4, 0xCCCCCCCC ^ checksum)


def main():
    ap = argparse.ArgumentParser(description="Patch M50II known-good AUTOEXEC so ml_init writes M50TRACE.TXT")
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--mode", choices=("fio", "marker"), default="fio")
    ap.add_argument("--max-stub-size", type=lambda x: int(x, 0), default=0x220)
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = bytearray(f.read())
    if len(data) % 4:
        raise SystemExit("AUTOEXEC size is not 4-byte aligned")
    if len(data) <= BLOB_OFF + 4 or (read_u32(data, BLOB_OFF) >> 24) != 0xEA:
        raise SystemExit("embedded magiclantern blob was not found at expected offset 0x148")

    lit_off, entry, name = find_ml_init_entry(data)
    stub = assemble_stub(entry, args.mode)
    if len(stub) > args.max_stub_size:
        raise SystemExit(f"stub too large: 0x{len(stub):x} > 0x{args.max_stub_size:x}")

    patch_off = vma_to_off(entry)
    data[patch_off:patch_off + len(stub)] = stub
    recompute_autoexec_checksum(data)

    with open(args.output, "wb") as f:
        f.write(data)

    print(f"ml_init literal table offset: 0x{lit_off:x}")
    print(f"ml_init entry: 0x{entry:08x}")
    print(f"ml_init name: 0x{name:08x}")
    print(f"patch file offset: 0x{patch_off:x}")
    print(f"stub size: 0x{len(stub):x}")


if __name__ == "__main__":
    main()
