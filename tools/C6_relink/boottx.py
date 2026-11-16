#!/usr/bin/env python3
"""Read-only: list host->camera (non-rx) lines per boot with offset from boot start, plus first [ABORT] offset.
usage: boottx.py FIRST LAST"""
import json, sqlite3, sys
RUN = "/mnt/data/WORK/eos/tools/mlserial/run"
a, b = int(sys.argv[1]), int(sys.argv[2])
db = sqlite3.connect(f"file:{RUN}/mllog.db?mode=ro", uri=True)
boots = db.execute("select boot_id,started,ended from boots where boot_id between ? and ? order by boot_id", (a, b)).fetchall()
idx = 0; seen_ab = set()
with open(f"{RUN}/uart.jsonl", errors="replace") as f:
    for line in f:
        i = line.find('"ts": ')
        try: ts = float(line[i+6:line.index(',', i)])
        except Exception: continue
        if ts < boots[0][1]: continue
        while idx < len(boots) - 1 and ts >= boots[idx+1][1]: idx += 1
        if ts > boots[-1][2] + 1: break
        bid, s, e = boots[idx]
        if '"dir": "rx"' in line:
            if "[ABORT] type" in line and bid not in seen_ab:
                seen_ab.add(bid); print(f"{bid:4d} +{ts-s:8.2f}s ABORT {json.loads(line)['text'][:60]}")
            continue
        j = json.loads(line)
        print(f"{bid:4d} +{ts-s:8.2f}s {j.get('dir')}/{j.get('kind')} {j.get('text','')[:110]}")
