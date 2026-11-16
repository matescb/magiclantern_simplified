#!/usr/bin/env python3
"""C9-safe-first: frame-budget arithmetic for M50 II raw recording design.

Pure host arithmetic, no camera access. All rates are ASSUMPTIONS passed in
as parameters; see docs/offline/C9-safe-first.md for where each comes from.

  python3 budget.py            # print all tables (markdown)
  python3 budget.py --test     # self-test
"""
import sys

MB = 1_000_000          # rates in decimal MB/s, like the ML bench prints
MIB = 1 << 20
LV_FPS = 23.976         # Canon 1080p24 LiveView; hardware cadence unverified (24-25)
VIDF_HDR = 32           # sizeof(mlv_vidf_block)
ALIGN = 0x1000          # slot alignment used for the frame ring


def frame_bytes(w, h, bpp):
    """Packed raw frame size. 14-bit packing needs w % 8 == 0 (8 px = 14 bytes)."""
    bits = w * h * bpp
    assert bits % 8 == 0, (w, h, bpp)
    return bits // 8


def slot_bytes(fb):
    """One ring slot = VIDF header + padding + payload, rounded up to ALIGN."""
    return -(-(fb + VIDF_HDR) // ALIGN) * ALIGN


def ring_frames(ring_bytes, fb):
    return ring_bytes // slot_bytes(fb)


def freeze_fps(fb, copy_rate, rearm_frames=3, duty=0.8):
    """Mode F: freeze Canon's buffer, copy, re-arm. duty = share of time the
    low-priority copy task actually runs (it yields on purpose)."""
    t = fb / (copy_rate * MB * duty) + rearm_frames / LV_FPS
    return 1.0 / t


def live_max_coherent_bytes(copy_rate, fps=LV_FPS):
    """Mode L: copy from the live (refreshed) buffer. Upper bound on the bytes
    that can be copied before Canon's next write passes over them."""
    return copy_rate * MB / fps


def continuous(fb, fps, card_rate, ring_bytes):
    """Seconds until the ring fills when frames arrive at fps and the card
    drains at card_rate. None = sustainable."""
    need = fb * fps
    if need <= card_rate * MB:
        return None
    return ring_bytes / (need - card_rate * MB)


OPTIONS = [  # label, w, h, bpp, note
    ("full 2096x1164", 2096, 1164, 14, ""),
    ("current 1792x896", 1792, 896, 14, ""),
    ("1280x720", 1280, 720, 14, ""),
    ("960x540", 960, 540, 14, ""),
    ("640x360", 640, 360, 14, ""),
    ("1792x896 12-bit", 1792, 896, 12, "CPU repack"),
    ("1792x896 10-bit", 1792, 896, 10, "CPU repack"),
    ("1280x720 10-bit", 1280, 720, 10, "CPU repack"),
]
RINGS = [32 * MIB, 48 * MIB]
CARDS = [20, 40, 80]
COPIES = [6, 25, 80]
SKIPS = [1, 2, 4]


def fmt_t(t):
    return "sustained" if t is None else f"{t:.1f} s"


def tables():
    out = []
    out.append("#### T1 frame size, required rate, ring capacity\n")
    out.append("| option | bytes/frame | MB/s @23.976 | MB/s @11.988 | MB/s @5.994 | frames in 32 MiB | frames in 48 MiB | burst s @23.976 (32/48) |")
    out.append("|---|---|---|---|---|---|---|---|")
    for label, w, h, bpp, note in OPTIONS:
        fb = frame_bytes(w, h, bpp)
        n = [ring_frames(r, fb) for r in RINGS]
        out.append(f"| {label}{' ('+note+')' if note else ''} | {fb:,} | "
                   + " | ".join(f"{fb*LV_FPS/s/MB:.1f}" for s in SKIPS)
                   + f" | {n[0]} | {n[1]} | {n[0]/LV_FPS:.2f} / {n[1]/LV_FPS:.2f} |")
    out.append("\n#### T2 mode F (freeze, copy, re-arm): effective fps by copy rate\n")
    out.append("| option | " + " | ".join(f"copy {c} MB/s" for c in COPIES) + " |")
    out.append("|---|" + "---|" * len(COPIES))
    for label, w, h, bpp, note in OPTIONS:
        if note:
            continue
        fb = frame_bytes(w, h, bpp)
        out.append(f"| {label} | " + " | ".join(f"{freeze_fps(fb, c):.2f} fps" for c in COPIES) + " |")
    out.append("\n#### T3 mode L (copy from the live buffer): copy rate needed, and what fits in one frame period\n")
    out.append("| copy rate | max coherent bytes per frame period | largest listed 14-bit option that fits |")
    out.append("|---|---|---|")
    for c in COPIES:
        lim = live_max_coherent_bytes(c)
        fits = [l for l, w, h, b, n in OPTIONS if not n and frame_bytes(w, h, b) <= lim]
        out.append(f"| {c} MB/s | {lim:,.0f} | {fits[0] if fits else 'none (< 640x360)'} |")
    out.append("\n#### T4 continuous writing: time until a 32 MiB ring fills (48 MiB in brackets), copy assumed not limiting\n")
    hdr = "| option | fps | need MB/s | " + " | ".join(f"card {c} MB/s" for c in CARDS) + " |"
    out.append(hdr)
    out.append("|---|---|---|" + "---|" * len(CARDS))
    for label, w, h, bpp, note in OPTIONS:
        fb = frame_bytes(w, h, bpp)
        for s in SKIPS:
            fps = LV_FPS / s
            cells = []
            for c in CARDS:
                a, b = (continuous(fb, fps, c, r) for r in RINGS)
                cells.append(fmt_t(a) if a is None else f"{a:.1f} s ({b:.1f} s)")
            out.append(f"| {label} | {fps:.3f} | {fb*fps/MB:.1f} | " + " | ".join(cells) + " |")
    out.append("\n#### T5 write-after-stop: flush time for a full ring\n")
    out.append("| ring | " + " | ".join(f"card {c} MB/s" for c in CARDS) + " |")
    out.append("|---|" + "---|" * len(CARDS))
    for r in RINGS:
        out.append(f"| {r//MIB} MiB | " + " | ".join(f"{r/(c*MB):.1f} s" for c in CARDS) + " |")
    return "\n".join(out)


def selftest():
    assert frame_bytes(2096, 1164, 14) == 4_269_552      # matches first_raw_capture.raw
    assert frame_bytes(1792, 896, 14) == 2_809_856       # matches HW log 4 x 2809856 B
    assert slot_bytes(2_809_856) == 2_813_952
    assert ring_frames(32 * MIB, 2_809_856) == 11
    assert continuous(1_612_800, LV_FPS, 80, 32 * MIB) is None
    t = continuous(2_809_856, LV_FPS, 40, 32 * MIB)
    assert 1.2 < t < 1.3, t
    assert abs(freeze_fps(2_809_856, 6) - 1 / (2_809_856 / 4.8e6 + 3 / LV_FPS)) < 1e-9
    assert live_max_coherent_bytes(6) < frame_bytes(640, 360, 14)
    print("budget.py self-test: 8 checks OK")


if __name__ == "__main__":
    if "--test" in sys.argv:
        selftest()
    else:
        print(tables())
