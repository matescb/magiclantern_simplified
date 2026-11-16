#!/usr/bin/env python3
"""mldeploy - push a file to the SD card OVER SERIAL (no card shuffle).

Uses the running ML's own FIO (proven working from serial_shell) to write a file to
the card, streaming the bytes through the shell's 256-byte `load` scratch buffer:
    FIO_CreateFile(path)  ->  loop[ load <=256B ; FIO_WriteFile(handle, scratch, n) ]  ->  FIO_CloseFile
then verifies via FIO_GetFileSize. The camera must be booted, ML alive, and idle
(not recording) so the SD is write-mounted on B:.

    python3 mldeploy.py <local_file> <card_path>      # e.g. ... mlv_lite.mo B:/ML/MODULES/mlv_lite.mo
    python3 mldeploy.py --selftest                     # write+verify a small pattern file, report throughput

WHY IT'S SLOW (investigated 2026-07-16 — it's NOT the 115200 baud):
  * The camera's RX is the bottleneck, not the wire. serial_shell tails Canon's SMALL
    ch0 console ring; a `load` burst >64 B OVERFLOWS it -> byte loss, at EVERY host pace
    (loads <=64 B are byte-perfect; 128 B+ corrupt even at the slow default). Lowering
    the host PACE does NOT help (bisected 0.02->0.0002; faster just hits the ring limit).
  * Long command RESPONSES (e.g. a 256-B md readback) get SHREDDED by the async heartbeat
    on the shared TX register, so verifying big transfers is unreliable too.
  * Separate bug: multi-write-to-one-handle over serial ends the file at size 0 (single
    writes persist fine). Not yet root-caused.
  => Real fast/reliable UART deploy needs CAMERA-SIDE work (autoexec rebuild): a bigger
     owned RX buffer + tighter drain loop, and g_ml_uart_quiet to mute heartbeats during
     commands. Until then this streams at ~15-21 B/s in 64-B chunks -> full 74 KB .mo ~1 hr;
     use it only for tiny patches, and PREFER THE PHYSICAL FLASH for modules.
Fixed FIO wrapper addrs (M50mk2 1.0.1): CreateFile 0xcf791, WriteFile 0xcf631,
GetFileSize 0xcf44d, CloseFile 0xe05b1c1b. Scratch size 256 (SH_SCRATCH_LEN).
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FIO_CREATE, FIO_WRITE, FIO_CLOSE = 0xcf7ed, 0xcf681, 0xe05b1c1b   # [2026-07-21] updated for module-settle build 7945a4fe
FIO_GETSIZE_DIRECT = 0xcf575   # returns size as the RETURN value (reliable; out-param ver is flaky)
SCRATCH_MAX = 64             # loads >64 B overflow the camera's small RX ring -> byte loss.
                             # 64 B is empirically loss-free; the 256 B scratch isn't the limit,
                             # the RX drain is. (Real speedup needs a camera-side RX fix.)
SCRATCH_ADDR = 0x14EEA8      # serial_shell sh_scratch (static, fixed per autoexec build)
SIZE_OUT = 0x14F800          # spare RAM to receive FIO_GetFileSize's out-param


def sh(text, timeout=40):
    r = subprocess.run(["python3", "mlserial.py", "cmd", text], cwd=HERE,
                       capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr


def load_blob(data):
    """Stage <=256 bytes into the shell scratch; return its RAM address.

    The `load` reply is often shredded by an async heartbeat on the shared UART, so
    don't parse it -- VERIFY the stage by reading the scratch buffer's first word
    back (a short, robust read) and retry on mismatch. Returns the fixed scratch addr.
    """
    want = int.from_bytes(data[:4].ljust(4, b"\x00"), "little")
    for _ in range(5):
        sh("load " + data.hex())
        if len(data) < 4 or call_read(SCRATCH_ADDR) == want:
            return SCRATCH_ADDR
        time.sleep(0.15)
    raise RuntimeError("load verify failed (scratch[0]!=0x%08x)" % want)


def call(addr, *args):
    out = sh("call 0x%x %s" % (addr, " ".join("0x%x" % a for a in args)))
    m = re.search(r"ret\s+(0x[0-9A-Fa-f]+)", out)
    return int(m.group(1), 16) if m else None


def stage_path(path):
    return load_blob(path.encode("latin-1") + b"\x00")


def call_read(addr):
    out = sh("rd 0x%x" % addr)
    m = re.search(r"0x[0-9A-Fa-f]+:\s*(0x[0-9A-Fa-f]+)", out)
    return int(m.group(1), 16) if m else None


def deploy(local, card_path):
    data = open(local, "rb").read()
    print("deploy %s (%d B) -> %s" % (local, len(data), card_path))
    h = call(FIO_CREATE, stage_path(card_path))
    if h is None or h > 0x100:
        raise RuntimeError("FIO_CreateFile returned %s" % (hex(h) if h else None))
    print("  handle=0x%x" % h)
    t0 = time.time()
    written = 0
    for off in range(0, len(data), SCRATCH_MAX):
        chunk = data[off:off + SCRATCH_MAX]
        addr = load_blob(chunk)
        r = call(FIO_WRITE, h, addr, len(chunk))
        if r != len(chunk):
            raise RuntimeError("write @%d: ret=%s expected %d" %
                               (off, hex(r) if r is not None else None, len(chunk)))
        written += len(chunk)
        if off % (SCRATCH_MAX * 8) == 0:
            print("  %d/%d B (%.0f B/s)" % (written, len(data), written / max(0.1, time.time() - t0)))
    call(FIO_CLOSE, h)
    dt = time.time() - t0
    sz = call(FIO_GETSIZE_DIRECT, stage_path(card_path))   # returns size directly
    ok = sz == len(data)
    print("  wrote %d B in %.1fs (%.0f B/s); on-card size=%s  %s" %
          (written, dt, written / max(0.1, dt), sz, "OK" if ok else "MISMATCH"))
    return ok


def selftest():
    import tempfile
    pattern = bytes(range(256)) * 2        # 512 B, spans 2 scratch chunks
    p = os.path.join(tempfile.gettempdir(), "mldeploy_selftest.bin")
    open(p, "wb").write(pattern)
    ok = deploy(p, "B:/MLDEPTST.BIN")
    print("SELFTEST %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    a = sys.argv[1:]
    if not a or a[0] == "--selftest":
        sys.exit(selftest())
    if len(a) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(0 if deploy(a[0], a[1]) else 1)


if __name__ == "__main__":
    main()
