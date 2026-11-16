#!/usr/bin/env python3
"""raminit.py - initial (boot-time) contents of Canon RAM regions, EOS M50 Mark II fw 1.0.1.

Canon's firmware_entry (ROM 0xE004003C..0xE00400C6) copies three images out of ROM0
(gang100.bin, base 0xE0000000) into RAM and zeroes two .bss ranges.  This tool maps a
RAM address back to the ROM0 file offset and prints the value the camera has there
immediately after that copy (i.e. BEFORE any Canon code has run).  Read-only.

Usage:
  raminit.py <ramaddr> [nwords]          print initial word(s)
  raminit.py map                         print the region table
  raminit.py extract <outdir>            write the three images as .bin (for Ghidra/QEMU blocks)
  raminit.py find <hexword>              find RAM addrs in the copied images holding this word
Options: --rom PATH (default: ../../../m50ii_dumper/dump_1.0.1/gang100.bin relative to this file)

Caveat: an initial value says nothing about the runtime value; .bss addresses are 0 at
boot and are filled in later by Canon init code.
"""
import os, struct, sys

ROM0_BASE = 0xE0000000
# (name, ram_start, ram_end(exclusive), rom_src or None for zero-fill)
# literals read from ROM pool 0xE00400C8..0xE00400FC
REGIONS = [
    ("data",   0x00004000, 0x0002AD1C, 0xE1394358),
    ("bss",    0x0002AD1C, 0x000C70D8, None),
    ("ramcode",0x0223B000, 0x02319530, 0xE13BB074),
    ("blob2",  0xDF002800, 0xDF0033A0, 0xE14995A4),
    ("bss2",   0xDF0033A0, 0xDF0033AC, None),
]
CACHE_BIT = 0x40000000

def default_rom():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "..", "..", "m50ii_dumper", "dump_1.0.1", "gang100.bin"))

def locate(addr):
    for name, s, e, src in REGIONS:
        if s <= addr < e:
            return name, s, e, src
    a2 = addr & ~CACHE_BIT
    if a2 != addr:
        for name, s, e, src in REGIONS:
            if s <= a2 < e:
                return name + "(uncached alias)", s | CACHE_BIT, e | CACHE_BIT, src
    return None

def main(argv):
    rom = default_rom()
    if "--rom" in argv:
        i = argv.index("--rom"); rom = argv[i + 1]; del argv[i:i + 2]
    if not argv:
        print(__doc__); return 2
    with open(rom, "rb") as f:
        data = f.read()
    if argv[0] == "map":
        print("%-8s %-10s %-10s %-9s %-10s %-10s" % ("name", "ram_start", "ram_end", "length", "rom_addr", "file_off"))
        for name, s, e, src in REGIONS:
            if src is None:
                print("%-8s 0x%08X 0x%08X 0x%-7X %-10s %-10s" % (name, s, e, e - s, "(zeroed)", "-"))
            else:
                print("%-8s 0x%08X 0x%08X 0x%-7X 0x%08X 0x%08X" % (name, s, e, e - s, src, src - ROM0_BASE))
        return 0
    if argv[0] == "extract":
        out = argv[1]; os.makedirs(out, exist_ok=True)
        for name, s, e, src in REGIONS:
            if src is None: continue
            p = os.path.join(out, "ram_%08X_%s.bin" % (s, name))
            with open(p, "wb") as f:
                f.write(data[src - ROM0_BASE: src - ROM0_BASE + (e - s)])
            print("wrote %s (0x%X bytes, load at 0x%08X)" % (p, e - s, s))
        return 0
    if argv[0] == "find":
        pat = struct.pack("<I", int(argv[1], 16))
        for name, s, e, src in REGIONS:
            if src is None: continue
            blob = data[src - ROM0_BASE: src - ROM0_BASE + (e - s)]
            i = blob.find(pat)
            while i >= 0:
                if i % 4 == 0:
                    print("0x%08X (%s, ROM 0x%08X)" % (s + i, name, src + i))
                i = blob.find(pat, i + 1)
        return 0
    addr = int(argv[0], 16)
    n = int(argv[1], 0) if len(argv) > 1 else 1
    for k in range(n):
        a = addr + 4 * k
        loc = locate(a)
        if loc is None:
            print("0x%08X: not in a boot-initialised region (heap/stack/free RAM; undefined at boot)" % a)
            continue
        name, s, e, src = loc
        if src is None:
            print("0x%08X: 0x00000000  [%s, zero-filled at boot, no ROM source]" % (a, name))
            continue
        rom_addr = src + (a - s)
        off = rom_addr - ROM0_BASE
        (w,) = struct.unpack_from("<I", data, off)
        print("0x%08X: 0x%08X  [%s, ROM 0x%08X, file off 0x%07X]" % (a, w, name, rom_addr, off))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
