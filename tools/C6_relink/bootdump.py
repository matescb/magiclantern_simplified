#!/usr/bin/env python3
"""Read-only: dump uart.jsonl text for one boot id (uses mllog.db read-only for the time window).
usage: bootdump.py BOOT_ID [pad_before_s] [pad_after_s]"""
import json, sqlite3, sys
RUN = "/mnt/data/WORK/eos/tools/mlserial/run"
b = int(sys.argv[1]); pb = float(sys.argv[2]) if len(sys.argv) > 2 else 0; pa = float(sys.argv[3]) if len(sys.argv) > 3 else 0
db = sqlite3.connect(f"file:{RUN}/mllog.db?mode=ro", uri=True)
s, e = db.execute("select started,ended from boots where boot_id=?", (b,)).fetchone()
s -= pb; e += pa
with open(f"{RUN}/uart.jsonl", errors="replace") as f:
    for line in f:
        i = line.find('"ts": ')
        try: ts = float(line[i+6:line.index(',', i)])
        except Exception: continue
        if ts < s: continue
        if ts > e: break
        try: j = json.loads(line)
        except Exception: continue
        print(f"{ts-s:8.3f} {j.get('dir','?'):3} {j.get('text','')}")
