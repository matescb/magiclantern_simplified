#!/usr/bin/env python3
"""Control a SONOFF iPlug S60 (eWeLink LAN mode) from the local network.

Usage:
    python3 plug.py setup                    # one-time: eWeLink login -> fetch & save devicekey
    python3 plug.py setup --devicekey KEY    # skip cloud login, paste a known key
    python3 plug.py on | off | toggle
    python3 plug.py status [--json]

The plug runs encrypted eWeLink LAN mode (apivers=1): commands are AES-128-CBC
encrypted with MD5(devicekey) and POSTed to http://<host>:8081/zeroconf/switch.
State is read from the plug's mDNS TXT record (unicast query, no daemon needed).

Config lives in ~/.config/sonoff_plug.json (chmod 600):
    {"deviceid": "10027b9c49", "devicekey": "<uuid>", "host": "192.168.1.52", "port": 8081}
"""

import argparse
import base64
import getpass
import hashlib
import hmac
import json
import os
import secrets
import socket
import string
import struct
import sys
import time
from pathlib import Path

import requests
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.padding import PKCS7

CONFIG_PATH = Path.home() / ".config" / "sonoff_plug.json"
DEFAULT_HOST = "192.168.1.52"
DEFAULT_DEVICEID = "10027b9c49"
LAN_PORT = 8081
MDNS_PORT = 5353
MDNS_SERVICE = "_ewelink._tcp.local"
LAN_TIMEOUT_S = 5
MDNS_TIMEOUT_S = 2
MDNS_RETRIES = 2

# Public eWeLink app credentials used across open-source tooling (AlexxIT/SonoffLAN).
# These identify the *client application*, not the user — they are not user secrets.
EWELINK_APPS = [
    ("4s1FXKC9FaGfoqXhmXSJneb3qcm1gOak", "oKvCM06gvwkRbfetd6qWRrbC3rFrbIpV"),
    ("R8Oq3y0eSZSYdKccHlrQzT1ACCOUT9Gv", "1ve5Qk9GXfUhKAn1svnKwpAlxXkMarru"),
]
EWELINK_REGIONS = ("eu", "us", "as", "cn")
ERR_REGION_REDIRECT = 10004


class PlugError(Exception):
    """User-facing error with a clear message."""


# --- config ----------------------------------------------------------------

def load_config(path=CONFIG_PATH):
    path = Path(path)
    if not path.exists():
        raise PlugError(f"No config at {path} — run `python3 plug.py setup` first.")
    return json.loads(path.read_text())


def save_config(cfg, path=CONFIG_PATH):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cfg, indent=2) + "\n")
    path.chmod(0o600)


# --- eWeLink LAN crypto ----------------------------------------------------

def _aes_key(devicekey):
    return hashlib.md5(devicekey.encode()).digest()


def encrypt_payload(data, devicekey, deviceid):
    """Wrap a command dict in the encrypted eWeLink LAN envelope."""
    plaintext = json.dumps(data, separators=(",", ":")).encode()
    iv = secrets.token_bytes(16)
    padder = PKCS7(128).padder()
    padded = padder.update(plaintext) + padder.finalize()
    enc = Cipher(algorithms.AES(_aes_key(devicekey)), modes.CBC(iv)).encryptor()
    ciphertext = enc.update(padded) + enc.finalize()
    return {
        "sequence": str(int(time.time() * 1000)),
        "deviceid": deviceid,
        "selfApikey": "123",
        "encrypt": True,
        "iv": base64.b64encode(iv).decode(),
        "data": base64.b64encode(ciphertext).decode(),
    }


def decrypt_payload(b64_data, b64_iv, devicekey):
    iv = base64.b64decode(b64_iv)
    ciphertext = base64.b64decode(b64_data)
    dec = Cipher(algorithms.AES(_aes_key(devicekey)), modes.CBC(iv)).decryptor()
    padded = dec.update(ciphertext) + dec.finalize()
    unpadder = PKCS7(128).unpadder()
    plaintext = unpadder.update(padded) + unpadder.finalize()
    return json.loads(plaintext)


# --- mDNS (unicast query, stdlib sockets only) -----------------------------

def build_ptr_query(service=MDNS_SERVICE):
    header = struct.pack("!HHHHHH", secrets.randbelow(0x10000), 0, 1, 0, 0, 0)
    qname = b"".join(bytes([len(p)]) + p.encode() for p in service.split(".")) + b"\x00"
    return header + qname + struct.pack("!HH", 12, 1)  # QTYPE=PTR, QCLASS=IN


def _skip_name(packet, off):
    while True:
        length = packet[off]
        if length == 0:
            return off + 1
        if length & 0xC0 == 0xC0:  # compression pointer ends the name
            return off + 2
        off += 1 + length


def parse_txt_records(packet):
    """Collect key=value pairs from every TXT record in a DNS response."""
    qdcount, ancount, nscount, arcount = struct.unpack_from("!HHHH", packet, 4)
    off = 12
    for _ in range(qdcount):
        off = _skip_name(packet, off) + 4
    txt = {}
    for _ in range(ancount + nscount + arcount):
        off = _skip_name(packet, off)
        rtype, _rclass, _ttl, rdlen = struct.unpack_from("!HHIH", packet, off)
        off += 10
        if rtype == 16:  # TXT
            pos, end = off, off + rdlen
            while pos < end:
                slen = packet[pos]
                entry = packet[pos + 1 : pos + 1 + slen]
                pos += 1 + slen
                if b"=" in entry:
                    key, val = entry.split(b"=", 1)
                    txt[key.decode()] = val.decode()
        off += rdlen
    return txt


def mdns_query_txt(host):
    """Query the plug directly for its _ewelink._tcp TXT record."""
    last_err = None
    for _ in range(MDNS_RETRIES):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(MDNS_TIMEOUT_S)
        try:
            sock.sendto(build_ptr_query(), (host, MDNS_PORT))
            packet, _addr = sock.recvfrom(8192)
            txt = parse_txt_records(packet)
            if txt:
                return txt
            last_err = PlugError(f"{host} answered mDNS but sent no TXT data.")
        except socket.timeout:
            last_err = PlugError(f"Plug unreachable: no mDNS reply from {host}:{MDNS_PORT}.")
        finally:
            sock.close()
    raise last_err


# --- LAN control -----------------------------------------------------------

def decrypt_status_txt(txt, devicekey):
    """Decode device state from mDNS TXT fields (data1..data4 concatenated)."""
    data = "".join(txt.get(f"data{i}", "") for i in range(1, 5))
    if txt.get("encrypt") != "true":
        return json.loads(data)
    try:
        return decrypt_payload(data, txt["iv"], devicekey)
    except Exception as exc:
        raise PlugError("Failed to decrypt device state — is the devicekey correct?") from exc


def get_status(cfg):
    txt = mdns_query_txt(cfg["host"])
    if txt.get("id") and txt["id"] != cfg["deviceid"]:
        raise PlugError(
            f"Device at {cfg['host']} reports id {txt['id']}, expected {cfg['deviceid']}."
        )
    return decrypt_status_txt(txt, cfg["devicekey"])


def get_switch_state(state):
    """Return 'on'/'off' from either state schema, or None if not present."""
    if "switch" in state:
        return state["switch"]
    switches = state.get("switches")
    if switches:
        return switches[0].get("switch")
    return None


def _post_lan(cfg, endpoint, data):
    envelope = encrypt_payload(data, cfg["devicekey"], cfg["deviceid"])
    url = f"http://{cfg['host']}:{cfg.get('port', LAN_PORT)}/zeroconf/{endpoint}"
    try:
        resp = requests.post(
            url,
            data=json.dumps(envelope, separators=(",", ":")).encode(),
            headers={"Content-Type": "application/json"},
            timeout=LAN_TIMEOUT_S,
        )
    except requests.exceptions.RequestException as exc:
        raise PlugError(f"Plug unreachable at {url}: {exc}") from exc
    if resp.status_code != 200:
        raise PlugError(f"Plug returned HTTP {resp.status_code}: {resp.text[:200]}")
    return resp.json().get("error")


def switch_payloads(state):
    """Command variants, most specific first. S60/POWR3 firmware (uiid 190)
    rejects the switch command with error 400 unless operSide is present."""
    return (
        ("switches", {"switches": [{"outlet": 0, "switch": state}], "operSide": 1}),
        ("switches", {"switches": [{"outlet": 0, "switch": state}]}),
        ("switch", {"switch": state}),
    )


def send_switch(cfg, state):
    last_error = None
    for endpoint, data in switch_payloads(state):
        last_error = _post_lan(cfg, endpoint, data)
        if last_error == 0:
            return
    hint = " (wrong devicekey?)" if last_error in (400, 401, 403) else ""
    raise PlugError(f"Plug rejected command, error {last_error}{hint}")


# --- eWeLink cloud (one-time devicekey fetch) ------------------------------

def sign(body, app_secret):
    return base64.b64encode(hmac.new(app_secret.encode(), body, hashlib.sha256).digest()).decode()


def _nonce():
    alphabet = string.ascii_letters + string.digits
    return "".join(secrets.choice(alphabet) for _ in range(8))


def ewelink_login(email, password, region, appid, app_secret, country_code="+420"):
    """Login; follows the region redirect the API may respond with."""
    payload = {"email": email, "password": password, "countryCode": country_code}
    body = json.dumps(payload, separators=(",", ":")).encode()
    for _ in range(2):
        resp = requests.post(
            f"https://{region}-apia.coolkit.cc/v2/user/login",
            data=body,
            headers={
                "Content-Type": "application/json",
                "X-CK-Appid": appid,
                "X-CK-Nonce": _nonce(),
                "Authorization": "Sign " + sign(body, app_secret),
            },
            timeout=15,
        )
        data = resp.json()
        if data.get("error") == ERR_REGION_REDIRECT:
            region = data["data"]["region"]
            continue
        if data.get("error") != 0:
            raise PlugError(f"eWeLink login failed (error {data.get('error')}): {data.get('msg', '')}")
        return data["data"]["at"], region
    raise PlugError("eWeLink login failed: too many region redirects.")


def ewelink_devices(token, region, appid):
    resp = requests.get(
        f"https://{region}-apia.coolkit.cc/v2/device/thing",
        params={"num": 0},
        headers={"Authorization": "Bearer " + token, "X-CK-Appid": appid},
        timeout=15,
    )
    data = resp.json()
    if data.get("error") != 0:
        raise PlugError(f"Fetching device list failed (error {data.get('error')}): {data.get('msg', '')}")
    return [t["itemData"] for t in data["data"]["thingList"] if "itemData" in t]


def fetch_devicekey(email, password, region, deviceid, country_code="+420"):
    last_err = None
    for appid, app_secret in EWELINK_APPS:
        try:
            token, region_found = ewelink_login(email, password, region, appid, app_secret, country_code)
        except PlugError as exc:
            last_err = exc
            continue
        devices = ewelink_devices(token, region_found, appid)
        for dev in devices:
            if dev.get("deviceid") == deviceid:
                return dev["devicekey"], dev.get("name", "")
        listing = "\n".join(
            f"  {d.get('deviceid')}  {d.get('name', '?')}  online={d.get('online')}"
            for d in devices
        ) or "  (none)"
        raise PlugError(
            f"Device {deviceid} not found in this eWeLink account. Devices seen:\n{listing}\n"
            "Re-run setup with --deviceid <id> if the plug is listed under another id."
        )
    raise last_err or PlugError("eWeLink login failed.")


# --- commands --------------------------------------------------------------

def cmd_setup(args):
    host = args.host
    deviceid = args.deviceid
    try:
        txt = mdns_query_txt(host)
        deviceid = txt.get("id", deviceid)
        print(f"Found plug at {host}: deviceid={deviceid}, type={txt.get('type', '?')}")
    except PlugError as exc:
        print(f"Warning: {exc} Using deviceid {deviceid}.")

    if args.devicekey:
        devicekey = args.devicekey
    else:
        print("One-time eWeLink cloud login (credentials are not stored, only the devicekey).")
        try:
            email = args.email or input("eWeLink email: ")
            password = os.environ.get("EWELINK_PASSWORD") or getpass.getpass("eWeLink password: ")
        except EOFError:
            raise PlugError(
                "stdin is not interactive. Run setup in a regular terminal, or use:\n"
                "  EWELINK_PASSWORD='...' python3 plug.py setup --email you@example.com"
            ) from None
        devicekey, name = fetch_devicekey(email, password, args.region, deviceid, args.country_code)
        print(f"Devicekey received for '{name or deviceid}'.")

    cfg = {"deviceid": deviceid, "devicekey": devicekey, "host": host, "port": LAN_PORT}
    save_config(cfg)
    print(f"Config saved to {CONFIG_PATH}")
    try:
        state = get_status(cfg)
        print(f"Verified: plug is {(get_switch_state(state) or '?').upper()}")
    except PlugError as exc:
        print(f"Warning: config saved, but status check failed: {exc}")


def cmd_switch(state):
    cfg = load_config()
    send_switch(cfg, state)
    print(f"Plug is {state.upper()}")


def cmd_toggle(_args):
    cfg = load_config()
    current = get_switch_state(get_status(cfg))
    if current not in ("on", "off"):
        raise PlugError(f"Unexpected switch state: {current!r}")
    new_state = "off" if current == "on" else "on"
    send_switch(cfg, new_state)
    print(f"Plug is {new_state.upper()} (was {current})")


def cmd_status(args):
    cfg = load_config()
    state = get_status(cfg)
    if args.json:
        print(json.dumps(state, indent=2))
        return
    print(f"Plug is {(get_switch_state(state) or '?').upper()}")
    for key, unit in (("voltage", "V"), ("current", "A"), ("power", "W")):
        if key in state:
            print(f"  {key}: {state[key] / 100:.2f} {unit}")  # device reports 0.01 units


def main(argv=None):
    parser = argparse.ArgumentParser(description="Control a SONOFF iPlug S60 over LAN.")
    sub = parser.add_subparsers(dest="command", required=True)

    p_setup = sub.add_parser("setup", help="fetch and save the devicekey (one-time)")
    p_setup.add_argument("--host", default=DEFAULT_HOST, help="plug IP address")
    p_setup.add_argument("--deviceid", default=DEFAULT_DEVICEID, help="eWeLink device id")
    p_setup.add_argument("--devicekey", help="paste a known devicekey, skip cloud login")
    p_setup.add_argument("--email", help="eWeLink account email")
    p_setup.add_argument("--region", default="eu", choices=EWELINK_REGIONS, help="eWeLink region")
    p_setup.add_argument("--country-code", default="+420", help="account country code, e.g. +420")

    sub.add_parser("on", help="switch the plug on")
    sub.add_parser("off", help="switch the plug off")
    sub.add_parser("toggle", help="invert the current state")
    p_status = sub.add_parser("status", help="show the current state")
    p_status.add_argument("--json", action="store_true", help="print full state as JSON")

    args = parser.parse_args(argv)
    try:
        if args.command == "setup":
            cmd_setup(args)
        elif args.command in ("on", "off"):
            cmd_switch(args.command)
        elif args.command == "toggle":
            cmd_toggle(args)
        elif args.command == "status":
            cmd_status(args)
    except PlugError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
