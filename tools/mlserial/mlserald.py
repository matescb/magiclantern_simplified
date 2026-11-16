#!/usr/bin/env python3
"""mlserald - M50 II serial harness daemon (host side).

Owns /dev/ttyUSB0 exclusively so nothing else races the port, continuously logs
every UART line (timestamped .log + structured .jsonl), and exposes a Unix-socket
control channel so `mlserial` (the CLI) can issue commands and get correlated
replies. The operator cold-boots the camera once; from then on all interaction
goes through this daemon -- no hand-typing at a terminal.

Wire protocol with the camera (see PROTOCOL.md):
  - a command is sent as  "@<seq> <text>\n"
  - the camera echoes it, prints body lines, then terminates with
    "=ok @<seq>" or "=err @<seq> <msg>"
  - asynchronous firmware logs keep the "[ML] ..." prefix and are routed to the
    log only, never into a command's response body.

Control socket (newline-delimited JSON), one request per connection:
  {"op":"cmd","text":"md 0x1000 4","timeout":5}  -> {"status":"ok|err|timeout","body":[...],"seq":N}
  {"op":"raw","text":"..."}                        -> sends raw bytes + "\n", no framing
  {"op":"status"}                                   -> daemon/serial status
  {"op":"ping"}                                     -> {"status":"ok"}
"""

import hashlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

try:
    import serial  # pyserial
except ImportError:
    sys.stderr.write("pyserial not installed (pip install pyserial)\n")
    sys.exit(3)

RUN_DIR = os.environ.get("MLSERIAL_RUN", os.path.join(os.path.dirname(os.path.abspath(__file__)), "run"))
DEV = os.environ.get("MLSERIAL_DEV", "/dev/ttyUSB0")
BAUD = int(os.environ.get("MLSERIAL_BAUD", "115200"))
# keep the socket short (AF_UNIX paths cap ~108 chars) regardless of RUN_DIR depth
SOCK_PATH = os.path.join(tempfile.gettempdir(), "mlserial-%s.sock" % hashlib.md5(RUN_DIR.encode()).hexdigest()[:10])
LOG_PATH = os.path.join(RUN_DIR, "uart.log")
JSONL_PATH = os.path.join(RUN_DIR, "uart.jsonl")
CMD_GAP = 0.02  # small settle after writing a command
# The camera's serial_shell polls RX with no FIFO and an msleep between reads, so a
# fast burst overruns the single RX data register. Pace TX at >1 poll interval/byte.
PACE_S = float(os.environ.get("MLSERIAL_PACE", "0.02"))


def now():
    return time.time()


def stamp(t):
    lt = time.localtime(t)
    return "%02d:%02d:%02d.%03d" % (lt.tm_hour, lt.tm_min, lt.tm_sec, int((t % 1) * 1000))


def classify(text):
    """Tag a received line by kind so the log is analyzable."""
    if text.startswith("[ML]"):
        rest = text[4:].strip()
        tag = rest.split(" ", 1)[0].rstrip(":") if rest else ""
        return "log", tag
    if text.startswith("=ok") or text.startswith("=err"):
        return "result", ""
    if text.startswith("=crc"):
        return "data", "crc"
    if text and text[0] in "DWE" and (len(text) > 1 and text[1] == " "):
        return "data", text[0]
    return "other", ""


class Daemon:
    def __init__(self):
        os.makedirs(RUN_DIR, exist_ok=True)
        self.ser = None
        self.lock = threading.Lock()        # serialize command execution
        self.state_lock = threading.Lock()
        self.seq = 0
        self.active = None                  # {"seq","body","event","status","msg"}
        self.logf = open(LOG_PATH, "a", buffering=1)
        self.jsonlf = open(JSONL_PATH, "a", buffering=1)
        self.connected = False
        self.rx_lines = 0

    # ---- logging ----
    def _log(self, direction, text, kind="", tag=""):
        t = now()
        self.logf.write("[%s] %s%s\n" % (stamp(t), (">> " if direction == "tx" else ""), text))
        rec = {"ts": round(t, 3), "dir": direction, "kind": kind, "tag": tag, "text": text}
        self.jsonlf.write(json.dumps(rec) + "\n")

    def session_marker(self, msg):
        self._log("sys", "==== %s %s ====" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg), "sys")

    # ---- serial open (self-healing perms + reopen loop) ----
    def _open(self):
        try:
            subprocess.run(["sudo", "-n", "chmod", "666", DEV],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
        except Exception:
            pass
        return serial.Serial(DEV, BAUD, timeout=0.2)

    def reader(self):
        buf = b""
        while True:
            if self.ser is None:
                try:
                    self.ser = self._open()
                    self.connected = True
                    self.session_marker("serial open %s @%d" % (DEV, BAUD))
                except Exception:
                    self.connected = False
                    time.sleep(0.5)
                    continue
            try:
                chunk = self.ser.read(4096)
            except Exception:
                self.connected = False
                try:
                    self.ser.close()
                except Exception:
                    pass
                self.ser = None
                self.session_marker("serial lost -- reopening")
                time.sleep(0.5)
                buf = b""
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.replace(b"\r", b"").decode("latin-1", "replace")
                self._on_line(line)

    def _on_line(self, line):
        self.rx_lines += 1
        kind, tag = classify(line)
        self._log("rx", line, kind, tag)

        with self.state_lock:
            act = self.active
        if not act:
            return

        # strip prompt bleed ("ml> " can prepend a line since it has no newline)
        s = line
        while s.startswith("ml> "):
            s = s[4:]
        if s == "ml>":
            return

        if s.startswith("=ok") or s.startswith("=err"):
            status = "ok" if s.startswith("=ok") else "err"
            msg = ""
            if status == "err":
                rest = s[len("=err"):].strip()
                if rest.startswith("@"):
                    sp = rest.split(" ", 1)
                    msg = sp[1] if len(sp) > 1 else ""
                else:
                    msg = rest
            with self.state_lock:
                if self.active:
                    self.active["status"] = status
                    self.active["msg"] = msg
                    self.active["event"].set()
            return

        if s.startswith("[ML]") or s.startswith("@"):
            return  # async log / command echo -> not part of the body
        act["body"].append(s)

    # write byte-by-byte so the camera's no-FIFO polled RX never overruns
    def _write_paced(self, data):
        for b in data:
            self.ser.write(bytes([b]))
            self.ser.flush()
            if PACE_S:
                time.sleep(PACE_S)

    # ---- command execution ----
    def run_command(self, text, timeout):
        if self.ser is None or not self.connected:
            return {"status": "err", "msg": "serial not connected", "body": [], "seq": None}
        with self.lock:
            with self.state_lock:
                self.seq += 1
                seq = self.seq
                ev = threading.Event()
                self.active = {"seq": seq, "body": [], "event": ev, "status": None, "msg": ""}
            payload = ("@%d %s\n" % (seq, text)).encode("latin-1", "replace")
            self._log("tx", "@%d %s" % (seq, text), "cmd")
            try:
                self._write_paced(payload)
            except Exception as e:
                with self.state_lock:
                    self.active = None
                return {"status": "err", "msg": "write failed: %s" % e, "body": [], "seq": seq}
            time.sleep(CMD_GAP)
            got = ev.wait(timeout)
            with self.state_lock:
                act = self.active
                self.active = None
            if not got:
                return {"status": "timeout", "msg": "no terminator in %ss" % timeout,
                        "body": act["body"] if act else [], "seq": seq}
            return {"status": act["status"], "msg": act["msg"], "body": act["body"], "seq": seq}

    def send_raw(self, text):
        if self.ser is None:
            return {"status": "err", "msg": "serial not connected"}
        try:
            self._write_paced((text + "\n").encode("latin-1", "replace"))
            self._log("tx", text, "raw")
            return {"status": "ok"}
        except Exception as e:
            return {"status": "err", "msg": str(e)}

    # ---- control socket ----
    def serve(self):
        if os.path.exists(SOCK_PATH):
            os.unlink(SOCK_PATH)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(SOCK_PATH)
        srv.listen(8)
        self.session_marker("daemon up, socket %s" % SOCK_PATH)
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, conn):
        try:
            data = b""
            conn.settimeout(60)
            while b"\n" not in data:
                c = conn.recv(65536)
                if not c:
                    return
                data += c
            req = json.loads(data.split(b"\n", 1)[0].decode("utf-8"))
            op = req.get("op")
            if op == "cmd":
                res = self.run_command(req.get("text", ""), float(req.get("timeout", 5)))
            elif op == "raw":
                res = self.send_raw(req.get("text", ""))
            elif op == "status":
                res = {"status": "ok", "connected": self.connected, "dev": DEV,
                       "baud": BAUD, "rx_lines": self.rx_lines, "log": LOG_PATH, "jsonl": JSONL_PATH}
            elif op == "ping":
                res = {"status": "ok"}
            else:
                res = {"status": "err", "msg": "unknown op"}
            conn.sendall((json.dumps(res) + "\n").encode("utf-8"))
        except Exception as e:
            try:
                conn.sendall((json.dumps({"status": "err", "msg": str(e)}) + "\n").encode("utf-8"))
            except Exception:
                pass
        finally:
            conn.close()

    def run(self):
        threading.Thread(target=self.reader, daemon=True).start()
        self.serve()


if __name__ == "__main__":
    Daemon().run()
