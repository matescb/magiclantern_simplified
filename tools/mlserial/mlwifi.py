#!/usr/bin/env python3
"""mlwifi - push an ML build to the camera over WiFi (host = TCP server).

The camera is the TCP CLIENT: after boot it dials back to this host and we stream
the freshly-built firmware down the wire. This replaces the card-shuffle (flash.sh)
for the autoexec.bin dev loop -- edit, build, and the next connect gets the new bits.

The camera drives the connection; we just serve. On each connect we validate the
"MLWF" hello, send a session header + one record per file, read the camera's per-file
ACK, and report status + CRC match. With --once we exit 0 after one clean deploy.

Wire protocol (all app-level u32s are little-endian, struct '<I'):
  1. camera -> host: 4 bytes  b"MLWF"                 (hello; anything else = close)
  2. host -> camera: 4 bytes  b"MLWF"  +  u32 file_count
  3. per file, host -> camera:
       u32 name_len ; name[name_len] (ASCII on-card path, no drive letter)
       u32 file_len ; u32 crc32 (zlib.crc32 & 0xFFFFFFFF) ; file_len bytes
  4. per file, camera -> host: u32 status (0=OK) ; u32 recv_crc
  5. after the last ack the camera closes the socket (then may reboot).

Examples:
  mlwifi.py serve                       # serve default autoexec.bin, keep serving
  mlwifi.py serve --once                # exit 0 after one clean deploy (used by flash-wifi.sh)
  mlwifi.py serve --file patch.bin:AUTOEXEC.BIN --once
  mlwifi.py crc autoexec.bin            # size + crc32, for cross-checking
"""

import argparse
import os
import socket
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BUILD_DIR = os.environ.get(
    "MLWIFI_BUILD_DIR",
    os.path.join(HERE, "..", "..", "magiclantern_simplified", "platform", "M50mk2.101", "build"),
)
DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 5455
DEFAULT_CARD_NAME = "AUTOEXEC.BIN"     # on-card target; camera prepends "B:/"

HELLO = b"MLWF"                        # literal bytes for BOTH hello and header magic
CONN_TIMEOUT_S = 30                    # per-connection recv timeout (wedged camera guard)
DEFAULT_WAIT_S = 180                   # --once: how long to wait for the camera to connect
PFX = "[mlwifi]"


def log(msg):
    print("%s %s" % (PFX, msg), flush=True)


def err(msg):
    sys.stderr.write("%s %s\n" % (PFX, msg))
    sys.stderr.flush()


# --- file helpers -----------------------------------------------------------

def crc_of(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def ip_to_dec(dotted):
    """Dotted IPv4 -> the decimal uint32 the camera's WIFI.CFG parser expects.

    For a.b.c.d this is  a | b<<8 | c<<16 | d<<24  (the four octets read
    little-endian), which is exactly what DryOS wants in sockaddr_in.sin_addr on
    the LE ARM core. E.g. 192.168.1.102 -> 1711384768 (matches the yolo example).
    """
    parts = dotted.split(".")
    if len(parts) != 4:
        raise ValueError("bad IPv4: %r" % dotted)
    a, b, c, d = (int(p) for p in parts)
    for v in (a, b, c, d):
        if v < 0 or v > 255:
            raise ValueError("bad IPv4 octet in %r" % dotted)
    return a | (b << 8) | (c << 16) | (d << 24)


def find_default_autoexec(build_dir):
    """Locate autoexec.bin in the build dir, case-insensitively. Returns path or None."""
    if not os.path.isdir(build_dir):
        return None
    for entry in os.listdir(build_dir):
        if entry.lower() == "autoexec.bin":
            return os.path.join(build_dir, entry)
    return None


def parse_file_arg(spec):
    """'HOSTPATH:CARDNAME' -> (hostpath, cardname). No colon -> cardname = basename upper."""
    if ":" in spec:
        hostpath, cardname = spec.rsplit(":", 1)
    else:
        hostpath, cardname = spec, ""
    if not cardname:
        cardname = os.path.basename(hostpath).upper()
    return hostpath, cardname


def resolve_files(args):
    """Build the [(cardname, bytes, crc32)] deploy list. --file entries override the default."""
    specs = []
    if args.file:
        for spec in args.file:
            specs.append(parse_file_arg(spec))
    else:
        auto = find_default_autoexec(args.build_dir)
        if not auto:
            raise FileNotFoundError(
                "no autoexec.bin in %s (build first, or pass --file HOST:CARD)" % args.build_dir)
        specs.append((auto, DEFAULT_CARD_NAME))

    files = []
    for hostpath, cardname in specs:
        if not os.path.isfile(hostpath):
            raise FileNotFoundError("file not found: %s" % hostpath)
        with open(hostpath, "rb") as f:
            data = f.read()
        files.append((cardname, data, crc_of(data)))
        log("file %-16s <- %s  (%d bytes, crc=0x%08X)" % (cardname, hostpath, len(data), crc_of(data)))
    return files


# --- socket helpers ---------------------------------------------------------

def recv_exact(sock, n):
    """Loop recv until exactly n bytes, or raise on EOF/short read."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed after %d/%d bytes" % (len(buf), n))
        buf += chunk
    return bytes(buf)


def send_file_record(conn, cardname, data, crc):
    name_b = cardname.encode("ascii")
    conn.sendall(struct.pack("<I", len(name_b)))
    conn.sendall(name_b)
    conn.sendall(struct.pack("<I", len(data)))
    conn.sendall(struct.pack("<I", crc))
    conn.sendall(data)


def handle_connection(conn, peer, files):
    """Run the full protocol against one camera. Returns True only on a clean deploy.

    Returns None for a non-camera connection (bad hello) so the caller can keep waiting
    without treating a portscan as a failed deploy.
    """
    conn.settimeout(CONN_TIMEOUT_S)
    hello = recv_exact(conn, 4)
    if hello != HELLO:
        err("bad hello from %s: %r (not a camera; closing)" % (peer, hello))
        return None

    log("camera %s: hello OK, sending %d file(s)" % (peer, len(files)))
    conn.sendall(HELLO)
    conn.sendall(struct.pack("<I", len(files)))

    all_ok = True
    for cardname, data, crc in files:
        send_file_record(conn, cardname, data, crc)
        status, recv_crc = struct.unpack("<II", recv_exact(conn, 8))
        crc_ok = (recv_crc == crc)
        ok = (status == 0 and crc_ok)
        all_ok = all_ok and ok
        log("  %-16s %d bytes sent  status=%d  crc=%s (sent 0x%08X / got 0x%08X)  %s" % (
            cardname, len(data), status,
            "MATCH" if crc_ok else "MISMATCH", crc, recv_crc,
            "OK" if ok else "FAIL"))
    log("camera %s: deploy %s" % (peer, "SUCCESS" if all_ok else "FAILED"))
    return all_ok


def serve(files, host, port, once, wait_s):
    """Bind, accept, and deploy. In --once mode, exit after the first real deploy attempt."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    log("serving on %s:%d  (%d file(s), %s)%s" % (
        host, port, len(files),
        "--once" if once else "persistent",
        ("  waiting up to %ds" % wait_s) if (once and wait_s) else ""))

    if once and wait_s:
        srv.settimeout(wait_s)

    try:
        while True:
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                err("no camera connected within %ds" % wait_s)
                return 2
            peer = "%s:%d" % addr
            try:
                result = handle_connection(conn, peer, files)
            except (ConnectionError, OSError, struct.error) as e:
                err("camera %s: connection error: %s" % (peer, e))
                result = False if once else None
            finally:
                try:
                    conn.close()
                except OSError:
                    pass
            if once and result is not None:
                return 0 if result else 1
            # persistent mode, or a stray non-camera connection: keep serving
    except KeyboardInterrupt:
        log("interrupted; stopping")
        return 0
    finally:
        srv.close()


# --- commands ---------------------------------------------------------------

def cmd_serve(args):
    try:
        files = resolve_files(args)
    except (FileNotFoundError, OSError) as e:
        err(str(e))
        return 3
    return serve(files, args.host, args.port, args.once, args.timeout)


def cmd_crc(args):
    if not os.path.isfile(args.path):
        err("file not found: %s" % args.path)
        return 3
    with open(args.path, "rb") as f:
        data = f.read()
    print("%s  size=%d  crc32=0x%08X" % (args.path, len(data), crc_of(data)))
    return 0


def cmd_mkcfg(args):
    """Write a WIFI.CFG: NUL-separated fields, decimal-uint32 IPs (camera parser format)."""
    try:
        fields = [
            args.ssid,
            args.pass_,
            str(ip_to_dec(args.client)),
            str(ip_to_dec(args.server)),
            str(ip_to_dec(args.gateway)),
            str(args.port),
        ]
    except ValueError as e:
        err(str(e))
        return 3
    blob = b"".join(f.encode("ascii") + b"\x00" for f in fields)
    with open(args.out, "wb") as f:
        f.write(blob)
    log("wrote %s (%d bytes)" % (args.out, len(blob)))
    log("  ssid=%s client=%s server=%s gw=%s port=%d" % (
        args.ssid, args.client, args.server, args.gateway, args.port))
    log("copy it to the card as ML/SETTINGS/WIFI.CFG")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mlwifi", description="push an ML build to the camera over WiFi")
    sub = ap.add_subparsers(dest="op", required=True)

    p = sub.add_parser("serve", help="serve the build to the camera (camera dials in)")
    p.add_argument("--host", default=DEFAULT_HOST, help="bind address (default %s)" % DEFAULT_HOST)
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help="TCP port (default %d)" % DEFAULT_PORT)
    p.add_argument("--file", action="append", metavar="HOSTPATH:CARDNAME",
                   help="file to deploy (repeatable); overrides the default autoexec.bin")
    p.add_argument("--once", action="store_true", help="exit 0 after one clean deploy, non-zero otherwise")
    p.add_argument("--timeout", type=int, default=DEFAULT_WAIT_S,
                   help="--once: seconds to wait for the camera (0 = forever, default %d)" % DEFAULT_WAIT_S)
    p.add_argument("--build-dir", default=DEFAULT_BUILD_DIR, help="where to find autoexec.bin")

    p = sub.add_parser("crc", help="print size + crc32 of a file")
    p.add_argument("path")

    p = sub.add_parser("mkcfg", help="write a WIFI.CFG (NUL-separated, decimal IPs) for the card")
    p.add_argument("--ssid", required=True, help="2.4GHz WPA2 network name")
    p.add_argument("--pass", dest="pass_", required=True, help="WPA2 passphrase")
    p.add_argument("--client", required=True, help="static IP the camera takes, e.g. 192.168.1.90")
    p.add_argument("--server", required=True, help="this host's LAN IP, e.g. 192.168.1.10")
    p.add_argument("--gateway", required=True, help="router IP, e.g. 192.168.1.1")
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help="TCP port (default %d)" % DEFAULT_PORT)
    p.add_argument("--out", default="WIFI.CFG", help="output path (default ./WIFI.CFG)")

    args = ap.parse_args(argv)
    if args.op == "serve":
        return cmd_serve(args)
    if args.op == "crc":
        return cmd_crc(args)
    if args.op == "mkcfg":
        return cmd_mkcfg(args)
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
