#!/usr/bin/env python3
"""Unit tests for logmine.py on a synthetic log (no camera data needed).  python3 test_logmine.py"""
import json, os, sqlite3, tempfile, unittest
import logmine as L

DUMP = """[ABORT] type=0x00000001 pc=0xE005DE52 spsr=0x80000033
[ABORT] DFSR=0x00000007 DFAR=0x00000000 IFSR=0x00000003 IFAR=0xC32220E8
[ABORT] sp=0x001F4888 lr=0xE005DE29
[ABORT] r0=0x00000000 r1=0x00000073 r2=0x00000042 r3=0xFFFFFFF6
[ABORT] stack scan (ROM addrs on faulting stack):
[ABORT]  sp+0x00000004 = 0xE005DC94
[ABORT] HANG (fault captured; looping short dump)
[AB] pc=0xE005DE52 lr=0xE005DE29 dfar=0x00000000
[AB] pc=0xE005DE52 lr=0xE005DE29 dfar=0x00000000""".split("\n")


def make(tmp):
    rows, t = [], [1000.0]

    def add(text, dt=0.2, d="rx"):
        t[0] += dt
        rows.append({"ts": t[0], "dir": d, "kind": "log", "tag": "", "text": text})

    # boot A: crashes at big.5.3
    for x in ["BootLoad", "Read File AUTOEXEC.bin (256448 bytes)", "e\u0000[ML] pre.0: enter", "[ML] post.1: wait",
              "[ML] post.4: x", "[ML] big.5.3: loading RBF fonts"] + DUMP:
        add(x)
    # boot B: silent restart at post.1
    for x in ["BootLoad", "Read File AUTOEXEC.bin (256448 bytes)", "[ML] pre.0: enter", "[ML] post.1: wait"]:
        add(x)
    # boot C: reaches big.8, records 3 frames, writes, assert burst, abort
    for x in ["BootLoad", "Read File AUTOEXEC.bin (257760 bytes)", "[ML] pre.0: enter", "[ML] post.4: x", "[ML] big.8: done",
              "[RVX] Video starting.", "[RVX] wk.dst=0x46331080", "[RVX] wk.line_size=0x00000C40", "[RVX] wk.h=0x00000380"]:
        add(x)
    for x in ["[RVX] cbr_w: give sem", "[RVX] cbr_w: give sem", "[RVX] cbr_w: give sem"]:
        add(x, 0.5)
    add("[RVX] worker: writing crop_buf -> card")
    add("[RVX] wbytes=0x00D66280", 0.0)
    add("[RVX] worker: FIO_WriteFile done", 0.4)
    for _ in range(3):
        add("[ML] !!! ASSERT (non-fatal): nullptr @ LiveViewDrive::EvfDevState.c L:00000174", 0.0)
    add("[ML] !!! ASSERT (non-fa", 0.0)
    add("[ABORT] type=0x00000000 pc=0x00000000 spsr=0x20000193")
    add("[ABORT] DFSR=0x00000809 DFAR=0xC09208D5 IFSR=0x00000007 IFAR=0x00000000", 0.0)
    add("[ABORT] sp=0xDEFFFFD0 lr=0xE05D1161", 0.0)
    add("[ABORT] HANG (one-shot dump complete)", 0.0)
    jl = os.path.join(tmp, "u.jsonl")
    with open(jl, "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
    db = os.path.join(tmp, "m.db")
    con = sqlite3.connect(db)
    con.execute("CREATE TABLE boots(boot_id INTEGER PRIMARY KEY, started REAL, ended REAL, build TEXT)")
    con.execute("CREATE TABLE deploys(ts REAL, md5 TEXT, note TEXT)")
    con.execute("INSERT INTO boots VALUES(1,1000.0,1005.0,'aaaaaaaabbbb')")
    con.execute("INSERT INTO boots VALUES(2,1005.0,1100.0,'ccccccccdddd')")
    con.commit(); con.close()
    return jl, db


class T(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="logmine_test_")
        jl, db = make(cls.tmp)
        cls.lines, boots, _ = L.analyse(jl, db)
        cls.boots = [b for b in boots if b.id != "0pre"]   # leading "BootLoad" line forms the pre-history stub

    def test_segments_one_boot_per_cold_start(self):
        self.assertEqual([b.epoch for b in self.boots], ["ax256448", "ax256448", "ax257760"])

    def test_full_dump_is_parsed_and_short_lines_are_merged(self):
        a = self.boots[0]
        self.assertEqual(len(a.crashes), 1)
        c = a.crashes[0]
        self.assertEqual((c["pc"], c["lr"], c["dfar"], c["type"]), (0xE005DE52, 0xE005DE29, 0, 1))
        self.assertTrue(c["complete"])
        self.assertEqual(c["dump"][0], DUMP[0])
        self.assertEqual(a.phase_before(c["ts"]), "big.5.3")

    def test_silent_restart_is_flagged_and_not_counted_as_crash(self):
        self.assertTrue(self.boots[1].silent_restart)
        self.assertFalse(self.boots[0].silent_restart)
        self.assertEqual(self.boots[1].crashes, [])

    def test_assert_site_and_garbled_line(self):
        c = self.boots[2]
        self.assertEqual(sum(1 for a in c.asserts if a[1] == "LiveViewDrive::EvfDevState.c" and a[2] == 0x174), 3)
        self.assertEqual(sum(1 for a in c.asserts if a[1] == "<garbled>"), 1)

    def test_recording_attempt_counts_frames_bytes_and_ending(self):
        recs = L.recordings(self.boots)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual((r["kind"], r["frames"], r["frame_bytes"], r["wbytes"]), ("raw_vidx", 3, 0xC40 * 0x380, 0xD66280))
        self.assertAlmostEqual(r["writes"][0][1], 0.4, places=3)
        self.assertIn("EvfDevState nullptr x3", r["how"])
        self.assertIn("ABORT pc=0x00000000 lr=0xE05D1161", r["how"])

    def test_rates_use_give_sem_intervals(self):
        recs = L.recordings(self.boots)
        md, agg = L.rate_tables(recs, {"batches": 1, "gap_p10": 0.2, "gap_p50": 0.4, "gap_p90": 0.6, "gap_p99": 0.6,
                                       "lines_per_batch_p50": 1, "lines_per_batch_p99": 2})
        self.assertEqual(agg["copy_frames"], 2)
        self.assertAlmostEqual(agg["copy_s"], 1.0, places=3)
        self.assertEqual(agg["write_bytes"], 0xD66280)

    def test_compact_ids(self):
        self.assertEqual(L.compact_ids([141, 142, "150b", 143, 200]), "141-143,150b,200")

    def test_tables_render(self):
        for fn in (L.crash_tables, L.assert_tables, L.build_table):
            self.assertIn("|", fn(self.boots)[0])


if __name__ == "__main__":
    unittest.main()
