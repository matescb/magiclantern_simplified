#!/usr/bin/env python3
"""Gate: can a module's imports be resolved by the core .sym?

Usage:
  check_module_syms.py <module.mo> [more.mo ...] <core.sym> [--list]
                       [--providers other.mo ...]

Imports = undefined symbols of the .mo (arm-none-eabi-nm -u). On the camera
the TCC loader resolves them by NAME from ML/MODULES/<MODEL>_<FW>.sym, plus
the exports of modules loaded together. A name missing from the core .sym
fails unless another module given on the command line / via --providers
defines it (reported as "provided by <module>").

Exit status: 0 all imports resolvable, 1 at least one missing, 2 input error.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import symlib
from symlib import hx


def check_module(mo, core, providers, show_list):
    """Return (missing, cross) for one module; prints its report."""
    imports = symlib.module_undefined(mo)
    missing, cross = [], []
    for name in imports:
        if name in core.symbols:
            continue
        owner = next((p for p, defs in providers.items()
                      if name in defs and p != str(mo)), None)
        if owner:
            cross.append((name, owner))
        else:
            missing.append(name)
    status = "FAIL" if missing else "ok"
    print(f"{Path(mo).name:<16} imports {len(imports):>3}  "
          f"cross-module {len(cross):>2}  missing {len(missing):>2}  {status}")
    if show_list:
        for name in imports:
            if name in core.symbols:
                print(f"      {hx(core.symbols[name]):>10} {name}")
    for name, owner in cross:
        print(f"      (provided by {Path(owner).name}) {name}")
    for name in missing:
        print(f"      MISSING {name}")
    return missing, cross


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="one or more .mo followed by the core .sym")
    ap.add_argument("--providers", nargs="*", default=[],
                    help="extra .mo files whose exports may satisfy imports")
    ap.add_argument("--list", action="store_true", help="print every resolved import")
    args = ap.parse_args(argv)
    if len(args.paths) < 2:
        ap.error("need at least one module and the core .sym")
    *modules, sym_path = args.paths

    try:
        core = symlib.load_sym(sym_path)
        providers = {str(m): symlib.module_defined(m)
                     for m in list(modules) + list(args.providers)}
        print(f"core sym: {core.path}  md5 {core.md5}  ({len(core.symbols)} symbols)")
        failed = []
        for mo in modules:
            missing, _ = check_module(mo, core, providers, args.list)
            if missing:
                failed.append(Path(mo).name)
    except symlib.SymError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    if failed:
        print(f"RESULT: FAIL - unresolved imports in: {', '.join(failed)}")
        return 1
    print(f"RESULT: PASS - {len(modules)} module(s), all imports resolvable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
