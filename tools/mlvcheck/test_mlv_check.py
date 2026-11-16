"""pytest tests for mlv_check.py.

The unit tests build small synthetic MLV files with struct (no external data).
The integration tests at the bottom run against the files produced by
host_writer/rvx_host_mlv (the real raw_vidx mlv_3.c writer) when they exist
under $MLVCHECK_SAMPLES (default /mnt/data/WORK/eos/sim/C1/out); otherwise skipped.
"""

import os
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mlv_check  # noqa: E402

SAMPLES = os.environ.get("MLVCHECK_SAMPLES", "/mnt/data/WORK/eos/sim/C1/out")

W, H, BPP = 16, 4, 14
FRAME_BYTES = W * H * BPP // 8  # 112


# ------------------------------------------------------------------ builders

def mlvi(frame_count, version=b"v2.0", fps_nom=25000, fps_denom=1000, video_class=1):
    return struct.pack("<4sI8sQHHIHHIIII", b"MLVI", 0x34, version, 0x1122334455667788,
                       0, 0, 4, video_class, 0, frame_count, 0, fps_nom, fps_denom)


def rawi(xres=W, yres=H, width=None, height=None, bpp=BPP, black=2047, white=16200, ts=10):
    width = xres if width is None else width
    height = yres if height is None else height
    pitch = width * bpp // 8
    info = [1, 0, height, width, pitch, pitch * height, bpp, black, white,
            0, 0, width, height,          # jpeg
            0, 0, height, width,          # active area y1 x1 y2 x2
            0, 0, 0x02010100, 1] + [0] * 18 + [1100]
    body = struct.pack("<HH", xres, yres) + struct.pack("<Ii" + "i" * 38, *info)
    blk = struct.pack("<4sIQ", b"RAWI", 0x10 + len(body), ts) + body
    assert len(blk) == 0xB4
    return blk


def generic(btype, payload=b"", ts=20):
    return struct.pack("<4sIQ", btype, 0x10 + len(payload), ts) + payload


def vidf(number, ts, payload=None, space=0x60):
    payload = bytes(bytearray((number + i) & 0xFF for i in range(FRAME_BYTES))) \
        if payload is None else payload
    size = 0x20 + space + len(payload)
    return (struct.pack("<4sIQIHHHHI", b"VIDF", size, ts, number, 0, 0, 0, 0, space)
            + b"\0" * space + payload)


def build(n_frames=4, declared=None, numbers=None, t0=1000, dt=40000, **rawi_kw):
    declared = n_frames if declared is None else declared
    numbers = list(range(n_frames)) if numbers is None else numbers
    out = mlvi(declared) + rawi(**rawi_kw) + generic(b"RTCI", b"\0" * 28, ts=30)
    for i, n in enumerate(numbers):
        out += vidf(n, t0 + i * dt)
    return out


@pytest.fixture
def write(tmp_path):
    def _write(data, name="t.mlv"):
        p = tmp_path / name
        p.write_bytes(data)
        return str(p)
    return _write


def codes(rep, severity=None):
    return rep.codes(severity)


# ------------------------------------------------------------------ good file

def test_good_file_has_no_errors_and_exits_zero(write):
    path = write(build(4))

    rep = mlv_check.parse(path)

    assert rep.errors == []
    assert rep.warnings == []
    assert rep.vidf_complete == 4
    assert rep.mlvi["video_frame_count"] == 4
    assert rep.truncated is False
    assert rep.expected_frame_size == FRAME_BYTES
    assert rep.exit_code() == 0
    assert mlv_check.main([path]) == 0


def test_good_file_reports_header_fields_and_block_list(write):
    path = write(build(3))

    rep = mlv_check.parse(path)

    assert rep.mlvi["version"] == "v2.0"
    assert (rep.mlvi["fps_nom"], rep.mlvi["fps_denom"]) == (25000, 1000)
    assert (rep.rawi["xRes"], rep.rawi["yRes"], rep.rawi["bits_per_pixel"]) == (W, H, BPP)
    assert [b.type for b in rep.blocks] == ["MLVI", "RAWI", "RTCI", "VIDF", "VIDF", "VIDF"]
    assert [b.timestamp for b in rep.blocks[3:]] == [1000, 41000, 81000]
    assert rep.blocks[3].fields["payload_size"] == FRAME_BYTES
    assert rep.mlvi["measured_fps"] == 25.0


def test_verbose_and_json_output_modes_run(write, capsys):
    path = write(build(2))

    assert mlv_check.main([path, "-v"]) == 0
    text = capsys.readouterr().out
    assert "frame=1 space=96 payload=112" in text
    assert mlv_check.main([path, "--json"]) == 0
    assert '"vidf_complete": 2' in capsys.readouterr().out


# ------------------------------------------------------------------ truncation

def test_truncated_last_frame_is_tolerated_and_reported(write):
    data = build(4)
    path = write(data[:-50])

    rep = mlv_check.parse(path)

    assert rep.truncated is True
    assert rep.truncated_missing == 50
    assert rep.vidf_complete == 3
    assert rep.vidf_partial == 1
    assert "TRUNCATED_BLOCK" in codes(rep, mlv_check.ERROR)
    assert rep.blocks[-1].complete is False
    assert rep.blocks[-1].fields["frame_number"] == 3   # header of the cut block still decoded
    assert rep.exit_code() == 1


def test_truncated_inside_block_header_is_tolerated(write):
    data = build(2) + b"VIDF\x10"  # 5 stray bytes: not even type+size

    rep = mlv_check.parse(write(data))

    assert rep.truncated is True
    assert "TRUNCATED_HEADER" in codes(rep, mlv_check.ERROR)
    assert rep.vidf_complete == 2
    assert rep.exit_code() == 1


def test_truncated_inside_vidf_fixed_header_does_not_raise(write):
    data = build(2)
    data += vidf(2, 99999)[:0x14]  # type, size, timestamp, half of frame number... cut

    rep = mlv_check.parse(write(data))

    assert rep.truncated is True
    assert rep.vidf_complete == 2
    assert rep.blocks[-1].fields == {}


@pytest.mark.parametrize("cut", [1, 7, 8, 9, 0x20, 0x34, 0x35, 0xE7, 0xE8, 0xF0, 300, 500])
def test_any_cut_point_is_parsed_without_exception(write, cut):
    data = build(3)

    rep = mlv_check.parse(write(data[:cut]))

    assert rep.exit_code() in (1, 2)


def test_crashed_recording_passes_with_crash_ok(write):
    # camera died: header never finalised (count 0) and the last block is torn
    data = build(5, declared=0)
    path = write(data[:-10])

    assert mlv_check.main([path]) == 1
    assert mlv_check.main([path, "--crash-ok"]) == 0
    assert mlv_check.main([path, "--crash-ok", "--strict"]) == 1


# ------------------------------------------------------------------ frame count

def test_wrong_frame_count_is_an_error(write):
    path = write(build(5, declared=8))

    rep = mlv_check.parse(path)

    assert "FRAME_COUNT" in codes(rep, mlv_check.ERROR)
    assert rep.vidf_complete == 5
    assert mlv_check.main([path]) == 1
    # a wrong (non-zero) count is NOT excused by --crash-ok
    assert mlv_check.main([path, "--crash-ok"]) == 1


def test_unfinalised_frame_count_is_distinguished(write):
    rep = mlv_check.parse(write(build(5, declared=0)))

    assert "FRAME_COUNT_UNFINALISED" in codes(rep, mlv_check.ERROR)
    assert "FRAME_COUNT" not in codes(rep)


def test_missing_and_duplicate_frame_numbers(write):
    gap = mlv_check.parse(write(build(4, numbers=[0, 1, 3, 4]), "gap.mlv"))
    dup = mlv_check.parse(write(build(4, numbers=[0, 1, 1, 2]), "dup.mlv"))

    assert "FRAME_NUMBER" in codes(gap, mlv_check.ERROR)
    assert "FRAME_NUMBER" in codes(dup, mlv_check.ERROR)


# ------------------------------------------------------------------ frame size / geometry

def test_frame_size_not_matching_resolution_is_an_error(write):
    data = mlvi(1) + rawi() + vidf(0, 1000, payload=b"\x55" * (FRAME_BYTES - 14))

    rep = mlv_check.parse(write(data))

    assert "FRAME_SIZE" in codes(rep, mlv_check.ERROR)
    assert rep.exit_code() == 1


def test_frame_space_larger_than_block_is_an_error(write):
    blk = bytearray(vidf(0, 1000))
    struct.pack_into("<I", blk, 0x1C, 0x7FFFFFF0)

    rep = mlv_check.parse(write(mlvi(1) + rawi() + bytes(blk)))

    assert "FRAME_SPACE" in codes(rep, mlv_check.ERROR)


def test_raw_info_describing_source_buffer_is_info_only(write):
    # what raw_vidx writes: frames 16x4, raw_info still the full 32x8 buffer
    path = write(build(2, width=32, height=8))

    rep = mlv_check.parse(path)

    assert rep.errors == []
    assert rep.warnings == []
    assert "RAWI_GEOMETRY" in codes(rep, mlv_check.INFO)
    assert "RAWI_ACTIVE_AREA" in codes(rep, mlv_check.INFO)
    assert mlv_check.main([path]) == 0
    assert mlv_check.main([path, "--strict"]) == 0   # same as mlv_lite: not a defect


def test_absolute_vidf_timestamps_are_flagged(write):
    rep = mlv_check.parse(write(build(3, t0=60 * 1000 * 1000)))

    assert "TIMESTAMP_ABSOLUTE" in codes(rep, mlv_check.WARNING)
    assert rep.errors == []


def test_non_monotonic_timestamps_are_an_error(write):
    data = mlvi(2) + rawi() + vidf(0, 5000) + vidf(1, 4000)

    rep = mlv_check.parse(write(data))

    assert "TIMESTAMP_ORDER" in codes(rep, mlv_check.ERROR)


def test_version_other_than_v2_is_a_warning(write):
    data = mlvi(1, version=b"v3.0") + rawi() + vidf(0, 1000)

    rep = mlv_check.parse(write(data))

    assert codes(rep) == ["VERSION"]
    assert rep.exit_code() == 0


def test_missing_rawi_is_an_error(write):
    rep = mlv_check.parse(write(mlvi(1) + vidf(0, 1000)))

    assert "NO_RAWI" in codes(rep, mlv_check.ERROR)


# ------------------------------------------------------------------ garbage / not MLV

def test_not_an_mlv_file_exits_two(write):
    path = write(b"RIFF" + b"\0" * 100)

    rep = mlv_check.parse(path)

    assert codes(rep) == ["NOT_MLV"]
    assert mlv_check.main([path]) == 2


def test_empty_and_missing_files_exit_two(write, tmp_path):
    assert mlv_check.main([write(b"")]) == 2
    assert mlv_check.main([str(tmp_path / "nope.mlv")]) == 2


def test_garbage_between_blocks_is_reported_and_resynced(write):
    data = mlvi(3) + rawi() + vidf(0, 1000) + b"\0" * 512 + vidf(1, 2000) + vidf(2, 3000)

    rep = mlv_check.parse(write(data))

    assert "GARBAGE" in codes(rep, mlv_check.ERROR)
    assert rep.vidf_complete == 3
    assert rep.exit_code() == 1


def test_garbage_tail_without_further_blocks(write):
    data = build(2) + b"\xff" * 4096

    rep = mlv_check.parse(write(data))

    assert "GARBAGE" in codes(rep, mlv_check.ERROR)
    assert rep.vidf_complete == 2


# ------------------------------------------------------------------ payload helpers

def test_extract_returns_exact_payload(write, tmp_path):
    payload = bytes(bytearray(range(FRAME_BYTES)))
    path = write(mlvi(1) + rawi() + vidf(0, 1000, payload=payload))
    out = str(tmp_path / "f0.raw")

    rc = mlv_check.main([path, "--extract", "0", out])

    assert rc == 0
    assert open(out, "rb").read() == payload
    assert mlv_check.main([path, "--extract", "5", out]) == 2


def test_unpack14_matches_ml_packing():
    # pack 8 known pixels the way src/raw.h raw_pixblock does (7 LE 16-bit words)
    px = [0x3FFF, 0, 0x2AAA, 0x1555, 1, 0x2000, 0x1234, 0x0ABC]
    a, b, c, d, e, f, g, h = px
    words = [
        (a << 2) | (b >> 12),
        ((b & 0xFFF) << 4) | (c >> 10),
        ((c & 0x3FF) << 6) | (d >> 8),
        ((d & 0xFF) << 8) | (e >> 6),
        ((e & 0x3F) << 10) | (f >> 4),
        ((f & 0xF) << 12) | (g >> 2),
        ((g & 0x3) << 14) | h,
    ]
    data = struct.pack("<7H", *words)

    assert mlv_check.unpack14_rows(data, 8, [0], 14) == [px]


def test_channel_stats_on_constant_cfa_sites():
    # 16x4 frame: R=1000 G=2000 G=2000 B=3000, packed via the same word layout
    def pack_row(vals):
        out = b""
        for i in range(0, len(vals), 8):
            a, b, c, d, e, f, g, h = vals[i:i + 8]
            out += struct.pack("<7H", (a << 2) | (b >> 12), ((b & 0xFFF) << 4) | (c >> 10),
                               ((c & 0x3FF) << 6) | (d >> 8), ((d & 0xFF) << 8) | (e >> 6),
                               ((e & 0x3F) << 10) | (f >> 4), ((f & 0xF) << 12) | (g >> 2),
                               ((g & 0x3) << 14) | h)
        return out
    even = pack_row([1000, 2000] * 8)
    odd = pack_row([2000, 3000] * 8)

    st = mlv_check.channel_stats((even + odd) * 2, 16, 4, step=1)

    assert st[(0, 0)]["mean"] == 1000
    assert st[(0, 1)]["mean"] == 2000
    assert st[(1, 0)]["mean"] == 2000
    assert st[(1, 1)]["mean"] == 3000


def test_command_line_exit_status(write):
    good = write(build(2), "good.mlv")
    bad = write(build(2, declared=3), "bad.mlv")
    script = os.path.join(HERE, "mlv_check.py")

    assert subprocess.call([sys.executable, script, good], stdout=subprocess.DEVNULL) == 0
    assert subprocess.call([sys.executable, script, bad], stdout=subprocess.DEVNULL) == 1


# ------------------------------------------------------------------ integration (real writer)

def _sample(name):
    p = os.path.join(SAMPLES, name)
    if not os.path.exists(p):
        pytest.skip("%s not generated (run host_writer/build.sh + rvx_host_mlv)" % p)
    return p


def test_real_writer_flushed_crop_file_is_structurally_sound():
    rep = mlv_check.parse(_sample("crop_flush.mlv"))

    assert rep.errors == []
    assert rep.vidf_complete == rep.mlvi["video_frame_count"] == 8
    assert (rep.rawi["xRes"], rep.rawi["yRes"]) == (1792, 896)
    assert rep.expected_frame_size == 1792 * 896 * 14 // 8
    # known deviations of raw_vidx/mlv_3.c from what classic MLV writers emit
    assert set(codes(rep, mlv_check.WARNING)) == {"VERSION", "TIMESTAMP_ABSOLUTE"}
    assert set(codes(rep, mlv_check.INFO)) == {"RAWI_GEOMETRY", "RAWI_ACTIVE_AREA"}


def test_real_writer_full_frame_file_is_structurally_sound():
    rep = mlv_check.parse(_sample("full_flush.mlv"))

    assert rep.errors == []
    assert rep.vidf_complete == 5
    assert rep.expected_frame_size == 4269552


def test_real_writer_camera_stop_path_loses_buffered_frames():
    # worker.c COMMAND_STOP does not flush the current crop buffer
    rep = mlv_check.parse(_sample("crop_camstop.mlv"))

    assert "FRAME_COUNT" in codes(rep, mlv_check.ERROR)
    assert rep.mlvi["video_frame_count"] == 8
    assert rep.vidf_complete == 5


def test_real_writer_crash_with_torn_tail():
    path = _sample("crop_crash_cut.mlv")
    rep = mlv_check.parse(path)

    assert rep.truncated is True
    assert rep.vidf_complete == 9 and rep.vidf_partial == 1
    assert {"TRUNCATED_BLOCK", "FRAME_COUNT_UNFINALISED"} <= set(codes(rep, mlv_check.ERROR))
    assert mlv_check.main([path, "--crash-ok"]) == 0
