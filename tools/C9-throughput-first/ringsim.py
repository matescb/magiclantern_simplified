#!/usr/bin/env python3
"""C9-throughput-first: frame budget + ring simulation for M50 II raw recording.

Host arithmetic only. Every rate is an input assumption (see
docs/offline/C9-throughput-first.md for the source of each number).

  python3 ringsim.py            # markdown tables used in the report
  python3 ringsim.py --test     # self-test (asserts, prints "selftest OK")

Model
  producer : one frame per 1/fps into the next free slot (never blocks; if no
             slot is free the frame is dropped and counted)
  ring     : N slots; `reserved` of them are never queued for the writer
             (flip mode: 1 being filled by Canon + 1 armed as next target;
              copy/DMA mode: 1 being filled)
  writer   : drains queued slots in FIFO order at `card` bytes/s, one
             FIO_WriteFile per slot payload; optional periodic stall
             (card garbage collection) of `stall_ms` every `stall_every_s`
"""
import sys
from dataclasses import dataclass

MB = 1_000_000
MIB = 1 << 20
ALIGN = 0x1000            # block and slot alignment (also 8 x 512-byte sectors)
VIDF_HDR = 32             # sizeof(mlv_vidf_hdr_t)
FULL_W, FULL_H = 2096, 1164
CARDS = (20, 40, 80)      # MB/s, as given in the task


def align_up(n, a=ALIGN):
    return -(-n // a) * a


def frame_bytes(w, h, bpp):
    bits = w * h * bpp
    assert bits % 8 == 0, (w, h, bpp)
    return bits // 8


@dataclass(frozen=True)
class Mode:
    name: str
    w: int
    h: int
    bpp: int
    source: str            # flip | copy | dma
    ratio: float = 1.0     # compression ratio estimate (1.0 = none)
    needs: str = ""        # firmware unknown that gates this mode

    @property
    def payload(self):
        return int(frame_bytes(self.w, self.h, self.bpp) * self.ratio)

    @property
    def block(self):
        """Bytes written per frame: VIDF header + frameSpace pad + payload, 4K multiple."""
        return align_up(self.payload + VIDF_HDR)

    @property
    def slot(self):
        """Bytes of RAM per ring slot.
        flip: Canon always writes the full 2096x1164 14-bit frame into the slot
        (we cannot crop a channel we must not touch), plus one header page.
        copy/dma: slot holds exactly one output block."""
        if self.source == "flip":
            return align_up(frame_bytes(FULL_W, FULL_H, 14)) + ALIGN
        return self.block

    @property
    def reserved(self):
        return 2 if self.source == "flip" else 1

    def rate(self, fps):
        return self.block * fps


MODES = (
    Mode("2096x1164 14b", 2096, 1164, 14, "flip"),
    Mode("2096x1080 14b", 2096, 1080, 14, "flip"),
    Mode("2096x896 14b", 2096, 896, 14, "flip"),
    Mode("2096x786 14b", 2096, 786, 14, "flip"),
    Mode("1920x1080 14b", 1920, 1080, 14, "dma"),
    Mode("1792x896 14b", 1792, 896, 14, "dma"),
    Mode("1280x720 14b", 1280, 720, 14, "copy"),
    Mode("2096x1164 12b", 2096, 1164, 12, "dma", needs="U6 pack mode"),
    Mode("2096x1164 10b", 2096, 1164, 10, "dma", needs="U6 pack mode"),
    Mode("1920x1080 10b", 1920, 1080, 10, "dma", needs="U6 pack mode"),
    Mode("2096x1164 14b LJ92~0.57", 2096, 1164, 14, "dma", ratio=0.57, needs="U7 lossless"),
)


def ring_slots(ring_bytes, mode):
    return ring_bytes // mode.slot


def analytic(mode, fps, card_mb, ring_bytes):
    """Returns (rate_MBps, slots, queueable, seconds_until_drop or None if continuous)."""
    n = ring_slots(ring_bytes, mode)
    q = max(n - mode.reserved, 0)
    rate = mode.rate(fps)
    out = card_mb * MB
    if rate <= out:
        return rate / MB, n, q, None
    # queue grows by (rate - out) bytes of *written* data per second; a slot is
    # freed each time `block` bytes are written.
    return rate / MB, n, q, q * mode.block / (rate - out)


def simulate(mode, fps, card_mb, ring_bytes, seconds=120.0,
             stall_ms=0.0, stall_every_s=0.0, dt=0.0005):
    """Discrete-time simulation. Returns dict(frames, dropped, max_fill, first_drop_s)."""
    n = ring_slots(ring_bytes, mode)
    q_cap = max(n - mode.reserved, 0)
    out = card_mb * MB
    queue = 0              # slots waiting or being written
    written_in_slot = 0.0
    t = 0.0
    next_frame = 0.0
    frames = dropped = 0
    max_fill = 0
    first_drop = None
    while t < seconds:
        if t >= next_frame:
            next_frame += 1.0 / fps
            if queue < q_cap:
                queue += 1
                frames += 1
                max_fill = max(max_fill, queue)
            else:
                dropped += 1
                if first_drop is None:
                    first_drop = t
        stalled = (stall_every_s > 0 and stall_ms > 0
                   and (t % stall_every_s) < stall_ms / 1000.0 and t >= stall_every_s)
        if queue and not stalled:
            written_in_slot += out * dt
            while queue and written_in_slot >= mode.block:
                written_in_slot -= mode.block
                queue -= 1
            if not queue:
                written_in_slot = 0.0
        t += dt
    return dict(slots=n, queueable=q_cap, frames=frames, dropped=dropped,
                max_fill=max_fill, first_drop_s=first_drop)


def fmt_t(s):
    if s is None:
        return "continuous"
    return f"{s:.1f} s" if s < 600 else "continuous*"


def table_budget(fps, ring_bytes):
    print(f"\n#### Frame budget at {fps} fps, ring {ring_bytes // MIB} MiB\n")
    print("| mode | source | payload B | block B | rate MB/s | ring slots (queueable) | "
          + " | ".join(f"card {c} MB/s" for c in CARDS) + " | gate |")
    print("|---|---|---|---|---|---|" + "---|" * len(CARDS) + "---|")
    for m in MODES:
        cells = []
        for c in CARDS:
            rate, n, q, t = analytic(m, fps, c, ring_bytes)
            cells.append(fmt_t(t))
        print(f"| {m.name} | {m.source} | {m.payload} | {m.block} | {rate:.1f} | {n} ({q}) | "
              + " | ".join(cells) + f" | {m.needs or '-'} |")


def table_fps_limit(ring_bytes):
    print("\n#### Highest continuous frame rate per mode (rate = card speed)\n")
    print("| mode | block B | " + " | ".join(f"fps at {c} MB/s" for c in (20, 40, 70, 75, 80)) + " |")
    print("|---|---|---|---|---|---|---|")
    for m in MODES:
        print(f"| {m.name} | {m.block} | "
              + " | ".join(f"{c * MB / m.block:.1f}" for c in (20, 40, 70, 75, 80)) + " |")


def table_stall(fps, ring_bytes):
    print(f"\n#### Stall tolerance at {fps} fps, ring {ring_bytes // MIB} MiB, 120 s simulated, "
          "card stalls 250 ms every 5 s\n")
    print("| mode | card MB/s | slots (queueable) | frames | dropped | max fill | first drop |")
    print("|---|---|---|---|---|---|---|")
    for m in MODES:
        for c in (75, 80):
            if m.rate(fps) > c * MB:
                continue
            r = simulate(m, fps, c, ring_bytes, stall_ms=250, stall_every_s=5)
            fd = "-" if r["first_drop_s"] is None else f"{r['first_drop_s']:.1f} s"
            print(f"| {m.name} | {c} | {r['slots']} ({r['queueable']}) | {r['frames']} | "
                  f"{r['dropped']} | {r['max_fill']} | {fd} |")


def table_copy_stage(fps):
    print(f"\n#### Fill-stage requirement at {fps} fps (copy/DMA must move this much per second, "
          "and must outrun Canon's next write to avoid tearing)\n")
    print("| mode | bytes read per frame | needed MB/s (mean) | needed MB/s (finish in half a frame) |")
    print("|---|---|---|---|")
    for m in MODES:
        if m.source == "flip":
            continue
        src = frame_bytes(m.w, m.h, 14)
        print(f"| {m.name} | {src} | {src * fps / MB:.1f} | {src * fps * 2 / MB:.1f} |")


def main():
    for ring in (40 * MIB, 48 * MIB):
        table_budget(23.976, ring)
    table_budget(25.0, 40 * MIB)
    table_fps_limit(40 * MIB)
    table_stall(23.976, 40 * MIB)
    table_copy_stage(23.976)


def selftest():
    full = Mode("f", 2096, 1164, 14, "flip")
    assert full.payload == 4_269_552                      # matches first_raw_capture.raw
    assert Mode("c", 1792, 896, 14, "dma").payload == 2_809_856
    assert full.block % ALIGN == 0 and full.block >= full.payload + VIDF_HDR
    assert full.slot == align_up(4_269_552) + ALIGN
    assert ring_slots(40 * MIB, full) == 9
    # continuous when card >= rate
    assert analytic(Mode("c", 1280, 720, 14, "copy"), 23.976, 80, 40 * MIB)[3] is None
    # overflow time: analytic and simulation agree within one frame period + 5 %
    m = Mode("c", 1792, 896, 14, "dma")
    _, n, q, t = analytic(m, 23.976, 40, 40 * MIB)
    r = simulate(m, 23.976, 40, 40 * MIB, seconds=30)
    assert r["first_drop_s"] is not None
    assert abs(r["first_drop_s"] - t) < 0.05 * t + 2 / 23.976, (r, t)
    # no drops when the card is faster than the stream and there are no stalls
    r = simulate(m, 23.976, 80, 40 * MIB, seconds=20)
    assert r["dropped"] == 0 and r["max_fill"] <= 2, r
    # a stall longer than the ring depth must drop frames
    r = simulate(m, 23.976, 80, 40 * MIB, seconds=20, stall_ms=2000, stall_every_s=5)
    assert r["dropped"] > 0
    print("selftest OK")


if __name__ == "__main__":
    if "--test" in sys.argv:
        selftest()
    else:
        main()
