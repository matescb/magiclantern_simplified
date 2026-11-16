#!/usr/bin/env python3
"""C4c: look in the M50 II ROM for (timerA, timerB) pairs consistent with a TG base clock.

For each candidate base clock F and each standard frame rate, a Canon sensor-mode table that
stores AccumH (timer A) and VSize (timer B) close together satisfies fps*(A)*(B) ~= F.
We count 16-bit pairs (same alignment, distance 2..16 bytes) whose product matches within
tol, restricted to A values in a plausible window, and cluster them in 256-byte windows.
A real base clock should show windows holding SEVERAL distinct frame rates with the SAME A;
decoy clocks give a noise floor.  Read-only.
"""
import struct, sys, collections
ROM = "/mnt/data/WORK/eos/m50ii_dumper/dump_1.0.1/gang100.bin"
FPS = [23.976, 25.0, 29.97, 50.0, 59.94, 100.0, 119.88]
BASES = [27.0e6, 32.0e6, 24.0e6, 28.8e6, 30.0e6, 35.0e6]   # last four = decoys / other Canon bases
TOL = 0.004

def main():
    d = open(ROM, "rb").read()
    n = len(d) // 2
    w = struct.unpack("<%dH" % n, d[: n * 2])
    res = {b: collections.defaultdict(set) for b in BASES}
    for i in range(n):
        a = w[i]
        if not (0x150 <= a <= 0x900):
            continue
        for dist in range(1, 9):
            if i + dist >= n:
                break
            b = w[i + dist]
            if not (0x150 <= b <= 0x1000):
                continue
            for base in BASES:
                for f in FPS:
                    for adj in (0, 1):          # stored as value or value-1
                        p = f * (a + adj) * (b + adj)
                        if abs(p - base) <= TOL * base:
                            res[base][(i * 2) >> 8].add((f, a + adj, b + adj, i * 2))
    for base in BASES:
        wins = res[base]
        multi = {k: v for k, v in wins.items() if len({x[0] for x in v}) >= 3}
        print("base %.1f MHz: windows with any hit=%d, windows with >=3 distinct fps=%d"
              % (base / 1e6, len(wins), len(multi)))
        for k in sorted(multi, key=lambda k: -len({x[0] for x in multi[k]}))[:6]:
            v = sorted(multi[k], key=lambda x: x[3])
            print("   0x%08X: %s" % (0xE0000000 + (k << 8),
                  ", ".join("%.2f:%X x %X=%.3fM@%X" % (f, a, b, f * a * b / 1e6, 0xE0000000 + o)
                            for f, a, b, o in v[:8])))
    return 0

if __name__ == "__main__":
    sys.exit(main())
