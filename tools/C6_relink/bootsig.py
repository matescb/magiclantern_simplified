#!/usr/bin/env python3
"""Read-only: per-boot build fingerprint from uart.jsonl: AUTOEXEC size, abort-stub addrs, DS buf addr,
last big.* marker, first [AB] crash line.  usage: bootsig.py FIRST LAST"""
import json, re, sqlite3, sys
RUN = "/mnt/data/WORK/eos/tools/mlserial/run"
a, b = int(sys.argv[1]), int(sys.argv[2])
db = sqlite3.connect(f"file:{RUN}/mllog.db?mode=ro", uri=True)
boots = db.execute("select boot_id,started,ended from boots where boot_id between ? and ? order by boot_id", (a, b)).fetchall()
sig = {bid: dict(size="?", pab="?", ds="?", last="-", ab="-", dur=int(e - s)) for bid, s, e in boots}
idx = 0
with open(f"{RUN}/uart.jsonl", errors="replace") as f:
    for line in f:
        i = line.find('"ts": ')
        try: ts = float(line[i+6:line.index(',', i)])
        except Exception: continue
        if ts < boots[0][1] - 3: continue
        while idx < len(boots) - 1 and ts >= boots[idx+1][1] - 3: idx += 1
        if ts > boots[-1][2] + 3: break
        bid = boots[idx][0]; d = sig[bid]
        m = re.search(r"AUTOEXEC.bin \((\d+) bytes", line)
        if m: d["size"] = m.group(1)
        m = re.search(r"pabort=(0x[0-9A-F]+) dabort=(0x[0-9A-F]+)", line)
        if m: d["pab"] = m.group(1) + "/" + m.group(2)
        m = re.search(r"DS buf addr=(0x[0-9A-F]+)", line)
        if m: d["ds"] = m.group(1)
        m = re.search(r"\[ML\] (big\.[0-9a-z.]+)", line)
        if m: d["last"] = m.group(1)
        m = re.search(r"\[AB\] (pc=0x[0-9A-F]+ lr=0x[0-9A-F]+ dfar=0x[0-9A-F]+)", line)
        if m and d["ab"] == "-": d["ab"] = m.group(1)
for bid, d in sig.items():
    print(f"{bid:4d} dur={d['dur']:6d}s size={d['size']:>7} abort={d['pab']:<21} ds={d['ds']:<10} last={d['last']:<8} {d['ab']}")
