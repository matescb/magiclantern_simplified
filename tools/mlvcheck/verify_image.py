#!/usr/bin/env python3
"""
verify_image.py -- pixel-level check of an MLV written by the raw_vidx writer.

Needs numpy (unlike mlv_check.py) and reuses tools/mlserial/raw_dng.py for the
14-bit unpacker and the DNG writer.

  1. unpack the source LiveView frame (the camera raw-buffer dump)
  2. unpack every complete VIDF payload of the MLV (per RAWI xRes/yRes)
  3. compare each frame with source[y:y+h, x:x+w]  -> must be identical
  4. optionally compare the DNGs mlv_dump extracted (--dng-glob) with the same
     reference (decodes the DNG strip itself: 16-bit LE, or 14-bit packed BE)
  5. print per-channel statistics; exit 1 on any mismatch

usage:
  verify_image.py --src first_raw_capture.raw --mlv out.mlv [--x 200 --y 100]
                  [--src-w 2096 --src-h 1164] [--dng-glob 'dir/prefix_*.dng']
                  [--write-dng frame0.dng]
"""

import argparse
import glob
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "mlserial"))

import mlv_check  # noqa: E402
import raw_dng    # noqa: E402


def cfa_stats(img, label, black, white):
    names = {(0, 0): "R ", (0, 1): "G1", (1, 0): "G2", (1, 1): "B "}
    print("  %s: %dx%d min=%d max=%d mean=%.1f  <=black %.2f%%  >=white %.2f%%  zero %.3f%%"
          % (label, img.shape[1], img.shape[0], img.min(), img.max(), img.mean(),
             100.0 * (img <= black).mean(), 100.0 * (img >= white).mean(),
             100.0 * (img == 0).mean()))
    for (r, c), n in sorted(names.items()):
        s = img[r::2, c::2]
        print("    %s min=%5d max=%5d mean=%8.1f std=%7.1f" % (n, s.min(), s.max(), s.mean(), s.std()))


def read_dng(path):
    """Minimal TIFF reader for the DNGs mlv_dump writes: returns (img, tags)."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"II*\0":
        raise ValueError("%s: not a little-endian TIFF" % path)
    tags = {}

    def read_ifd(off):
        n = struct.unpack_from("<H", d, off)[0]
        out = {}
        for i in range(n):
            tag, typ, cnt, val = struct.unpack_from("<HHII", d, off + 2 + i * 12)
            if typ == 3 and cnt == 1:
                val &= 0xFFFF
            out[tag] = (typ, cnt, val)
        return out

    ifd = read_ifd(struct.unpack_from("<I", d, 4)[0])
    # mlv_dump puts the raw image either in IFD0 or in a SubIFD (tag 330)
    if 330 in ifd and ifd.get(254, (0, 0, 0))[2] == 1:
        sub = read_ifd(ifd[330][2])
        tags.update(ifd)
        ifd = sub
    tags.update(ifd)
    w, h = tags[256][2], tags[257][2]
    bps = tags[258][2]
    off = tags[273][2]
    if bps == 16:
        img = np.frombuffer(d, dtype="<u2", count=w * h, offset=off).reshape(h, w)
    else:
        # DNG packs samples MSB-first as a continuous bit stream
        nbytes = (w * h * bps + 7) // 8
        bits = np.unpackbits(np.frombuffer(d, dtype=np.uint8, count=nbytes, offset=off))
        bits = bits[:w * h * bps].reshape(w * h, bps).astype(np.uint32)
        weights = (1 << np.arange(bps - 1, -1, -1)).astype(np.uint32)
        img = (bits * weights).sum(axis=1).astype(np.uint16).reshape(h, w)
    return img, {"width": w, "height": h, "bps": bps,
                 "black": tags.get(50714, (0, 0, None))[2],
                 "white": tags.get(50717, (0, 0, None))[2],
                 "compression": tags.get(259, (0, 0, None))[2]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--mlv", required=True)
    ap.add_argument("--src-w", type=int, default=2096)
    ap.add_argument("--src-h", type=int, default=1164)
    ap.add_argument("--x", type=int, default=200)
    ap.add_argument("--y", type=int, default=100)
    ap.add_argument("--dng-glob")
    ap.add_argument("--write-dng", help="write frame 0 as DNG with raw_dng.write_dng()")
    args = ap.parse_args()

    bad = 0
    with open(args.src, "rb") as f:
        src = raw_dng.unpack_14bit(f.read(), args.src_w, args.src_h, args.src_w * 14 // 8)

    rep = mlv_check.parse(args.mlv)
    if not rep.rawi:
        print("no RAWI block in %s" % args.mlv)
        return 1
    w, h = rep.rawi["xRes"], rep.rawi["yRes"]
    black, white = rep.rawi["black_level"], rep.rawi["white_level"]
    # worker.c: src_x bytes = (x+7)/8*BPP  -> pixels (x+7)/8*8 ; src_y = y/2*2
    x = (args.x + 7) // 8 * 8
    y = args.y // 2 * 2
    ref = src[y:y + h, x:x + w]
    print("reference: source %dx%d cropped to %dx%d at x=%d y=%d" % (args.src_w, args.src_h, w, h, x, y))
    cfa_stats(src, "source ", black, white)
    cfa_stats(ref, "ref crop", black, white)

    frames = []
    for i in range(rep.vidf_complete):
        payload = mlv_check.frame_payload(args.mlv, rep, i)
        if len(payload) != w * h * 14 // 8:
            print("frame %d: payload %d bytes != %d -> MISMATCH" % (i, len(payload), w * h * 14 // 8))
            bad += 1
            continue
        img = raw_dng.unpack_14bit(payload, w, h, w * 14 // 8)
        frames.append(img)
        same = np.array_equal(img, ref)
        if not same:
            bad += 1
        if i == 0 or not same:
            cfa_stats(img, "MLV frame %d" % i, black, white)
        print("MLV frame %d vs reference crop: %s" % (i, "IDENTICAL" if same else
              "DIFFERENT (%d px)" % int((img != ref).sum())))

    if args.write_dng and frames:
        raw_dng.write_dng(args.write_dng, frames[0], w, h, black, white, "RGGB", "Canon EOS M50m2")
        print("wrote %s via raw_dng.write_dng" % args.write_dng)

    if args.dng_glob:
        paths = sorted(glob.glob(args.dng_glob))
        if not paths:
            print("no DNG matches %s" % args.dng_glob)
            bad += 1
        for p in paths:
            img, t = read_dng(p)
            same = img.shape == ref.shape and np.array_equal(img, ref)
            if not same:
                bad += 1
            print("%s: %dx%d %d bps comp=%s black=%s white=%s -> %s"
                  % (os.path.basename(p), t["width"], t["height"], t["bps"], t["compression"],
                     t["black"], t["white"], "IDENTICAL to reference" if same else "DIFFERENT"))
            if p == paths[0] or not same:
                cfa_stats(img, "DNG", black, white)

    print("RESULT: %s" % ("OK" if not bad else "%d MISMATCH(ES)" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
