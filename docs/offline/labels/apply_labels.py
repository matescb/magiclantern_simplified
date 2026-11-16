# -*- coding: utf-8 -*-
# Apply docs/offline/labels/MERGED.csv to the open Ghidra program as labels / function names.
#
# @category M50II
# @menupath Tools.M50II.Apply merged offline labels
#
# WHAT THIS IS
#   A Ghidra script (GhidraScript API, written so that the same file runs under Jython 2.7
#   "Script Manager" and under PyGhidra / Python 3).  It reads MERGED.csv, produced by sprint
#   agent D5 (see docs/offline/D5.md), with the columns
#       address,name,source_reports,evidence,confidence,conflict
#   and proposes one name per address for the program gang100.bin (Canon EOS M50 Mark II,
#   firmware 1.0.1, ROM0 mapped at 0xE0000000).
#
# STATUS
#   Written offline by an agent that was NOT allowed to modify the Ghidra project.  This script
#   has never been executed inside Ghidra; only its Python syntax was checked.  Run the dry run
#   first and read its output before applying anything.
#
# SAFETY RULES IMPLEMENTED
#   1. DRY RUN IS THE DEFAULT.  Without the argument "--apply" (or DRY_RUN = False below) the
#      script only prints what it would do and makes no change to the program.
#   2. It never overwrites a name a person or an import put there.  A symbol is replaced only
#      when its source type is DEFAULT (FUN_xxxxxxxx, LAB_xxxxxxxx, DAT_xxxxxxxx, ...).
#      Anything USER_DEFINED, IMPORTED or ANALYSIS is kept and reported as KEEP.
#   3. It never deletes anything, never creates or deletes functions (unless
#      "--create-functions" is given), never changes data types, and never saves the program:
#      the changes sit in one undoable transaction ("Undo" in Ghidra reverts all of them).
#   4. If the proposed name already exists at a DIFFERENT address in the program the row is
#      skipped and reported as NAME_ELSEWHERE (pass "--allow-duplicates" to label anyway).
#      MERGED.csv flags the known cases in its "conflict" column, for example the ML stub
#      LockEngineResources, which stubs.S places at 0xE05BE8A6 although the real function is
#      0xE05BE6A6.
#   5. Addresses that are not in a memory block of the program are skipped (NO_MEMORY).
#
# ARGUMENTS (Script Manager: "Run" with arguments, or headless -postScript apply_labels.py ...)
#   --apply               really write (default: dry run, print only)
#   --csv=<path>          CSV to read (default: MERGED.csv next to this script)
#   --comments            also set a plate comment "[matched]"/"[hypothesis]" + evidence + sources,
#                         but only where no plate comment exists yet
#   --prefix-tiers        prepend the convention prefix "maybe_" / "unk_" to function names whose
#                         confidence is maybe / unk (ghidra/naming-conventions.md allows either
#                         the prefix or the tag in the comment; default is the tag only)
#   --create-functions    where the CSV row is code, an instruction already exists at the address
#                         and no function contains it, create a function there (default: label only)
#   --allow-duplicates    create the label even if the same name exists at another address
#   --only=<report>       only rows whose source_reports contains this id, e.g. --only=A5
#   --min-confidence=matched   skip rows with confidence "maybe"/"unk"
#
# OUTPUT
#   One line per row:  ACTION address name  (detail)   and a summary of counters at the end.
#   Actions: RENAME_FUNCTION, CREATE_LABEL, CREATE_FUNCTION, SAME (already has this name),
#   KEEP (existing non-default name kept), NAME_ELSEWHERE, NO_MEMORY, INSIDE_FUNCTION (label
#   placed inside another function's body: check the address), BAD_ROW, ERROR.

from __future__ import print_function

import csv
import os
import sys

from ghidra.program.model.symbol import SourceType
from ghidra.program.model.listing import CodeUnit

DRY_RUN = True                      # overridden by --apply
DEFAULT_CSV_NAME = "MERGED.csv"
FALLBACK_CSV = "/mnt/data/WORK/eos/docs/offline/labels/MERGED.csv"
TIER_TAG = {"matched": "[matched]", "maybe": "[hypothesis]", "unk": "[unk]", "bad": "[bad]"}
TIER_RANK = {"bad": 0, "unk": 1, "maybe": 2, "matched": 3}


def parse_args(argv):
    opt = {"apply": False, "csv": None, "comments": False, "prefix": False,
           "create_functions": False, "allow_dup": False, "only": None, "min_conf": None}
    for a in argv:
        a = str(a)
        if a == "--apply":
            opt["apply"] = True
        elif a.startswith("--csv="):
            opt["csv"] = a[len("--csv="):]
        elif a == "--comments":
            opt["comments"] = True
        elif a == "--prefix-tiers":
            opt["prefix"] = True
        elif a == "--create-functions":
            opt["create_functions"] = True
        elif a == "--allow-duplicates":
            opt["allow_dup"] = True
        elif a.startswith("--only="):
            opt["only"] = a[len("--only="):]
        elif a.startswith("--min-confidence="):
            opt["min_conf"] = a[len("--min-confidence="):]
        else:
            print("apply_labels: unknown argument %r ignored" % a)
    return opt


def find_csv(opt):
    if opt["csv"]:
        return opt["csv"]
    try:
        here = getSourceFile().getParentFile().getAbsolutePath()   # noqa: F821 (GhidraScript)
        cand = os.path.join(str(here), DEFAULT_CSV_NAME)
        if os.path.exists(cand):
            return cand
    except Exception:
        pass
    return FALLBACK_CSV


def read_rows(path):
    if sys.version_info[0] >= 3:
        f = open(path, "r", newline="", encoding="utf-8")
    else:
        f = open(path, "rb")
    try:
        return list(csv.DictReader(f))
    finally:
        f.close()


def is_default(sym):
    """True when the symbol is an auto-generated placeholder that may be replaced."""
    if sym is None:
        return True
    try:
        if sym.isDynamic():
            return True
    except Exception:
        pass
    return sym.getSource() == SourceType.DEFAULT


def name_elsewhere(symtab, name, addr):
    """Addresses (other than addr) that already carry a global symbol with this name."""
    out = []
    it = symtab.getSymbols(name)
    while it.hasNext():
        s = it.next()
        if not s.getAddress().equals(addr):
            out.append(s.getAddress())
    return out


def is_code_row(name, addr_val):
    if addr_val < 0x10000000:
        return False                                   # RAM global
    return not (name.startswith("str_") or name.startswith("table_")
                or name.startswith("g_") or name.startswith("rom_src_"))


def plate_text(row):
    tag = TIER_TAG.get(row.get("confidence", ""), "[hypothesis]")
    lines = ["%s M50II 1.0.1, offline sprint (static analysis only, not run on the camera)" % tag,
             "sources: %s" % row.get("source_reports", "")]
    for part in row.get("evidence", "").split(" || "):
        lines.append("evidence: " + part)
    if row.get("conflict"):
        for part in row["conflict"].split(" ;; "):
            lines.append("conflict: " + part)
    return "\n".join(lines)


def main():
    opt = parse_args(getScriptArgs())                               # noqa: F821
    dry = DRY_RUN and not opt["apply"]
    path = find_csv(opt)
    if not os.path.exists(path):
        print("apply_labels: CSV not found: %s" % path)
        return
    rows = read_rows(path)
    prog = currentProgram                                           # noqa: F821
    symtab = prog.getSymbolTable()
    fmgr = prog.getFunctionManager()
    listing = prog.getListing()
    mem = prog.getMemory()
    space = prog.getAddressFactory().getDefaultAddressSpace()
    counts = {}

    def report(action, addr_s, name, detail=""):
        counts[action] = counts.get(action, 0) + 1
        print("%-16s %s %-44s %s" % (action, addr_s, name, detail))

    print("apply_labels: program=%s csv=%s rows=%d mode=%s"
          % (prog.getName(), path, len(rows), "DRY RUN (no changes)" if dry else "APPLY"))

    for row in rows:
        if monitor.isCancelled():                                   # noqa: F821
            break
        addr_s = (row.get("address") or "").strip()
        name = (row.get("name") or "").strip()
        conf = (row.get("confidence") or "").strip()
        try:
            val = int(addr_s, 16)
        except ValueError:
            report("BAD_ROW", addr_s, name, "address is not hexadecimal")
            continue
        if not name:
            report("BAD_ROW", addr_s, name, "empty name")
            continue
        if opt["only"] and opt["only"] not in (row.get("source_reports") or "").split(";"):
            continue
        if opt["min_conf"] and TIER_RANK.get(conf, 0) < TIER_RANK.get(opt["min_conf"], 0):
            continue
        code = is_code_row(name, val)
        if code and (val & 1):
            val &= ~1                                               # Thumb bit never belongs in a label address
        if code and opt["prefix"] and conf in ("maybe", "unk") and not name.startswith(conf + "_"):
            name = conf + "_" + name
        addr = space.getAddress(val)
        if not mem.contains(addr):
            report("NO_MEMORY", addr_s, name, "address is not in any memory block")
            continue

        func_at = fmgr.getFunctionAt(addr)
        func_in = fmgr.getFunctionContaining(addr)
        primary = symtab.getPrimarySymbol(addr)

        # 1. the address already carries exactly this name
        already = False
        for s in symtab.getSymbols(addr):
            if s.getName() == name:
                already = True
        if already:
            report("SAME", addr_s, name)
            continue

        # 2. never overwrite a non-default name
        if not is_default(primary):
            report("KEEP", addr_s, name, "existing %s name '%s' kept"
                   % (primary.getSource(), primary.getName()))
            continue

        # 3. the same name somewhere else in the program
        other = name_elsewhere(symtab, name, addr)
        if other and not opt["allow_dup"]:
            report("NAME_ELSEWHERE", addr_s, name,
                   "name already used at %s; row skipped" % ", ".join(str(a) for a in other[:4]))
            continue

        try:
            if func_at is not None:
                if not dry:
                    func_at.setName(name, SourceType.USER_DEFINED)
                report("RENAME_FUNCTION", addr_s, name, "was '%s'" % func_at.getName())
            else:
                action = "CREATE_LABEL"
                detail = ""
                if code and func_in is not None:
                    action = "INSIDE_FUNCTION"
                    detail = "label only; address lies inside %s at %s" % (
                        func_in.getName(), func_in.getEntryPoint())
                elif code and opt["create_functions"]:
                    if listing.getInstructionAt(addr) is not None:
                        action = "CREATE_FUNCTION"
                        if not dry:
                            f = createFunction(addr, name)                # noqa: F821
                            if f is None:
                                action = "CREATE_LABEL"
                                detail = "createFunction failed; label only"
                            else:
                                f.setName(name, SourceType.USER_DEFINED)
                    else:
                        detail = "no instruction at address (not disassembled); label only"
                elif code and listing.getInstructionAt(addr) is None:
                    detail = "note: no instruction at address (not disassembled yet)"
                if action != "CREATE_FUNCTION" and not dry:
                    symtab.createLabel(addr, name, SourceType.USER_DEFINED).setPrimary()
                report(action, addr_s, name, detail)

            if opt["comments"]:
                existing = listing.getComment(CodeUnit.PLATE_COMMENT, addr)
                if existing is None or len(str(existing).strip()) == 0:
                    if not dry:
                        listing.setComment(addr, CodeUnit.PLATE_COMMENT, plate_text(row))
                    counts["PLATE_COMMENT"] = counts.get("PLATE_COMMENT", 0) + 1
                else:
                    counts["PLATE_COMMENT_KEPT"] = counts.get("PLATE_COMMENT_KEPT", 0) + 1
        except Exception as exc:                                    # keep going, report the row
            report("ERROR", addr_s, name, str(exc))

    print("apply_labels: summary (%s)" % ("dry run, nothing was changed" if dry else "applied"))
    for k in sorted(counts):
        print("  %-18s %d" % (k, counts[k]))
    if dry:
        print("apply_labels: re-run with --apply to write; the program is not saved by this script.")


main()
