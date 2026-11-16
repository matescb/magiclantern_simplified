"""pytest tests for tools/symgate (synthetic .sym / autoexec fixtures).

Run:  cd /mnt/data/WORK/eos/tools/symgate && python3 -m pytest -q tests
"""
import json
import struct
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import check_card_image  # noqa: E402
import check_module_syms  # noqa: E402
import check_symbol_stability  # noqa: E402
import symlib  # noqa: E402

BASE_SYM = """\
00001010 current_task
000c8001 alpha
000c8041 beta
000c8081 gamma
000d0000 raw_info
e0001235 msleep
"""


def write(tmp_path, name, text):
    p = tmp_path / name
    p.write_text(text)
    return p


# ---------------------------------------------------------------- symlib

def test_parse_sym_reads_addresses_and_ignores_comments():
    tbl = symlib.parse_sym_text("# c\n000c8001 alpha\n\ne0001235 msleep\n")
    assert tbl.symbols == {"alpha": 0xC8001, "msleep": 0xE0001235}


def test_parse_sym_rejects_garbage_line():
    with pytest.raises(symlib.SymError):
        symlib.parse_sym_text("000c8001 alpha\nnot a symbol line here\n")


def test_parse_sym_rejects_empty_file():
    with pytest.raises(symlib.SymError):
        symlib.parse_sym_text("\n\n")


def test_diff_reports_first_divergence_and_total_shift():
    base = symlib.parse_sym_text(BASE_SYM)
    new = symlib.parse_sym_text(BASE_SYM.replace("000c8041 beta", "000c8049 beta")
                                .replace("000c8081 gamma", "000c8091 gamma"))
    diff = symlib.diff_syms(base, new)
    assert [m[0] for m in diff.moved] == ["beta", "gamma"]
    assert diff.first_divergence() == 0xC8041
    assert diff.total_shift() == 0x10
    assert diff.shift_histogram() == {8: 1, 16: 1}


def test_diff_separates_rom_stub_moves_from_core_moves():
    base = symlib.parse_sym_text(BASE_SYM)
    new = symlib.parse_sym_text(BASE_SYM.replace("e0001235", "e0001239"))
    diff = symlib.diff_syms(base, new)
    assert diff.moved_core == ()
    assert [m[0] for m in diff.moved_rom] == ["msleep"]


# ------------------------------------------------- check_symbol_stability

def test_stability_passes_on_identical_layout(tmp_path, capsys):
    b = write(tmp_path, "base.sym", BASE_SYM)
    n = write(tmp_path, "new.sym", BASE_SYM)
    assert check_symbol_stability.main([str(n), "--baseline", str(b)]) == 0
    assert "RESULT: PASS" in capsys.readouterr().out


def test_stability_passes_when_symbol_only_added_at_end(tmp_path, capsys):
    b = write(tmp_path, "base.sym", BASE_SYM)
    n = write(tmp_path, "new.sym", BASE_SYM + "000d0100 new_thing\n")
    assert check_symbol_stability.main([str(n), "--baseline", str(b)]) == 0
    out = capsys.readouterr().out
    assert "added: 1" in out and "new_thing" in out


def test_stability_fails_when_existing_symbol_moved(tmp_path, capsys):
    b = write(tmp_path, "base.sym", BASE_SYM)
    n = write(tmp_path, "new.sym", BASE_SYM.replace("000c8081 gamma", "000c8119 gamma")
              .replace("000d0000 raw_info", "000d0098 raw_info"))
    assert check_symbol_stability.main([str(n), "--baseline", str(b)]) == 1
    out = capsys.readouterr().out
    assert "first divergence at baseline address: 0xc8081" in out
    assert "total shift (highest moved core symbol): +152 bytes" in out
    assert "RESULT: FAIL" in out


def test_stability_ignore_option_tolerates_named_symbol_only(tmp_path, capsys):
    b = write(tmp_path, "base.sym", BASE_SYM)
    n = write(tmp_path, "new.sym", BASE_SYM.replace("000d0000 raw_info", "000cfffd raw_info"))
    assert check_symbol_stability.main([str(n), "--baseline", str(b)]) == 1
    assert check_symbol_stability.main(
        [str(n), "--baseline", str(b), "--ignore", "raw_info"]) == 0
    assert "ignored move (--ignore): 0xd0000 -> 0xcfffd (-3) raw_info" in capsys.readouterr().out
    assert check_symbol_stability.main(
        [str(n), "--baseline", str(b), "--ignore", "alpha"]) == 1


def test_stability_fails_on_removed_symbol_unless_allowed(tmp_path):
    b = write(tmp_path, "base.sym", BASE_SYM)
    n = write(tmp_path, "new.sym", BASE_SYM.replace("000c8041 beta\n", ""))
    assert check_symbol_stability.main([str(n), "--baseline", str(b)]) == 1
    assert check_symbol_stability.main(
        [str(n), "--baseline", str(b), "--allow-removed"]) == 0


def test_stability_checks_anchors_from_meta(tmp_path, capsys):
    b = write(tmp_path, "base.sym", BASE_SYM)
    n = write(tmp_path, "new.sym", BASE_SYM)
    meta = write(tmp_path, "meta.json", json.dumps(
        {"anchors": {"raw_info": "0xd0040", "ghost": None}}))
    rc = check_symbol_stability.main([str(n), "--baseline", str(b), "--meta", str(meta)])
    assert rc == 1
    assert "anchor raw_info: expected 0xd0040, got 0xd0000" in capsys.readouterr().out


def test_stability_refuses_tampered_baseline(tmp_path):
    b = write(tmp_path, "base.sym", BASE_SYM)
    meta = write(tmp_path, "meta.json", json.dumps({"sym_md5": "0" * 32}))
    assert check_symbol_stability.main([str(b), "--baseline", str(b),
                                        "--meta", str(meta)]) == 2


def test_stability_missing_file_is_input_error(tmp_path):
    b = write(tmp_path, "base.sym", BASE_SYM)
    assert check_symbol_stability.main([str(tmp_path / "nope.sym"),
                                        "--baseline", str(b)]) == 2


def test_stability_section_end_change_fails(tmp_path, monkeypatch, capsys):
    b = write(tmp_path, "base.sym", BASE_SYM)
    meta = write(tmp_path, "meta.json", json.dumps(
        {"elf_layout": {"symbols": {"_bss_end": "0x14fc00"}, "sections": {}}}))
    monkeypatch.setattr(symlib, "elf_layout", lambda elf: {
        "symbols": {"_bss_end": 0x180700}, "sections": {}})
    rc = check_symbol_stability.main([str(b), "--baseline", str(b), "--meta", str(meta),
                                      "--elf", "fake.elf"])
    assert rc == 1
    assert "_bss_end" in capsys.readouterr().out


# ------------------------------------------------------ check_module_syms

def fake_nm(table):
    def run(args):
        mo = Path(args[-1]).name
        if "-u" in args:
            return "".join(f"         U {n}\n" for n in table[mo]["undef"])
        return "".join(f"00000000 T {n}\n" for n in table[mo]["defs"])
    return run


def test_module_passes_when_all_imports_in_core(tmp_path, monkeypatch):
    core = write(tmp_path, "core.sym", BASE_SYM)
    monkeypatch.setattr(symlib, "run_tool", fake_nm(
        {"a.mo": {"undef": ["alpha", "msleep"], "defs": []}}))
    assert check_module_syms.main(["a.mo", str(core)]) == 0


def test_module_fails_and_names_missing_import(tmp_path, monkeypatch, capsys):
    core = write(tmp_path, "core.sym", BASE_SYM)
    monkeypatch.setattr(symlib, "run_tool", fake_nm(
        {"a.mo": {"undef": ["alpha", "socket_create"], "defs": []}}))
    assert check_module_syms.main(["a.mo", str(core)]) == 1
    assert "MISSING socket_create" in capsys.readouterr().out


def test_module_import_satisfied_by_other_module(tmp_path, monkeypatch, capsys):
    core = write(tmp_path, "core.sym", BASE_SYM)
    monkeypatch.setattr(symlib, "run_tool", fake_nm({
        "a.mo": {"undef": ["helper"], "defs": []},
        "b.mo": {"undef": [], "defs": ["helper"]}}))
    assert check_module_syms.main(["a.mo", "b.mo", str(core)]) == 0
    assert "(provided by b.mo) helper" in capsys.readouterr().out


# ------------------------------------------------------- check_card_image

LOAD = 0xC78E0
STUB = 0x150
STRINGS = [("build_user", "tester@host"), ("build_date", "2026-01-02 03:04:05 UTC"),
           ("build_id", "abc1234 branch"), ("build_version", "2026-01-02.M50mk2.101")]


def make_card(tmp_path, sym_shift=0, shift_from=0, sym_name="M50mk2_101.sym"):
    """Synthetic card: loader stub + core (entry branch, 240 Thumb funcs with a
    push prologue every 0x40 bytes, version strings) and a matching .sym.
    sym_shift is added to every .sym address >= shift_from (mismatch injection)."""
    core = bytearray(struct.pack("<I", 0xEA000010))
    syms = {}
    for i in range(240):
        while len(core) % 0x40:
            core += b"\x70\x47"            # bx lr filler (never a push)
        syms[f"func_{i:03d}"] = (LOAD + len(core)) | 1
        core += b"\x10\xb5\x00\x20"         # push {r4, lr}; movs r0, #0
    core += b"\x00\x00"
    for name, text in STRINGS:
        syms[name] = LOAD + len(core)
        core += text.encode() + b"\x00"
    core += b"ML/MODULES/M50mk2_101.sym\x00"
    blob = bytes(STUB) + bytes(core)
    (tmp_path / "ML" / "modules").mkdir(parents=True)
    (tmp_path / "autoexec.bin").write_bytes(blob)
    lines = [f"{a + (sym_shift if a >= shift_from else 0):08x} {n}"
             for n, a in sorted(syms.items(), key=lambda kv: kv[1])]
    lines.append("e0001235 msleep")
    (tmp_path / "ML" / "modules" / sym_name).write_text("\n".join(lines) + "\n")
    return tmp_path


def card_args(card):
    return [str(card), "--load-addr", hex(LOAD)]


def test_card_passes_when_sym_matches_autoexec(tmp_path, capsys):
    assert check_card_image.main(card_args(make_card(tmp_path))) == 0
    out = capsys.readouterr().out
    assert "prologue score 240/" in out and "RESULT: PASS" in out


def test_card_fails_when_whole_sym_uniformly_shifted(tmp_path, capsys):
    card = make_card(tmp_path, sym_shift=0x40)
    assert check_card_image.main(card_args(card)) == 1
    assert "not the core entry" in capsys.readouterr().out


def test_card_fails_when_sym_shifted_part_way(tmp_path, capsys):
    card = make_card(tmp_path, sym_shift=0x98, shift_from=LOAD + 0x2000)
    assert check_card_image.main(card_args(card)) == 1
    out = capsys.readouterr().out
    assert "RESULT: FAIL" in out


def test_card_fails_when_only_tail_functions_shifted(tmp_path, capsys):
    """Shift that leaves the version strings alone but moves late functions."""
    card = make_card(tmp_path)
    sym = card / "ML" / "modules" / "M50mk2_101.sym"
    out_lines = []
    for line in sym.read_text().splitlines():
        addr, name = line.split()
        if name.startswith("func_") and int(name[5:]) >= 120:
            addr = f"{int(addr, 16) + 8:08x}"
        out_lines.append(f"{addr} {name}")
    sym.write_text("\n".join(out_lines) + "\n")
    assert check_card_image.main(card_args(card)) == 1
    assert "layout diverges part-way" in capsys.readouterr().out


def test_card_fails_on_wrong_sym_filename(tmp_path, capsys):
    card = make_card(tmp_path, sym_name="M50_110.sym")
    assert check_card_image.main(card_args(card)) == 1
    assert "autoexec loads ML/MODULES/M50mk2_101.sym" in capsys.readouterr().out


def test_card_fails_when_sym_missing(tmp_path):
    card = make_card(tmp_path)
    (card / "ML" / "modules" / "M50mk2_101.sym").unlink()
    assert check_card_image.main(card_args(card)) == 1


def test_card_without_autoexec_is_input_error(tmp_path):
    (tmp_path / "ML" / "modules").mkdir(parents=True)
    assert check_card_image.main(card_args(tmp_path)) == 2


def test_card_manifest_roundtrip_and_tamper_detection(tmp_path, capsys):
    card = make_card(tmp_path)
    assert check_card_image.main(card_args(card) + ["--write-manifest"]) == 0
    assert (card / "ML" / "SYMGATE.TXT").exists()
    assert check_card_image.main(card_args(card)) == 0
    assert "file md5s match" in capsys.readouterr().out
    sym = card / "ML" / "modules" / "M50mk2_101.sym"
    sym.write_text(sym.read_text() + "000d0100 extra\n")   # still layout-consistent
    assert check_card_image.main(card_args(card)) == 1
    assert "manifest md5 mismatch" in capsys.readouterr().out


# ------------------------------------------------- real build (if present)

REAL_BUILD = Path("/mnt/data/WORK/eos/magiclantern_simplified/platform/M50mk2.101/build")


@pytest.mark.skipif(not (REAL_BUILD / "M50mk2_101.sym").exists(), reason="no real build")
def test_shipped_baseline_matches_its_recorded_md5_and_anchors():
    meta = json.loads((HERE.parent / "baseline" / "BASELINE.json").read_text())
    base = symlib.load_sym(HERE.parent / "baseline" / "M50mk2_101.sym")
    assert base.md5 == meta["sym_md5"]
    assert base.symbols["raw_lv_redirect_edmac"] == 0xE5319
    assert "m50ii_seed_edmac_cbrs" not in base.symbols
