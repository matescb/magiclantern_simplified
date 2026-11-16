#!/usr/bin/env python3
"""Gate: does a freshly built core .sym keep the known-booting symbol layout?

Usage:
  check_symbol_stability.py <new.sym> [--baseline baseline/M50mk2_101.sym]
                            [--elf build/magiclantern] [--meta baseline/BASELINE.json]
                            [--allow-removed] [--allow-section-change] [--ignore NAME]... [--max-list N]

Exit status: 0 = layout identical for every pre-existing symbol,
             1 = a pre-existing symbol moved / was removed / anchor or
                 section end changed, 2 = usage or input error.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import symlib
from symlib import hx

HERE = Path(__file__).resolve().parent
DEFAULT_BASELINE = HERE / "baseline" / "M50mk2_101.sym"
DEFAULT_META = HERE / "baseline" / "BASELINE.json"


def _list(title, rows, fmt, limit):
    print(f"{title}: {len(rows)}")
    for row in rows[:limit]:
        print("    " + fmt(row))
    if len(rows) > limit:
        print(f"    ... {len(rows) - limit} more (raise --max-list)")


def check_anchors(meta, new):
    """Return list of failure strings for the anchors recorded in the meta."""
    fails = []
    for name, want in (meta.get("anchors") or {}).items():
        have = new.symbols.get(name)
        if want is None:
            state = "absent in baseline"
            if have is not None:
                state += f", now {hx(have)} (new symbol)"
            print(f"    anchor {name}: {state}")
            continue
        want_i = int(want, 16)
        if have == want_i:
            print(f"    anchor {name}: {hx(have)} OK")
        else:
            got = "MISSING" if have is None else hx(have)
            print(f"    anchor {name}: expected {hx(want_i)}, got {got}  <-- FAIL")
            fails.append(name)
    return fails


def check_sections(meta, elf):
    """Compare ELF section ends against the baseline meta. Returns changes."""
    layout = symlib.elf_layout(elf)
    base = meta.get("elf_layout") or {}
    changes = []
    for name, val in sorted(layout["symbols"].items(), key=lambda kv: kv[1]):
        want = (base.get("symbols") or {}).get(name)
        if want is None:
            print(f"    {name:<18} {hx(val)}  (no baseline)")
        elif int(want, 16) == val:
            print(f"    {name:<18} {hx(val)}  OK")
        else:
            d = val - int(want, 16)
            print(f"    {name:<18} {hx(int(want, 16))} -> {hx(val)}  ({d:+d} bytes)  <-- CHANGED")
            changes.append(name)
    for name, sec in layout["sections"].items():
        want = (base.get("sections") or {}).get(name)
        if want is None:
            print(f"    {name:<18} end {hx(sec['end'])}  (no baseline)")
        elif int(want["end"], 16) == sec["end"] and int(want["addr"], 16) == sec["addr"]:
            print(f"    {name:<18} {hx(sec['addr'])}..{hx(sec['end'])}  OK")
        else:
            d = sec["end"] - int(want["end"], 16)
            print(f"    {name:<18} {want['addr']}..{want['end']} -> "
                  f"{hx(sec['addr'])}..{hx(sec['end'])}  (end {d:+d})  <-- CHANGED")
            changes.append(name)
    return changes


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("new_sym")
    ap.add_argument("--baseline", default=str(DEFAULT_BASELINE))
    ap.add_argument("--meta", default=None,
                    help="BASELINE.json (default: next to the default baseline)")
    ap.add_argument("--elf", help="core ELF of the NEW build (build/magiclantern)")
    ap.add_argument("--allow-removed", action="store_true")
    ap.add_argument("--allow-section-change", action="store_true")
    ap.add_argument("--max-list", type=int, default=15)
    ap.add_argument("--ignore", action="append", default=[], metavar="NAME",
                    help="symbol whose move is reported but not counted (repeatable); "
                         "e.g. build_version, which follows the variable-length "
                         "build_id string '<commit> <branch>'")
    args = ap.parse_args(argv)

    try:
        base = symlib.load_sym(args.baseline)
        new = symlib.load_sym(args.new_sym)
    except symlib.SymError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    meta = {}
    meta_path = Path(args.meta) if args.meta else (
        DEFAULT_META if Path(args.baseline).resolve() == DEFAULT_BASELINE.resolve() else None)
    if meta_path:
        try:
            meta = json.loads(meta_path.read_text())
        except (OSError, ValueError) as exc:
            print(f"ERROR: cannot read meta {meta_path}: {exc}", file=sys.stderr)
            return 2
        if meta.get("sym_md5") and meta["sym_md5"] != base.md5:
            print(f"ERROR: baseline {args.baseline} md5 {base.md5} does not match "
                  f"meta sym_md5 {meta['sym_md5']} (baseline file was altered)",
                  file=sys.stderr)
            return 2

    diff = symlib.diff_syms(base, new)
    ignored = tuple(m for m in diff.moved if m[0] in args.ignore)
    if ignored:
        diff = symlib.SymDiff(diff.added, diff.removed,
                              tuple(m for m in diff.moved if m[0] not in args.ignore))
    print(f"baseline: {base.path}  md5 {base.md5}  ({len(base.symbols)} symbols)")
    print(f"new     : {new.path}  md5 {new.md5}  ({len(new.symbols)} symbols)")
    if meta.get("commit"):
        print(f"baseline commit: {meta['commit']}")
    if base.md5 == new.md5:
        print("files are byte-identical")
    for tbl in (base, new):
        if tbl.duplicates:
            print(f"WARNING: {tbl.path}: duplicate names with different addresses: "
                  f"{', '.join(tbl.duplicates[:8])}")

    lim = args.max_list
    _list("added", diff.added, lambda r: f"{hx(r[1]):>10} {r[0]}", lim)
    _list("removed", diff.removed, lambda r: f"{hx(r[1]):>10} {r[0]}", lim)
    _list("moved (ML core)", diff.moved_core,
          lambda r: f"{hx(r[1]):>10} -> {hx(r[2]):>10} ({r[2] - r[1]:+d}) {r[0]}", lim)
    _list("moved (ROM stubs >= 0xE0000000)", diff.moved_rom,
          lambda r: f"{hx(r[1]):>10} -> {hx(r[2]):>10} {r[0]}", lim)

    for name, old, new_addr in ignored:
        print(f"ignored move (--ignore): {hx(old)} -> {hx(new_addr)} ({new_addr - old:+d}) {name}")
    first = diff.first_divergence()
    if first is not None:
        print(f"first divergence at baseline address: {hx(first)}")
    if diff.moved_core:
        print(f"total shift (highest moved core symbol): {diff.total_shift():+d} bytes")
        hist = diff.shift_histogram()
        top = ", ".join(f"{d:+d}: {c}" for d, c in list(hist.items())[:6])
        print(f"shift histogram (delta: symbols): {top}")

    failures = []
    if diff.moved:
        failures.append(f"{len(diff.moved)} pre-existing symbol(s) moved")
    if diff.removed and not args.allow_removed:
        failures.append(f"{len(diff.removed)} symbol(s) removed")

    if meta.get("anchors"):
        print("anchors:")
        bad = check_anchors(meta, new)
        if bad:
            failures.append("anchor mismatch: " + ", ".join(bad))

    if args.elf:
        print(f"section ends ({args.elf}):")
        try:
            changed = check_sections(meta, args.elf)
        except symlib.SymError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 2
        if changed and not args.allow_section_change:
            failures.append("section layout changed: " + ", ".join(changed))

    if failures:
        print("RESULT: FAIL - " + "; ".join(failures))
        return 1
    note = " (new symbols added, none moved)" if diff.added else ""
    print("RESULT: PASS - every pre-existing symbol is at its baseline address" + note)
    return 0


if __name__ == "__main__":
    sys.exit(main())
