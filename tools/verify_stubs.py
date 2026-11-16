#!/usr/bin/env python3
"""
verify_stubs.py — regression harness for the M50 II 1.0.1 stubs.S.

Purpose (WS-F7, "strong foundations"): make every stub edit *verifiable*.
For each stub in platform/M50mk2.101/stubs.S it re-derives ground truth
straight from the ROM (capstone Thumb-2, same as romdis.py) and assigns a
verdict, so a bad address can never silently rot the port.

Checks per stub:
  code stub (THUMB_FN / code NSTUB):
    - region sane (ROM 0xE0.. / 0xE1.. , or CPU1-runtime 0xDF00xxxx)
    - address decodes cleanly (no capstone gap at entry)
    - not landing mid-instruction (heuristic: nothing straddles the entry)
    - has a plausible entry (push{..,lr} frame) OR is a clean leaf (reaches
      bx lr / pop {..,pc} with no intervening push) -> both are OK
  data stub (DATA_PTR / data NSTUB):
    - region sane (low-RAM global, MMIO, or ROM constant); 0x0 = intentional

Verdicts: OK | LEAF | REVIEW | BAD | SKIP(cpu1) | DATA | NULL

Usage:
  verify_stubs.py                 full table + summary to stdout
  verify_stubs.py --baseline P    also write a stable, diffable baseline to P
  verify_stubs.py --only BAD,REVIEW   filter rows by verdict
Also runs a fixed regression guard for the EDMAC completion-CBR crash-fix
targets (RegisterEDmacCompleteCBR/UnregisterEDmacCompleteCBR stub addresses
+ the ROM completion-CBR table-base literal at 0xE05B7E40).
Exit code: number of BAD stubs + CBR-guard failures (0 = clean), capped at 125.
"""
import sys, struct, re, os

BASE = 0xE0000000
ROM_PATH = os.environ.get(
    "ROMDIS_ROM", "/mnt/data/WORK/eos/m50ii_dumper/dump_1.0.1/gang100.bin")
STUBS_PATH = os.environ.get(
    "ROMDIS_STUBS",
    "/mnt/data/WORK/eos/magiclantern_simplified/platform/M50mk2.101/stubs.S")

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN

# A *call* is exactly bl / blx (optionally .w). NOT blo/bls/ble/blt, which are
# conditional branches that merely start with "bl".
CALL_RE = re.compile(r"^blx?(\.w)?$")
# Conditional branch mnemonics: as the *first* instruction they imply the
# condition flags were set before this address -> we landed mid-function.
COND_B_RE = re.compile(r"^b(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)(\.w)?$")

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)
with open(ROM_PATH, "rb") as f:
    ROM = f.read()
ROM_END = BASE + len(ROM)

STUB_RE = re.compile(
    r"^\s*(THUMB_FN|NSTUB|DATA_PTR)\s*\(\s*(0x[0-9A-Fa-f]+|[0-9]+)\s*,\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)")


def in_rom(va):
    return BASE <= va < ROM_END


def decode(va, n):
    """Decode up to n insns from va; return list of (addr, size, mnem, ops)."""
    off = va - BASE
    code = ROM[off:off + n * 4 + 8]
    out = []
    for insn in md.disasm(code, va):
        out.append((insn.address, insn.size, insn.mnemonic, insn.op_str))
        if len(out) >= n:
            break
    return out


def straddles_entry(va):
    """Heuristic mid-instruction check: does an insn starting at va-2 span past va?"""
    for back in (2, 4):
        ins = decode(va - back, 1)
        if ins and ins[0][0] < va and ins[0][0] + ins[0][1] > va:
            return True
    return False


def classify_code(va):
    # CPU1 runtime blob (relocated, not ROM-resident at this VA)
    if 0xDF000000 <= va <= 0xDF00FFFF:
        return "SKIP", "cpu1-runtime (src @0xE14995A4, not ROM-resident here)"
    if not in_rom(va):
        return "BAD", "outside ROM (0xE0000000-0x%08X)" % (ROM_END - 1)
    if va & 1:
        return "BAD", "odd address (Thumb entry must be even in table)"
    ins = decode(va, 40)
    if not ins or ins[0][0] != va:
        return "BAD", "does not decode at entry"
    first = "%s %s" % (ins[0][2], ins[0][3])
    mid = straddles_entry(va)
    fmn, fops = ins[0][2], ins[0][3]

    # First-instruction mid/tail signals (strong, low false-positive):
    #  - a conditional branch first => flags were set before this addr
    #  - a return (bx lr / pop{..pc}) as insn 0 => this is a function tail
    if COND_B_RE.match(fmn):
        return "REVIEW", "1st: %s ; cond-branch at entry (mid-func)" % first
    if (fmn == "bx" and fops.strip() == "lr") or (fmn.startswith("pop") and "pc" in fops):
        return "REVIEW", "1st: %s ; return at entry (func tail)" % first

    # A real function that calls a subroutine (bl/blx) must first save lr.
    # A bl/blx reached before any frame/push means we landed mid-function.
    saved_lr = False
    frame_at_entry = False
    ret_at = None
    call_before_save = None
    for i, (a, sz, mn, ops) in enumerate(ins):
        if mn.startswith("push"):
            if "lr" in ops:
                saved_lr = True
            if i <= 1:                       # push at entry = frame (incl. r0-r3 spill)
                frame_at_entry = True
        if mn in ("sub", "sub.w") and ops.startswith("sp") and i <= 1:
            frame_at_entry = True   # frame alloc (may follow a leading ldr-literal)
        if CALL_RE.match(mn) and not saved_lr and not frame_at_entry \
                and call_before_save is None:
            call_before_save = i
        if (mn == "bx" and ops.strip() == "lr") or (mn.startswith("pop") and "pc" in ops):
            ret_at = i
            break

    note = "1st: %s" % first
    if mid:
        note += " ; STRADDLES prev insn"
    if frame_at_entry:
        return ("REVIEW" if mid else "OK"), note
    if call_before_save is not None:
        return "REVIEW", note + " ; MID-FUNC? call@%d before frame" % call_before_save
    if mid:
        return "REVIEW", note + " ; no frame + straddles"
    if ret_at is not None:
        return "LEAF", note + " ; leaf (no frame), ret@%d" % ret_at
    return "LEAF", note + " ; no-frame entry (thunk/tail-call/long fn)"


# ---- EDMAC completion-CBR fix regression guard -----------------------------
# The raw-record crash fix pins three ROM/stub invariants; a ROM or stub drift
# could silently relocate them and re-arm the crash. Assert them explicitly.
CBR_STUBS = {
    "RegisterEDmacCompleteCBR": 0xE05B7C48,
    "UnregisterEDmacCompleteCBR": 0xE05B7F68,
}
CBR_TABLE_BASE_ADDR = 0xE05B7E40   # ROM literal: completion-CBR table base
CBR_TABLE_BASE_WORD = 0x00083F24   # expected value at CBR_TABLE_BASE_ADDR


def read_rom_word(va):
    """32-bit little-endian ROM word at virtual addr va, using the same
    offset mechanism as decode() (off = va - BASE); None if out of range."""
    off = va - BASE
    if 0 <= off <= len(ROM) - 4:
        return struct.unpack_from("<I", ROM, off)[0]
    return None


def check_cbr_targets(rows):
    """Regression guard for the EDMAC completion-CBR crash-fix targets.
    Returns (n_fail, lines); n_fail feeds the exit code so a drift fails."""
    lines = []
    n_fail = 0
    found = {name: va for _, name, va, _, _ in rows}
    for name, want in CBR_STUBS.items():
        got = found.get(name)
        if got is None:
            lines.append("FAIL %-28s missing from stubs.S (expected 0x%08X)"
                         % (name, want))
            n_fail += 1
        elif got != want:
            lines.append("FAIL %-28s 0x%08X != expected 0x%08X"
                         % (name, got, want))
            n_fail += 1
        else:
            lines.append("PASS %-28s 0x%08X" % (name, got))
    w = read_rom_word(CBR_TABLE_BASE_ADDR)
    if w == CBR_TABLE_BASE_WORD:
        lines.append("PASS %-28s [0x%08X] = 0x%08X"
                     % ("CBR-table-base", CBR_TABLE_BASE_ADDR, w))
    else:
        lines.append("FAIL %-28s [0x%08X] = %s != expected 0x%08X"
                     % ("CBR-table-base", CBR_TABLE_BASE_ADDR,
                        "None(out-of-ROM)" if w is None else "0x%08X" % w,
                        CBR_TABLE_BASE_WORD))
        n_fail += 1
    return n_fail, lines


def classify_data(va):
    if va == 0:
        return "NULL", "intentional null (see comment)"
    if 0xC0000000 <= va <= 0xC1FFFFFF:
        return "DATA", "GIC/periph MMIO"
    if 0xD0000000 <= va <= 0xDFFFFFFF:
        return "DATA", "MMIO"
    if in_rom(va):
        return "DATA", "ROM constant"
    if va < 0x400000:
        return "DATA", "low-RAM global"
    return "REVIEW", "unusual data region"


def main():
    argv = sys.argv[1:]
    baseline = None
    only = None
    if "--baseline" in argv:
        i = argv.index("--baseline")
        baseline = argv[i + 1]
    if "--only" in argv:
        i = argv.index("--only")
        only = set(argv[i + 1].split(","))

    rows = []
    for line in open(STUBS_PATH):
        m = STUB_RE.match(line)
        if not m:
            continue
        kind, addr_s, name = m.group(1), m.group(2), m.group(3)
        va = int(addr_s, 0)
        if kind == "DATA_PTR":
            verdict, note = classify_data(va)
        elif kind == "NSTUB":
            # NSTUB is used for both code (firmware_entry) and data (task_max)
            if va >= 0xDF000000 or in_rom(va):
                verdict, note = classify_code(va)
            else:
                verdict, note = classify_data(va)
        else:  # THUMB_FN
            verdict, note = classify_code(va)
        rows.append((verdict, name, va, kind, note))

    order = {"BAD": 0, "REVIEW": 1, "SKIP": 2, "LEAF": 3, "OK": 4,
             "DATA": 5, "NULL": 6}
    counts = {}
    for v, *_ in rows:
        counts[v] = counts.get(v, 0) + 1

    def emit(fh, sorted_view):
        seq = sorted(rows, key=lambda r: (order.get(r[0], 9), r[1])) \
            if sorted_view else rows
        for v, name, va, kind, note in seq:
            if only and v not in only:
                continue
            fh.write("%-7s %-30s 0x%08X  %-9s %s\n" %
                     (v, name, va, kind, note))

    total = len(rows)
    summary = "  ".join("%s=%d" % (k, counts[k])
                        for k in sorted(counts, key=lambda k: order.get(k, 9)))
    print("# verify_stubs.py — %d stubs from %s" % (total, os.path.basename(STUBS_PATH)))
    print("# summary: %s\n" % summary)
    emit(sys.stdout, sorted_view=True)

    cbr_fail, cbr_lines = check_cbr_targets(rows)
    print("\n# EDMAC completion-CBR fix regression guard: %s"
          % ("PASS" if cbr_fail == 0 else "FAIL (%d)" % cbr_fail))
    for ln in cbr_lines:
        print("  " + ln)

    if baseline:
        with open(baseline, "w") as fh:
            fh.write("# stub verification baseline (sorted by name; diff-stable)\n")
            fh.write("# summary: %s\n\n" % summary)
            for v, name, va, kind, note in sorted(rows, key=lambda r: r[1]):
                fh.write("%-7s %-30s 0x%08X %s\n" % (v, name, va, kind))
        print("\n# baseline written -> %s" % baseline)

    return min(counts.get("BAD", 0) + cbr_fail, 125)


if __name__ == "__main__":
    sys.exit(main())
