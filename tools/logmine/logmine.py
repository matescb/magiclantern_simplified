#!/usr/bin/env python3
"""logmine.py - offline crash / assert / reliability / recording catalogue from the
M50 II hardware UART history.  READ-ONLY on its inputs.

Inputs : tools/mlserial/run/uart.jsonl (ts, dir, kind, tag, text)
         tools/mlserial/run/mllog.db   (opened with mode=ro; boots + deploys tables only)
Output : Markdown tables in --out (default tools/logmine/out/):
         crashes.md asserts.md builds.md recordings.md rates.md summary.json

Usage  : python3 logmine.py [--jsonl P] [--db P] [--out DIR]
Tests  : python3 test_logmine.py
"""
import argparse, bisect, collections, json, os, re, sqlite3, statistics, time

HERE = os.path.dirname(os.path.abspath(__file__))
RUN = os.path.join(HERE, "..", "mlserial", "run")

# ---------------------------------------------------------------- patterns
HEX = r"0x([0-9A-Fa-f]{8})"
RE_AB_TYPE = re.compile(r"^\[ABORT\] type=%s pc=%s spsr=%s$" % (HEX, HEX, HEX))
RE_AB_FSR = re.compile(r"^\[ABORT\] DFSR=%s DFAR=%s IFSR=%s IFAR=%s$" % (HEX, HEX, HEX, HEX))
RE_AB_SPLR = re.compile(r"^\[ABORT\] sp=%s lr=%s$" % (HEX, HEX))
RE_AB_SHORT = re.compile(r"^\[AB\] pc=%s lr=%s dfar=%s$" % (HEX, HEX, HEX))
RE_AB_ANY = re.compile(r"\[ABORT\]|\[AB\] ")
RE_PHASE = re.compile(r"\[ML\] ((?:pre|post|big)\.[0-9a-z.]+)")
RE_ASSERT = re.compile(r"ASSERT[^@]*?(?:: *(?P<msg>[^@]*?))? *@ *(?P<file>[A-Za-z]+::[A-Za-z0-9_]+\.c) L:(?P<line>[0-9A-Fa-f]{8})\b")
RE_ASSERT_ANY = re.compile(r"ASSERT")
RE_WFD_LEN = re.compile(r"\[WFD\] file_len=(0x[0-9a-fA-F]+)")
RE_STARTUP = re.compile(r"^\s*\d+:\s*([0-9.]+) \[STARTUP\] K468 ICU Firmware Version")
RE_VER = re.compile(r"^ML M50mk2 (\S.*)$")
RE_REC = re.compile(r"\[RVX\]|\[rawrec\]|@ML> |\[wh\] ")
RE_REC_START = re.compile(r"\[RVX\] (Video starting|keypress_cbr)|\[rawrec\] create|@ML> mark pre-setup_buffers")
RE_WKDST = re.compile(r"\[RVX\] wk\.dst=%s" % HEX)
RE_WBYTES = re.compile(r"\[RVX\] wbytes=%s" % HEX)
RE_LINESZ = re.compile(r"\[RVX\] wk\.line_size=%s" % HEX)
RE_WKH = re.compile(r"\[RVX\] wk\.h=%s" % HEX)
RE_FW_OK = re.compile(r"@ML> framewrite-(ok|FAIL)\S* path=(\S+) ret=(-?\d+)")
RE_WH = re.compile(r"\[wh\] #\d+ hdr=\S+ bs=(\d+) ret=(-?\d+)")
STORM_ASSERTS = ("MvRawProDs.c", "RawPro.c", "LvJob.c")
REC_GAP_S = 20.0      # markers further apart than this start a new attempt
BURST_GAP_S = 2.0     # asserts further apart than this start a new burst


def day(ts):
    return time.strftime("%m-%d", time.gmtime(ts))


def hms(ts):
    return time.strftime("%m-%d %H:%M:%S", time.gmtime(ts))


# ---------------------------------------------------------------- loading
def load_lines(path):
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            try:
                j = json.loads(raw)
            except ValueError:
                continue
            out.append((j["ts"], j["dir"], j["text"]))
    return out


def load_boots(dbpath):
    con = sqlite3.connect("file:%s?mode=ro" % os.path.abspath(dbpath), uri=True)
    boots = con.execute("SELECT boot_id,started,ended,build FROM boots ORDER BY boot_id").fetchall()
    deploys = con.execute("SELECT ts,md5,note FROM deploys ORDER BY ts").fetchall()
    con.close()
    return boots, deploys


class Boot:
    def __init__(self, bid, started, ended, build):
        self.id, self.started, self.ended = bid, started, ended
        self.dbbuild = (build or "")[:8] or "unmarked"
        self.epoch = self.dbbuild
        self.lines = []          # (ts, dir, text)
        self.phases = []         # (ts, tag)
        self.crashes = []        # dict
        self.asserts = []        # (ts, file, line, msg)
        self.garbled_abort = 0
        self.startup_ms = None
        self.ver = None
        self.rec = []            # (ts, text)
        self.axsize = None       # AUTOEXEC.bin size printed by the bootloader = core build fingerprint
        self.silent_restart = False
        self.speed = "?"         # slow / fast boot build family (see speed_class)
        self.cold_starts = 0     # number of '[ML] pre.0' markers inside this db boot

    def reached(self, tag):
        return any(p == tag for _, p in self.phases)

    def phase_before(self, ts):
        last = None
        for t, p in self.phases:
            if t > ts:
                break
            last = p
        return last or "none"


RE_AXSIZE = re.compile(r"Read File AUTOEXEC\.bin \((\d+) bytes\)")
RE_COLD = re.compile(r"\[ML\] pre\.0\b(?![0-9a-z.])")


DB_STARTS, DB_IDS = [], []


def dbid_at(ts):
    """mllog.db boot id that contains timestamp ts (the numbering used in docs / memory notes)."""
    if not DB_STARTS:
        return 0
    return DB_IDS[max(bisect.bisect_right(DB_STARTS, ts) - 1, 0)]


def segment(lines, boots_rows):
    """One Boot per cold start = per '[ML] pre.0:' marker.  mllog.db's own boot table both
    over-segments (heartbeat-counter resets, daemon restarts: fragments with no boot markers) and
    under-segments (457 pre.0 markers in 367 db boots), so it is used only for LABELS: a boot is
    named after the db boot id that contains its pre.0, with a letter suffix (306, 306b, ...) when
    that db boot holds several cold starts.  Lines before the first pre.0 form boot '0pre'."""
    starts = [r[1] for r in boots_rows]
    DB_STARTS[:] = starts
    DB_IDS[:] = [r[0] for r in boots_rows]
    cold = [ts_i for ts_i in (i for i, ln in enumerate(lines) if ln[1] == "rx" and RE_COLD.search(ln[2]))]
    boots, seen = [], collections.Counter()
    # the bootloader prints 'BootLoad / Read File AUTOEXEC.bin (N bytes) / Now jump' just before pre.0
    cold2 = []
    for i in cold:
        k = i
        for back in range(1, 9):
            if i - back >= 0 and RE_AXSIZE.search(lines[i - back][2]):
                k = i - back
                break
        cold2.append(k)
    cold = cold2
    bounds = [0] + cold + [len(lines)]
    for k in range(len(bounds) - 1):
        a, e = bounds[k], bounds[k + 1]
        if a == e:
            continue
        ts0 = lines[a][0]
        j = max(bisect.bisect_right(starts, ts0) - 1, 0)
        dbid, build = boots_rows[j][0], boots_rows[j][3]
        if k == 0:
            label = "0pre"
        else:
            seen[dbid] += 1
            label = "%d%s" % (dbid, "" if seen[dbid] == 1 else chr(ord("a") + seen[dbid] - 1))
        b = Boot(label, ts0, lines[e - 1][0], build)
        b.dbid = dbid
        b.lines = lines[a:e]
        boots.append(b)
    return boots


def assign_epochs(boots):
    """Build epoch = db build tag (mark-build md5) + count of successful WiFi deploys
    since that tag ('[WFD] file_len=' followed by a reboot).  Card flashes done without
    mark-build are invisible -> an epoch can silently span several binaries."""
    wfd, prev_tag, pending = 0, None, None
    for b in boots:
        if b.dbbuild != prev_tag:
            wfd, prev_tag, pending = 0, b.dbbuild, None
        if pending:
            wfd += 1
            last_len, pending = pending, None
            b._wfd_len = last_len
        b.epoch = b.dbbuild if wfd == 0 else "%s+wfd%02d" % (b.dbbuild, wfd)
        for _, _, text in b.lines:
            m = RE_WFD_LEN.search(text)
            if m:
                pending = m.group(1)


# ---------------------------------------------------------------- per boot parse
def parse_boot(b):
    L = b.lines
    i, n = 0, len(L)
    seen_sig = set()
    while i < n:
        ts, d, text = L[i]
        if d != "rx":
            i += 1
            continue
        m = RE_PHASE.search(text)
        if m:
            b.phases.append((ts, m.group(1)))
        if b.startup_ms is None:
            m = RE_STARTUP.match(text)
            if m:
                b.startup_ms = float(m.group(1))
        m = RE_VER.match(text)
        if m:
            b.ver = m.group(1).strip()
        if b.axsize is None and i < 12:
            m = RE_AXSIZE.search(text)
            if m:
                b.axsize = int(m.group(1))
        if RE_REC.search(text):
            b.rec.append((ts, text))
        if RE_ASSERT_ANY.search(text):
            m = RE_ASSERT.search(text)
            if m:
                b.asserts.append((ts, m.group("file"), int(m.group("line"), 16), (m.group("msg") or "").strip()))
            else:
                b.asserts.append((ts, "<garbled>", 0, ""))
        m = RE_AB_TYPE.match(text)
        if m:
            dump = [text]
            j = i + 1
            while j < n and j < i + 60:
                t2 = L[j][2]
                if not t2.startswith("[ABORT]"):
                    break
                dump.append(t2)
                j += 1
                if "HANG" in t2:
                    break
            c = {"ts": ts, "type": int(m.group(1), 16), "pc": int(m.group(2), 16),
                 "spsr": int(m.group(3), 16), "dump": dump, "full": True, "lr": None,
                 "dfar": None, "ifar": None, "dfsr": None, "ifsr": None}
            for t2 in dump:
                m2 = RE_AB_FSR.match(t2)
                if m2:
                    c["dfsr"], c["dfar"], c["ifsr"], c["ifar"] = (int(x, 16) for x in m2.groups())
                m2 = RE_AB_SPLR.match(t2)
                if m2:
                    c["sp"], c["lr"] = int(m2.group(1), 16), int(m2.group(2), 16)
            c["complete"] = c["lr"] is not None and c["dfar"] is not None and any("HANG" in x for x in dump)
            b.crashes.append(c)
            seen_sig.add((c["pc"], c["lr"]))
            i = j
            continue
        m = RE_AB_SHORT.match(text)
        if m:
            pc, lr, dfar = (int(x, 16) for x in m.groups())
            if (pc, lr) not in seen_sig:
                seen_sig.add((pc, lr))
                b.crashes.append({"ts": ts, "type": None, "pc": pc, "lr": lr, "dfar": dfar, "ifar": None,
                                  "dfsr": None, "ifsr": None, "dump": [text], "full": False, "complete": False})
        elif RE_AB_ANY.search(text) and not text.startswith("[ABORT]  sp+") and "PROP_ABORT" not in text:
            if not (text.startswith("[ABORT] ") and re.match(r"^\[ABORT\] (DFSR|sp=|r\d|stack scan|HANG)", text)):
                b.garbled_abort += 1
        i += 1
    # merge short-form entries into full entries of the same pc/lr (keeps one per (pc,lr))
    uniq = {}
    for c in b.crashes:
        k = (c["pc"], c["lr"])
        if k not in uniq or (c["full"] and not uniq[k]["full"]) or (c["complete"] and not uniq[k]["complete"]):
            if k in uniq:
                c["ts"] = min(c["ts"], uniq[k]["ts"])
            uniq[k] = c
    b.crashes = sorted(uniq.values(), key=lambda c: c["ts"])


def speed_class(b):
    """Build-family proxy that survives unmarked card flashes: the Canon '[STARTUP] ... Firmware
    Version' line is logged ~8152 ms after power-on with the slow (LED-beacon) core, ~3218 ms with
    the 2026-07-19 fast-boot core.  Fallback when that line was not captured: pre.0 -> post.4 time."""
    b.cold_starts = sum(1 for _, p in b.phases if p == "pre.0")
    if b.startup_ms is not None and b.startup_ms > 1000:
        b.speed = "fast" if b.startup_ms < 5500 else "slow"
        return
    t0 = [t for t, p in b.phases if p == "pre.0"]
    t4 = [t for t, p in b.phases if p == "post.4" and t0 and t >= t0[0]]
    if t0 and t4:
        b.speed = "fast" if t4[0] - t0[0] < 5.5 else "slow"


def sig_of(c):
    kind = {0: "prefetch", 1: "data", None: "?"}[c["type"]]
    return "pc=0x%08X lr=%s" % (c["pc"], "0x%08X" % c["lr"] if c["lr"] is not None else "?"), kind


def rec_context(b, ts):
    last = None
    for t, text in b.rec:
        if t > ts:
            break
        last = (t, text)
    if last and ts - last[0] < 120:
        m = re.search(r"(\[RVX\][^\[]{0,40}|\[rawrec\][^\[]{0,30}|@ML> \S+ \S+|\[wh\] \S+)", last[1])
        return "recording: '%s' %.1fs before" % ((m.group(1) if m else last[1][:40]).strip(), ts - last[0])
    return None


# ---------------------------------------------------------------- tables
def md_table(headers, rows):
    out = ["| " + " | ".join(headers) + " |", "|" + "|".join("---" for _ in headers) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(x).replace("|", "\\|") for x in r) + " |")
    return "\n".join(out) + "\n"


def compact_ids(ids):
    """'141,142,150b' -> '141-142,150b' (ranges only over plain db boot ids)."""
    def key(x):
        m = re.match(r"(\d+)(.*)", str(x))
        return (int(m.group(1)), m.group(2))
    ids = sorted(set(str(x) for x in ids), key=key)
    out, run = [], []
    def flush():
        if run:
            out.append(run[0] if len(run) == 1 else "%s-%s" % (run[0], run[-1]))
            del run[:]
    for x in ids:
        n, suf = key(x)
        if suf:
            flush()
            out.append(x)
        elif run and int(run[-1]) + 1 == n:
            run.append(x)
        else:
            flush()
            run.append(x)
    flush()
    return ",".join(out)


def crash_tables(boots):
    sigs = collections.OrderedDict()
    for b in boots:
        for c in b.crashes:
            key, kind = sig_of(c)
            s = sigs.setdefault(key, {"kind": set(), "boots": [], "epochs": collections.Counter(), "phases": collections.Counter(),
                                      "dfar": collections.Counter(), "ifar": collections.Counter(), "first": c["ts"], "last": c["ts"],
                                      "sample": None, "rec": 0, "t_since_boot": []})
            if c["type"] is not None:
                s["kind"].add(kind)
            s["boots"].append(b.id)
            s.setdefault("dbids", []).append(dbid_at(c["ts"]))
            s["epochs"][b.epoch] += 1
            ph = b.phase_before(c["ts"])
            rc = rec_context(b, c["ts"])
            if rc:
                s["rec"] += 1
            s["phases"][ph + (" +rec" if rc else "")] += 1
            if c["dfar"] is not None:
                s["dfar"]["0x%08X" % c["dfar"]] += 1
            if c["ifar"] is not None:
                s["ifar"]["0x%08X" % c["ifar"]] += 1
            s["first"], s["last"] = min(s["first"], c["ts"]), max(s["last"], c["ts"])
            s["t_since_boot"].append(c["ts"] - b.started)
            if c["complete"] and (s["sample"] is None):
                s["sample"] = (b.id, c, rc)
            elif s["sample"] is None and c["full"]:
                s.setdefault("partial", (b.id, c, rc))
    rows = []
    order = sorted(sigs.items(), key=lambda kv: -len(set(kv[1]["boots"])))
    for key, s in order:
        nb = len(set(s["boots"]))
        top = lambda cnt, k=3: ", ".join("%s x%d" % kv for kv in cnt.most_common(k)) + (" (+%d more)" % (len(cnt) - k) if len(cnt) > k else "")
        rows.append([key, "/".join(sorted(s["kind"])) or "? (short [AB] only)", nb, day(s["first"]), day(s["last"]),
                     top(s["dfar"]), top(s["ifar"], 2), top(s["phases"]), top(s["epochs"], 4),
                     "%.0f" % statistics.median(s["t_since_boot"]), compact_ids(s["dbids"])])
    md = "## Abort signatures (%d distinct, %d cold-start boots with a clean abort line)\n\n" % (
        len(sigs), sum(1 for b in boots if b.crashes))
    md += md_table(["signature", "fault", "boots", "first", "last", "DFAR", "IFAR", "last ML phase before",
                    "core builds (AUTOEXEC size)", "median s after cold start", "mllog db boot ids"], rows)
    md += "\n## One verbatim dump per signature\n"
    for key, s in order:
        samp = s["sample"] or s.get("partial")
        if not samp:
            md += "\n### %s\n\nNo full `[ABORT]` dump was ever captured for this signature (only the short `[AB]` loop line).\n" % key
            continue
        bid, c, rc = samp
        md += "\n### %s  (db boot %s, %s UTC%s%s)\n\n```\n%s\n```\n" % (
            key, dbid_at(c["ts"]), hms(c["ts"]), "" if c["complete"] else ", dump INCOMPLETE in log", "; " + rc if rc else "", "\n".join(c["dump"]))
    return md, sigs


def assert_tables(boots):
    per = {}
    bursts = collections.OrderedDict()
    burst_list = []
    for b in boots:
        cur = None
        for ts, f, ln, msg in b.asserts:
            if f == "<garbled>":
                k = "<garbled/truncated ASSERT line>"
            else:
                k = "%s:%d (L:%08X)" % (f, ln, ln)
            p = per.setdefault(k, {"n": 0, "boots": set(), "epochs": collections.Counter(), "first": ts, "last": ts,
                                   "phases": collections.Counter(), "msg": collections.Counter()})
            p["n"] += 1
            p["boots"].add(b.id)
            p["epochs"][b.epoch] += 1
            p["first"], p["last"] = min(p["first"], ts), max(p["last"], ts)
            if b.id not in p.setdefault("_ph_seen", set()):
                p["_ph_seen"].add(b.id)
                p["phases"][b.phase_before(ts) + (" +rec" if rec_context(b, ts) else "")] += 1
            if msg:
                p["msg"][msg] += 1
            if cur is None or ts - cur["end"] > BURST_GAP_S:
                cur = {"boot": b.id, "start": ts, "end": ts, "keys": collections.Counter(), "epoch": b.epoch}
                burst_list.append(cur)
            cur["end"] = ts
            if f != "<garbled>":
                cur["keys"]["%s:%d" % (f.split("::")[1], ln)] += 1
        # annotate bursts with what follows
    byboot = {b.id: b for b in boots}
    for bu in burst_list:
        b = byboot[bu["boot"]]
        nxt = [c for c in b.crashes if bu["start"] - 1 <= c["ts"] <= bu["end"] + 5]
        bu["abort"] = sig_of(nxt[0])[0] if nxt else None
        bu["phase"] = b.phase_before(bu["start"])
        bu["rec"] = rec_context(b, bu["start"]) is not None
        key = " + ".join(sorted(bu["keys"])) or "<garbled only>"
        g = bursts.setdefault(key, {"n": 0, "boots": set(), "sizes": [], "durs": [], "aborts": collections.Counter(),
                                    "first": bu["start"], "last": bu["start"], "phases": collections.Counter(), "rec": 0,
                                    "epochs": collections.Counter()})
        g["n"] += 1
        g["boots"].add(bu["boot"])
        g["sizes"].append(sum(bu["keys"].values()))
        g["durs"].append(bu["end"] - bu["start"])
        g["aborts"][bu["abort"] or "none"] += 1
        g["first"], g["last"] = min(g["first"], bu["start"]), max(g["last"], bu["start"])
        g["phases"][bu["phase"]] += 1
        g["rec"] += bu["rec"]
        g["epochs"][bu["epoch"]] += 1
    top = lambda cnt, k=3: ", ".join("%s x%d" % kv for kv in cnt.most_common(k)) + (" (+%d more)" % (len(cnt) - k) if len(cnt) > k else "")
    rows = [[k, top(p["msg"], 2), p["n"], len(p["boots"]), day(p["first"]), day(p["last"]), top(p["phases"]), top(p["epochs"]),
             compact_ids(p["boots"]) if len(p["boots"]) <= 45 else "%d boots" % len(p["boots"])]
            for k, p in sorted(per.items(), key=lambda kv: -len(kv[1]["boots"]))]
    md = "## Assert sites (file:line decimal, raw hex in brackets)\n\n" + md_table(
        ["site", "message", "lines", "boots", "first", "last", "last ML phase before first hit in boot (boots)", "build epochs (lines)", "boot ids"], rows)
    rows = [[k, g["n"], len(g["boots"]), "%d" % statistics.median(g["sizes"]), max(g["sizes"]), "%.1f" % max(g["durs"]),
             day(g["first"]), day(g["last"]), top(g["phases"]), g["rec"], top(g["aborts"]), top(g["epochs"])]
            for k, g in sorted(bursts.items(), key=lambda kv: -kv[1]["n"])]
    md += "\n## Assert bursts (asserts < %.0f s apart in one boot = one burst; grouped by the set of sites)\n\n" % BURST_GAP_S
    md += md_table(["burst = set of sites", "bursts", "boots", "median lines", "max lines", "max duration s", "first", "last",
                    "ML phase at burst start", "bursts within 120 s of a recording marker", "abort within 5 s after", "build epochs"], rows)
    return md, per, bursts


def build_table(boots):
    eps = collections.OrderedDict()
    for b in boots:
        e = eps.setdefault(b.epoch, {"boots": [], "big8": 0, "crashed": 0, "sig": collections.Counter(), "stalled": 0,
                                     "first": b.started, "last": b.started, "tags": collections.Counter(), "ver": collections.Counter(),
                                     "t8": [], "stall": collections.Counter(), "silent": 0, "late": 0, "speed": collections.Counter()})
        e["boots"].append(b.id)
        e["last"] = b.started
        e["tags"][b.tag] += 1
        e["speed"][b.speed] += 1
        if b.silent_restart:
            e["silent"] += 1
            continue
        r8 = b.reached("big.8")
        e["big8"] += r8
        if b.crashes:
            c = b.crashes[0]
            if r8 and c["ts"] > [t for t, p in b.phases if p == "big.8"][0]:
                e["late"] += 1
                e["sig"]["0x%08X(after big.8)" % c["pc"]] += 1
            else:
                e["crashed"] += 1
                e["sig"]["0x%08X@%s" % (c["pc"], b.phase_before(c["ts"]))] += 1
        elif not r8:
            e["stalled"] += 1
            e["stall"][b.phases[-1][1] if b.phases else "no ML marker"] += 1
        if r8:
            t8 = [t for t, p in b.phases if p == "big.8"][0]
            t0 = [t for t, p in b.phases if p == "pre.0"]
            if t0 and t0[0] < t8:
                e["t8"].append(t8 - t0[0])
        if b.ver:
            e["ver"][b.ver.replace(".M50mk2.101", "")] += 1
    rows = []
    top = lambda cnt, k=4: ", ".join("%s x%d" % kv for kv in cnt.most_common(k)) + (" (+%d)" % (len(cnt) - k) if len(cnt) > k else "")
    for ep, e in eps.items():
        n = len(e["boots"])
        eff = n - e["silent"]
        rows.append([ep, day(e["first"]) + ".." + day(e["last"]), compact_ids(e["boots"]), n, e["silent"], eff,
                     "%d (%d%%)" % (e["big8"], round(100.0 * e["big8"] / eff)) if eff else "-", e["crashed"], e["stalled"], e["late"],
                     top(e["sig"]) or "-", top(e["stall"], 3) or "-",
                     "%.0f" % statistics.median(e["t8"]) if e["t8"] else "-", top(e["speed"], 2), top(e["tags"], 3), top(e["ver"], 2) or "-"])
    md = ("## Per core-build reliability\n\nBuild key = AUTOEXEC.bin size printed by the Canon bootloader at every cold start "
          "(`Read File AUTOEXEC.bin (N bytes)`); modules (.mo) are NOT part of it. `mllog tag` = the mark-build md5 that mllog "
          "attached (mostly module md5s; stale after 07-19 13:12). `silent restart` = boot that dies without any abort line at "
          "post.0c..post.2 and re-enters the bootloader within 15 s; excluded from `effective`.\n\n")
    md += md_table(["core build", "dates", "boot ids", "cold starts", "silent restart at post.1", "effective", "reached big.8",
                    "abort before big.8", "stalled before big.8 (no abort)", "abort after big.8", "first abort per boot (pc@phase)",
                    "last marker of stalled", "median pre.0->big.8 s", "boot speed", "mllog tag", "ver string"], rows)
    return md, eps


# ---------------------------------------------------------------- recordings
def recordings(boots):
    atts = []
    for b in boots:
        cur = None
        for ts, text in b.rec:
            if "crop_buf was NULL" in text or "couldn't take edmac_sem" in text:
                if cur is not None and ts - cur["last"] <= REC_GAP_S:
                    cur["err"][re.search(r"ERROR: ([a-z_' ]+)", text).group(1)[:28]] += 1
                    cur["last_any"] = ts
                continue
            if cur is None or ts - cur["last"] > REC_GAP_S:
                if not RE_REC_START.search(text) and cur is None and not re.search(r"\[RVX\]|\[rawrec\] create|framewrite", text):
                    continue
                cur = {"boot": b, "start": ts, "last": ts, "last_any": ts, "lines": [], "err": collections.Counter()}
                atts.append(cur)
            cur["last"] = ts
            cur["last_any"] = max(cur["last_any"], ts)
            cur["lines"].append((ts, text))
    out = []
    for a in atts:
        b = a["boot"]
        T = [t for _, t in a["lines"]]
        kind = "raw_vidx" if any("[RVX]" in t for t in T) else "mlv_lite"
        r = {"boot": "%s (db %d)" % (b.id, dbid_at(a["start"])), "epoch": b.epoch, "start": a["start"], "kind": kind, "dur": a["last_any"] - a["start"]}
        gives = [ts for ts, t in a["lines"] if "cbr_w: give sem" in t]
        dsts = []
        for ts, t in a["lines"]:
            m = RE_WKDST.search(t)
            if m and (not dsts or dsts[-1][1] != int(m.group(1), 16)):
                dsts.append((ts, int(m.group(1), 16)))
        r["gives"], r["give_ts"] = len(gives), gives
        r["dsts"] = dsts
        ls = [int(m.group(1), 16) for m in (RE_LINESZ.search(t) for t in T) if m]
        hh = [int(m.group(1), 16) for m in (RE_WKH.search(t) for t in T) if m]
        r["frame_bytes"] = ls[0] * hh[0] if ls and hh else None
        wb = [(ts, int(m.group(1), 16)) for ts, m in ((ts, RE_WBYTES.search(t)) for ts, t in a["lines"]) if m]
        ws = [ts for ts, t in a["lines"] if "worker: writing crop_buf" in t]
        wd = [ts for ts, t in a["lines"] if "FIO_WriteFile done" in t]
        r["wbytes"] = sum(v for _, v in wb)
        r["writes"] = []
        for i, t0 in enumerate(ws):
            done = [t for t in wd if t >= t0]
            byts = [v for t, v in wb if t >= t0]
            r["writes"].append((byts[0] if byts else None, (done[0] - t0) if done else None))
        fw = [m for m in (RE_FW_OK.search(t) for t in T) if m]
        r["fw_ok"] = sum(1 for m in fw if m.group(1) == "ok")
        r["fw_bytes"] = sum(int(m.group(3)) for m in fw if int(m.group(3)) > 0)
        wh = [m for m in (RE_WH.search(t) for t in T) if m]
        r["hdr_bytes"] = sum(int(m.group(2)) for m in wh if int(m.group(2)) > 0)
        r["hdr_fail"] = sum(1 for m in wh if int(m.group(2)) != int(m.group(1)))
        r["frames"] = max(len(gives), len(dsts)) if kind == "raw_vidx" else r["fw_ok"]
        r["ready"] = any("raw job READY" in t for t in T)
        r["queue_full"] = any("All events are non-empty" in t for t in T)
        r["err"] = dict(a["err"])
        # how it ended
        end = a["last_any"]
        ab = [c for c in b.crashes if a["start"] - 1 <= c["ts"] <= end + 15]
        asr = collections.Counter(f.split("::")[1] for ts, f, ln, _ in b.asserts if a["start"] - 1 <= ts <= end + 15 and f != "<garbled>")
        storm = sum(v for k, v in asr.items() if k in STORM_ASSERTS)
        evf = asr.get("EvfDevState.c", 0)
        how = []
        if storm:
            how.append("MvRawPro/RawPro/LvJob storm x%d" % storm)
        if evf:
            how.append("EvfDevState nullptr x%d" % evf)
        other = {k: v for k, v in asr.items() if k not in STORM_ASSERTS and k != "EvfDevState.c"}
        if other:
            how.append("asserts " + ",".join("%s x%d" % kv for kv in sorted(other.items())))
        if ab:
            how.append("ABORT %s %.1fs after last marker" % (sig_of(ab[0])[0], ab[0]["ts"] - a["last"]))
        alive = b.ended - end
        nxt_hb = sum(1 for ts, d, t in b.lines if ts > end + 15 and "[ML] <3" in t)
        if not ab:
            how.append("no abort; boot log continues %.0fs (%d heartbeats later)" % (alive, nxt_hb) if nxt_hb else
                       "no abort; NO heartbeat after +15s (boot log ends %.0fs later: hang or power-off)" % alive)
        if r["queue_full"]:
            how.insert(0, "queue full -> self-stop")
        r["how"] = "; ".join(how)
        out.append(r)
    return out


def rec_tables(recs):
    rows = []
    for i, r in enumerate(recs, 1):
        wr = "; ".join("%s B in %s" % ("%d" % w if w else "?", "%.1fs" % d if d is not None else "? (no 'done')") for w, d in r["writes"]) or "-"
        if r["kind"] == "mlv_lite":
            wr = "hdr %d B%s; framewrite-ok x%d (%d B)" % (r["hdr_bytes"], " (%d short)" % r["hdr_fail"] if r["hdr_fail"] else "", r["fw_ok"], r["fw_bytes"])
        errs = ", ".join("%s x%d" % kv for kv in r["err"].items())
        rows.append([i, hms(r["start"]), r["boot"], r["epoch"], r["kind"], r["frames"],
                     ("%d" % r["frame_bytes"]) if r["frame_bytes"] else "-", wr, "%.1f" % r["dur"],
                     ("READY-poll ok; " if r["ready"] else "") + r["how"] + ("; log errors: " + errs if errs else "")])
    md = "## Recording attempts (%d)\n\n" % len(recs)
    md += ("`frames` for raw_vidx = max(count of `cbr_w: give sem`, distinct `wk.dst`) - a LOWER bound, the module logs are rate-limited "
           "from build 81fd5d54 on. For mlv_lite = count of `@ML> framewrite-ok`. `elapsed` = first to last marker of the attempt; "
           "every timestamp is the host arrival time of a ~0.2 s serial read batch (see rates.md).\n\n")
    md += md_table(["#", "start UTC", "cold-start boot (mllog db boot)", "core build", "module", "frames (logged)", "frame bytes", "bytes written", "elapsed s", "how it ended"], rows)
    return md


def batching_stats(lines):
    ts = sorted(set(t for t, d, _ in lines if d == "rx"))
    gaps = [b - a for a, b in zip(ts, ts[1:]) if b - a < 2.0]
    gaps.sort()
    q = lambda p: gaps[int(p * (len(gaps) - 1))]
    per_batch = collections.Counter(t for t, d, _ in lines if d == "rx")
    sizes = sorted(per_batch.values())
    return {"batches": len(ts), "gap_p10": q(0.10), "gap_p50": q(0.50), "gap_p90": q(0.90), "gap_p99": q(0.99),
            "lines_per_batch_p50": sizes[len(sizes) // 2], "lines_per_batch_p99": sizes[int(0.99 * (len(sizes) - 1))]}


def rate_tables(recs, bst):
    q = bst["gap_p50"]
    md = "## Timestamp resolution\n\n"
    md += ("`ts` is stamped by the host daemon when a serial read returns, not by the camera. Distinct rx timestamps: %d; gap between "
           "consecutive batches (gaps < 2 s): p10 %.3f s, p50 %.3f s, p90 %.3f s, p99 %.3f s; lines per batch p50 %d, p99 %d. "
           "So a single event is known to about -%.1f/+0 s (it happened some time in the batch interval before its stamp) and an interval "
           "between two events has an error of about +/-%.1f s (one batch each end), more when the UART is saturated by Canon spam "
           "(115200 baud = 11.5 kB/s; backlog delays everything behind it).\n\n" % (
               bst["batches"], bst["gap_p10"], q, bst["gap_p90"], bst["gap_p99"], bst["lines_per_batch_p50"], bst["lines_per_batch_p99"], q, q))
    rows, crate, wrate = [], [], []
    for i, r in enumerate(recs, 1):
        if r["kind"] != "raw_vidx":
            continue
        fb = r["frame_bytes"]
        g = r["give_ts"]
        if len(g) >= 2 and fb:
            span = g[-1] - g[0]
            n = len(g) - 1
            lo, hi = max(span - q, 0.05), span + q
            rate = n * fb / span / 1e6 if span > 0 else None
            crate.append((n, span))
            rows.append([i, r["boot"], "copy (give sem x%d)" % len(g), "%d x %d B" % (n, fb), "%.2f" % span,
                         "%.1f" % rate if rate else "-", "%.1f .. %.1f" % (n * fb / hi / 1e6, n * fb / lo / 1e6),
                         "%.2f" % (n / span) if span > 0 else "-"])
        for w, d in r["writes"]:
            if w and d is not None:
                lo, hi = max(d - q, 0.05), d + q
                wrate.append((w, d))
                rows.append([i, r["boot"], "write (writing -> FIO_WriteFile done)", "%d B" % w, "%.2f" % d,
                             "%.1f" % (w / d / 1e6) if d > 0 else ">%.0f" % (w / q / 1e6),
                             "%.1f .. %.1f" % (w / hi / 1e6, w / lo / 1e6), "-"])
    md += "## Measured copy and write intervals (raw_vidx)\n\n"
    md += md_table(["attempt #", "boot", "what", "amount", "interval s", "MB/s (point)", "MB/s range for +/-1 batch", "frames/s"], rows)
    agg = {}
    if crate:
        n = sum(x for x, _ in crate); s = sum(x for _, x in crate)
        agg["copy_frames"], agg["copy_s"] = n, s
        md += "\nPooled copy cadence: %d frame intervals in %.2f s = %.2f frames/s logged cadence.\n" % (n, s, n / s if s else 0)
    if wrate:
        n = sum(x for x, _ in wrate); s = sum(x for _, x in wrate)
        agg["write_bytes"], agg["write_s"] = n, s
        md += "Pooled write: %d B in %.2f s = %.1f MB/s (%d writes).\n" % (n, s, n / s / 1e6 if s else 0, len(wrate))
    return md, agg


# ---------------------------------------------------------------- main
def analyse(jsonl, db):
    lines = load_lines(jsonl)
    rows, deploys = load_boots(db)
    boots = segment(lines, rows)
    assign_epochs(boots)
    for b in boots:
        parse_boot(b)
        speed_class(b)
        b.tag = b.epoch                      # mllog mark-build tag (+wfd count): module/deploy label
        b.epoch = "ax%d" % b.axsize if b.axsize else "ax?"
    # silent early restart: no abort, dies at post.0c..post.2 within 20 s and the camera re-enters the
    # bootloader within 15 s (seen as BootLoad + pre.0 again).
    for b, nxt in zip(boots, boots[1:]):
        last = b.phases[-1] if b.phases else None
        if (not b.crashes and last and last[1] in ("post.0c", "post.1", "post.2") and b.ended - b.started < 20
                and nxt.started - last[0] < 15):
            b.silent_restart = True
    return lines, boots, deploys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jsonl", default=os.path.join(RUN, "uart.jsonl"))
    ap.add_argument("--db", default=os.path.join(RUN, "mllog.db"))
    ap.add_argument("--out", default=os.path.join(HERE, "out"))
    a = ap.parse_args()
    lines, boots, deploys = analyse(a.jsonl, a.db)
    os.makedirs(a.out, exist_ok=True)
    cmd, sigs = crash_tables(boots)
    amd, per, bursts = assert_tables(boots)
    bmd, eps = build_table(boots)
    recs = recordings(boots)
    rmd = rec_tables(recs)
    bst = batching_stats(lines)
    ratemd, agg = rate_tables(recs, bst)
    for name, md in (("crashes.md", cmd), ("asserts.md", amd), ("builds.md", bmd), ("recordings.md", rmd), ("rates.md", ratemd)):
        with open(os.path.join(a.out, name), "w") as f:
            f.write(md)
    summ = {"lines": len(lines), "boots": len(boots), "boots_with_abort": sum(1 for b in boots if b.crashes),
            "boots_big8": sum(1 for b in boots if b.reached("big.8")), "signatures": len(sigs),
            "assert_sites": len(per), "burst_kinds": len(bursts), "recording_attempts": len(recs),
            "garbled_abort_only_boots": sum(1 for b in boots if b.garbled_abort and not b.crashes),
            "batching": bst, "rates": agg,
            "per_boot": [{"boot": b.id, "epoch": b.epoch, "started": b.started, "big8": b.reached("big.8"),
                          "last_phase": b.phases[-1][1] if b.phases else None,
                          "aborts": [sig_of(c)[0] for c in b.crashes], "asserts": len(b.asserts)} for b in boots]}
    with open(os.path.join(a.out, "summary.json"), "w") as f:
        json.dump(summ, f, indent=1)
    print(json.dumps({k: v for k, v in summ.items() if k != "per_boot"}, indent=1))


if __name__ == "__main__":
    main()
