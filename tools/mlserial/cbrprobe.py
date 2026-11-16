#!/usr/bin/env python3
"""cbrprobe.py -- probe / seed the M50 II EDMAC completion-CBR table over serial.

Root cause of the raw-record crash (see docs/32, this session's RE): Canon's EDMAC completion
IRQ dispatcher (FUN_e05b7de6 @0xE05B7DE6) does `bx r1` at 0xE05B7E74 with
    r1 = *(EDMAC_CBR_TABLE + channel*8)     # per-channel completion-CBR fn pointer
For the raw-capture channel that slot is NULL (never registered) -> bx 0 -> pc=0 -> crash.
Canon's own default no-op CBR stub is 0xE05B7C73 (Thumb-odd), which Unregister installs.

Filling any NULL fn slot with the default stub makes `bx r1` land on a harmless stub instead of 0.
This tool reads the table (`dump`), fills NULL slots live (`seed`), and diffs snapshots (`diff`) --
all with zero rebuilds, to validate the fix before baking it into edmac-d8.c.

Usage (run from tools/mlserial/, camera booted + raw armed):
    python3 cbrprobe.py dump [--save snap.json]
    python3 cbrprobe.py seed [--dry]
    python3 cbrprobe.py diff before.json after.json
"""
import argparse, json, re, subprocess, sys

EDMAC_CBR_TABLE = 0x00083F24   # base; per channel: +0 = fn ptr, +4 = arg
EDMAC_DEFAULT_CBR = 0xE05B7C73  # Canon default no-op CBR stub (Thumb-odd)
EDMAC_CBR_CHANNELS = 76         # channels 0..75 (table span 0x260 = 76*8)

WORD_RE = re.compile(r"0x([0-9A-Fa-f]{1,8})")


def mlserial(cmd, timeout=15):
    """Run one serial-shell command, return stdout text."""
    out = subprocess.run(
        [sys.executable, "mlserial.py", "cmd", cmd],
        capture_output=True, text=True, timeout=timeout,
    )
    return out.stdout + out.stderr


def read_table():
    """md the whole table, return list of (fn, arg) per channel."""
    nwords = EDMAC_CBR_CHANNELS * 2
    txt = mlserial("md 0x%X 0x%X" % (EDMAC_CBR_TABLE, nwords))
    words = []
    for line in txt.splitlines():
        if ":" not in line:
            continue
        # "<addr>: w0 w1 w2 w3" -> take words AFTER the colon
        rhs = line.split(":", 1)[1]
        words += [int(h, 16) for h in WORD_RE.findall(rhs)]
    if len(words) < nwords:
        print("WARN: parsed only %d/%d words -- is the camera up? raw output:\n%s"
              % (len(words), nwords, txt), file=sys.stderr)
    return [(words[c * 2], words[c * 2 + 1]) if c * 2 + 1 < len(words) else (None, None)
            for c in range(EDMAC_CBR_CHANNELS)]


def classify(fn):
    if fn is None:
        return "??"
    if fn == 0:
        return "NULL"     # <- the pc=0 crash source
    if fn == EDMAC_DEFAULT_CBR or fn == (EDMAC_DEFAULT_CBR & ~1):
        return "default"
    return "real"


def do_dump(save=None):
    tbl = read_table()
    nulls = []
    for ch, (fn, arg) in enumerate(tbl):
        kind = classify(fn)
        if kind in ("NULL", "real", "default"):
            addr = EDMAC_CBR_TABLE + ch * 8
            print("ch %2d @0x%08X  fn=%s arg=0x%08X  [%s]"
                  % (ch, addr, "0x%08X" % fn if fn is not None else "????",
                     arg if arg is not None else 0, kind))
        if kind == "NULL":
            nulls.append(ch)
    print("\nsummary: %d channels, NULL(fn=0) = %s"
          % (EDMAC_CBR_CHANNELS, nulls if nulls else "none"))
    if save:
        json.dump({"table": tbl}, open(save, "w"))
        print("saved -> %s" % save)
    return nulls


def do_seed(dry=False):
    tbl = read_table()
    nulls = [ch for ch, (fn, _) in enumerate(tbl) if fn == 0]
    if not nulls:
        print("no NULL fn slots -- nothing to seed.")
        return
    print("NULL fn slots: %s" % nulls)
    for ch in nulls:
        addr = EDMAC_CBR_TABLE + ch * 8
        if dry:
            print("  DRY would: wr 0x%08X 0x%08X" % (addr, EDMAC_DEFAULT_CBR))
            continue
        r = mlserial("wr 0x%X 0x%X" % (addr, EDMAC_DEFAULT_CBR))
        print("  wr 0x%08X 0x%08X -> %s" % (addr, EDMAC_DEFAULT_CBR, r.strip().splitlines()[-1:]))
    if not dry:
        remaining = [ch for ch, (fn, _) in enumerate(read_table()) if fn == 0]
        print("readback: NULL fn slots now = %s" % (remaining if remaining else "none (seeded OK)"))


def do_diff(f1, f2):
    a = json.load(open(f1))["table"]
    b = json.load(open(f2))["table"]
    changed = 0
    for ch in range(min(len(a), len(b))):
        if a[ch] != b[ch]:
            changed += 1
            print("ch %2d: %s -> %s" % (ch, a[ch], b[ch]))
    print("%d channel(s) changed" % changed)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("dump"); p.add_argument("--save")
    p = sub.add_parser("seed"); p.add_argument("--dry", action="store_true")
    p = sub.add_parser("diff"); p.add_argument("before"); p.add_argument("after")
    args = ap.parse_args()
    if args.cmd == "dump":
        do_dump(args.save)
    elif args.cmd == "seed":
        do_seed(args.dry)
    elif args.cmd == "diff":
        do_diff(args.before, args.after)


if __name__ == "__main__":
    main()
