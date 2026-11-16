#!/usr/bin/env python3
"""Gate: does the .sym on a card (or staging dir / FAT image) belong to the
autoexec.bin next to it, and can every module on the card resolve its imports?

Usage:
  check_card_image.py <dir | fat-image> [--elf build/magiclantern]
        [--load-addr HEX | --no-delta-check] [--workdir DIR]
        [--write-manifest]            (directory input only)

What can be compared (the ML core has NO built-in sym/core consistency check,
see README): autoexec.bin contains the core image uncompressed, so a symbol
address A maps to file offset A + delta (delta = blob offset - load address).
  1. sym filename == the "ML/MODULES/<name>.sym" string inside autoexec.bin
  2. build_user/build_date/build_id/build_version (version.c): the bytes at
     the addresses the .sym claims must be exactly those C strings
  3. delta derived from (2) must map the load address (RESTARTSTART 0xc78e0)
     onto the core entry branch right after the loader stub (absolute anchor)
  4. Thumb prologue score: fraction of function symbols (odd address) whose
     first instruction in autoexec.bin is a push; a mismatched .sym scores
     near the shifted-control level
  5. --elf: .sym regenerated from the ELF must equal the card .sym (exact)
  6. every ML/modules/*.mo import resolves in the card .sym
  7. ML/SYMGATE.TXT manifest (md5 of autoexec.bin/.sym/.mo), if present

Exit status: 0 consistent, 1 mismatch, 2 input error.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import symlib
from symlib import hx

HERE = Path(__file__).resolve().parent
DEFAULT_META = HERE / "baseline" / "BASELINE.json"
MANIFEST_NAME = "SYMGATE.TXT"
VERSION_SYMS = ("build_user", "build_date", "build_id", "build_version")
DATE_RE = re.compile(rb"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d UTC\x00")
VERSION_RE = re.compile(r"^\d{4}-\d\d-\d\d\.\w+\.\d+$|^\S+\.\w+\.\d+$")
SYM_PATH_RE = re.compile(rb"ML/MODULES/([\w.-]+\.sym)\x00", re.I)
PROLOGUE_MIN = 0.30          # measured: matching 0.45, shifted controls <= 0.09
PROLOGUE_CONTROL_RATIO = 2.0
PROLOGUE_BINS = 8
PROLOGUE_BIN_MIN = 0.15
PROLOGUE_BIN_MIN_FUNCS = 20
CONTROL_SHIFTS = (2, 4, 8, 16, -2, -4, -8, -16)
BLOB_OFF_MIN = 0x40
BLOB_OFF_MAX = 0x1000


class Report:
    def __init__(self):
        self.failures = []
        self.warnings = []

    def ok(self, msg):
        print(f"  [ok]   {msg}")

    def fail(self, msg):
        print(f"  [FAIL] {msg}")
        self.failures.append(msg)

    def warn(self, msg):
        print(f"  [warn] {msg}")
        self.warnings.append(msg)

    def info(self, msg):
        print(f"  [info] {msg}")


def find_ci(root: Path, *parts):
    """Case-insensitive path lookup below root; None if absent."""
    cur = root
    for part in parts:
        if not cur.is_dir():
            return None
        nxt = [c for c in cur.iterdir() if c.name.lower() == part.lower()]
        if not nxt:
            return None
        cur = nxt[0]
    return cur


def extract_image(image: Path, workdir) -> Path:
    """Extract autoexec.bin + ML/modules from a FAT image with 7z (no sudo)."""
    if not shutil.which("7z"):
        raise symlib.SymError("7z not found (needed to read FAT images without mtools)")
    out = Path(tempfile.mkdtemp(prefix="symgate_card_", dir=workdir))
    res = subprocess.run(
        ["7z", "x", "-y", f"-o{out}", str(image),
         "-ir!autoexec.bin", "-ir!*.sym", "-ir!*.mo", f"-ir!{MANIFEST_NAME}"],
        capture_output=True, text=True, check=False)
    if res.returncode not in (0, 1):
        raise symlib.SymError(f"7z failed on {image}: {res.stdout[-300:]}{res.stderr[-300:]}")
    return out


def cstring(blob: bytes, off: int, maxlen: int = 128):
    """Printable NUL-terminated string at off, or None."""
    if off < 0 or off >= len(blob):
        return None
    end = blob.find(b"\x00", off, off + maxlen)
    if end <= off:
        return None
    raw = blob[off:end]
    if any(c < 0x20 or c > 0x7e for c in raw):
        return None
    return raw.decode("ascii")


def derive_delta(blob: bytes, sym: symlib.SymTable):
    """Candidate deltas from every build_date-looking string in the binary."""
    addr = sym.symbols.get("build_date")
    if addr is None:
        return []
    return [m.start() - addr for m in DATE_RE.finditer(blob)]


def blob_start_ok(blob, delta, load_addr):
    """True if (load_addr + delta) looks like the start of the embedded core:
    4-aligned, inside a plausible loader-stub size, and an ARM 'b' instruction
    (entry.S begins with an unconditional branch)."""
    off = load_addr + delta
    return (BLOB_OFF_MIN <= off < BLOB_OFF_MAX and off % 4 == 0
            and off + 4 <= len(blob) and blob[off + 3] == 0xEA)


def pick_delta(blob, cands, load_addr):
    if load_addr is not None:
        for cand in cands:
            if blob_start_ok(blob, cand, load_addr):
                return cand
    return cands[0]


def check_blob_start(blob, delta, load_addr, skip, rep):
    """Absolute anchor: a uniformly shifted .sym would hide in the derived
    delta, so require that delta puts the load address on the core entry."""
    if load_addr is None or skip:
        rep.warn(f"address->file delta {delta:+#x} not anchored (no load address / "
                 "--no-delta-check): a uniformly shifted .sym would go unnoticed")
        return
    off = load_addr + delta
    if blob_start_ok(blob, delta, load_addr):
        rep.ok(f"address->file delta {delta:+#x}: load address {hx(load_addr)} maps to "
               f"file offset {hx(off)}, which is the core entry branch")
    else:
        rep.fail(f"address->file delta {delta:+#x} maps load address {hx(load_addr)} to "
                 f"file offset {off:#x}, which is not the core entry (expected a "
                 "4-aligned ARM branch just after the loader stub): the .sym is shifted "
                 "against this autoexec.bin, or RESTARTSTART changed (--load-addr)")


def check_version_strings(blob, sym, delta, rep):
    """Verify version.c strings sit exactly where the .sym says. Returns dict."""
    found = {}
    have = [(sym.symbols[n], n) for n in VERSION_SYMS if n in sym.symbols]
    if len(have) < len(VERSION_SYMS):
        rep.fail("sym lacks version.c symbols: "
                 + ", ".join(n for n in VERSION_SYMS if n not in sym.symbols))
    for addr, name in sorted(have):
        off = addr + delta
        text = cstring(blob, off)
        starts_clean = off == 0 or (0 < off < len(blob) and blob[off - 1] == 0)
        if text is None or len(text) < 3 or not starts_clean:
            rep.fail(f"{name} @ {hx(addr)}: bytes at file offset {hx(max(off, 0))} are not "
                     f"the start of a C string ({blob[max(off, 0):max(off, 0) + 12].hex()})")
            continue
        found[name] = (addr, text)
    if "build_date" in found and not DATE_RE.match(found["build_date"][1].encode() + b"\x00"):
        rep.fail(f"build_date does not look like a build date: {found['build_date'][1]!r}")
    if "build_version" in found and not VERSION_RE.match(found["build_version"][1]):
        rep.fail(f"build_version looks wrong: {found['build_version'][1]!r}")
    chain = sorted(found.values())
    for (a0, t0), (a1, _) in zip(chain, chain[1:]):
        gap = a1 - a0 - (len(t0) + 1)
        if 0 <= a1 - a0 <= 256 and gap not in (0, 1, 2, 3):
            rep.fail(f"version strings not contiguous: {hx(a0)}+{len(t0) + 1} != {hx(a1)}")
    if len(found) == len(VERSION_SYMS) and not rep.failures:
        rep.ok("version.c strings found exactly at the addresses the .sym claims")
    for name in VERSION_SYMS:
        if name in found:
            rep.info(f"{name:<13} @ {hx(found[name][0])} = {found[name][1]!r}")
    return found


def prologue_stats(blob, sym, delta, shift=0):
    """(matches, total, per-bin list) for Thumb function symbols inside the blob."""
    funcs = sorted(a for a in sym.symbols.values()
                   if a & 1 and not symlib.is_rom(a) and 0 <= (a & ~1) + delta < len(blob) - 1)
    hits = []
    for addr in funcs:
        off = (addr & ~1) + delta + shift
        if not 0 <= off < len(blob) - 1:
            hits.append(False)
            continue
        word = blob[off] | (blob[off + 1] << 8)
        hits.append((word & 0xFE00) == 0xB400 or word == 0xE92D)
    bins = []
    if funcs:
        step = max(1, -(-len(funcs) // PROLOGUE_BINS))
        for i in range(0, len(funcs), step):
            chunk = hits[i:i + step]
            bins.append((funcs[i], len(chunk), sum(chunk)))
    return sum(hits), len(funcs), bins


def check_prologues(blob, sym, delta, rep):
    hit, total, bins = prologue_stats(blob, sym, delta)
    if total < 50:
        rep.warn(f"only {total} Thumb function symbols map into autoexec.bin; "
                 "prologue check skipped")
        return
    score = hit / total
    control = max(prologue_stats(blob, sym, delta, s)[0] for s in CONTROL_SHIFTS) / total
    line = f"prologue score {hit}/{total} = {score:.2f} (best shifted control {control:.2f})"
    if score < PROLOGUE_MIN or score < control * PROLOGUE_CONTROL_RATIO:
        rep.fail(line + " - function symbols do not point at function entries")
    else:
        rep.ok(line)
    weak = [(a, n, h) for a, n, h in bins
            if n >= PROLOGUE_BIN_MIN_FUNCS and h / n < PROLOGUE_BIN_MIN]
    rep.info("per-range score: " + "  ".join(f"{hx(a)}:{h}/{n}" for a, n, h in bins))
    if weak:
        rep.fail("layout diverges part-way: prologue score collapses from "
                 f"{hx(weak[0][0])} (first weak address range)")


def check_modules(mod_dir, sym, rep):
    mods = sorted(p for p in mod_dir.iterdir() if p.suffix.lower() == ".mo")
    if not mods:
        rep.info("no .mo files on the card")
        return
    providers = {str(m): symlib.module_defined(m) for m in mods}
    for mo in mods:
        imports = symlib.module_undefined(mo)
        missing = [n for n in imports if n not in sym.symbols
                   and not any(n in d for p, d in providers.items() if p != str(mo))]
        if missing:
            rep.fail(f"{mo.name}: {len(missing)} import(s) missing from card .sym: "
                     + ", ".join(missing[:8]))
        else:
            rep.ok(f"{mo.name}: {len(imports)} imports resolvable")


def manifest_content(autoexec, sym_path, mod_dir, strings):
    files = {"autoexec.bin": symlib.md5_of(autoexec),
             f"ML/modules/{sym_path.name}": symlib.md5_of(sym_path)}
    for mo in sorted(p for p in mod_dir.iterdir() if p.suffix.lower() == ".mo"):
        files[f"ML/modules/{mo.name}"] = symlib.md5_of(mo)
    return {"symgate_manifest": 1,
            "build": {k: v[1] for k, v in strings.items()},
            "md5": files}


def check_manifest(root, autoexec, sym_path, mod_dir, rep):
    man = find_ci(root, "ML", MANIFEST_NAME)
    if man is None:
        rep.info(f"no ML/{MANIFEST_NAME} manifest on the card (optional)")
        return
    try:
        want = json.loads(man.read_text())["md5"]
    except (OSError, ValueError, KeyError) as exc:
        rep.fail(f"unreadable manifest {man}: {exc}")
        return
    have = manifest_content(autoexec, sym_path, mod_dir, {})["md5"]
    lower_have = {k.lower(): v for k, v in have.items()}
    bad = [k for k, v in want.items() if lower_have.get(k.lower()) != v]
    extra = [k for k in have if k.lower() not in {w.lower() for w in want}]
    if bad:
        rep.fail("manifest md5 mismatch / missing: " + ", ".join(bad))
    else:
        rep.ok(f"manifest ML/{MANIFEST_NAME}: {len(want)} file md5s match")
    if extra:
        rep.warn("files not covered by manifest: " + ", ".join(extra))


def run_checks(root: Path, args, rep: Report) -> None:
    autoexec = find_ci(root, "autoexec.bin")
    mod_dir = find_ci(root, "ML", "modules")
    if autoexec is None:
        raise symlib.SymError(f"no autoexec.bin in {root}")
    if mod_dir is None:
        raise symlib.SymError(f"no ML/modules directory in {root}")
    syms = sorted(p for p in mod_dir.iterdir() if p.suffix.lower() == ".sym")
    blob = autoexec.read_bytes()
    print(f"autoexec.bin: {len(blob)} bytes  md5 {symlib.md5_of(autoexec)}")

    wanted = SYM_PATH_RE.search(blob)
    wanted_name = wanted.group(1).decode() if wanted else None
    if wanted_name is None:
        rep.warn("autoexec.bin has no 'ML/MODULES/<x>.sym' string (modules disabled?)")
    if not syms:
        rep.fail(f"no .sym in ML/modules (autoexec wants {wanted_name})")
        return
    sym_path = next((p for p in syms if wanted_name and p.name.lower() == wanted_name.lower()),
                    None)
    if sym_path is None:
        sym_path = syms[0]
        if wanted_name:
            rep.fail(f"autoexec loads ML/MODULES/{wanted_name} but card has "
                     + ", ".join(p.name for p in syms))
    else:
        rep.ok(f"sym filename matches the name embedded in autoexec.bin: {sym_path.name}")
    sym = symlib.load_sym(sym_path)
    print(f"sym: {sym_path.name}  md5 {sym.md5}  ({len(sym.symbols)} symbols)")

    load_addr = None
    if args.load_addr is not None:
        load_addr = int(args.load_addr, 0)
    elif DEFAULT_META.exists():
        meta = json.loads(DEFAULT_META.read_text())
        if meta.get("core_load_address") is not None:
            load_addr = int(meta["core_load_address"], 0)

    cands = derive_delta(blob, sym)
    strings = {}
    if not cands:
        rep.fail("cannot locate the core image: no build-date string in autoexec.bin "
                 "or no build_date symbol in the .sym")
    else:
        delta = pick_delta(blob, cands, load_addr)
        check_blob_start(blob, delta, load_addr, args.no_delta_check, rep)
        strings = check_version_strings(blob, sym, delta, rep)
        check_prologues(blob, sym, delta, rep)

    if args.elf:
        regenerated = symlib.elf_sym(args.elf)
        d = symlib.diff_syms(regenerated, sym)
        if d.added or d.removed or d.moved:
            rep.fail(f"card .sym differs from ELF {args.elf}: {len(d.moved)} moved, "
                     f"{len(d.removed)} missing, {len(d.added)} extra")
        else:
            rep.ok(f"card .sym is exactly the symbol table of {args.elf}")

    check_modules(mod_dir, sym, rep)

    if args.write_manifest:
        if rep.failures:
            rep.warn("not writing manifest: checks failed")
        else:
            target = mod_dir.parent / MANIFEST_NAME
            target.write_text(json.dumps(
                manifest_content(autoexec, sym_path, mod_dir, strings), indent=2) + "\n")
            rep.ok(f"wrote manifest {target}")
    else:
        check_manifest(root, autoexec, sym_path, mod_dir, rep)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("card", help="directory (card root / build/zip) or FAT image file")
    ap.add_argument("--elf", help="core ELF (build/magiclantern) for an exact comparison")
    ap.add_argument("--load-addr", help="core load address RESTARTSTART "
                    "(default: baseline/BASELINE.json core_load_address = 0xc78e0)")
    ap.add_argument("--no-delta-check", action="store_true")
    ap.add_argument("--workdir", help="where to extract FAT images (default: system temp)")
    ap.add_argument("--write-manifest", action="store_true",
                    help=f"write ML/{MANIFEST_NAME} after a passing check (directory only)")
    args = ap.parse_args(argv)

    card = Path(args.card)
    rep = Report()
    tmp = None
    try:
        if card.is_dir():
            root = card
        elif card.is_file():
            if args.write_manifest:
                print("ERROR: --write-manifest needs a directory", file=sys.stderr)
                return 2
            tmp = extract_image(card, args.workdir)
            root = tmp
        else:
            print(f"ERROR: {card} is neither a directory nor a file", file=sys.stderr)
            return 2
        print(f"card: {card}")
        run_checks(root, args, rep)
    except symlib.SymError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    finally:
        if tmp is not None:
            shutil.rmtree(tmp, ignore_errors=True)

    if rep.failures:
        print(f"RESULT: FAIL - {len(rep.failures)} problem(s); do NOT boot this card")
        return 1
    print("RESULT: PASS - .sym, autoexec.bin and modules are mutually consistent"
          + (f" ({len(rep.warnings)} warning(s))" if rep.warnings else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
