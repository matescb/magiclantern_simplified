#!/usr/bin/env python3
"""mlserial - CLI for the M50 II serial harness.

Front-end to the mlserald daemon. This is what gets invoked from the shell to
drive the camera: send commands, pull memory dumps, inject keys, scan EDMAC,
tail/analyze the structured log, and symbolicate addresses -- all without the
operator touching a terminal.

Examples:
  mlserial daemon start           # spawn the port-owning daemon (once per session)
  mlserial ping                   # ver round-trip -> proves the link is live
  mlserial cmd "md 0x1000 8"      # arbitrary command
  mlserial key up                 # inject a GUI keycode by name
  mlserial dump 0xd0420000 0x400 edmac.bin
  mlserial edmac-scan             # find live raw-LV EDMAC channels
  mlserial watch 0xd0420200 200 30
  mlserial log --follow           # structured UART log
  mlserial symbolize 0x000d7a41   # ML address -> nearest symbol
"""

import argparse
import hashlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
RUN_DIR = os.environ.get("MLSERIAL_RUN", os.path.join(HERE, "run"))
# must match mlserald.py's derivation exactly
SOCK_PATH = os.path.join(tempfile.gettempdir(), "mlserial-%s.sock" % hashlib.md5(RUN_DIR.encode()).hexdigest()[:10])
LOG_PATH = os.path.join(RUN_DIR, "uart.log")
PID_PATH = os.path.join(RUN_DIR, "mlserald.pid")
DAEMON_LOG = os.path.join(RUN_DIR, "mlserald.out")
SYM_PATH = os.environ.get(
    "MLSERIAL_SYM",
    os.path.join(HERE, "..", "..", "magiclantern_simplified", "platform", "M50mk2.101", "build", "magiclantern.sym"),
)

# named keys -> keycodes (from src/gui.c M50II_KEY_*)
KEYS = {
    "up": 0x40, "down": 0x42, "left": 0x3E, "right": 0x3C,
    "set": 0x0A, "q": 0x0A, "menu": 0x0C, "info": 0x0E,
    "fn": 0x22, "view": 0x12, "play": 0x12, "dial": 0xB7,
}


def die(msg, code=1):
    sys.stderr.write(msg + "\n")
    sys.exit(code)


def req(obj, timeout=65):
    """Send one JSON request to the daemon, return the parsed reply."""
    if not os.path.exists(SOCK_PATH):
        die("daemon not running (no socket). start it: mlserial daemon start", 4)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(SOCK_PATH)
    except Exception as e:
        die("cannot connect to daemon: %s" % e, 4)
    s.sendall((json.dumps(obj) + "\n").encode("utf-8"))
    data = b""
    while b"\n" not in data:
        c = s.recv(65536)
        if not c:
            break
        data += c
    s.close()
    if not data:
        die("empty reply from daemon", 4)
    return json.loads(data.split(b"\n", 1)[0].decode("utf-8"))


def cmd(text, timeout=5):
    return req({"op": "cmd", "text": text, "timeout": timeout})


def print_result(res):
    for line in res.get("body", []):
        print(line)
    st = res.get("status")
    if st != "ok":
        sys.stderr.write("[%s] %s\n" % (st, res.get("msg", "")))
    return 0 if st == "ok" else (2 if st == "timeout" else 1)


# ---- daemon lifecycle ----
def daemon_start():
    if os.path.exists(SOCK_PATH):
        r = req({"op": "ping"}, timeout=3)
        if r.get("status") == "ok":
            print("daemon already running")
            return 0
    os.makedirs(RUN_DIR, exist_ok=True)
    out = open(DAEMON_LOG, "a")
    p = subprocess.Popen([sys.executable, os.path.join(HERE, "mlserald.py")],
                         stdout=out, stderr=out, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    with open(PID_PATH, "w") as f:
        f.write(str(p.pid))
    for _ in range(50):
        if os.path.exists(SOCK_PATH):
            r = req({"op": "ping"}, timeout=3)
            if r.get("status") == "ok":
                print("daemon started pid=%d" % p.pid)
                return 0
        time.sleep(0.1)
    print("daemon spawned pid=%d but socket not ready; see %s" % (p.pid, DAEMON_LOG))
    return 1


def daemon_stop():
    if os.path.exists(PID_PATH):
        try:
            pid = int(open(PID_PATH).read().strip())
            os.kill(pid, 15)
            print("stopped pid=%d" % pid)
        except Exception as e:
            print("stop: %s" % e)
    if os.path.exists(SOCK_PATH):
        try:
            os.unlink(SOCK_PATH)
        except Exception:
            pass
    return 0


def daemon_status():
    if not os.path.exists(SOCK_PATH):
        print("daemon: DOWN (no socket)")
        return 1
    r = req({"op": "status"}, timeout=3)
    print(json.dumps(r, indent=2))
    return 0 if r.get("connected") else 1


# ---- dump (with CRC verify) ----
def do_dump(addr, length, outfile, timeout):
    res = cmd("dump %s %s" % (hexs(addr), hexs(length)), timeout=timeout)
    if res.get("status") != "ok":
        return print_result(res)
    data = bytearray()
    crc_expected = None
    for line in res["body"]:
        if line.startswith("D "):
            parts = line.split()
            if len(parts) >= 3:
                data += bytes.fromhex(parts[2])
        elif line.startswith("=crc"):
            crc_expected = int(line.split()[1], 16)
    crc_actual = zlib.crc32(bytes(data)) & 0xFFFFFFFF
    ok = (crc_expected is not None and crc_actual == crc_expected)
    with open(outfile, "wb") as f:
        f.write(bytes(data))
    print("dumped %d bytes -> %s  crc=0x%08X expected=%s  %s" % (
        len(data), outfile, crc_actual,
        ("0x%08X" % crc_expected) if crc_expected is not None else "None",
        "OK" if ok else "CRC MISMATCH"))
    return 0 if ok else 1


# ---- edmac scan (host-side heuristic over md) ----
def looks_like_ram_ptr(v):
    if v in (0, 0xFFFFFFFF):
        return False
    return (0x00001000 <= v <= 0x3FFFFFFF) or (0x40001000 <= v <= 0x4FFFFFFF)


def do_edmac_scan(base, count, stride, timeout):
    print("scanning %d MMIO words from %s stride %s for live RAM pointers..." % (count, hexs(base), hexs(stride)))
    hits = 0
    row_words = max(1, stride // 4)
    addr = base
    for _ in range(count):
        res = cmd("md %s %d" % (hexs(addr), row_words), timeout=timeout)
        if res.get("status") == "ok":
            for line in res["body"]:
                if ":" not in line:
                    continue
                a_str, rest = line.split(":", 1)
                try:
                    a = int(a_str, 16)
                except ValueError:
                    continue
                for i, tok in enumerate(rest.split()):
                    try:
                        v = int(tok, 16)
                    except ValueError:
                        continue
                    if looks_like_ram_ptr(v):
                        print("  %s = 0x%08X  <- candidate RAM ptr" % (hexs(a + i * 4), v))
                        hits += 1
        addr += stride
    print("scan done, %d candidate(s)" % hits)
    return 0


# ---- symbolize ----
def load_syms():
    syms = []
    try:
        with open(SYM_PATH) as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    try:
                        syms.append((int(parts[0], 16), parts[1]))
                    except ValueError:
                        pass
    except FileNotFoundError:
        die("sym file not found: %s (set MLSERIAL_SYM)" % SYM_PATH, 3)
    syms.sort()
    return syms


def sym_for(syms, addr):
    if addr >= 0xE0000000:
        return "(ROM/Canon 0x%08X -- use Ghidra)" % addr
    lo, hi, best = 0, len(syms) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            best = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    if best is None:
        return "(no symbol)"
    off = addr - best[0]
    return "%s+0x%X" % (best[1], off) if off else best[1]


def do_symbolize(args):
    syms = load_syms()
    if args:
        for a in args:
            try:
                addr = int(a, 16)
            except ValueError:
                print("%s: not hex" % a)
                continue
            print("0x%08X  %s" % (addr, sym_for(syms, addr)))
    else:
        import re
        pat = re.compile(r"0x[0-9A-Fa-f]{6,8}|[0-9A-Fa-f]{8}")
        for line in sys.stdin:
            line = line.rstrip("\n")
            out = line
            for m in set(pat.findall(line)):
                try:
                    addr = int(m, 16)
                except ValueError:
                    continue
                if addr >= 0x1000:
                    out += "   ; %s -> %s" % (m, sym_for(syms, addr))
            print(out)
    return 0


def hexs(v):
    return v if isinstance(v, str) else ("0x%X" % v)


def parse_num(s):
    return int(s, 16) if s.lower().startswith("0x") else int(s)


def do_log(args):
    if not os.path.exists(LOG_PATH):
        die("no log yet: %s" % LOG_PATH, 1)
    if args.follow:
        with open(LOG_PATH) as f:
            f.seek(0, 2)
            try:
                while True:
                    line = f.readline()
                    if not line:
                        time.sleep(0.2); continue
                    if args.grep and args.grep not in line:
                        continue
                    sys.stdout.write(line); sys.stdout.flush()
            except KeyboardInterrupt:
                return 0
    else:
        lines = open(LOG_PATH).read().splitlines()
        if args.grep:
            lines = [l for l in lines if args.grep in l]
        for l in lines[-args.n:]:
            print(l)
    return 0


def main():
    ap = argparse.ArgumentParser(prog="mlserial", description="M50 II serial harness CLI")
    sub = ap.add_subparsers(dest="op")

    p = sub.add_parser("daemon"); p.add_argument("action", choices=["start", "stop", "status", "restart"])
    sub.add_parser("ping")
    p = sub.add_parser("cmd"); p.add_argument("text"); p.add_argument("--timeout", type=float, default=5)
    p = sub.add_parser("raw"); p.add_argument("text")
    p = sub.add_parser("key"); p.add_argument("name")
    p = sub.add_parser("menu"); p.add_argument("action", choices=["open", "close", "toggle"])
    p = sub.add_parser("dump"); p.add_argument("addr"); p.add_argument("len"); p.add_argument("out"); p.add_argument("--timeout", type=float, default=120)
    p = sub.add_parser("edmac-scan"); p.add_argument("base", nargs="?", default="0xd0420000"); p.add_argument("count", nargs="?", type=int, default=0x20); p.add_argument("stride", nargs="?", default="0x100"); p.add_argument("--timeout", type=float, default=5)
    p = sub.add_parser("watch"); p.add_argument("addr"); p.add_argument("ms", nargs="?", default="200"); p.add_argument("n", nargs="?", default="20"); p.add_argument("--timeout", type=float, default=0)
    p = sub.add_parser("log"); p.add_argument("--follow", "-f", action="store_true"); p.add_argument("--n", type=int, default=40); p.add_argument("--grep", default=None)
    p = sub.add_parser("symbolize"); p.add_argument("addr", nargs="*")

    args = ap.parse_args()

    if args.op == "daemon":
        if args.action == "start":
            return daemon_start()
        if args.action == "stop":
            return daemon_stop()
        if args.action == "restart":
            daemon_stop(); time.sleep(0.5); return daemon_start()
        return daemon_status()

    if args.op == "ping":
        return print_result(cmd("ver", timeout=4))

    if args.op == "cmd":
        return print_result(cmd(args.text, timeout=args.timeout))

    if args.op == "raw":
        print(json.dumps(req({"op": "raw", "text": args.text})))
        return 0

    if args.op == "key":
        code = KEYS.get(args.name.lower())
        if code is None:
            code = parse_num(args.name)
        return print_result(cmd("key 0x%X" % code, timeout=4))

    if args.op == "menu":
        return print_result(cmd("key 0x22", timeout=4))  # FN toggles ML menu

    if args.op == "dump":
        return do_dump(parse_num(args.addr), parse_num(args.len), args.out, args.timeout)

    if args.op == "edmac-scan":
        return do_edmac_scan(parse_num(args.base), args.count, parse_num(args.stride), args.timeout)

    if args.op == "watch":
        n = parse_num(args.n); ms = parse_num(args.ms)
        to = args.timeout if args.timeout > 0 else (n * ms / 1000.0 + 5)
        return print_result(cmd("watch %s %d %d" % (hexs(parse_num(args.addr)), ms, n), timeout=to))

    if args.op == "log":
        return do_log(args)

    if args.op == "symbolize":
        return do_symbolize(args.addr)

    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
