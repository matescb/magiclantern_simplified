#!/usr/bin/env python3
"""test_mlwifi - loopback self-test for the mlwifi host framing.

We can't touch the camera, so we prove the host side of the protocol end-to-end by
launching `mlwifi.py serve --once` as a subprocess and driving it with a tiny in-process
"fake camera": connect, send b"MLWF", read the header + file records, verify the received
bytes' crc32 matches the sent crc32, then ack. If the host framing is correct, mlwifi
exits 0 on a clean deploy and non-zero when we lie about the crc or never speak MLWF.

Run:  python3 test_mlwifi.py      (no pytest needed; prints PASS/FAIL, exits non-zero on failure)
"""

import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
MLWIFI = os.path.join(HERE, "mlwifi.py")
HELLO = b"MLWF"


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_server(port, hostpath, cardname="TEST.BIN", extra=None):
    cmd = [sys.executable, MLWIFI, "serve", "--once",
           "--host", "127.0.0.1", "--port", str(port),
           "--file", "%s:%s" % (hostpath, cardname),
           "--timeout", "10"]
    if extra:
        cmd += extra
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def connect_retry(port, tries=100, delay=0.05):
    """Connect once the server's accept() is up; that connection is our fake camera."""
    last = None
    for _ in range(tries):
        try:
            c = socket.create_connection(("127.0.0.1", port), timeout=5)
            return c
        except OSError as e:
            last = e
            time.sleep(delay)
    raise last


def read_header_and_records(conn):
    """Read the host header + all file records. Returns list of (name, data, sent_crc)."""
    magic = _recv(conn, 4)
    assert magic == HELLO, "bad header magic: %r" % magic
    (count,) = struct.unpack("<I", _recv(conn, 4))
    files = []
    for _ in range(count):
        (name_len,) = struct.unpack("<I", _recv(conn, 4))
        name = _recv(conn, name_len).decode("ascii")
        (file_len,) = struct.unpack("<I", _recv(conn, 4))
        (sent_crc,) = struct.unpack("<I", _recv(conn, 4))
        data = _recv(conn, file_len)
        files.append((name, data, sent_crc))
    return files


def _recv(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed after %d/%d" % (len(buf), n))
        buf += chunk
    return bytes(buf)


# --- test cases -------------------------------------------------------------

def test_happy_path(tmpdir):
    """A well-behaved camera: correct crc ack -> mlwifi --once exits 0."""
    payload = os.urandom(200000)  # bigger than one recv; exercises the loop
    path = os.path.join(tmpdir, "fw.bin")
    with open(path, "wb") as f:
        f.write(payload)
    expected_crc = zlib.crc32(payload) & 0xFFFFFFFF

    port = free_port()
    proc = start_server(port, path, "AUTOEXEC.BIN")
    conn = connect_retry(port)
    conn.sendall(HELLO)
    files = read_header_and_records(conn)
    assert len(files) == 1, "expected 1 file, got %d" % len(files)
    name, data, sent_crc = files[0]
    assert name == "AUTOEXEC.BIN", "wrong card name: %r" % name
    assert data == payload, "payload mismatch (%d vs %d bytes)" % (len(data), len(payload))
    got_crc = zlib.crc32(data) & 0xFFFFFFFF
    assert got_crc == sent_crc == expected_crc, "crc mismatch"
    # ack: status=0, echo the crc we computed over what we received
    conn.sendall(struct.pack("<II", 0, got_crc))
    conn.close()
    rc = proc.wait(timeout=10)
    out = proc.stdout.read()
    assert rc == 0, "expected exit 0, got %d\n%s" % (rc, out)
    return out


def test_crc_mismatch(tmpdir):
    """Camera acks status=0 but a wrong crc -> host must report failure, exit non-zero."""
    payload = os.urandom(4096)
    path = os.path.join(tmpdir, "fw2.bin")
    with open(path, "wb") as f:
        f.write(payload)
    port = free_port()
    proc = start_server(port, path)
    conn = connect_retry(port)
    conn.sendall(HELLO)
    files = read_header_and_records(conn)
    _name, _data, sent_crc = files[0]
    conn.sendall(struct.pack("<II", 0, sent_crc ^ 0xFFFFFFFF))  # deliberately wrong
    conn.close()
    rc = proc.wait(timeout=10)
    out = proc.stdout.read()
    assert rc != 0, "expected non-zero exit on crc mismatch, got 0\n%s" % out
    return out


def test_camera_error_status(tmpdir):
    """Camera acks a nonzero error status -> host must report failure, exit non-zero."""
    payload = os.urandom(1024)
    path = os.path.join(tmpdir, "fw3.bin")
    with open(path, "wb") as f:
        f.write(payload)
    port = free_port()
    proc = start_server(port, path)
    conn = connect_retry(port)
    conn.sendall(HELLO)
    files = read_header_and_records(conn)
    _name, _data, sent_crc = files[0]
    conn.sendall(struct.pack("<II", 5, sent_crc))  # crc right, but status=5 (FIO error)
    conn.close()
    rc = proc.wait(timeout=10)
    out = proc.stdout.read()
    assert rc != 0, "expected non-zero exit on error status, got 0\n%s" % out
    return out


def test_portscan_then_camera(tmpdir):
    """A stray non-MLWF connection is ignored; the real camera that follows still succeeds."""
    payload = os.urandom(2048)
    path = os.path.join(tmpdir, "fw4.bin")
    with open(path, "wb") as f:
        f.write(payload)
    port = free_port()
    proc = start_server(port, path)

    # stray/portscan connection: wrong hello, server should close and keep waiting
    scan = connect_retry(port)
    scan.sendall(b"XXXX")
    try:
        scan.recv(16)  # expect EOF (server closed us)
    except OSError:
        pass
    scan.close()

    # now the real camera connects
    conn = connect_retry(port)
    conn.sendall(HELLO)
    files = read_header_and_records(conn)
    _name, data, sent_crc = files[0]
    conn.sendall(struct.pack("<II", 0, zlib.crc32(data) & 0xFFFFFFFF))
    conn.close()
    rc = proc.wait(timeout=10)
    out = proc.stdout.read()
    assert rc == 0, "expected exit 0 after portscan+camera, got %d\n%s" % (rc, out)
    return out


def test_crc_subcommand(tmpdir):
    """`mlwifi.py crc` prints a size + crc that matches zlib."""
    payload = os.urandom(500)
    path = os.path.join(tmpdir, "fw5.bin")
    with open(path, "wb") as f:
        f.write(payload)
    expected = zlib.crc32(payload) & 0xFFFFFFFF
    out = subprocess.check_output([sys.executable, MLWIFI, "crc", path], text=True)
    assert ("size=%d" % len(payload)) in out, out
    assert ("crc32=0x%08X" % expected) in out, out
    return out.strip()


def main():
    tests = [
        ("happy path (200KB, crc match)", test_happy_path),
        ("crc mismatch -> fail", test_crc_mismatch),
        ("error status -> fail", test_camera_error_status),
        ("portscan then camera -> ok", test_portscan_then_camera),
        ("crc subcommand", test_crc_subcommand),
    ]
    passed = 0
    with tempfile.TemporaryDirectory() as tmpdir:
        for name, fn in tests:
            try:
                fn(tmpdir)
                print("PASS  %s" % name)
                passed += 1
            except Exception as e:
                print("FAIL  %s\n      %s" % (name, e))
    print("\n%d/%d passed" % (passed, len(tests)))
    return 0 if passed == len(tests) else 1


if __name__ == "__main__":
    sys.exit(main())
