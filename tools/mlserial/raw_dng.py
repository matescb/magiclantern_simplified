#!/usr/bin/env python3
"""
raw_dng.py -- host-side unpacker: ML 14-bit packed raw buffer  ->  DNG (+ optional PGM).

Purpose (the R2 "prove the buffer" gate, docs/31 & docs/32):
    We have NEVER rendered a single frame from the M50 II raw buffer. save_dng() on the
    camera only ever wrote 0 bytes, and the record path crashes before any frame lands.
    This tool sidesteps the whole camera FIO / record blocker: dump the live raw buffer
    over the serial harness, unpack it HERE, and write a DNG you can open on the desktop.
    If it's a real Bayer image with sane levels -> the buffer/geometry/pack are CONFIRMED
    and every downstream record effort rests on proven ground. If it's garbage -> we found
    the real problem before burning more time on the record path.

How to produce the input .bin (live camera, no rebuild):
    1. Camera in MOVIE mode, LiveView showing (standby). Arm raw so the stream is up:
         mlserial call lv_set_mm 1
         mlserial call lv_save_raw 1              # ch2 ram_addr (0xd04202a0) now valid
       (or trigger raw_lv_request() via the harness -- see docs/31 Phase B)
    2. Read the buffer pointer, then dump a full frame (2096x1164x14/8 = 4269552 bytes):
         mlserial dump 0x6BC61CC8 0x4125F0 frame.bin
       (0x6BC61CC8 is the uncached buffer; raw_info.buffer mirror = 0x2BC61CC8. Re-read the
        ram_addr live -- it can move between boots. See docs/32 per-boot resolution.)
    3. Unpack + build the DNG:
         python3 raw_dng.py frame.bin -o frame.dng --pgm frame.pgm

Bit layout is the ground truth from magiclantern_simplified/src/raw.h (the raw_pixblock
comment block) + the PA..PH macros in src/raw.c: 8 pixels packed into 14 bytes, stored as
7 little-endian 16-bit words per group. Even lines start Red, odd lines start Green -> RGGB.
"""

import argparse
import struct
import sys

import numpy as np

# ---- defaults: the live-confirmed M50 II raw_info (docs/31 Phase B, docs/32) ----------
DEF_WIDTH = 2096
DEF_HEIGHT = 1164
DEF_BITS = 14
DEF_BLACK = 2047
DEF_WHITE = 16200
DEF_MODEL = "Canon EOS M50m2"

# dcraw adobe_coeff for the Canon EOS M50 (XYZ_D65 -> camera, x1e4). The M50 II sensor is
# the same family; this is approximate and only affects color rendering, not whether the
# frame decodes as a real image. Override is not exposed -- edit here if you get a real DCP.
COLOR_MATRIX1 = [8532, -701, -1167, -4095, 11879, 2508, -797, 2424, 7010]

# CFA phase name -> 4 CFAPattern bytes (R=0, G=1, B=2), row-major over the 2x2 tile.
CFA_PATTERNS = {
    "RGGB": [0, 1, 1, 2],
    "BGGR": [2, 1, 1, 0],
    "GRBG": [1, 0, 2, 1],
    "GBRG": [1, 2, 0, 1],
}


def unpack_14bit(data, width, height, pitch):
    """ML 14-bit packed raw -> uint16 image (height, width).

    Layout (src/raw.h): each 14-byte group holds 8 pixels a..h. Read the group as 7
    little-endian uint16 words w0..w6 (raw.h shows the words are 16-bit big-endian
    conceptually but stored low-byte-first in memory, i.e. a plain LE u16 read), then:

        a = w0 >> 2
        b = ((w0 & 0x0003) << 12) | (w1 >> 4)
        c = ((w1 & 0x000F) << 10) | (w2 >> 6)
        d = ((w2 & 0x003F) <<  8) | (w3 >> 8)
        e = ((w3 & 0x00FF) <<  6) | (w4 >> 10)
        f = ((w4 & 0x03FF) <<  4) | (w5 >> 12)
        g = ((w5 & 0x0FFF) <<  2) | (w6 >> 14)
        h =   w6 & 0x3FFF

    This mirrors the PA..PH macros in src/raw.c exactly.
    """
    if width % 8 != 0:
        raise ValueError("width %d is not a multiple of 8 (ML packs 8 pixels/group)" % width)
    blocks = width // 8            # 14-byte groups per row (2096 -> 262)
    row_bytes = blocks * 14        # packed bytes actually consumed per row (== pitch, no pad)

    need = height * pitch
    buf = np.frombuffer(data, dtype=np.uint8)
    if buf.size < need:
        sys.stderr.write("WARNING: input %d bytes < expected %d; zero-padding.\n"
                         % (buf.size, need))
        buf = np.concatenate([buf, np.zeros(need - buf.size, np.uint8)])
    elif buf.size > need:
        sys.stderr.write("NOTE: input %d bytes > expected %d; truncating.\n"
                         % (buf.size, need))
        buf = buf[:need]

    rows = buf.reshape(height, pitch)[:, :row_bytes]        # drop any row padding
    b = rows.reshape(height, blocks, 7, 2).astype(np.uint32)
    w = b[..., 0] | (b[..., 1] << 8)                        # little-endian u16 -> (h, blocks, 7)
    w0, w1, w2, w3, w4, w5, w6 = (w[..., i] for i in range(7))

    px = np.empty((height, blocks, 8), dtype=np.uint16)
    px[..., 0] = w0 >> 2
    px[..., 1] = ((w0 & 0x0003) << 12) | (w1 >> 4)
    px[..., 2] = ((w1 & 0x000F) << 10) | (w2 >> 6)
    px[..., 3] = ((w2 & 0x003F) << 8) | (w3 >> 8)
    px[..., 4] = ((w3 & 0x00FF) << 6) | (w4 >> 10)
    px[..., 5] = ((w4 & 0x03FF) << 4) | (w5 >> 12)
    px[..., 6] = ((w5 & 0x0FFF) << 2) | (w6 >> 14)
    px[..., 7] = w6 & 0x3FFF
    return px.reshape(height, width)


def pack_14bit(img):
    """EXACT inverse of unpack_14bit: uint16 image (height, width) -> ML 14-bit packed bytes.

    Produces blocks*14 packed bytes per row (no row padding, i.e. pitch == width*14/8), so
    unpack_14bit(pack_14bit(img), width, height, width*14//8) == (img & 0x3FFF) bit-exact.

    Inverts the PA..PH algebra: given 8 pixels a..h (each 14-bit), the 7 LE16 words are

        w0 = (a << 2) | (b >> 12)
        w1 = ((b & 0x0FFF) << 4) | (c >> 10)
        w2 = ((c & 0x03FF) << 6) | (d >>  8)
        w3 = ((d & 0x00FF) << 8) | (e >>  6)
        w4 = ((e & 0x003F) << 10) | (f >> 4)
        w5 = ((f & 0x000F) << 12) | (g >> 2)
        w6 = ((g & 0x0003) << 14) | h
    """
    img = np.asarray(img)
    if img.ndim != 2:
        raise ValueError("img must be 2D (height, width)")
    height, width = img.shape
    if width % 8 != 0:
        raise ValueError("width %d is not a multiple of 8 (ML packs 8 pixels/group)" % width)
    blocks = width // 8
    px = (img.astype(np.uint32) & 0x3FFF).reshape(height, blocks, 8)
    a, b, c, d, e, f, g, h = (px[..., i] for i in range(8))

    words = np.empty((height, blocks, 7), dtype=np.uint32)
    words[..., 0] = (a << 2) | (b >> 12)
    words[..., 1] = ((b & 0x0FFF) << 4) | (c >> 10)
    words[..., 2] = ((c & 0x03FF) << 6) | (d >> 8)
    words[..., 3] = ((d & 0x00FF) << 8) | (e >> 6)
    words[..., 4] = ((e & 0x003F) << 10) | (f >> 4)
    words[..., 5] = ((f & 0x000F) << 12) | (g >> 2)
    words[..., 6] = ((g & 0x0003) << 14) | h
    return (words & 0xFFFF).astype("<u2").tobytes()


def _validate_nbit(bpp, width):
    if bpp not in (10, 12, 14):
        raise ValueError("bpp %r not supported (only 10, 12, 14)" % (bpp,))
    if (width * bpp) % 16 != 0:
        raise ValueError(
            "width*bpp must be a multiple of 16 for LE16-word packing "
            "(width=%d, bpp=%d -> %d bits/row)" % (width, bpp, width * bpp))
    return width * bpp // 8            # packed bytes per row


def unpack_nbit(buf, width, height, bpp):
    """General MSB-first LE16-word unpacker for bpp in {10,12,14} -> uint16 image (height, width).

    Same bitstream scheme as unpack_14bit generalized to bpp: each row is width*bpp/8 packed
    bytes stored as little-endian 16-bit words; byteswap the words to a big-endian byte stream,
    read that stream MSB-first, and slice off bpp bits per pixel. For bpp=14 this is identical
    to unpack_14bit(data, width, height, width*14//8).
    """
    row_bytes = _validate_nbit(bpp, width)
    need = height * row_bytes
    data = np.frombuffer(buf, dtype=np.uint8)
    if data.size < need:
        sys.stderr.write("WARNING: input %d bytes < expected %d; zero-padding.\n"
                         % (data.size, need))
        data = np.concatenate([data, np.zeros(need - data.size, np.uint8)])
    elif data.size > need:
        data = data[:need]

    rows = data.reshape(height, row_bytes)
    # byteswap LE16 words -> big-endian byte order == MSB-first bitstream
    swapped = rows.reshape(height, row_bytes // 2, 2)[:, :, ::-1].reshape(height, row_bytes)
    bits = np.unpackbits(swapped, axis=1).reshape(height, width, bpp)
    weights = (1 << np.arange(bpp - 1, -1, -1, dtype=np.uint32))
    return (bits * weights).sum(axis=2).astype(np.uint16)


def pack_nbit(img, bpp):
    """EXACT inverse of unpack_nbit for bpp in {10,12,14}: uint16 image -> packed bytes.

    Builds the MSB-first bitstream (bpp bits/pixel), packs it big-endian, then byteswaps back
    to little-endian 16-bit word storage. unpack_nbit(pack_nbit(img, bpp), w, h, bpp) round-trips
    bit-exact for values within bpp bits (input is masked to 2**bpp-1).
    """
    img = np.asarray(img)
    if img.ndim != 2:
        raise ValueError("img must be 2D (height, width)")
    height, width = img.shape
    row_bytes = _validate_nbit(bpp, width)
    px = (img.astype(np.uint32) & ((1 << bpp) - 1))
    shifts = np.arange(bpp - 1, -1, -1, dtype=np.uint32)
    bits = ((px[:, :, None] >> shifts) & 1).astype(np.uint8).reshape(height, width * bpp)
    packed = np.packbits(bits, axis=1)          # (height, row_bytes) big-endian byte stream
    # byteswap big-endian byte stream -> little-endian 16-bit word storage
    swapped = packed.reshape(height, row_bytes // 2, 2)[:, :, ::-1].reshape(height, row_bytes)
    return swapped.tobytes()


def _encode(typ, values):
    """Return (count, raw_bytes) for a TIFF tag value list."""
    if typ == 2:  # ASCII (values is a str)
        raw = values.encode("ascii") + b"\x00"
        return len(raw), raw
    if typ in (1, 7):  # BYTE / UNDEFINED
        return len(values), bytes(values)
    if typ == 3:  # SHORT
        return len(values), b"".join(struct.pack("<H", v) for v in values)
    if typ == 4:  # LONG
        return len(values), b"".join(struct.pack("<I", v) for v in values)
    if typ == 5:  # RATIONAL (values is list of (num, den))
        return len(values), b"".join(struct.pack("<II", n, d) for n, d in values)
    if typ == 10:  # SRATIONAL
        return len(values), b"".join(struct.pack("<ii", n, d) for n, d in values)
    raise ValueError("unhandled TIFF type %d" % typ)


def _build_ifd(tags, ext_base):
    """Serialize one IFD. tags = list of (tag, type, values) ascending by tag id.
    ext_base = file offset where this IFD's overflow data region begins."""
    entries = bytearray()
    ext = bytearray()
    for tag, typ, values in tags:
        count, raw = _encode(typ, values)
        if len(raw) <= 4:
            val = raw + b"\x00" * (4 - len(raw))
        else:
            if len(ext) % 2:
                ext += b"\x00"
            val = struct.pack("<I", ext_base + len(ext))
            ext += raw
        entries += struct.pack("<HHI", tag, typ, count) + val
    ifd = struct.pack("<H", len(tags)) + bytes(entries) + struct.pack("<I", 0)
    return ifd, bytes(ext)


def write_dng(path, img16, width, height, black, white, cfa_name, model):
    cfa = CFA_PATTERNS[cfa_name]
    strip = img16.astype("<u2").tobytes()
    strip_bytes = len(strip)

    def tags(strip_offset):
        return [
            (254, 4, [0]),                                   # NewSubfileType
            (256, 4, [width]),                               # ImageWidth
            (257, 4, [height]),                              # ImageLength
            (258, 3, [16]),                                  # BitsPerSample
            (259, 3, [1]),                                   # Compression = none
            (262, 3, [32803]),                               # PhotometricInterpretation = CFA
            (271, 2, "Canon"),                               # Make
            (272, 2, model),                                 # Model
            (273, 4, [strip_offset]),                        # StripOffsets
            (274, 3, [1]),                                   # Orientation
            (277, 3, [1]),                                   # SamplesPerPixel
            (278, 4, [height]),                              # RowsPerStrip
            (279, 4, [strip_bytes]),                         # StripByteCounts
            (284, 3, [1]),                                   # PlanarConfiguration
            (305, 2, "mlserial raw_dng"),                    # Software
            (33421, 3, [2, 2]),                              # CFARepeatPatternDim
            (33422, 1, cfa),                                 # CFAPattern
            (50706, 1, [1, 4, 0, 0]),                        # DNGVersion
            (50707, 1, [1, 3, 0, 0]),                        # DNGBackwardVersion
            (50708, 2, model),                               # UniqueCameraModel
            (50711, 3, [1]),                                 # CFALayout = rectangular
            (50714, 3, [black]),                             # BlackLevel
            (50717, 4, [white]),                             # WhiteLevel
            (50721, 10, [(v, 10000) for v in COLOR_MATRIX1]),  # ColorMatrix1
            (50728, 5, [(52, 100), (100, 100), (63, 100)]),  # AsShotNeutral (daylight-ish)
            (50778, 3, [21]),                                # CalibrationIlluminant1 = D65
        ]

    # IFD length is fixed; ext length is independent of strip_offset -> compute, then place.
    ifd0, _ = _build_ifd(tags(0), 0)
    ext_base = 8 + len(ifd0)
    _, ext = _build_ifd(tags(0), ext_base)
    strip_offset = ext_base + len(ext)
    if strip_offset % 2:
        strip_offset += 1
    ifd0, ext = _build_ifd(tags(strip_offset), ext_base)
    pad = b"\x00" * (strip_offset - (ext_base + len(ext)))

    with open(path, "wb") as f:
        f.write(struct.pack("<2sHI", b"II", 42, 8))   # TIFF header, IFD0 at offset 8
        f.write(ifd0)
        f.write(ext)
        f.write(pad)
        f.write(strip)


def write_pgm(path, img16):
    """16-bit grayscale PGM (P5, big-endian per spec). Undemosaiced -- shows exposure/structure."""
    h, w = img16.shape
    maxv = int(img16.max())
    with open(path, "wb") as f:
        f.write(("P5\n%d %d\n%d\n" % (w, h, max(maxv, 1))).encode("ascii"))
        f.write(img16.astype(">u2").tobytes())


def print_stats(img, cfa_name, black, white):
    cfa = CFA_PATTERNS[cfa_name]
    names = ["R", "G", "B"]
    print("--- buffer stats (is this a real frame?) ---------------------------")
    print("  overall: min=%d max=%d mean=%.1f  (expect black~%d, white~%d)"
          % (img.min(), img.max(), img.mean(), black, white))
    # 2x2 CFA sub-sampling
    subs = {(0, 0): img[0::2, 0::2], (0, 1): img[0::2, 1::2],
            (1, 0): img[1::2, 0::2], (1, 1): img[1::2, 1::2]}
    order = [(0, 0), (0, 1), (1, 0), (1, 1)]
    for i, pos in enumerate(order):
        s = subs[pos]
        print("  %s @tile%s: min=%d max=%d mean=%.1f"
              % (names[cfa[i]], pos, s.min(), s.max(), s.mean()))
    frac_black = float((img <= black).mean())
    frac_clip = float((img >= white).mean())
    zeros = float((img == 0).mean())
    print("  %%<=black=%.1f%%  %%>=white=%.1f%%  %%exact-zero=%.1f%%"
          % (100 * frac_black, 100 * frac_clip, 100 * zeros))
    if zeros > 0.9:
        print("  !! >90%% exact-zero: buffer likely NOT populated (stream not armed?)")
    elif img.max() - img.min() < 64:
        print("  !! near-constant buffer: no real image content")
    else:
        print("  -> varied content present; open the DNG/PGM to confirm it's a real image.")
    print("--------------------------------------------------------------------")


def main():
    ap = argparse.ArgumentParser(
        description="Unpack an ML 14-bit packed raw buffer dump into a DNG.",
        epilog="Produce input with: mlserial dump 0x6BC61CC8 0x4125F0 frame.bin "
               "(arm raw in movie-LV standby first).")
    ap.add_argument("input", help="packed raw buffer dump (.bin from `mlserial dump`)")
    ap.add_argument("-o", "--out", help="output DNG path (default: <input>.dng)")
    ap.add_argument("--pgm", help="also write a 16-bit grayscale PGM here (quick sanity view)")
    ap.add_argument("--width", type=int, default=DEF_WIDTH)
    ap.add_argument("--height", type=int, default=DEF_HEIGHT)
    ap.add_argument("--bits", type=int, default=DEF_BITS, help="packed bit depth (only 14 supported)")
    ap.add_argument("--pitch", type=int, default=0, help="row stride in bytes (default width*bits/8)")
    ap.add_argument("--black", type=int, default=DEF_BLACK)
    ap.add_argument("--white", type=int, default=DEF_WHITE)
    ap.add_argument("--cfa", default="RGGB", choices=sorted(CFA_PATTERNS), help="Bayer phase at (0,0)")
    ap.add_argument("--model", default=DEF_MODEL)
    args = ap.parse_args()

    if args.bits != 14:
        ap.error("only --bits 14 is supported (ML raw_pixblock packing)")
    pitch = args.pitch or (args.width * args.bits // 8)
    out = args.out or (args.input.rsplit(".", 1)[0] + ".dng")

    with open(args.input, "rb") as f:
        data = f.read()

    print("input=%s (%d bytes)  geometry=%dx%d  pitch=%d  black=%d white=%d cfa=%s"
          % (args.input, len(data), args.width, args.height, pitch, args.black, args.white, args.cfa))
    img = unpack_14bit(data, args.width, args.height, pitch)
    print_stats(img, args.cfa, args.black, args.white)

    write_dng(out, img, args.width, args.height, args.black, args.white, args.cfa, args.model)
    print("wrote DNG -> %s" % out)
    if args.pgm:
        write_pgm(args.pgm, img)
        print("wrote PGM -> %s" % args.pgm)


if __name__ == "__main__":
    main()
