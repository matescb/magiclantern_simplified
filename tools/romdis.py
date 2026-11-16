#!/usr/bin/env python3
"""
romdis.py — annotated Thumb-2 disassembler for the M50 II 1.0.1 ROM.

Purpose: read Canon boot/scheduler code straight from the raw ROM dump when
Ghidra is unavailable. Resolves PC-relative literal loads to their word values,
names BL/BLX/B targets and data addresses from a symbol table that is
auto-seeded from platform/M50mk2.101/stubs.S plus the boot-trace state words.

ROM model: gang100.bin is ROM0 at virtual base 0xE0000000 (main fw @ 0xE0040000).

Usage:
  romdis.py <hexaddr> [maxinsn]     disassemble a function (default 100 insns)
  romdis.py word <hexaddr>          read a 32-bit little-endian word
  romdis.py sym  <hexaddr>          name an address (nearest symbol)

All addresses are virtual (0xE0......). Thumb only (DryOS D8 is all-Thumb).
"""
import sys, struct, re, os

BASE = 0xE0000000
ROM_PATH = os.environ.get(
    "ROMDIS_ROM",
    "/mnt/data/WORK/eos/m50ii_dumper/dump_1.0.1/gang100.bin",
)
STUBS_PATH = os.environ.get(
    "ROMDIS_STUBS",
    "/mnt/data/WORK/eos/magiclantern_simplified/platform/M50mk2.101/stubs.S",
)

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)
md.detail = True

with open(ROM_PATH, "rb") as f:
    ROM = f.read()


def va2off(va):
    return va - BASE


def in_rom(va):
    o = va2off(va)
    return 0 <= o < len(ROM)


def read_word(va):
    o = va2off(va)
    if 0 <= o < len(ROM) - 3:
        return struct.unpack_from("<I", ROM, o)[0]
    return None


# ---- symbol table -----------------------------------------------------------
# CODE[addr]=name (entry, bit0 stripped), DATA[addr]=name
CODE = {}
DATA = {}


def _seed_from_stubs():
    if not os.path.exists(STUBS_PATH):
        return
    txt = open(STUBS_PATH).read()
    for m in re.finditer(r"THUMB_FN\(\s*(0x[0-9A-Fa-f]+)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)", txt):
        CODE[int(m.group(1), 16) & ~1] = m.group(2)
    for m in re.finditer(r"NSTUB\(\s*([0-9A-Fa-fx]+)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)", txt):
        try:
            CODE[int(m.group(1), 0) & ~1] = m.group(2)
        except ValueError:
            pass
    for m in re.finditer(r"DATA_PTR\(\s*([0-9A-Fa-fx]+)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)", txt):
        try:
            DATA[int(m.group(1), 0)] = m.group(2)
        except ValueError:
            pass


# Boot-handoff functions & state words (docs/08 + docs/09), override stubs.S.
_HANDOFF_CODE = {
    0xE00A674B: "create_helper1_E00A674B",
    0xE00A6C74: "task_trampoline",
    0xE00A6C94: "task_create_trampoline_E00A6C94",
    0xE0166A5D: "setup_helper_dryos_init_c_E0166A5D",
    0xE01669D5: "setup_helper_after_create_E01669D5",
    0xE0166A77: "canon_first_init_helper_E0166A77",
    0xE0166AD5: "final_setup_helper_E0166AD5",
    0xE01B19B7: "get_or_create_current_task_E01B19B7",
    0xE01B1BE7: "task_ready_E01B1BE7",
    0xE01B2139: "sched_lock_helper_E01B2139",
    0xE01B21CD: "critsec_save_token_E01B21CD",
    0xE01B21F3: "critsec_restore_E01B21F3",
    0xE01B2237: "trampoline_unlock_E01B2237",
    0xE03C51C3: "context_restore_E03C51C3",
    0xE0121128: "send_software_interrupt",
}
_HANDOFF_DATA = {
    0x1000: "dryos_kernel_cb",
    0x1008: "sched_cpu_task_slot",
    0x1010: "current_interrupt",
    0x1028: "current_task",
    0x2C054: "task_table_2C054",
    0x4000: "DRYOS_ASSERT_HANDLER",
    0x4004: "boot_state_word_4004",
    0x4008: "DRYOS_SGI_HANDLERS_PTR",
    0x4024: "sched_lock_word_4024",
    0x40CC: "task_max",
    0x4108: "task_dispatch_hook",
    0x4138: "init_helper_desc_4138",
    0x4140: "init_helper_desc_4140",
    0x4178: "init_helper_state_4178",
    0x417C: "saved_init_task_entry_417C",
}
_MMIO = {
    0xD01202F0: "UART1_RX_SELECT_CONFIG",
    0xD01202F4: "UART1_TX_SELECT_CONFIG",
    0xD01302F0: "UART1_RX_SELECT_MASK",
    0xD01302F4: "UART1_TX_SELECT_MASK",
}

_seed_from_stubs()
CODE.update(_HANDOFF_CODE)
DATA.update(_HANDOFF_DATA)
DATA.update(_MMIO)


def name_code(va):
    e = va & ~1
    return CODE.get(e)


def name_data(va):
    return DATA.get(va)


def annotate_value(val):
    if val is None:
        return ""
    parts = []
    n = name_code(val)
    if n:
        parts.append("-> %s%s" % (n, " (thumb)" if val & 1 else ""))
    d = name_data(val)
    if d:
        parts.append("= %s" % d)
    if 0xD0000000 <= val <= 0xDFFFFFFF and not d:
        parts.append("(MMIO)")
    if 0xC0000000 <= val <= 0xC1FFFFFF:
        parts.append("(GIC/periph MMIO?)")
    return "  " + " ".join(parts) if parts else ""


# ---- disassembly ------------------------------------------------------------

def disasm_func(start, maxinsn=100):
    out = []
    code = ROM[va2off(start):va2off(start) + maxinsn * 4 + 8]
    n = 0
    for insn in md.disasm(code, start):
        if n >= maxinsn:
            break
        n += 1
        va = insn.address
        line = "%08X  %-8s %s" % (va, insn.mnemonic, insn.op_str)
        note = ""

        mo = re.search(r"\[pc,\s*#(-?0x[0-9a-fA-F]+|-?\d+)\]", insn.op_str)
        if mo and insn.mnemonic.startswith("ldr"):
            imm = int(mo.group(1), 0)
            pool = ((va + 4) & ~3) + imm
            w = read_word(pool)
            note = "  ; [%08X]=%08X%s" % (pool, w if w is not None else 0,
                                          annotate_value(w))

        if re.match(r"(adr|add)\b", insn.mnemonic) and "pc" in insn.op_str:
            mi = re.search(r"#(-?0x[0-9a-fA-F]+|-?\d+)", insn.op_str)
            if mi:
                imm = int(mi.group(1), 0)
                tgt = ((va + 4) & ~3) + imm
                note = "  ; &%08X%s" % (tgt, annotate_value(tgt) or "")

        if insn.mnemonic.startswith(("b", "cb")):
            mt = re.search(r"#?(0x[0-9a-fA-F]{6,})", insn.op_str)
            if mt:
                tgt = int(mt.group(1), 0)
                nm = name_code(tgt)
                if nm:
                    note = "  ; -> %s" % nm

        out.append(line + note)

        if insn.mnemonic == "pop" and "pc" in insn.op_str:
            out.append("        ; ---- return (pop pc) ----")
        if insn.mnemonic == "bx" and insn.op_str.strip() == "lr":
            out.append("        ; ---- return (bx lr) ----")
    return "\n".join(out)


def main():
    a = sys.argv
    if len(a) < 2:
        print(__doc__)
        return 1
    if a[1] == "word":
        va = int(a[2], 16)
        w = read_word(va)
        print("[%08X] = %08X%s" % (va, w, annotate_value(w)))
        return 0
    if a[1] == "sym":
        va = int(a[2], 16)
        print(name_code(va) or name_data(va) or "(unknown)")
        return 0
    start = int(a[1], 16)
    maxinsn = int(a[2]) if len(a) > 2 else 100
    nm = name_code(start)
    print("; %08X %s" % (start, nm or ""))
    print(disasm_func(start, maxinsn))
    return 0


if __name__ == "__main__":
    sys.exit(main())
