"""Unit tests for raw_dng.py — host-side decoder self-test, no hardware needed.

Round-trip tests prove the bit-pack/unpack is self-consistent. test_green_match uses the
established 'unpack-correct' oracle: on a real RGGB frame the two green CFA sites must read
near-identical mean levels — if they diverge, the unpack is wrong.
"""

import os

import numpy as np
import pytest

import raw_dng

CAPTURE = os.path.join(os.path.dirname(__file__), "captures", "first_raw_capture.raw")
CAP_WIDTH = 2096
CAP_HEIGHT = 1164


def _random_image(height, width, bpp, seed):
    rng = np.random.default_rng(seed)
    return rng.integers(0, 1 << bpp, size=(height, width), dtype=np.uint16)


# --- round-trip: pack -> unpack must be bit-exact ---------------------------

def test_roundtrip_14():
    img = _random_image(64, 48, 14, seed=14) & 0x3FFF
    packed = raw_dng.pack_14bit(img)
    out = raw_dng.unpack_14bit(packed, 48, 64, 48 * 14 // 8)
    assert np.array_equal(img, out)


def test_roundtrip_10():
    img = _random_image(64, 48, 10, seed=10)
    packed = raw_dng.pack_nbit(img, 10)
    out = raw_dng.unpack_nbit(packed, 48, 64, 10)
    assert np.array_equal(img, out)


def test_roundtrip_12():
    img = _random_image(64, 48, 12, seed=12)
    packed = raw_dng.pack_nbit(img, 12)
    out = raw_dng.unpack_nbit(packed, 48, 64, 12)
    assert np.array_equal(img, out)


def test_nbit_14_matches_dedicated_unpacker():
    # the general path at bpp=14 must agree with the hand-written unpack_14bit
    img = _random_image(64, 48, 14, seed=99) & 0x3FFF
    packed = raw_dng.pack_14bit(img)
    assert np.array_equal(
        raw_dng.unpack_nbit(packed, 48, 64, 14),
        raw_dng.unpack_14bit(packed, 48, 64, 48 * 14 // 8),
    )


# --- real-frame oracle: the two RGGB green sites must match -----------------

def _cfa_means(img, cfa_name):
    """Per-CFA mean levels, mirroring raw_dng.print_stats sub-sampling logic."""
    cfa = raw_dng.CFA_PATTERNS[cfa_name]
    subs = {
        (0, 0): img[0::2, 0::2], (0, 1): img[0::2, 1::2],
        (1, 0): img[1::2, 0::2], (1, 1): img[1::2, 1::2],
    }
    order = [(0, 0), (0, 1), (1, 0), (1, 1)]
    names = ["R", "G", "B"]
    return [(names[cfa[i]], pos, float(subs[pos].mean())) for i, pos in enumerate(order)]


def test_green_match():
    if not os.path.exists(CAPTURE):
        pytest.skip("captures/first_raw_capture.raw missing — skipping real-frame oracle test")
    with open(CAPTURE, "rb") as f:
        data = f.read()
    img = raw_dng.unpack_14bit(data, CAP_WIDTH, CAP_HEIGHT, CAP_WIDTH * 14 // 8)

    # RGGB tile: G1 at (0,1), G2 at (1,0)
    g1 = float(img[0::2, 1::2].mean())
    g2 = float(img[1::2, 0::2].mean())
    rel = abs(g1 - g2) / ((g1 + g2) / 2.0)
    assert rel < 0.03, "green means diverge %.3f%% (G1=%.1f G2=%.1f) — unpack likely wrong" % (
        100 * rel, g1, g2)


def test_green_labelled_as_green_in_cfa():
    if not os.path.exists(CAPTURE):
        pytest.skip("captures/first_raw_capture.raw missing — skipping real-frame oracle test")
    with open(CAPTURE, "rb") as f:
        data = f.read()
    img = raw_dng.unpack_14bit(data, CAP_WIDTH, CAP_HEIGHT, CAP_WIDTH * 14 // 8)
    means = _cfa_means(img, "RGGB")
    # positions (0,1) and (1,0) are the two greens in RGGB
    assert means[1][0] == "G" and means[1][1] == (0, 1)
    assert means[2][0] == "G" and means[2][1] == (1, 0)


if __name__ == "__main__":
    import sys
    sys.exit(pytest.main([__file__, "-v"]))
