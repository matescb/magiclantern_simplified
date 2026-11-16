"""Shared helpers for the symgate tools (Magic Lantern .sym files).

A .sym file is what platform/Makefile generates from the core ELF:
one "<8 hex digits> <name>" line per global symbol, sorted by address.
Thumb functions carry bit 0 set (0xe5319 == code at 0xe5318).
"""
from __future__ import annotations

import hashlib
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

SYM_LINE = re.compile(r"^([0-9a-fA-F]{1,8})\s+(\S+)\s*$")
ROM_BASE = 0xE0000000
SECTION_END_SYMBOLS = (
    "_text_start", "_text_end", "_data_start", "_bss_start", "_bss_end",
    "ml_reserved_mem",
)
NM = "arm-none-eabi-nm"
READELF = "arm-none-eabi-readelf"


class SymError(Exception):
    """Raised for unreadable / malformed inputs."""


@dataclass(frozen=True)
class SymTable:
    path: str
    symbols: dict          # name -> address
    duplicates: tuple      # names defined more than once with different addrs
    md5: str

    def by_address(self):
        return sorted(self.symbols.items(), key=lambda kv: (kv[1], kv[0]))


def md5_of(path) -> str:
    return hashlib.md5(Path(path).read_bytes()).hexdigest()


def parse_sym_text(text: str, path: str = "<text>", md5: str = "") -> SymTable:
    symbols: dict = {}
    dups = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = SYM_LINE.match(line)
        if not m:
            raise SymError(f"{path}:{lineno}: not a '<hexaddr> <name>' line: {raw!r}")
        addr, name = int(m.group(1), 16), m.group(2)
        if name in symbols and symbols[name] != addr:
            dups.append(name)
        symbols[name] = addr
    if not symbols:
        raise SymError(f"{path}: no symbols found")
    return SymTable(path=path, symbols=symbols, duplicates=tuple(dups), md5=md5)


def load_sym(path) -> SymTable:
    p = Path(path)
    try:
        data = p.read_bytes()
    except OSError as exc:
        raise SymError(f"cannot read {p}: {exc}") from exc
    return parse_sym_text(data.decode("ascii", errors="replace"), str(p),
                          hashlib.md5(data).hexdigest())


def is_rom(addr: int) -> bool:
    return addr >= ROM_BASE


@dataclass(frozen=True)
class SymDiff:
    added: tuple       # (name, addr)
    removed: tuple     # (name, addr)
    moved: tuple       # (name, old, new) sorted by old address

    @property
    def moved_core(self):
        return tuple(m for m in self.moved if not is_rom(m[1]))

    @property
    def moved_rom(self):
        return tuple(m for m in self.moved if is_rom(m[1]))

    def first_divergence(self):
        """Lowest baseline address of a moved/removed core symbol, or None."""
        cands = [old for _, old, _ in self.moved_core]
        cands += [a for _, a in self.removed if not is_rom(a)]
        cands += [a for _, a in self.added if not is_rom(a)]
        return min(cands) if cands else None

    def shift_histogram(self):
        """{delta: count} over moved core symbols."""
        hist: dict = {}
        for _, old, new in self.moved_core:
            hist[new - old] = hist.get(new - old, 0) + 1
        return dict(sorted(hist.items(), key=lambda kv: -kv[1]))

    def total_shift(self):
        """Shift of the highest-addressed moved core symbol (accumulated)."""
        core = self.moved_core
        if not core:
            return 0
        _, old, new = max(core, key=lambda m: m[1])
        return new - old


def diff_syms(base: SymTable, new: SymTable) -> SymDiff:
    b, n = base.symbols, new.symbols
    added = sorted(((k, n[k]) for k in n.keys() - b.keys()), key=lambda x: x[1])
    removed = sorted(((k, b[k]) for k in b.keys() - n.keys()), key=lambda x: x[1])
    moved = sorted(((k, b[k], n[k]) for k in b.keys() & n.keys() if b[k] != n[k]),
                   key=lambda x: x[1])
    return SymDiff(tuple(added), tuple(removed), tuple(moved))


def run_tool(args) -> str:
    try:
        res = subprocess.run(args, capture_output=True, text=True, check=False)
    except FileNotFoundError as exc:
        raise SymError(f"tool not found: {args[0]}") from exc
    if res.returncode != 0:
        raise SymError(f"{' '.join(map(str, args))} failed: {res.stderr.strip()}")
    return res.stdout


def elf_layout(elf) -> dict:
    """Section-end symbols (nm) and allocated section addr/size (readelf)."""
    out: dict = {"symbols": {}, "sections": {}}
    for line in run_tool([NM, str(elf)]).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in SECTION_END_SYMBOLS:
            out["symbols"][parts[2]] = int(parts[0], 16)
    sec_re = re.compile(
        r"^\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]{8})\s+[0-9a-f]+\s+([0-9a-f]+)\s+\S+\s+(\S*A\S*)\s")
    for line in run_tool([READELF, "-S", "-W", str(elf)]).splitlines():
        m = sec_re.match(line)
        if m:
            addr, size = int(m.group(2), 16), int(m.group(3), 16)
            out["sections"][m.group(1)] = {"addr": addr, "size": size, "end": addr + size}
    return out


def elf_sym(elf) -> SymTable:
    """Regenerate the .sym content from an ELF the way platform/Makefile does."""
    lines = []
    for line in run_tool([READELF, "-sW", str(elf)]).splitlines():
        f = line.split()
        if len(f) != 8 or f[4] != "GLOBAL" or f[6] == "UND":
            continue
        if f[3] not in ("FUNC", "OBJECT", "NOTYPE") and f[6] != "ABS":
            continue
        name = f[7]
        if "__config_" in name or re.match(r"_[a-zA-Z]", name):
            continue
        lines.append(f"{f[1]} {name}")
    return parse_sym_text("\n".join(sorted(lines)), f"{elf} (regenerated)")


def module_undefined(mo) -> list:
    out = run_tool([NM, "-u", str(mo)])
    return sorted({ln.split()[-1] for ln in out.splitlines() if ln.split()})


def module_defined(mo) -> set:
    out = run_tool([NM, "-g", "--defined-only", str(mo)])
    return {ln.split()[-1] for ln in out.splitlines() if len(ln.split()) == 3}


def hx(v: int) -> str:
    return f"0x{v:x}"
