#!/usr/bin/env python3
"""Read-only audit: for each boot, the core-layout fingerprint (pabort stub addr printed by ML at pre.2b)
and every raw RAM address the host poked via serial 'call'/'wr' (0x000Cxxxx-0x0015xxxx).
usage: calladdr_audit.py FIRST LAST"""
import json, re, sqlite3, sys, collections
RUN = "/mnt/data/WORK/eos/tools/mlserial/run"
a, b = int(sys.argv[1]), int(sys.argv[2])
db = sqlite3.connect(f"file:{RUN}/mllog.db?mode=ro", uri=True)
boots = db.execute("select boot_id,started,ended from boots where boot_id between ? and ? order by boot_id", (a, b)).fetchall()
fp = {}; cmds = collections.defaultdict(collections.Counter); ab = {}
idx = 0
with open(f"{RUN}/uart.jsonl", errors="replace") as f:
    for line in f:
        i = line.find('"ts": ')
        try: ts = float(line[i+6:line.index(',', i)])
        except Exception: continue
        if ts < boots[0][1] - 3: continue
        while idx < len(boots) - 1 and ts >= boots[idx+1][1] - 3: idx += 1
        if ts > boots[-1][2] + 3: break
        bid = boots[idx][0]
        if '"dir": "tx"' in line:
            t = json.loads(line).get("text", "")
            m = re.search(r"\b(call|wr)\s+(0x[0-9a-fA-F]+)", t)
            if m and 0xC0000 <= int(m.group(2), 16) < 0x160000:
                cmds[bid][m.group(1) + " " + m.group(2).lower()] += 1
        else:
            m = re.search(r"pabort=(0x[0-9A-F]+)", line)
            if m: fp[bid] = m.group(1)
            m = re.search(r"\[ABORT\] type=\S+ pc=(0x[0-9A-F]+)", line)
            if m and bid not in ab: ab[bid] = m.group(1)
for bid, _, _ in boots:
    if cmds[bid]:
        print(f"{bid:4d} layout(pabort)={fp.get(bid,'?'):<11} abort={ab.get(bid,'-'):<11} " + ", ".join(f"{k} x{v}" for k, v in cmds[bid].items()))
print("layouts seen:", dict(collections.Counter(fp.values())))
