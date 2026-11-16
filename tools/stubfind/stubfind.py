#!/usr/bin/env python3
"""
stubfind.py — locate Canon firmware functions in a raw ROM dump for Magic Lantern ports.

Designed for the EOS M50 Mark II 1.0.1 port (Digic 8, Thumb-2 DryOS), but generic.
No reference ROM is required: functions are located by the unique strings they
reference (string-anchoring) or by direct address inspection (verify/disasm).

ROM model:
  gang100.bin is ROM0, mapped at virtual base 0xE0000000; main firmware at 0xE0040000.

Subcommands:
  findstr   <substr>            list virtual addresses of matching C strings
  xref      <hexaddr>           list literal-pool slots whose word == addr, and the
                                 function (prologue) that loads each slot
  callarg   <string>            find code that loads <string> then BLs a function;
                                 tally BL targets (used to identify e.g. task_create,
                                 which receives task-name strings as an argument)
  prologue  <hexaddr>           nearest Thumb function prologue at//before addr
  disasm    <hexaddr> [n]       disassemble n Thumb instructions at addr
  verify    <hexaddr> [name]    sanity-check that addr is a plausible function entry

All addresses are virtual (0xE00.....). Exit code 0 on success.
"""
import sys, struct
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN

BASE = 0xE0000000
ROM = None  # bytes

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)
md.detail = True


def load(path):
    global ROM
    with open(path, "rb") as f:
        ROM = f.read()


def va2off(va):
    return va - BASE


def off2va(off):
    return off + BASE


def in_rom(va):
    o = va2off(va)
    return 0 <= o < len(ROM)


def read_word(va):
    o = va2off(va)
    return struct.unpack_from("<I", ROM, o)[0]


# ---- string search -----------------------------------------------------------

def find_string(s):
    """Yield virtual addresses of NUL-terminated occurrences of s (ASCII)."""
    needle = s.encode()
    start = 0
    while True:
        i = ROM.find(needle, start)
        if i < 0:
            return
        # require a proper C-string terminator right after the match
        end = i + len(needle)
        if end < len(ROM) and ROM[end] == 0:
            yield off2va(i)
        start = i + 1


# ---- literal-pool reference search ------------------------------------------

def find_word_refs(target_va):
    """Yield virtual addresses of 4-byte-aligned words equal to target_va."""
    packed = struct.pack("<I", target_va)
    start = 0
    while True:
        i = ROM.find(packed, start)
        if i < 0:
            return
        if i % 4 == 0:
            yield off2va(i)
        start = i + 1


def find_ldr_of_pool(pool_va, window=1024):
    """Find Thumb LDR Rd,[pc,#imm] instructions that resolve to pool_va.
       Returns list of (ldr_va, reg)."""
    hits = []
    lo = pool_va - window
    if lo < BASE:
        lo = BASE
    code = ROM[va2off(lo):va2off(pool_va) + 4]
    for insn in md.disasm(code, lo):
        if insn.mnemonic.startswith("ldr") and "[pc," in insn.op_str:
            # compute pc-relative target: pc = align(insn.address+4,4) + imm
            try:
                imm = int(insn.op_str.split("#")[-1].rstrip("]"), 0)
            except ValueError:
                continue
            pc = (insn.address + 4) & ~3
            if pc + imm == pool_va:
                reg = insn.op_str.split(",")[0].strip()
                hits.append((insn.address, reg))
    return hits


# ---- MOVW/MOVT address materialization (Digic 8 / Thumb-2) -------------------

def _enc_movw(rd, imm16):
    i = (imm16 >> 11) & 1
    imm4 = (imm16 >> 12) & 0xF
    imm3 = (imm16 >> 8) & 0x7
    imm8 = imm16 & 0xFF
    hw1 = 0xF240 | (i << 10) | imm4
    hw2 = (imm3 << 12) | (rd << 8) | imm8
    return struct.pack("<HH", hw1, hw2)


def _enc_movt(rd, imm16):
    i = (imm16 >> 11) & 1
    imm4 = (imm16 >> 12) & 0xF
    imm3 = (imm16 >> 8) & 0x7
    imm8 = imm16 & 0xFF
    hw1 = 0xF2C0 | (i << 10) | imm4
    hw2 = (imm3 << 12) | (rd << 8) | imm8
    return struct.pack("<HH", hw1, hw2)


def _find_all(pattern):
    out, start = [], 0
    while True:
        i = ROM.find(pattern, start)
        if i < 0:
            return out
        if i % 2 == 0:  # Thumb halfword alignment
            out.append(i)
        start = i + 1


def find_addr_refs(addr, adj=0x28):
    """Find MOVW Rd,#lo ; MOVT Rd,#hi pairs (same Rd, within adj bytes) that
       materialize `addr`. Returns list of ref-site virtual addresses (the MOVW)."""
    lo = addr & 0xFFFF
    hi = (addr >> 16) & 0xFFFF
    sites = []
    for rd in range(13):  # r0..r12
        movw_off = set(_find_all(_enc_movw(rd, lo)))
        movt_off = _find_all(_enc_movt(rd, hi))
        for mt in movt_off:
            # accept movw before movt (typical) or movt before movw
            for cand in movw_off:
                if 0 <= (mt - cand) <= adj or 0 <= (cand - mt) <= adj:
                    sites.append(off2va(min(cand, mt)))
                    break
    return sorted(set(sites))


# ---- ADR (PC-relative) reference finding -------------------------------------
# DryOS references most strings via ADR/ADD Rd,PC,#imm (range ~+/-4KB), NOT pooled
# words, so scan a window around the string and decode.

def find_adr_refs(target_va, window=0x1010):
    """Find adr/add/sub Rd,pc,#imm instructions (within +/-window) resolving to
       target_va. Decodes one insn independently at each even offset to avoid the
       flow-desync that linear disasm hits on interleaved data. Returns [(va, reg)]."""
    lo = max(BASE, target_va - window)
    hi = min(off2va(len(ROM)), target_va + 0x10)
    hits = []
    a = lo
    while a < hi:
        chunk = ROM[va2off(a):va2off(a) + 4]
        for insn in md.disasm(chunk, a):
            m, ops = insn.mnemonic, insn.op_str
            if m == "adr" or ((m.startswith("add") or m.startswith("sub")) and "pc" in ops):
                try:
                    imm = int(ops.split("#")[-1], 0)
                except ValueError:
                    break
                pc = (insn.address + 4) & ~3
                tgt = pc - imm if m.startswith("sub") else pc + imm
                if tgt == target_va:
                    hits.append((insn.address, ops.split(",")[0].strip()))
            break  # only the first insn decoded at this offset
        a += 2
    return hits


# ---- prologue detection ------------------------------------------------------

def is_push_lr(halfword):
    # Thumb 16-bit PUSH {..., lr}: 1011 0101 xxxxxxxx  => 0xB5xx
    return (halfword & 0xFF00) == 0xB500


def is_push32(first_hw):
    # Thumb-2 32-bit STMDB sp!, {...} : 0xE92D....  (first halfword 0xE92D)
    return first_hw == 0xE92D


def find_prologue(va, back=0x400):
    """Scan backwards from va for the nearest plausible Thumb function prologue.
       Returns (prologue_va, kind) or (None, None)."""
    o = va2off(va)
    a = o
    lo = max(0, o - back)
    while a >= lo:
        hw = struct.unpack_from("<H", ROM, a)[0]
        if is_push_lr(hw):
            return (off2va(a), "push16")
        if is_push32(hw):
            return (off2va(a), "push32")
        a -= 2
    return (None, None)


# ---- BL target extraction ----------------------------------------------------

def next_bl_target(from_va, span=0x40):
    """Disassemble forward from from_va up to span bytes; return first BL target VA."""
    code = ROM[va2off(from_va):va2off(from_va) + span]
    for insn in md.disasm(code, from_va):
        if insn.mnemonic == "bl":
            try:
                return int(insn.op_str.lstrip("#"), 0)
            except ValueError:
                return None
    return None


# ---- subcommands -------------------------------------------------------------

def cmd_findstr(args):
    s = args[0]
    hits = list(find_string(s))
    for va in hits:
        print(f"0x{va:08X}  {s!r}")
    print(f"# {len(hits)} occurrence(s)")


def cmd_xref(args):
    target = int(args[0], 0)
    pools = list(find_word_refs(target))
    print(f"# {len(pools)} literal-pool slot(s) pointing at 0x{target:08X}")
    for pool in pools[:40]:
        ldrs = find_ldr_of_pool(pool)
        for ldr_va, reg in ldrs:
            pro, kind = find_prologue(ldr_va)
            ps = f"0x{pro:08X}({kind})" if pro else "??"
            print(f"  pool 0x{pool:08X}  ldr@0x{ldr_va:08X} {reg}  func={ps}")


def cmd_refs(args):
    """List code sites that materialize <hexaddr> via MOVW/MOVT, + their function."""
    addr = int(args[0], 0)
    sites = find_addr_refs(addr)
    print(f"# {len(sites)} MOVW/MOVT site(s) building 0x{addr:08X}")
    for s in sites[:40]:
        pro, kind = find_prologue(s)
        ps = f"0x{pro:08X}({kind})" if pro else "??"
        print(f"  ref@0x{s:08X}  func={ps}")


def cmd_strref(args):
    """Find functions that reference string <s> (via MOVW/MOVT of its ROM VA)."""
    s = args[0]
    n = 0
    for str_va in find_string(s):
        sites = find_addr_refs(str_va)
        print(f"# string {s!r} @0x{str_va:08X}: {len(sites)} ref site(s)")
        for site in sites[:20]:
            pro, kind = find_prologue(site)
            ps = f"0x{pro:08X}({kind})" if pro else "??"
            print(f"  ref@0x{site:08X}  func={ps}")
            n += 1
    if n == 0:
        print("# (no MOVW/MOVT refs — string may be RAM-relocated)")


def cmd_adrref(args):
    """Find functions that ADR-reference string <s> (PC-relative)."""
    s = args[0]
    total = 0
    for str_va in find_string(s):
        hits = find_adr_refs(str_va)
        print(f"# string {s!r} @0x{str_va:08X}: {len(hits)} ADR site(s)")
        for va, reg in hits:
            pro, kind = find_prologue(va)
            ps = f"0x{pro:08X}({kind})" if pro else "??"
            print(f"  adr@0x{va:08X} {reg}  func={ps}")
            total += 1
    if total == 0:
        print("# (no ADR refs found in +/-4KB window)")


def cmd_callarg(args):
    """Identify a function that receives <string> as an argument then is BL'd.
       Tallies BL targets across all ADR + MOVW/MOVT ref sites; dominant = callee."""
    s = args[0]
    tally, sites = {}, 0
    for str_va in find_string(s):
        refsites = [va for va, _ in find_adr_refs(str_va)] + find_addr_refs(str_va)
        for site in refsites:
            tgt = next_bl_target(site, span=0x80)
            if tgt and in_rom(tgt):
                tally[tgt] = tally.get(tgt, 0) + 1
                sites += 1
    print(f"# string {s!r}: {sites} ref+BL site(s)")
    for tgt, n in sorted(tally.items(), key=lambda kv: -kv[1])[:8]:
        first = struct.unpack_from("<H", ROM, va2off(tgt))[0]
        selfpro = is_push_lr(first) or is_push32(first)
        pro, kind = find_prologue(tgt)
        tag = "ENTRY" if selfpro else (f"prologue~0x{pro:08X}" if pro else "?")
        print(f"  BL 0x{tgt:08X}  x{n}  [{tag}]")


def cmd_prologue(args):
    va = int(args[0], 0)
    pro, kind = find_prologue(va)
    if pro:
        print(f"0x{pro:08X}  ({kind})")
    else:
        print("no prologue found")


def cmd_disasm(args):
    va = int(args[0], 0)
    n = int(args[1]) if len(args) > 1 else 16
    code = ROM[va2off(va):va2off(va) + n * 4]
    c = 0
    for insn in md.disasm(code, va):
        print(f"0x{insn.address:08X}:  {insn.mnemonic:<8} {insn.op_str}")
        c += 1
        if c >= n:
            break


def cmd_verify(args):
    va = int(args[0], 0)
    name = args[1] if len(args) > 1 else "?"
    if not in_rom(va):
        print(f"FAIL {name}: 0x{va:08X} not in ROM")
        sys.exit(1)
    hw = struct.unpack_from("<H", ROM, va2off(va))[0]
    entry = is_push_lr(hw) or is_push32(hw)
    print(f"{'OK ' if entry else 'WARN'} {name} @0x{va:08X}: "
          f"first hw=0x{hw:04X} {'(push prologue)' if entry else '(no push prologue)'}")
    cmd_disasm([args[0], "6"])


CMDS = {
    "findstr": cmd_findstr, "xref": cmd_xref, "refs": cmd_refs,
    "strref": cmd_strref, "adrref": cmd_adrref, "callarg": cmd_callarg,
    "prologue": cmd_prologue, "disasm": cmd_disasm, "verify": cmd_verify,
}


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        print("usage: stubfind.py <rom.bin> <cmd> [args...]")
        sys.exit(2)
    load(sys.argv[1])
    cmd = sys.argv[2]
    if cmd not in CMDS:
        print(f"unknown cmd {cmd}; one of {list(CMDS)}")
        sys.exit(2)
    CMDS[cmd](sys.argv[3:])


if __name__ == "__main__":
    main()
