#!/usr/bin/env python3
"""C4c: estimate the M50 II vsync-hook rate from logged `rd <addr>` replies in uart.jsonl.

Read-only. Groups consecutive counter readings into runs (monotonic counter, gap < max_gap s)
and reports counts/second per run (end-to-end and least-squares slope).
Usage: vsync_rate.py [uart.jsonl] [addr=0x0014C360]
"""
import json, re, sys

def load(path, addr):
    pat = re.compile(r"0x%08X: 0x([0-9A-Fa-f]{8})" % addr)
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            if "%08X:" % addr not in line:
                continue
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            m = pat.search(rec.get("text", ""))
            if m and rec.get("dir") == "rx":
                out.append((rec["ts"], int(m.group(1), 16)))
    return out

def runs(samples, max_gap=120.0):
    cur = []
    for ts, v in samples:
        if cur and (v < cur[-1][1] or ts - cur[-1][0] > max_gap):
            yield cur
            cur = []
        cur.append((ts, v))
    if cur:
        yield cur

def slope(run):
    n = len(run)
    mx = sum(t for t, _ in run) / n
    my = sum(v for _, v in run) / n
    den = sum((t - mx) ** 2 for t, _ in run)
    return sum((t - mx) * (v - my) for t, v in run) / den if den else float("nan")

def main(argv):
    path = argv[1] if len(argv) > 1 else "/mnt/data/WORK/eos/tools/mlserial/run/uart.jsonl"
    addr = int(argv[2], 0) if len(argv) > 2 else 0x0014C360
    samples = load(path, addr)
    print("samples: %d" % len(samples))
    for run in runs(samples):
        if len(run) < 2:
            continue
        dt = run[-1][0] - run[0][0]
        dv = run[-1][1] - run[0][1]
        if dt <= 0 or dv <= 0:
            continue
        print("n=%3d span=%8.3fs counts=%6d  end-to-end=%7.3f/s  lsq=%7.3f/s  (+-%.3f/s from 0.2s host ts jitter)"
              % (len(run), dt, dv, dv / dt, slope(run), dv / dt * 0.2 / dt))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
