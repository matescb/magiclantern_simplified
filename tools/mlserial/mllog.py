#!/usr/bin/env python3
"""mllog - structured log database + analyzer for the M50 II serial harness.

The daemon (mlserald.py) streams every UART line to run/uart.jsonl. That stream is
95% noise you never care about (heartbeats that are always fine, Canon [DAF]/[LENS]
spam). This tool ingests that stream ONCE into a SQLite DB, classifies each line,
segments it into boots, decodes heartbeats, and lets you ASK for just the deltas:

    python3 mllog.py ingest                 # fold new uart.jsonl lines into the DB (idempotent)
    python3 mllog.py boots [N]              # last N boots: build, reached-big8?, errors, crash?
    python3 mllog.py errors [--boot ID]     # deduped FIO errors / asserts / crash (last boot default)
    python3 mllog.py markers [--boot ID]    # OUR diagnostic markers, de-interleaved, in order
    python3 mllog.py heartbeat [--boot ID]  # one-line health verdict (no reading 400 heartbeats)
    python3 mllog.py phases [--boot ID]     # boot phase timeline (pre.N/big.N/post.N)
    python3 mllog.py mark-build <md5> [note] # record which build was just flashed (ties boots->build)
    python3 mllog.py tail [--boot ID] [N]   # last N non-spam lines of a boot, cleaned

Every query auto-ingests first, so you never forget. Boots are segmented by heartbeat
counter reset or a >20s silence gap (a power cycle). Build is taken from an
'@ML> build=<md5>' marker if the module prints one, else the most recent mark-build.
"""
import json
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RUN_DIR = os.environ.get("MLSERIAL_RUN", os.path.join(HERE, "run"))
JSONL = os.path.join(RUN_DIR, "uart.jsonl")
DB = os.path.join(RUN_DIR, "mllog.db")

GAP_NEW_BOOT_S = 20.0          # silence longer than this = a power cycle
HB_RESET_DELTA = 0x100         # heartbeat n dropping by at least this = a reboot

# --- classification ------------------------------------------------------------
HB_RE = re.compile(
    r"<3 n=0x([0-9A-Fa-f]+) phase=0x([0-9A-Fa-f]+) us=0x([0-9A-Fa-f]+)"
    r" cpu1rdy=0x([0-9A-Fa-f]+) ovr=0x([0-9A-Fa-f]+)")
HB_N_RE = re.compile(r"<3 n=0x([0-9A-Fa-f]+)")   # loose: survives interleaved/truncated lines
PHASE_RE = re.compile(r"\b(pre|big|post)\.(\d+)")
BUILD_RE = re.compile(r"@ML>\s*build=([0-9a-fA-F]+)")
# structured atomic FIO-op line emitted by mlv_lite: "@ML> <op> path=<path> ret=<ret>"
ML_OP_RE = re.compile(r"@ML>\s+(\S+)\s+path=(\S+)\s+ret=(-?\d+)")

# our diagnostic markers (sentinel first, then the historical ad-hoc tags)
MARKER_SUBSTR = ("@ML>", "[rawrec]", "[bisect]", "[wh]", "[ctrl", "[probe]",
                 "[hdrw]", "[M50II]")
FIO_SUBSTR = ("[FIO] ERROR", "Not mounted", "efat_openfent", "AcqSize(",
              "FSUBlockWrite", "buf is ROM")
AE_SUBSTR = ("Over the Deadline", "EV_EXPBALANCE", "EV_SET_FRAMECOUNT",
             "FrameBufferDummy", "[LAETC]", "[EIS] ERROR", "DAFPath_SetDAFParam")
SPAM_TAGS = ("[DAF]", "[LENS]", "[DAFZERO]", "[GUI_M]", "[DISP]", "[CAPD]", "[LVEVFC]",
             "[PROPAD]", "[PRP] ERROR", "not adding prop handler", "serrx reg=",
             "rgb_vram_info was NULL")


def classify(text):
    """Return (event_class, detail). event_class drives storage + querying."""
    t = text
    if "<3 n=0x" in t:
        return "heartbeat", ""
    if "[AB] pc=0x" in t:
        return "crash", ""
    if "ASSERT" in t:
        m = re.search(r"@\s*([\w:]+\.c)\s+L:?([0-9A-Fa-f]+)", t)
        return "assert", (m.group(0) if m else "")
    if any(s in t for s in MARKER_SUBSTR):
        return "marker", ""
    if any(s in t for s in FIO_SUBSTR):
        return "fio_error", ""
    if any(s in t for s in AE_SUBSTR):
        return "ae_storm", ""
    m = PHASE_RE.search(t)
    if m:
        return "phase", m.group(0)
    if any(tag in t for tag in SPAM_TAGS):
        return "spam", ""
    return "other", ""


def norm_error(text):
    """Collapse a noisy error line to a stable key so we can dedupe+count it."""
    s = re.sub(r"^\s*\d+:\s*\d+\.\d+\s*", "", text)          # strip Canon "N: ts" prefix
    # cut a glued-in interleaved fragment (serial interleaves another subsystem mid-line)
    for tag in ("[ML]", "[DAF]", "[LENS]", "[EIS]"):
        k = s.find(tag, 1)
        if k > 0:
            s = s[:k]
    s = re.sub(r"0x[0-9A-Fa-f]+", "0x*", s)                   # mask addresses/ids
    s = re.sub(r"\bID=\d+", "ID=*", s)
    s = re.sub(r"\s+", " ", s).strip()
    return s


# --- db -----------------------------------------------------------------------
def db():
    con = sqlite3.connect(DB)
    con.execute("""CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT)""")
    con.execute("""CREATE TABLE IF NOT EXISTS boots(
        boot_id INTEGER PRIMARY KEY, started REAL, ended REAL, build TEXT)""")
    con.execute("""CREATE TABLE IF NOT EXISTS hb(
        boot_id INTEGER, ts REAL, n INTEGER, phase INTEGER, ovr INTEGER, cpu1rdy INTEGER)""")
    con.execute("""CREATE TABLE IF NOT EXISTS events(
        boot_id INTEGER, ts REAL, class TEXT, detail TEXT, text TEXT)""")
    con.execute("""CREATE TABLE IF NOT EXISTS deploys(ts REAL, md5 TEXT, note TEXT)""")
    con.execute("""CREATE INDEX IF NOT EXISTS ix_ev ON events(boot_id, class)""")
    return con


def _get(con, k, default=None):
    r = con.execute("SELECT v FROM meta WHERE k=?", (k,)).fetchone()
    return r[0] if r else default


def _set(con, k, v):
    con.execute("INSERT OR REPLACE INTO meta(k,v) VALUES(?,?)", (k, str(v)))


def _recent_deploy(con, ts):
    r = con.execute("SELECT md5 FROM deploys WHERE ts<=? ORDER BY ts DESC LIMIT 1", (ts,)).fetchone()
    return r[0] if r else None


def ingest(con):
    """Fold new uart.jsonl lines into the DB. Idempotent via a byte offset."""
    if not os.path.exists(JSONL):
        return 0
    off = int(_get(con, "offset", "0"))
    size = os.path.getsize(JSONL)
    if off > size:                                            # log rotated/truncated
        off = 0
        for t in ("boots", "hb", "events"):
            con.execute("DELETE FROM %s" % t)
        _set(con, "boot_id", "0"); _set(con, "last_ts", "0"); _set(con, "last_hb_n", "0")
    boot_id = int(_get(con, "boot_id", "0"))
    last_ts = float(_get(con, "last_ts", "0"))
    last_hb = int(_get(con, "last_hb_n", "0"))
    added = 0
    with open(JSONL, "r") as f:
        f.seek(off)
        for line in f:
            off += len(line.encode("utf-8", "replace"))
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except Exception:
                continue
            if rec.get("dir") != "rx":                       # only camera->host lines
                continue
            ts, text = rec.get("ts", 0.0), rec.get("text", "")
            cls, detail = classify(text)
            new_boot = boot_id == 0
            if last_ts and ts - last_ts > GAP_NEW_BOOT_S:
                new_boot = True
            hb_n = None
            if cls == "heartbeat":
                mn = HB_N_RE.search(text)
                if mn:
                    hb_n = int(mn.group(1), 16)
                    if last_hb and hb_n + HB_RESET_DELTA < last_hb:
                        new_boot = True
            if new_boot:
                if boot_id:
                    con.execute("UPDATE boots SET ended=? WHERE boot_id=?", (last_ts, boot_id))
                boot_id += 1
                con.execute("INSERT INTO boots(boot_id,started,ended,build) VALUES(?,?,?,?)",
                            (boot_id, ts, ts, _recent_deploy(con, ts)))
                last_hb = 0
            last_ts = ts
            m = BUILD_RE.search(text)
            if m:
                con.execute("UPDATE boots SET build=? WHERE boot_id=?", (m.group(1), boot_id))
            g = HB_RE.search(text) if cls == "heartbeat" else None
            if g:                                            # full heartbeat -> hb table
                con.execute("INSERT INTO hb(boot_id,ts,n,phase,ovr,cpu1rdy) VALUES(?,?,?,?,?,?)",
                            (boot_id, ts, int(g.group(1), 16), int(g.group(2), 16),
                             int(g.group(5), 16), int(g.group(4), 16)))
                last_hb = int(g.group(1), 16)
            elif cls == "heartbeat":                         # truncated/interleaved heartbeat
                if hb_n is not None:
                    last_hb = hb_n                           # still advance for segmentation
            else:
                con.execute("INSERT INTO events(boot_id,ts,class,detail,text) VALUES(?,?,?,?,?)",
                            (boot_id, ts, cls, detail, text))
            added += 1
    _set(con, "offset", off); _set(con, "boot_id", boot_id)
    _set(con, "last_ts", last_ts); _set(con, "last_hb_n", last_hb)
    con.commit()
    return added


def last_boot(con):
    r = con.execute("SELECT MAX(boot_id) FROM boots").fetchone()
    return r[0] if r and r[0] else 0


def _bid(con, args):
    for i, a in enumerate(args):
        if a == "--boot" and i + 1 < len(args):
            return int(args[i + 1], 0)
    return last_boot(con)


# --- queries ------------------------------------------------------------------
def q_boots(con, n=12):
    import time as _t
    rows = con.execute("SELECT boot_id,started,ended,build FROM boots ORDER BY boot_id DESC LIMIT ?",
                       (n,)).fetchall()
    print("%-5s %-19s %-7s %-5s %-5s %-6s %-6s %s" %
          ("boot", "started", "dur", "big8", "fio", "assert", "crash", "build"))
    for bid, st, en, build in rows:
        big8 = con.execute("SELECT 1 FROM events WHERE boot_id=? AND class='phase' AND detail='big.8'",
                           (bid,)).fetchone()
        fio = con.execute("SELECT COUNT(*) FROM events WHERE boot_id=? AND class='fio_error'", (bid,)).fetchone()[0]
        asrt = con.execute("SELECT COUNT(*) FROM events WHERE boot_id=? AND class='assert'", (bid,)).fetchone()[0]
        crash = con.execute("SELECT COUNT(*) FROM events WHERE boot_id=? AND class='crash'", (bid,)).fetchone()[0]
        stx = _t.strftime("%Y-%m-%d %H:%M:%S", _t.localtime(st)) if st else "?"
        dur = ("%.0fs" % (en - st)) if (st and en) else "?"
        print("%-5d %-19s %-7s %-5s %-5d %-6d %-6s %s" %
              (bid, stx, dur, "yes" if big8 else "NO", fio, asrt,
               ("%d!" % crash) if crash else "-", (build or "?")[:12]))


def q_errors(con, bid):
    print("== boot %d errors (deduped) ==" % bid)
    seen, order = {}, []
    for cls in ("fio_error", "assert", "crash"):
        for (text,) in con.execute("SELECT text FROM events WHERE boot_id=? AND class=? ORDER BY ts",
                                    (bid, cls)):
            key = (cls, norm_error(text))
            if key not in seen:
                seen[key] = 0
                order.append(key)
            seen[key] += 1
    if not order:
        print("  (none)")
    for key in order:
        print("  %5dx [%s] %s" % (seen[key], key[0], key[1]))


def q_markers(con, bid):
    print("== boot %d markers (ours, in order) ==" % bid)
    rows = con.execute("SELECT text FROM events WHERE boot_id=? AND class='marker' ORDER BY ts", (bid,)).fetchall()
    if not rows:
        print("  (none)")
    for (text,) in rows:
        for tag in MARKER_SUBSTR:
            k = text.find(tag)
            if k >= 0:
                print("  " + text[k:].strip())
                break


def q_fio(con, bid):
    """Decode the atomic @ML> FIO-op lines: op, path, drive (path[0]), ret, fail?.
    ID=0/drive A: is the absent CF slot on the M50 II -> a ret<0 on drive 0 is the bug."""
    print("== boot %d @ML> FIO ops ==" % bid)
    print("  %-12s %-4s %-4s %-24s %s" % ("op", "drv", "ret", "path", "note"))
    hits = 0
    for (text,) in con.execute("SELECT text FROM events WHERE boot_id=? AND class='marker' ORDER BY ts", (bid,)):
        m = ML_OP_RE.search(text)
        if not m:
            continue
        hits += 1
        op, path, ret = m.group(1), m.group(2), int(m.group(3))
        drv = path[0] if path and path[0].isalpha() else "?"
        drv_idx = (ord(drv.upper()) - 0x41) if drv != "?" else None
        note = ""
        if ret < 0:
            note = "FAIL" + (" (drive %d=%s%s)" % (drv_idx, drv, "=absent CF!" if drv_idx == 0 else "") if drv_idx is not None else "")
        print("  %-12s %-4s %-4d %-24s %s" % (op, "%s/%s" % (drv, drv_idx), ret, path[:24], note))
    if not hits:
        print("  (no @ML> op lines — build lacks atomic FIO logging, or none ran)")


def q_heartbeat(con, bid):
    rows = con.execute("SELECT n,ovr,cpu1rdy FROM hb WHERE boot_id=? ORDER BY ts", (bid,)).fetchall()
    if not rows:
        print("boot %d: no heartbeats (did not reach steady state?)" % bid)
        return
    # heartbeat n is a small monotonic counter; drop absurd values from serial interleave
    ns = [r[0] for r in rows if r[0] < 0x100000]
    bad_ovr = sum(1 for r in rows if r[1] != 0)      # overrun: the real health signal
    bad_cpu = sum(1 for r in rows if r[2] != 1)      # CPU1 down
    verdict = "HEALTHY" if (not bad_ovr and not bad_cpu) else "ANOMALY"
    rng = ("n=0x%x->0x%x" % (ns[0], ns[-1])) if ns else "n=?"
    print("boot %d heartbeat: %s  %s (%d beats)  ovr!=0:%d  cpu1rdy!=1:%d" %
          (bid, verdict, rng, len(rows), bad_ovr, bad_cpu))


def q_phases(con, bid):
    print("== boot %d phases ==" % bid)
    seen = []
    for ts, d in con.execute("SELECT ts,detail FROM events WHERE boot_id=? AND class='phase' ORDER BY ts", (bid,)):
        if d not in seen:
            seen.append(d)
            print("  %s" % d)
    if not seen:
        print("  (no phase markers)")


def q_tail(con, bid, n):
    rows = con.execute(
        "SELECT class,text FROM events WHERE boot_id=? AND class!='spam' ORDER BY ts DESC LIMIT ?",
        (bid, n)).fetchall()
    for cls, text in reversed(rows):
        print("  [%s] %s" % (cls, text.strip()[:160]))


def main():
    a = sys.argv[1:]
    con = db()
    if a and a[0] == "mark-build":
        import time as _t
        con.execute("INSERT INTO deploys(ts,md5,note) VALUES(?,?,?)",
                    (_t.time(), a[1], " ".join(a[2:]) if len(a) > 2 else ""))
        con.commit()
        print("recorded build %s" % a[1])
        return
    n = ingest(con)
    cmd = a[0] if a else "boots"
    if cmd == "ingest":
        print("ingested %d new lines; %d boots total" % (n, last_boot(con)))
    elif cmd == "boots":
        q_boots(con, int(a[1]) if len(a) > 1 and a[1].isdigit() else 12)
    elif cmd == "errors":
        q_errors(con, _bid(con, a))
    elif cmd == "markers":
        q_markers(con, _bid(con, a))
    elif cmd == "fio":
        q_fio(con, _bid(con, a))
    elif cmd == "heartbeat":
        q_heartbeat(con, _bid(con, a))
    elif cmd == "phases":
        q_phases(con, _bid(con, a))
    elif cmd == "tail":
        # a bare trailing count, NOT the value that follows --boot
        nn = 30
        for i, x in enumerate(a):
            if x.isdigit() and (i == 0 or a[i - 1] != "--boot"):
                nn = int(x)
        q_tail(con, _bid(con, a), nn)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
