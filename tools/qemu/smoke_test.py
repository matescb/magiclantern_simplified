#!/usr/bin/env python3
"""smoke_test.py - boot the M50 Mark II ML build in QEMU and poke the serial shell.

    smoke_test.py --sd IMG --outdir DIR [--port 4487] [--boot-timeout 240]

Steps
  1. start run_m50mk2.sh --detach (UART on tcp:127.0.0.1:PORT + log file)
  2. tail the UART log, time-stamping every boot marker ([ML] pre.N/post.N/big.N),
     until the wanted marker (default "big.8") shows up or --boot-timeout expires
  3. over the TCP serial shell send "@1 ver" and "@2 help", one character every
     --char-delay seconds (QEMU's UART RX drops characters on fast bursts,
     docs/33), and collect each reply up to its "=ok @N" / "=err @N" terminator
     (framing: tools/mlserial/PROTOCOL.md)
  4. kill QEMU by PID, write DIR/summary.txt + DIR/summary.json

The shell is tried even when the boot marker never appears (the shell task is
created at post.0h, long before big.8), so a partial boot still yields data.

Exit code: 0 = marker seen AND both commands answered "=ok"
           1 = boot marker not seen          2 = marker seen, shell failed
           3 = harness error (QEMU did not start, ...)
Nothing here proves behaviour on the real camera.
"""
import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RUNNER = os.path.join(HERE, "run_m50mk2.sh")

MARKER_RE = re.compile(r"\[ML\] ((?:pre|post|big)\.[0-9A-Za-z.]+):")
# Lines that merely announce that a handler was installed are not faults.
BENIGN_RE = re.compile(
    r"\[ML\] abort: (VBAR=|installed|vec\[)|installing CPU0 abort handler|"
    r"non-fatal assert handler installed")
FAULT_RE = re.compile(r"assert|abort|\[AB\]|exception|panic|undef(ined)? instr|pc=0x", re.I)
MODULE_RE = re.compile(r"module_task|ML/MODULES|module|\.mo\b|\.sym\b", re.I)
NOISE_RE = re.compile(r"rgb_vram_info was NULL")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def qemu_alive(pid):
    try:
        return os.path.basename(os.readlink("/proc/%d/exe" % pid)).startswith("qemu-system-arm")
    except OSError:
        return False


def kill_qemu(pid):
    """PID-based cleanup only; never signals anything that is not our qemu."""
    if not qemu_alive(pid):
        return
    os.kill(pid, signal.SIGTERM)
    for _ in range(25):
        if not qemu_alive(pid):
            return
        time.sleep(0.2)
    if qemu_alive(pid):
        os.kill(pid, signal.SIGKILL)


class LogTail:
    """Incrementally reads the UART log and time-stamps complete lines."""

    def __init__(self, path, t0):
        self.path, self.t0 = path, t0
        self.pos, self.partial = 0, b""
        self.lines = []          # (seconds since start, text)
        self.markers = []        # (seconds, marker) first occurrence only
        self._seen = set()

    def poll(self):
        try:
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                data = f.read()
        except OSError:
            return
        if not data:
            return
        self.pos += len(data)
        now = time.monotonic() - self.t0
        chunks = (self.partial + data).split(b"\n")
        self.partial = chunks.pop()
        for raw in chunks:
            text = ANSI_RE.sub("", raw.decode("latin-1")).rstrip("\r")
            self.lines.append((now, text))
            m = MARKER_RE.search(text)
            if m and m.group(1) not in self._seen:
                self._seen.add(m.group(1))
                self.markers.append((now, m.group(1)))

    def has(self, needle):
        return any(needle in text for _, text in self.lines)


class Shell:
    def __init__(self, port, char_delay):
        self.char_delay = char_delay
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(0.2)
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.stop = False
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        while not self.stop:
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                return
            with self.lock:
                self.buf += data

    def send_paced(self, text):
        for ch in text.encode("ascii"):
            self.sock.sendall(bytes([ch]))
            time.sleep(self.char_delay)

    def command(self, seq, cmd, timeout):
        """Returns dict(cmd, status, body[], raw, seconds)."""
        with self.lock:
            start = len(self.buf)
        t0 = time.monotonic()
        self.send_paced("@%d %s\n" % (seq, cmd))
        term = re.compile(r"=(ok|err) @%d\b(.*)" % seq)
        status, msg = "timeout", ""
        while time.monotonic() - t0 < timeout:
            with self.lock:
                chunk = bytes(self.buf[start:])
            m = term.search(chunk.decode("latin-1"))
            if m:
                status, msg = m.group(1), m.group(2).strip()
                break
            time.sleep(0.05)
        time.sleep(0.2)
        with self.lock:
            raw = ANSI_RE.sub("", bytes(self.buf[start:]).decode("latin-1"))
        body = []
        for line in raw.replace("\r", "").split("\n"):
            s = line.strip()
            if not s or NOISE_RE.search(s) or s.startswith("[ML] "):
                continue                      # async firmware log, not a reply
            if s.startswith("@%d " % seq) or s.endswith("@%d %s" % (seq, cmd)):
                continue                      # echo of our own line
            if term.search(s):
                continue
            if s in ("@%d %s" % (seq, cmd)) and len(s) < len(cmd) + 4:
                continue                      # echo fragment split by an async log line
            if re.match(r"\s*\d+:\s*\d+\.\d+ ", line):
                continue                      # Canon DebugMsg line
            body.append(s[4:] if s.startswith("ml> ") else s)
        return {"cmd": cmd, "status": status, "msg": msg, "body": body, "raw": raw,
                "seconds": round(time.monotonic() - t0, 2)}

    def close(self):
        self.stop = True
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sd", required=True, help="SD image (from mk_sd_image.sh)")
    ap.add_argument("--outdir", required=True, help="directory for uart.log, summary.*")
    ap.add_argument("--port", type=int, default=4487, help="TCP port for the UART (default 4487)")
    ap.add_argument("--marker", default="big.8", help="boot marker to wait for (default big.8)")
    ap.add_argument("--boot-timeout", type=float, default=240, help="seconds to wait for the marker")
    ap.add_argument("--char-delay", type=float, default=0.02, help="seconds between TX characters")
    ap.add_argument("--cmd-timeout", type=float, default=20, help="seconds to wait for each reply")
    ap.add_argument("--settle", type=float, default=3, help="seconds to keep logging after the marker")
    ap.add_argument("--commands", default="ver,help", help="comma-separated shell commands")
    ap.add_argument("--gdb", type=int, default=0, help="also open a gdbstub on this port")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    log = os.path.join(os.path.abspath(args.outdir), "uart.log")
    hard_limit = int(args.boot_timeout + args.settle + 3 * args.cmd_timeout + 60)
    cmd = [RUNNER, "--sd", args.sd, "--log", log, "--tcp", str(args.port),
           "--timeout", str(hard_limit), "--detach"]
    if args.gdb:
        cmd += ["--gdb", str(args.gdb)]
    t0 = time.monotonic()
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    sys.stderr.write(proc.stderr)
    m = re.search(r"QEMU_PID=(\d+)", proc.stdout)
    if proc.returncode != 0 or not m:
        print("HARNESS ERROR: run_m50mk2.sh failed (rc=%d)" % proc.returncode)
        return 3
    pid = int(m.group(1))

    result = {"sd": os.path.abspath(args.sd), "uart_log": log, "port": args.port,
              "marker": args.marker, "qemu_pid": pid}
    tail = LogTail(log, t0)
    replies, shell_err = [], ""
    try:
        deadline = t0 + args.boot_timeout
        while time.monotonic() < deadline and qemu_alive(pid):
            tail.poll()
            if tail.has(args.marker):
                break
            time.sleep(0.05)
        tail.poll()
        booted = tail.has(args.marker)
        boot_secs = next((t for t, text in tail.lines if args.marker in text), None)
        result.update(booted=booted, boot_seconds=boot_secs and round(boot_secs, 1),
                      qemu_alive_after_boot_wait=qemu_alive(pid))
        if booted:
            end = time.monotonic() + args.settle
            while time.monotonic() < end:
                tail.poll()
                time.sleep(0.05)

        if qemu_alive(pid):
            try:
                sh = Shell(args.port, args.char_delay)
                try:
                    sh.send_paced("\n")           # flush any half-typed line
                    time.sleep(0.5)
                    for i, c in enumerate([c for c in args.commands.split(",") if c], 1):
                        replies.append(sh.command(i, c.strip(), args.cmd_timeout))
                finally:
                    sh.close()
            except OSError as e:
                shell_err = "serial TCP connect/IO failed: %s" % e
        else:
            shell_err = "QEMU was no longer running"
        time.sleep(0.5)
        tail.poll()
    finally:
        kill_qemu(pid)
    total = time.monotonic() - t0

    lines = [text for _, text in tail.lines]
    quiet = [(t, text) for t, text in tail.lines if not NOISE_RE.search(text)]
    faults = [(t, text) for t, text in quiet if FAULT_RE.search(text) and not BENIGN_RE.search(text)]
    modules = [(t, text) for t, text in quiet if MODULE_RE.search(text)]
    shell_ok = bool(replies) and all(r["status"] == "ok" for r in replies)
    result.update(
        total_seconds=round(total, 1), uart_lines=len(lines),
        noise_lines_rgb_vram=len(lines) - len(quiet),
        markers=[{"t": round(t, 1), "marker": mk} for t, mk in tail.markers],
        last_marker=tail.markers[-1][1] if tail.markers else None,
        fault_lines=[{"t": round(t, 1), "line": text} for t, text in faults],
        module_lines=[{"t": round(t, 1), "line": text} for t, text in modules],
        module_loading_started=any(re.search(r"module_task|ML/MODULES", text, re.I) for _, text in modules),
        replies=replies, shell_ok=shell_ok, shell_error=shell_err)
    rc = 0 if (booted and shell_ok) else (2 if booted else 1)
    result["exit_code"] = rc

    out = []
    out.append("=== M50mk2 QEMU smoke test ===")
    out.append("sd image      : %s" % result["sd"])
    out.append("uart log      : %s (%d lines, %d 'rgb_vram_info was NULL' noise lines)"
               % (log, len(lines), len(lines) - len(quiet)))
    out.append("boot marker   : %r %s" % (args.marker,
               "SEEN after %.1f s wall" % boot_secs if booted else
               "NOT seen within %.0f s (last marker: %s)" % (args.boot_timeout, result["last_marker"])))
    out.append("markers (wall seconds since QEMU start, first occurrence):")
    out.append("  " + "  ".join("%s@%.1f" % (mk, t) for t, mk in tail.markers))
    out.append("assert/abort/fault lines: %d" % len(faults))
    out += ["  [%6.1fs] %s" % (t, text[:200]) for t, text in faults]
    out.append("module lines: %d (module loading started: %s)"
               % (len(modules), "YES" if result["module_loading_started"] else "NO"))
    out += ["  [%6.1fs] %s" % (t, text[:200]) for t, text in modules]
    if shell_err:
        out.append("shell         : %s" % shell_err)
    for r in replies:
        out.append("shell '%s' -> %s%s (%.2f s)" % (r["cmd"], r["status"].upper(),
                   " " + r["msg"] if r["msg"] else "", r["seconds"]))
        out += ["    | " + b for b in r["body"]]
    out.append("total run time: %.1f s" % total)
    out.append("RESULT        : %s (exit %d)" % ("PASS" if rc == 0 else "FAIL", rc))
    text = "\n".join(out) + "\n"
    sys.stdout.write(text)
    with open(os.path.join(args.outdir, "summary.txt"), "w") as f:
        f.write(text)
    with open(os.path.join(args.outdir, "summary.json"), "w") as f:
        json.dump(result, f, indent=1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
