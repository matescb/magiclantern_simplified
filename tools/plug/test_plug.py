"""Unit tests for plug.py — pure functions only, no hardware or cloud access."""

import base64
import json
import stat
import struct

import pytest

import plug

DUMMY_KEY = "8bcf47f6-9214-4b32-8f4e-2b1a5d9c7e11"
DEVICE_ID = "10027b9c49"


# --- request signing -------------------------------------------------------

def test_sign_matches_known_answer():
    body = b'{"email":"a@b.c","password":"x"}'
    assert plug.sign(body, "testsecret") == "S0Pmt45Ld7XIToi2IsN598+pRPBt7h/X2qDmcikAqKY="


# --- AES envelope ----------------------------------------------------------

def test_encrypt_decrypt_round_trip():
    data = {"switch": "on"}
    env = plug.encrypt_payload(data, DUMMY_KEY, DEVICE_ID)
    assert plug.decrypt_payload(env["data"], env["iv"], DUMMY_KEY) == data


def test_envelope_has_required_ewelink_fields():
    env = plug.encrypt_payload({"switch": "off"}, DUMMY_KEY, DEVICE_ID)
    assert env["deviceid"] == DEVICE_ID
    assert env["encrypt"] is True
    assert env["selfApikey"] == "123"
    assert env["sequence"].isdigit()
    base64.b64decode(env["iv"])
    base64.b64decode(env["data"])


def test_iv_differs_between_calls():
    a = plug.encrypt_payload({"switch": "on"}, DUMMY_KEY, DEVICE_ID)
    b = plug.encrypt_payload({"switch": "on"}, DUMMY_KEY, DEVICE_ID)
    assert a["iv"] != b["iv"]


def test_decrypt_with_wrong_key_raises():
    env = plug.encrypt_payload({"switch": "on"}, DUMMY_KEY, DEVICE_ID)
    with pytest.raises(Exception):
        plug.decrypt_payload(env["data"], env["iv"], "wrong-key")


# --- DNS/mDNS parsing ------------------------------------------------------

def _txt_rdata(strings):
    return b"".join(bytes([len(s)]) + s for s in strings)


def _dns_response(records):
    """Build a minimal DNS response with the given TXT rdata blobs."""
    header = struct.pack("!HHHHHH", 0, 0x8400, 0, len(records), 0, 0)
    name = b"\x08_ewelink\x04_tcp\x05local\x00"
    body = b""
    for rdata in records:
        body += name + struct.pack("!HHIH", 16, 1, 120, len(rdata)) + rdata
    return header + body


def test_parse_txt_records_extracts_key_values():
    rdata = _txt_rdata([b"id=10027b9c49", b"encrypt=true", b"type=plug"])
    txt = plug.parse_txt_records(_dns_response([rdata]))
    assert txt == {"id": "10027b9c49", "encrypt": "true", "type": "plug"}


def test_parse_txt_records_handles_compression_pointer_names():
    rdata = _txt_rdata([b"id=10027b9c49"])
    header = struct.pack("!HHHHHH", 0, 0x8400, 1, 1, 0, 0)
    qname = b"\x08_ewelink\x04_tcp\x05local\x00"
    question = qname + struct.pack("!HH", 12, 1)
    # answer name is a compression pointer back to offset 12 (the question name)
    answer = b"\xc0\x0c" + struct.pack("!HHIH", 16, 0x8001, 120, len(rdata)) + rdata
    txt = plug.parse_txt_records(header + question + answer)
    assert txt["id"] == "10027b9c49"


def test_parse_txt_records_ignores_non_txt_records():
    name = b"\x08_ewelink\x04_tcp\x05local\x00"
    srv = name + struct.pack("!HHIH", 33, 1, 120, 2) + b"\x00\x00"
    txt_rdata = _txt_rdata([b"seq=5"])
    txt_rec = name + struct.pack("!HHIH", 16, 1, 120, len(txt_rdata)) + txt_rdata
    header = struct.pack("!HHHHHH", 0, 0x8400, 0, 2, 0, 0)
    txt = plug.parse_txt_records(header + srv + txt_rec)
    assert txt == {"seq": "5"}


def test_build_ptr_query_is_parseable():
    q = plug.build_ptr_query()
    qdcount = struct.unpack_from("!H", q, 4)[0]
    assert qdcount == 1
    assert b"\x08_ewelink\x04_tcp\x05local\x00" in q


# --- status TXT decryption -------------------------------------------------

def test_decrypt_status_txt_concatenates_data_parts_in_order():
    state = {"switch": "off", "startup": "off", "voltage": 231.4}
    env = plug.encrypt_payload(state, DUMMY_KEY, DEVICE_ID)
    cipher_b64 = env["data"]
    split = len(cipher_b64) // 2
    # real devices list data2 before data1 in the TXT record; order must not matter
    txt = {
        "id": DEVICE_ID,
        "encrypt": "true",
        "iv": env["iv"],
        "data2": cipher_b64[split:],
        "data1": cipher_b64[:split],
    }
    assert plug.decrypt_status_txt(txt, DUMMY_KEY) == state


def test_decrypt_status_txt_plaintext_when_not_encrypted():
    state = {"switch": "on"}
    txt = {"id": DEVICE_ID, "data1": json.dumps(state)}
    assert plug.decrypt_status_txt(txt, DUMMY_KEY) == state


# --- switch state schemas --------------------------------------------------

def test_get_switch_state_flat_schema():
    assert plug.get_switch_state({"switch": "on"}) == "on"


def test_get_switch_state_switches_array_schema():
    state = {"switches": [{"switch": "off", "outlet": 0}], "voltage": 23954}
    assert plug.get_switch_state(state) == "off"


def test_get_switch_state_unknown_schema_returns_none():
    assert plug.get_switch_state({"voltage": 23954}) is None


def test_switch_payloads_tries_operside_variant_first():
    endpoint, data = plug.switch_payloads("on")[0]
    assert endpoint == "switches"
    assert data == {"switches": [{"outlet": 0, "switch": "on"}], "operSide": 1}


def test_switch_payloads_keeps_flat_switch_fallback():
    endpoints = [ep for ep, _ in plug.switch_payloads("off")]
    assert endpoints[-1] == "switch"


# --- config ----------------------------------------------------------------

def test_config_save_load_round_trip(tmp_path):
    path = tmp_path / "sonoff_plug.json"
    cfg = {
        "deviceid": DEVICE_ID,
        "devicekey": DUMMY_KEY,
        "host": "192.168.1.52",
        "port": 8081,
    }
    plug.save_config(cfg, path)
    assert plug.load_config(path) == cfg


def test_config_saved_with_owner_only_permissions(tmp_path):
    path = tmp_path / "sonoff_plug.json"
    plug.save_config({"devicekey": DUMMY_KEY}, path)
    assert stat.S_IMODE(path.stat().st_mode) == 0o600


def test_load_config_missing_file_raises_with_hint(tmp_path):
    with pytest.raises(plug.PlugError, match="setup"):
        plug.load_config(tmp_path / "nope.json")
