"""Tests for hwprobe: reply parsing, expression evaluation, pass/fail evaluation and gating.

Everything runs against ReplayTransport / DryRunTransport; no camera and no daemon are involved,
so these tests prove the tool's logic, not anything about the camera.
"""
import io
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hwprobe as hp  # noqa: E402

SYM_TEXT = """000c8000 first_ml_function
000e5dd1 raw_lv_request
000f0001 ml_assert_handler
00104434 raw_info
0014efd4 ml_started
e05cbf83 call
"""
P, Q, D, C, DCTX, BLOB, V = 0x700000, 0x701000, 0x702000, 0x703000, 0x704000, 0x705000, 0x706000
RAWBUF = 0x6BC61CC8


def healthy_memory():
    """Sparse word image in which every stage 0-2 prediction of probes.json holds."""
    m = {0x14EFD4: 1, 0x8E8C: P, P + 4: 0xE137558C, P + 0xC: 0xE05BB2B3, P + 0x10: 0xE0B5050C,
         P + 0x14: 0x11, P + 0x18: 8, P + 0x1C: 5,
         0x10108: Q, Q + 0x10: 0xE0BBE0F0, Q + 0x1C: 3,
         0x9B48: D, D + 8: D + 0x100, D + 0x14: 0xE, D + 0x18: 0xE,
         0x554C: C, C + 8: C + 0x100, C + 0x14: 0xD, C + 0x18: 0x13,
         0x10284: 1, 0x10288: 1, 0x1029C: 0xE06AD51B,
         0x5280: 0x123400, 0x51F4: 0x5678, 0x4FF4: 0x9ABC00,
         0x5154: DCTX, DCTX + 0x38: 0x05000605, 0x5158: 1, 0x1014: 0xDF001000,
         0x39B84: 0xBEE18, 0x39B88: 1, 0xA590: BLOB, 0x4000: 0xF0001,
         0x10654: 9, 0x216E4: 0xF0014914, 0x50AF8: 1, 0xAB10: V,
         V + 0xB0: [0x1000000, 0x13F4800, 0x17E9000, 0x1000000]}
    table = [0xE057CDDF, 0xE057CD15, 0xE00412B7, 0xE057B849, 0xE057A80D,
             0xE00412A1, 0xE057F49B, 0xE0041295, 0xE057F439, 0xE057F3D5]
    m.update({0x5128 + 4 * i: w for i, w in enumerate(table)})
    m.update({0xBEE18 + 16 * i: 0xBEE18 + 16 * (i + 1) for i in range(1535)})   # last node links to 0
    m.update({0xDF000000 + 4 * i: 0xEEEEEEEE for i in range(128)})
    return m


def raw_enabled_memory():
    m = healthy_memory()
    m.update({0x104438: RAWBUF, 0x10278: 1, 0x10290: RAWBUF & ~0x40000000, 0x10298: 1,
              RAWBUF: [0x11111111, 0x22222222], RAWBUF & ~0x40000000: 0x11111111})
    return m


def as_replay(memory, replies=None):
    return {"memory": {"0x%X" % k: v for k, v in memory.items()}, "replies": replies or {}}


@pytest.fixture
def sym(tmp_path):
    path = tmp_path / "magiclantern.sym"
    path.write_text(SYM_TEXT)
    return str(path)


def run(sym, replay, argv=(), tmp_path=None):
    """Run through the real CLI option parser with a replay transport; return (run dict, transport)."""
    opts = hp.build_parser().parse_args(["run", "--sym", sym] + list(argv))
    doc = json.load(open(opts.probes))
    if opts.only:
        doc["probes"] = [p for p in doc["probes"] if p["id"] in opts.only.split(",")]
    transport = hp.ReplayTransport(replay)
    sess = hp.Session(transport, hp.SymTable(sym), opts, sleep=lambda s: None, out=io.StringIO())
    stages = sorted(int(s) for s in opts.stages.split(","))
    return hp.execute(doc, sess, opts, stages), transport


def by_id(result, pid, stage=None):
    return next(r for r in result["results"] if r["id"] == pid and stage in (None, r["stage"]))


# ---- parsing -------------------------------------------------------------------------------
def test_parse_words_ignores_echo_and_log_fragments():
    body = ["@2 md 0x1000 [ML] <3 n=0x000000A6 phase=0x00000001", "",
            "0x00001000: 0xE51FF004 0xE0000000 0x00000000 0x00000000", "0x00001010: 0x00000001"]
    assert hp.parse_words(body, 0x1000, 5) == [0xE51FF004, 0xE0000000, 0, 0, 1]


def test_parse_words_rejects_short_reply_and_wrong_address():
    with pytest.raises(hp.ProbeError):
        hp.parse_words(["0x00001000: 0xE51FF004 0xE000"], 0x1000, 2)
    with pytest.raises(hp.ProbeError):
        hp.parse_words(["0x00002000: 0x00000001"], 0x1000, 1)


def test_parse_dump_checks_crc():
    good = hp.ReplayTransport(as_replay({0x1000: 0xAABBCCDD})).cmd("dump 0x1000 0x40", 1)["body"]
    assert hp.parse_dump(good, 0x40)[:4] == bytes.fromhex("ddccbbaa")
    with pytest.raises(hp.ProbeError):
        hp.parse_dump(good[:-1] + ["=crc 0x00000000"], 0x40)


def test_parse_call_return_value():
    assert hp.parse_call(["@7 call 0xE05BA079 0x702100", "ret 0x00000003"]) == 3
    with pytest.raises(hp.ProbeError):
        hp.parse_call(["garbage"])


# ---- expressions and symbols ---------------------------------------------------------------
def test_expression_evaluation_with_symbols_and_variables(sym):
    syms = hp.SymTable(sym)
    variables = {"buf": [RAWBUF], "p": [0x10, 0x20]}
    assert hp.evaluate('sym("raw_info") + 4', variables, syms) == 0x104438
    assert hp.evaluate("buf[0] & 0xBFFFFFFF", variables, syms) == 0x2BC61CC8
    assert hp.evaluate("p[1] + 0x1C", variables, syms) == 0x3C
    with pytest.raises(hp.ProbeError):
        hp.evaluate('sym("no_such_symbol")', variables, syms)
    with pytest.raises(hp.ProbeError):
        hp.evaluate("__import__('os')", variables, syms)


def test_freelist_walk_counts_nodes_and_flags_broken_lists():
    pool = bytearray(64)
    pool[0:4] = (0x1010).to_bytes(4, "little")       # node 0 -> node 1 -> end
    assert hp.walk_freelist(bytes(pool), 0x1000, 0x1000, 16) == (2, 1)
    pool[16:20] = (0x9999).to_bytes(4, "little")     # node 1 points outside the pool
    assert hp.walk_freelist(bytes(pool), 0x1000, 0x1000, 16) == (2, 0)
    assert hp.walk_freelist(bytes(pool), 0, 0x1000, 16) == (0, 1)


# ---- evaluation on a healthy image ---------------------------------------------------------
def test_healthy_image_passes_stage_0_and_1_without_any_call_or_write(sym):
    result, transport = run(sym, as_replay(healthy_memory()), ["--stages", "0,1", "--samples", "4"])
    verdicts = {(r["id"], r["stage"]): r["verdict"] for r in result["results"]}
    assert result["aborted"] is None
    assert [k for k, v in verdicts.items() if v in ("FAIL", "ERROR")] == []
    assert verdicts[("A2.1", 0)] == "PASS" and verdicts[("F4.1", 0)] == "PASS"
    assert verdicts[("A2.4", 1)] == "SKIPPED" and verdicts[("A13.1", 0)] == "SKIPPED"
    assert not [c for c in transport.sent if c.split()[0] in ("call", "wr", "exec", "load", "key")]
    assert not [c for c in transport.sent if c.split()[0] in ("rd", "md")
                and c.split()[1].lower().startswith(("0xd0", "0xc1", "0xbf"))]
    assert by_id(result, "F4.1", 0)["values"]["evfree"] == ["0x00000600", "0x00000001"]
    assert by_id(result, "A14c.9")["checks"][0]["observed"].endswith("(ml_assert_handler+0x0)")
    assert len(by_id(result, "A14c.8")["distinct"]["yuv"]) == 3


def test_pointer_reads_marked_once_are_not_repeated_in_the_sampling_loop(sym):
    _, transport = run(sym, as_replay(healthy_memory()), ["--stages", "1", "--only", "F1.2", "--samples", "5"])
    assert transport.sent.count("rd 0x8E8C") == 1
    assert transport.sent.count("rd 0x%X" % (P + 0x1C)) == 5


# ---- evaluation of faults ------------------------------------------------------------------
def test_wrong_evfstate_name_pointer_fails_identity_probe(sym):
    mem = healthy_memory()
    mem[P + 4] = 0xE1375000
    r = by_id(run(sym, as_replay(mem), ["--stages", "0", "--only", "A2.1"])[0], "A2.1")
    assert r["verdict"] == "FAIL"
    failed = [c for c in r["checks"] if c["outcome"] == "fail"]
    assert len(failed) == 1 and "not the EvfState object" in failed[0]["meaning"]


def test_null_object_pointer_skips_instead_of_reading_address_zero(sym):
    mem = healthy_memory()
    mem[0x8E8C] = 0
    result, transport = run(sym, as_replay(mem), ["--stages", "0", "--only", "A2.1"])
    assert by_id(result, "A2.1")["verdict"] == "SKIPPED"
    assert "md 0x0 8" not in transport.sent


def test_ring_count_of_two_in_one_sample_fails_and_is_counted(sym):
    mem = healthy_memory()
    mem[0x23804] = [0, 1, 2, 0]
    r = by_id(run(sym, as_replay(mem), ["--stages", "1", "--only", "A1.4", "--samples", "4"])[0], "A1.4")
    ds = r["checks"][0]
    assert r["verdict"] == "FAIL" and (ds["n_pass"], ds["n_fail"]) == (3, 1)
    assert ds["observed"] == "0x00000002" and r["checks"][1]["n_fail"] == 0


def test_all_lvjob_slots_busy_fails(sym):
    mem = healthy_memory()
    mem[0x50AF8] = 0
    r = by_id(run(sym, as_replay(mem), ["--stages", "1", "--only", "A1.6", "--samples", "2"])[0], "A1.6")
    assert r["verdict"] == "FAIL" and r["checks"][0]["meaning"] == "all six slots busy"


def test_depleted_event_pool_fails_level_check(sym):
    mem = healthy_memory()
    mem[0x39B84] = 0xBEE18 + 16 * 1500             # only 36 nodes left on the list
    r = by_id(run(sym, as_replay(mem), ["--stages", "0", "--only", "F4.1"])[0], "F4.1")
    assert r["verdict"] == "FAIL" and r["values"]["evfree"][0] == "0x00000024"


def test_shredded_reply_is_retried_then_parsed(sym):
    replies = {"rd 0x14EFD4": [["0x0014EFD4: 0x0000[ML] <3 n=0x1"], ["0x0014EFD4: 0x00000001"]]}
    result, transport = run(sym, as_replay(healthy_memory(), replies), ["--stages", "0", "--only", "S0.sym"])
    assert by_id(result, "S0.sym")["verdict"] == "PASS"
    assert transport.sent.count("rd 0x14EFD4") == 2


def test_command_error_gives_error_verdict_not_a_crash(sym):
    replies = {"md 0x5128 10": [{"status": "err", "msg": "bad addr"}]}
    r = by_id(run(sym, as_replay(healthy_memory(), replies), ["--stages", "0", "--only", "A8.1"])[0], "A8.1")
    assert r["verdict"] == "ERROR" and "bad addr" in r["reason"]


# ---- stage 2 -------------------------------------------------------------------------------
def test_stage_2_after_raw_enable_checks_buffer_and_views(sym):
    result, transport = run(sym, as_replay(raw_enabled_memory()), ["--stages", "2", "--samples", "2"])
    assert result["raw_enable"] == {"how": "operator"}
    assert by_id(result, "A1.2")["verdict"] == "PASS"
    assert by_id(result, "A13.2")["verdict"] == "PASS"
    assert "md 0x2BC61CC8 4" in transport.sent and "md 0x6BC61CC8 4" in transport.sent
    assert not [c for c in transport.sent if c.startswith("call")]


def test_stage_2_fails_when_no_buffer_is_supplied(sym):
    mem = raw_enabled_memory()
    mem[0x10290] = 0
    r = by_id(run(sym, as_replay(mem), ["--stages", "2", "--only", "A1.2"])[0], "A1.2")
    assert r["verdict"] == "FAIL"
    assert any("supplies no buffer" in c["meaning"] for c in r["checks"])


def test_enable_raw_by_call_resolves_address_from_sym(sym):
    replies = {"call 0xE5DD1": [["ret 0x00000000"]]}
    result, transport = run(sym, as_replay(raw_enabled_memory(), replies),
                            ["--stages", "2", "--only", "A1.2", "--allow-call", "--enable-raw-by-call"])
    assert transport.sent[1] == "call 0xE5DD1" and result["raw_enable"]["how"] == "call"


# ---- gating --------------------------------------------------------------------------------
def test_allow_call_sends_queue_depth_call_with_taskclass_argument(sym):
    replies = {"call 0xE05BA079 0x%X" % (D + 0x100): [["ret 0x00000002"]],
               "call 0xE05BA079 0x%X" % (C + 0x100): [["ret 0x00000000"]]}
    result, _ = run(sym, as_replay(healthy_memory(), replies),
                    ["--stages", "1", "--only", "A2.4", "--allow-call", "--samples", "3"])
    r = by_id(result, "A2.4")
    assert r["verdict"] == "PASS" and r["n_samples"] == 3


def test_call_timeout_aborts_the_run_and_sends_nothing_more(sym):
    replies = {"call 0xE05BA079 0x%X" % (D + 0x100): [{"status": "timeout"}]}
    result, transport = run(sym, as_replay(healthy_memory(), replies),
                            ["--stages", "1,3", "--allow-call", "--samples", "3"])
    assert "wedged" in result["aborted"]
    assert transport.sent[-1].startswith("call 0xE05BA079")


def test_write_steps_need_allow_wr(sym, tmp_path):
    doc = {"version": 1, "stages": {"0": "t"}, "enable_raw": {}, "log_patterns": {}, "probes": [
        {"id": "T.wr", "source": "test", "stages": [0], "state": "any", "title": "t", "decision": "d",
         "steps": [{"wr": "0x5158", "value": "0"}], "checks": []}]}
    probes = tmp_path / "p.json"
    probes.write_text(json.dumps(doc))
    result, transport = run(sym, as_replay({}), ["--stages", "0", "--probes", str(probes)])
    assert by_id(result, "T.wr")["verdict"] == "SKIPPED" and "wr 0x5158 0x0" not in transport.sent
    _, transport = run(sym, as_replay({}, {"wr 0x5158 0x0": [[]]}), ["--stages", "0", "--probes", str(probes), "--allow-wr"])
    assert "wr 0x5158 0x0" in transport.sent


def test_every_shipped_probe_is_documented_and_no_default_probe_calls_or_writes():
    doc = json.load(open(hp.DEFAULT_PROBES))
    ids = [p["id"] for p in doc["probes"]]
    assert len(ids) == len(set(ids))
    for p in doc["probes"]:
        assert p["source"] and p["decision"] and p["state"] and p["stages"], p["id"]
        assert not [s for s in p["steps"] if "wr" in s], p["id"]
        if any("call" in s for s in p["steps"]):
            assert p.get("risk") == "call", p["id"]


# ---- CLI: plan, dry run, replay, after ------------------------------------------------------
def test_plan_lists_calls_and_gated_probes(sym):
    out = io.StringIO()
    assert hp.main(["plan", "--sym", sym, "--stages", "0,1,2,3"], out=out) == 0
    text = out.getvalue()
    assert "CALL 0xE05BA079 D_tc[0]" in text
    assert "SKIP (needs --allow-call)" in text and "SKIP (needs --allow-risky-read)" in text
    assert "Stage 2 asks the operator" in text


def test_dry_run_prints_commands_with_symbolic_pointers_and_writes_no_result(sym, tmp_path):
    out = io.StringIO()
    rc = hp.main(["run", "--sym", sym, "--dry-run", "--stages", "0,1,2,3", "--out", str(tmp_path / "res")], out=out)
    text = out.getvalue()
    assert rc == 0 and not (tmp_path / "res").exists()
    assert "  rd 0x8E8C\n  md <P_ptr[0]> 8\n" in text
    assert "  rd 0x14EFD4\n" in text and "  dump 0xBEE18 0x6000\n" in text
    assert "\n  call " not in text and "0xD04" not in text.split("# stage 0")[1]


def test_replay_cli_writes_json_and_markdown_and_after_appends_checker_output(sym, tmp_path):
    replay = tmp_path / "canned.json"
    replay.write_text(json.dumps(as_replay(healthy_memory(), {"ver": [["ML M50mk2 1.0.1 a10b16c23"]]})))
    out = io.StringIO()
    rc = hp.main(["run", "--sym", sym, "--replay", str(replay), "--stages", "0", "--label", "photo lv",
                  "--out", str(tmp_path)], out=out)
    assert rc == 0
    js = next(p for p in tmp_path.iterdir() if p.name.startswith("hwprobe-") and p.suffix == ".json")
    md = js.with_suffix(".md")
    data = json.loads(js.read_text())
    assert data["label"] == "photo lv" and by_id(data, "S0.ver")["values"]["ver"] == ["ML M50mk2 1.0.1 a10b16c23"]
    assert "| 0 | A2.1 EvfState object identity" in md.read_text()

    bogus = tmp_path / "TST00.MLV"
    bogus.write_bytes(b"not an mlv file at all")
    rc = hp.main(["after", "--result", str(js), str(bogus)], out=io.StringIO())
    data = json.loads(js.read_text())
    assert rc == 1 and data["card_files"][0]["checker"] == "mlv_check.py" and data["card_files"][0]["exit"] != 0
    assert "## Card file" in md.read_text()
