#!/usr/bin/env python3
"""
mlv_check.py -- dependency-free structural validator for Magic Lantern MLV files.

Walks the block chain and reports:
  * MLVI header fields (version, GUID, fps, classes, declared frame count)
  * RAWI geometry / levels, RAWC, IDNT, EXPO, LENS, RTCI, WBAL
  * every block: offset, type, size, timestamp
  * VIDF frames: number, padding, payload size vs xRes*yRes*bpp/8
  * frame count in the file vs the count declared in MLVI
  * a truncated tail (last block cut off, or a partial block header)
  * garbage between blocks (resyncs on the next plausible block)

It never raises on a damaged file: a recording whose final block is cut off
(camera crashed before close) is parsed up to the cut and the cut is reported.

Exit status:
    0  no inconsistencies (warnings / infos may be present)
    1  inconsistencies found (errors), or warnings with --strict
    2  not an MLV file / unreadable
With --crash-ok the two findings that are *expected* after a crash (truncated
tail, MLVI frame count never finalised) are downgraded to warnings, so the exit
status tells you whether the part that did reach the card is sound.

Usage:
    mlv_check.py FILE.MLV [-v] [--json] [--strict] [--crash-ok]
                          [--extract N OUT.raw] [--stats]

Only the Python standard library is used.
"""

import argparse
import json
import os
import struct
import sys

# ---------------------------------------------------------------- constants

KNOWN_TYPES = (
    b"MLVI", b"VIDF", b"AUDF", b"RAWI", b"RAWC", b"WAVI", b"EXPO", b"LENS",
    b"RTCI", b"IDNT", b"XREF", b"INFO", b"DISO", b"MARK", b"STYL", b"ELVL",
    b"WBAL", b"DEBG", b"VERS", b"NULL", b"BKUP", b"DARK", b"FLAT",
)

MLVI_SIZE = 0x34
VIDF_HDR_SIZE = 0x20
AUDF_HDR_SIZE = 0x18
RAWI_SIZE = 0xB4
MIN_BLOCK_SIZE = 0x10            # type + size + timestamp
MAX_BLOCK_SIZE = 0x7FFFFFFF

VIDEO_CLASS_RAW = 0x01
VIDEO_FLAG_LZMA = 0x80
VIDEO_FLAG_DELTA = 0x40
VIDEO_FLAG_LJ92 = 0x20

# A VIDF timestamp this far (in us) beyond the last metadata block, for the very
# first frame, means the writer did not make it relative to recording start.
ABS_TIMESTAMP_SLACK_US = 10 * 1000 * 1000

ERROR = "error"
WARNING = "warning"
INFO = "info"        # normal for ML writers, shown for context only


def _cstr(raw):
    return raw.split(b"\0", 1)[0].decode("latin-1", "replace")


class Issue(object):
    def __init__(self, severity, code, message, offset=None):
        self.severity = severity
        self.code = code
        self.message = message
        self.offset = offset

    def as_dict(self):
        return {"severity": self.severity, "code": self.code,
                "message": self.message, "offset": self.offset}

    def __str__(self):
        where = "" if self.offset is None else " @0x%08x" % self.offset
        return "%-7s %s%s: %s" % (self.severity.upper(), self.code, where, self.message)


class Block(object):
    def __init__(self, index, offset, btype, size, timestamp, fields, complete, present):
        self.index = index
        self.offset = offset
        self.type = btype
        self.size = size                # declared size
        self.timestamp = timestamp      # None for MLVI
        self.fields = fields
        self.complete = complete        # whole block is present in the file
        self.present = present          # bytes of the block present in the file

    def as_dict(self):
        return {"index": self.index, "offset": self.offset, "type": self.type,
                "size": self.size, "timestamp": self.timestamp,
                "complete": self.complete, "present": self.present,
                "fields": self.fields}


class Report(object):
    def __init__(self, path):
        self.path = path
        self.file_size = 0
        self.blocks = []
        self.issues = []
        self.mlvi = None
        self.rawi = None
        self.vidf_complete = 0
        self.vidf_partial = 0
        self.truncated = False
        self.truncated_missing = 0
        self.expected_frame_size = None

    # -- issue helpers
    def error(self, code, message, offset=None):
        self.issues.append(Issue(ERROR, code, message, offset))

    def warn(self, code, message, offset=None):
        self.issues.append(Issue(WARNING, code, message, offset))

    def info(self, code, message, offset=None):
        self.issues.append(Issue(INFO, code, message, offset))

    @property
    def errors(self):
        return [i for i in self.issues if i.severity == ERROR]

    @property
    def warnings(self):
        return [i for i in self.issues if i.severity == WARNING]

    def codes(self, severity=None):
        return [i.code for i in self.issues if severity in (None, i.severity)]

    def exit_code(self, strict=False):
        if "NOT_MLV" in self.codes() or "UNREADABLE" in self.codes():
            return 2
        if self.errors or (strict and self.warnings):
            return 1
        return 0

    def as_dict(self):
        return {
            "path": self.path, "file_size": self.file_size,
            "mlvi": self.mlvi, "rawi": self.rawi,
            "vidf_complete": self.vidf_complete, "vidf_partial": self.vidf_partial,
            "truncated": self.truncated, "truncated_missing": self.truncated_missing,
            "expected_frame_size": self.expected_frame_size,
            "blocks": [b.as_dict() for b in self.blocks],
            "issues": [i.as_dict() for i in self.issues],
        }


# ---------------------------------------------------------------- block parsers

def _parse_mlvi(d):
    (ver, guid, fnum, fcount, flags, vclass, aclass, vframes, aframes,
     fps_nom, fps_denom) = struct.unpack_from("<8sQHHIHHIIII", d, 8)
    return {
        "version": _cstr(ver), "guid": guid, "file_num": fnum, "file_count": fcount,
        "file_flags": flags, "video_class": vclass, "audio_class": aclass,
        "video_frame_count": vframes, "audio_frame_count": aframes,
        "fps_nom": fps_nom, "fps_denom": fps_denom,
    }


def _parse_rawi(d):
    xres, yres = struct.unpack_from("<HH", d, 0x10)
    v = struct.unpack_from("<Ii" + "i" * 38, d, 0x14)
    return {
        "xRes": xres, "yRes": yres,
        "api_version": v[0], "buffer": v[1] & 0xFFFFFFFF,
        "height": v[2], "width": v[3], "pitch": v[4], "frame_size": v[5],
        "bits_per_pixel": v[6], "black_level": v[7], "white_level": v[8],
        "jpeg": {"x": v[9], "y": v[10], "width": v[11], "height": v[12]},
        "active_area": {"y1": v[13], "x1": v[14], "y2": v[15], "x2": v[16]},
        "exposure_bias": [v[17], v[18]],
        "cfa_pattern": v[19] & 0xFFFFFFFF,
        "calibration_illuminant1": v[20],
        "color_matrix1": list(v[21:39]),
        "dynamic_range": v[39],
    }


def _parse_rawc(d):
    sx, sy, crop, _res, bx, skx, by, sky, ox, oy = struct.unpack_from("<HHHHBBBBhh", d, 0x10)
    return {"sensor_res_x": sx, "sensor_res_y": sy, "sensor_crop": crop,
            "binning_x": bx, "skipping_x": skx, "binning_y": by, "skipping_y": sky,
            "offset_x": ox, "offset_y": oy}


def _parse_idnt(d):
    name, model, serial = struct.unpack_from("<32sI32s", d, 0x10)
    return {"camera_name": _cstr(name), "camera_model": model, "camera_serial": _cstr(serial)}


def _parse_expo(d):
    mode, iso, analog, dgain, shutter = struct.unpack_from("<IIIIQ", d, 0x10)
    return {"iso_mode": mode, "iso_value": iso, "iso_analog": analog,
            "digital_gain": dgain, "shutter_us": shutter}


def _parse_lens(d):
    fl, fd, ap, stab, af, flags, lid, name, serial = struct.unpack_from("<HHHBBII32s32s", d, 0x10)
    return {"focal_length": fl, "focal_dist": fd, "aperture": ap, "stabilizer": stab,
            "autofocus": af, "flags": flags, "lens_id": lid,
            "lens_name": _cstr(name), "lens_serial": _cstr(serial)}


def _parse_rtci(d):
    v = struct.unpack_from("<10H8s", d, 0x10)
    return {"sec": v[0], "min": v[1], "hour": v[2], "mday": v[3], "mon": v[4],
            "year": v[5], "wday": v[6], "yday": v[7], "isdst": v[8], "gmtoff": v[9],
            "zone": _cstr(v[10])}


def _parse_wbal(d):
    v = struct.unpack_from("<7I", d, 0x10)
    return {"wb_mode": v[0], "kelvin": v[1], "wbgain_r": v[2], "wbgain_g": v[3],
            "wbgain_b": v[4], "wbs_gm": v[5], "wbs_ba": v[6]}


def _parse_vidf(d):
    num, cx, cy, px, py, space = struct.unpack_from("<IHHHHI", d, 0x10)
    return {"frame_number": num, "crop_x": cx, "crop_y": cy, "pan_x": px, "pan_y": py,
            "frame_space": space}


def _parse_audf(d):
    num, space = struct.unpack_from("<II", d, 0x10)
    return {"frame_number": num, "frame_space": space}


# type -> (minimum bytes needed to decode the fixed part, parser)
PARSERS = {
    "MLVI": (MLVI_SIZE, _parse_mlvi),
    "RAWI": (RAWI_SIZE, _parse_rawi),
    "RAWC": (0x20, _parse_rawc),
    "IDNT": (0x54, _parse_idnt),
    "EXPO": (0x28, _parse_expo),
    "LENS": (0x60, _parse_lens),
    "RTCI": (0x2C, _parse_rtci),
    "WBAL": (0x2C, _parse_wbal),
    "VIDF": (VIDF_HDR_SIZE, _parse_vidf),
    "AUDF": (AUDF_HDR_SIZE, _parse_audf),
}


def _plausible_type(raw):
    if len(raw) != 4:
        return False
    return all((0x41 <= c <= 0x5A) or (0x30 <= c <= 0x39) for c in bytearray(raw))


def _resync(f, start, file_size, chunk=1 << 20):
    """Find the next offset > start that looks like a known block header."""
    pos = start + 1
    while pos < file_size:
        f.seek(pos)
        buf = f.read(chunk + 8)
        if not buf:
            return None
        best = None
        for magic in KNOWN_TYPES:
            i = buf.find(magic)
            while i != -1 and i < chunk:
                if i + 8 <= len(buf):
                    size = struct.unpack_from("<I", buf, i + 4)[0]
                    if MIN_BLOCK_SIZE <= size <= MAX_BLOCK_SIZE and size % 4 == 0:
                        if best is None or i < best:
                            best = i
                        break
                i = buf.find(magic, i + 1)
        if best is not None:
            return pos + best
        pos += chunk
    return None


# ---------------------------------------------------------------- main walk

def parse(path):
    """Parse an MLV file; always returns a Report (never raises on bad content)."""
    rep = Report(path)
    try:
        rep.file_size = os.path.getsize(path)
        f = open(path, "rb")
    except (IOError, OSError) as e:
        rep.error("UNREADABLE", str(e))
        return rep

    with f:
        head = f.read(8)
        if len(head) < 8 or head[:4] != b"MLVI":
            rep.error("NOT_MLV", "file does not start with an MLVI block", 0)
            return rep

        offset = 0
        index = 0
        while offset < rep.file_size:
            remaining = rep.file_size - offset
            if remaining < 8:
                rep.truncated = True
                rep.error("TRUNCATED_HEADER",
                          "%d stray byte(s) at end of file: block header is cut off" % remaining,
                          offset)
                break

            f.seek(offset)
            hdr = f.read(8)
            raw_type = hdr[:4]
            size = struct.unpack_from("<I", hdr, 4)[0]
            min_size = MLVI_SIZE if raw_type == b"MLVI" else MIN_BLOCK_SIZE

            if not _plausible_type(raw_type) or size < min_size or size > MAX_BLOCK_SIZE:
                nxt = _resync(f, offset, rep.file_size)
                if nxt is None:
                    rep.error("GARBAGE",
                              "invalid block header (type %r, size 0x%x); no further block "
                              "found, %d byte(s) unparsed" % (raw_type, size, remaining), offset)
                    break
                rep.error("GARBAGE",
                          "invalid block header (type %r, size 0x%x); skipped %d byte(s) to "
                          "next block" % (raw_type, size, nxt - offset), offset)
                offset = nxt
                continue

            btype = raw_type.decode("ascii")
            complete = size <= remaining
            present = min(size, remaining)
            need, parser = PARSERS.get(btype, (MIN_BLOCK_SIZE, None))
            fixed = f.read(min(max(need, MIN_BLOCK_SIZE), present) - 8)
            data = hdr + fixed

            timestamp = None
            if btype != "MLVI" and len(data) >= 0x10:
                timestamp = struct.unpack_from("<Q", data, 8)[0]

            fields = {}
            if parser is not None:
                if size < need:
                    rep.error("BLOCK_SIZE", "%s block declares size 0x%x, smaller than its "
                              "fixed header 0x%x" % (btype, size, need), offset)
                elif len(data) >= need:
                    fields = parser(data)

            block = Block(index, offset, btype, size, timestamp, fields, complete, present)
            rep.blocks.append(block)
            index += 1

            if raw_type not in KNOWN_TYPES:
                rep.warn("UNKNOWN_BLOCK", "unknown block type %s" % btype, offset)
            if size % 4:
                rep.warn("BLOCK_ALIGN", "%s block size 0x%x is not a multiple of 4"
                         % (btype, size), offset)

            if not complete:
                rep.truncated = True
                rep.truncated_missing = size - remaining
                rep.error("TRUNCATED_BLOCK",
                          "%s block #%d declares %d bytes but only %d are in the file "
                          "(%d missing)" % (btype, block.index, size, remaining, size - remaining),
                          offset)
                break
            offset += size

    _analyse(rep)
    return rep


def _analyse(rep):
    blocks = rep.blocks
    if not blocks:
        return

    # ---- MLVI
    mlvi_blocks = [b for b in blocks if b.type == "MLVI"]
    first = blocks[0]
    if first.type != "MLVI" or not first.fields:
        rep.error("NO_MLVI", "first block is not a complete MLVI header", first.offset)
        return
    if len(mlvi_blocks) > 1:
        rep.warn("MULTI_MLVI", "%d MLVI blocks in one file" % len(mlvi_blocks))
    mlvi = first.fields
    rep.mlvi = mlvi
    if first.size != MLVI_SIZE:
        rep.warn("MLVI_SIZE", "MLVI size 0x%x, expected 0x%x" % (first.size, MLVI_SIZE), 0)
    if mlvi["version"] != "v2.0":
        rep.warn("VERSION", "version string is %r (classic tools write/expect 'v2.0')"
                 % mlvi["version"], 0)
    if mlvi["fps_denom"] == 0 or mlvi["fps_nom"] == 0:
        rep.error("FPS", "fps %d/%d is not usable" % (mlvi["fps_nom"], mlvi["fps_denom"]), 0)
    else:
        fps = float(mlvi["fps_nom"]) / mlvi["fps_denom"]
        if not 1.0 <= fps <= 240.0:
            rep.warn("FPS", "fps %.3f (%d/%d) is outside 1..240"
                     % (fps, mlvi["fps_nom"], mlvi["fps_denom"]), 0)
    vclass = mlvi["video_class"]
    compressed = bool(vclass & (VIDEO_FLAG_LZMA | VIDEO_FLAG_LJ92 | VIDEO_FLAG_DELTA))

    # ---- RAWI
    rawi_blocks = [b for b in blocks if b.type == "RAWI" and b.fields]
    vidf = [b for b in blocks if b.type == "VIDF"]
    rawi = None
    if rawi_blocks:
        rawi = rawi_blocks[0].fields
        rep.rawi = rawi
        _check_rawi(rep, rawi_blocks[0])
        if not compressed and (vclass & 0x0F) == VIDEO_CLASS_RAW:
            bpp = rawi["bits_per_pixel"]
            if rawi["xRes"] > 0 and rawi["yRes"] > 0 and 0 < bpp <= 16:
                rep.expected_frame_size = (rawi["xRes"] * rawi["yRes"] * bpp + 7) // 8
        if vidf and vidf[0].offset < rawi_blocks[0].offset:
            rep.error("RAWI_ORDER", "a VIDF block precedes the RAWI block", vidf[0].offset)
    elif (vclass & 0x0F) == VIDEO_CLASS_RAW:
        rep.error("NO_RAWI", "video class is RAW but there is no complete RAWI block")

    # ---- VIDF frames
    complete_frames = [b for b in vidf if b.complete]
    rep.vidf_complete = len(complete_frames)
    rep.vidf_partial = len(vidf) - len(complete_frames)

    bad_size = 0
    for b in vidf:
        if not b.fields:
            continue
        space = b.fields["frame_space"]
        if space > b.size - VIDF_HDR_SIZE:
            rep.error("FRAME_SPACE", "VIDF #%d frameSpace %d exceeds the block payload %d"
                      % (b.fields["frame_number"], space, b.size - VIDF_HDR_SIZE), b.offset)
            continue
        payload = b.size - VIDF_HDR_SIZE - space
        b.fields["payload_size"] = payload
        if rep.expected_frame_size is not None and payload != rep.expected_frame_size:
            bad_size += 1
            if bad_size <= 5:
                rep.error("FRAME_SIZE",
                          "VIDF #%d payload is %d bytes, RAWI says %dx%d @ %d bpp = %d"
                          % (b.fields["frame_number"], payload, rawi["xRes"], rawi["yRes"],
                             rawi["bits_per_pixel"], rep.expected_frame_size), b.offset)
    if bad_size > 5:
        rep.error("FRAME_SIZE", "... %d more frames with a wrong payload size" % (bad_size - 5))

    numbers = [b.fields["frame_number"] for b in complete_frames if b.fields]
    if numbers:
        seen = set()
        dups = sorted(set(n for n in numbers if n in seen or seen.add(n)))
        if dups:
            rep.error("FRAME_NUMBER", "duplicate frame number(s): %s"
                      % ", ".join(str(n) for n in dups[:10]))
        missing = sorted(set(range(min(numbers), max(numbers) + 1)) - set(numbers))
        if numbers[0] != 0:
            rep.warn("FRAME_NUMBER", "first frame number is %d, not 0" % numbers[0])
        if missing:
            rep.error("FRAME_NUMBER", "%d frame number(s) missing between %d and %d "
                      "(first: %s)" % (len(missing), min(numbers), max(numbers),
                                       ", ".join(str(n) for n in missing[:10])))

    # ---- frame count vs header
    declared = mlvi["video_frame_count"]
    if declared != rep.vidf_complete:
        if declared == 0 and rep.vidf_complete:
            rep.error("FRAME_COUNT_UNFINALISED",
                      "MLVI.videoFrameCount is 0 but %d complete VIDF block(s) are present "
                      "(header never finalised: recording was not closed)" % rep.vidf_complete, 0)
        else:
            rep.error("FRAME_COUNT",
                      "MLVI.videoFrameCount says %d, file holds %d complete VIDF block(s)%s"
                      % (declared, rep.vidf_complete,
                         " (+%d cut off)" % rep.vidf_partial if rep.vidf_partial else ""), 0)

    # ---- timestamps
    meta_ts = [b.timestamp for b in blocks
               if b.timestamp is not None and b.type not in ("VIDF", "AUDF", "NULL")]
    vts = [(b.fields.get("frame_number"), b.timestamp, b.offset) for b in vidf
           if b.timestamp is not None]
    back = [(n, t, o) for (pn, pt, po), (n, t, o) in zip(vts, vts[1:]) if t <= pt]
    if back:
        rep.error("TIMESTAMP_ORDER", "%d VIDF block(s) have a timestamp <= the previous frame "
                  "(first: frame #%s)" % (len(back), back[0][0]), back[0][2])
    if vts and meta_ts:
        first_ts = vts[0][1]
        if first_ts - max(meta_ts) > ABS_TIMESTAMP_SLACK_US:
            rep.warn("TIMESTAMP_ABSOLUTE",
                     "first VIDF timestamp is %.3f s while metadata blocks are at <= %.3f ms: "
                     "VIDF timestamps are not relative to recording start"
                     % (first_ts / 1e6, max(meta_ts) / 1e3), vts[0][2])
    if len(vts) >= 2 and mlvi["fps_nom"] and mlvi["fps_denom"] and not back:
        span = vts[-1][1] - vts[0][1]
        fps = float(mlvi["fps_nom"]) / mlvi["fps_denom"]
        measured = (len(vts) - 1) * 1e6 / span if span else 0.0
        rep.mlvi["measured_fps"] = round(measured, 3)
        if measured and not (0.5 * fps <= measured <= 2.0 * fps):
            rep.warn("FPS_MISMATCH", "frame timestamps give %.3f fps, header says %.3f"
                     % (measured, fps))


def _check_rawi(rep, block):
    r = block.fields
    off = block.offset
    bpp = r["bits_per_pixel"]
    if not 1 <= bpp <= 16:
        rep.error("RAWI_BPP", "bits_per_pixel %d is not usable" % bpp, off)
    if r["xRes"] == 0 or r["yRes"] == 0:
        rep.error("RAWI_RES", "resolution %dx%d" % (r["xRes"], r["yRes"]), off)
        return
    if r["black_level"] >= r["white_level"]:
        rep.error("RAWI_LEVELS", "black %d >= white %d" % (r["black_level"], r["white_level"]), off)
    elif 1 <= bpp <= 16 and r["white_level"] > (1 << bpp):
        rep.warn("RAWI_LEVELS", "white level %d exceeds the %d-bit range" % (r["white_level"], bpp), off)
    if (r["width"], r["height"]) != (r["xRes"], r["yRes"]):
        # mlv_lite / mlv_rec also copy the core raw_info unchanged, and mlv_dump's DNG
        # writer resets the active area when it lies outside xRes/yRes -> info only.
        rep.info("RAWI_GEOMETRY",
                 "raw_info is %dx%d (pitch %d, frame_size %d) but frames are %dx%d: raw_info "
                 "describes the source buffer, not the recorded frame"
                 % (r["width"], r["height"], r["pitch"], r["frame_size"], r["xRes"], r["yRes"]), off)
    if 1 <= bpp <= 16 and r["pitch"] != r["width"] * bpp // 8:
        rep.warn("RAWI_PITCH", "raw_info.pitch %d != width*bpp/8 = %d"
                 % (r["pitch"], r["width"] * bpp // 8), off)
    a = r["active_area"]
    if a["x2"] > r["xRes"] or a["y2"] > r["yRes"]:
        rep.info("RAWI_ACTIVE_AREA",
                 "active_area x %d..%d y %d..%d lies outside the %dx%d frame"
                 % (a["x1"], a["x2"], a["y1"], a["y2"], r["xRes"], r["yRes"]), off)
    elif a["x1"] >= a["x2"] or a["y1"] >= a["y2"]:
        rep.warn("RAWI_ACTIVE_AREA", "active_area is empty", off)
    if r["xRes"] % 8 and bpp == 14:
        rep.warn("RAWI_RES", "xRes %d is not a multiple of 8 (14-bit packing unit)" % r["xRes"], off)


# ---------------------------------------------------------------- frame access

def frame_payload(path, report, frame_index):
    """Return the image payload (bytes) of the frame_index-th complete VIDF block."""
    frames = [b for b in report.blocks if b.type == "VIDF" and b.complete and b.fields]
    if not 0 <= frame_index < len(frames):
        raise IndexError("frame %d not available (%d complete frames)" % (frame_index, len(frames)))
    b = frames[frame_index]
    space = b.fields["frame_space"]
    with open(path, "rb") as f:
        f.seek(b.offset + VIDF_HDR_SIZE + space)
        return f.read(b.size - VIDF_HDR_SIZE - space)


def unpack14_rows(data, width, rows, pitch):
    """Unpack selected rows of ML 14-bit packed raw (8 px in 7 LE 16-bit words)."""
    out = []
    groups = width // 8
    for row in rows:
        base = row * pitch
        w = struct.unpack_from("<%dH" % (groups * 7), data, base)
        line = []
        for g in range(groups):
            w0, w1, w2, w3, w4, w5, w6 = w[g * 7:g * 7 + 7]
            line.extend((
                w0 >> 2,
                ((w0 & 0x3) << 12) | (w1 >> 4),
                ((w1 & 0xF) << 10) | (w2 >> 6),
                ((w2 & 0x3F) << 8) | (w3 >> 8),
                ((w3 & 0xFF) << 6) | (w4 >> 10),
                ((w4 & 0x3FF) << 4) | (w5 >> 12),
                ((w5 & 0xFFF) << 2) | (w6 >> 14),
                w6 & 0x3FFF,
            ))
        out.append(line)
    return out


def channel_stats(payload, width, height, step=8):
    """Per-CFA-site min/max/mean on a row subsample (14-bit packed only)."""
    pitch = width * 14 // 8
    rows = [r for r in range(0, height - 1, 2 * step)]
    rows = sorted(set(rows + [r + 1 for r in rows]))
    lines = unpack14_rows(payload, width, rows, pitch)
    stats = {}
    for row, line in zip(rows, lines):
        for col_phase in (0, 1):
            key = (row & 1, col_phase)
            vals = line[col_phase::2]
            s = stats.setdefault(key, [1 << 30, 0, 0, 0])
            s[0] = min(s[0], min(vals))
            s[1] = max(s[1], max(vals))
            s[2] += sum(vals)
            s[3] += len(vals)
    return dict((k, {"min": s[0], "max": s[1], "mean": float(s[2]) / s[3], "n": s[3]})
                for k, s in stats.items())


# ---------------------------------------------------------------- output

def _fmt_ts(ts):
    return "-" if ts is None else "%.3f ms" % (ts / 1000.0)


def print_report(rep, verbose=False, out=None):
    w = (out or sys.stdout).write
    w("file: %s (%d bytes)\n" % (rep.path, rep.file_size))
    m = rep.mlvi
    if m:
        fps = (float(m["fps_nom"]) / m["fps_denom"]) if m["fps_denom"] else 0.0
        w("MLVI: version=%r guid=0x%016x file=%d/%d flags=0x%x video_class=0x%x "
          "audio_class=0x%x\n" % (m["version"], m["guid"], m["file_num"], m["file_count"],
                                  m["file_flags"], m["video_class"], m["audio_class"]))
        w("      fps=%.3f (%d/%d) declared video frames=%d audio frames=%d\n"
          % (fps, m["fps_nom"], m["fps_denom"], m["video_frame_count"], m["audio_frame_count"]))
        if "measured_fps" in m:
            w("      fps from VIDF timestamps=%.3f\n" % m["measured_fps"])
    r = rep.rawi
    if r:
        a = r["active_area"]
        w("RAWI: %dx%d  raw_info %dx%d pitch=%d frame_size=%d bpp=%d black=%d white=%d\n"
          % (r["xRes"], r["yRes"], r["width"], r["height"], r["pitch"], r["frame_size"],
             r["bits_per_pixel"], r["black_level"], r["white_level"]))
        w("      active_area x=%d..%d y=%d..%d cfa=0x%08x dynamic_range=%d\n"
          % (a["x1"], a["x2"], a["y1"], a["y2"], r["cfa_pattern"], r["dynamic_range"]))
    for b in rep.blocks:
        if b.type in ("MLVI", "RAWI", "VIDF", "AUDF"):
            continue
        if b.fields:
            w("%s: %s\n" % (b.type, " ".join("%s=%s" % (k, b.fields[k]) for k in sorted(b.fields))))

    counts = {}
    for b in rep.blocks:
        counts[b.type] = counts.get(b.type, 0) + 1
    w("blocks: %d  (%s)\n" % (len(rep.blocks),
                               ", ".join("%s x%d" % kv for kv in sorted(counts.items()))))
    w("frames: %d complete VIDF" % rep.vidf_complete)
    if rep.vidf_partial:
        w(" + %d cut off" % rep.vidf_partial)
    if m:
        w(", header declares %d" % m["video_frame_count"])
    if rep.expected_frame_size is not None:
        w(", expected payload %d bytes" % rep.expected_frame_size)
    w("\n")
    if rep.truncated:
        w("tail:   TRUNCATED (%d byte(s) of the last block missing)\n" % rep.truncated_missing)
    else:
        w("tail:   clean (last block ends at end of file)\n")

    if verbose:
        w("\n  idx     offset type       size  timestamp        details\n")
        for b in rep.blocks:
            det = ""
            if b.type in ("VIDF", "AUDF") and b.fields:
                det = "frame=%d space=%d" % (b.fields["frame_number"], b.fields["frame_space"])
                if "payload_size" in b.fields:
                    det += " payload=%d" % b.fields["payload_size"]
                if b.type == "VIDF":
                    det += " crop=%d,%d pan=%d,%d" % (b.fields["crop_x"], b.fields["crop_y"],
                                                      b.fields["pan_x"], b.fields["pan_y"])
            if not b.complete:
                det += " [CUT: %d of %d bytes present]" % (b.present, b.size)
            w("%5d 0x%08x %s %10d  %-16s %s\n"
              % (b.index, b.offset, b.type, b.size, _fmt_ts(b.timestamp), det))

    w("\n")
    if not rep.issues:
        w("OK: no inconsistencies\n")
    for i in rep.issues:
        w("%s\n" % i)
    w("result: %d error(s), %d warning(s), %d info\n"
      % (len(rep.errors), len(rep.warnings), len(rep.codes(INFO))))


CRASH_EXPECTED = ("TRUNCATED_BLOCK", "TRUNCATED_HEADER", "FRAME_COUNT_UNFINALISED")


def downgrade_crash_findings(rep):
    for i in rep.issues:
        if i.severity == ERROR and i.code in CRASH_EXPECTED:
            i.severity = WARNING


def main(argv=None):
    ap = argparse.ArgumentParser(description="Validate the block structure of an MLV file.")
    ap.add_argument("file")
    ap.add_argument("-v", "--verbose", action="store_true", help="list every block")
    ap.add_argument("--json", action="store_true", help="machine-readable report on stdout")
    ap.add_argument("--strict", action="store_true", help="warnings also give exit status 1")
    ap.add_argument("--crash-ok", action="store_true",
                    help="truncated tail / unfinalised frame count are warnings, not errors")
    ap.add_argument("--extract", nargs=2, metavar=("N", "OUT"),
                    help="write the payload of the N-th complete frame to OUT")
    ap.add_argument("--stats", action="store_true",
                    help="per-CFA-site statistics of the first and last complete frame "
                         "(uncompressed 14-bit only, row subsample)")
    args = ap.parse_args(argv)

    rep = parse(args.file)
    if args.crash_ok:
        downgrade_crash_findings(rep)

    if args.json:
        json.dump(rep.as_dict(), sys.stdout, indent=1)
        sys.stdout.write("\n")
    else:
        print_report(rep, args.verbose)

    if args.extract:
        try:
            data = frame_payload(args.file, rep, int(args.extract[0]))
        except (IndexError, ValueError) as e:
            sys.stderr.write("extract: %s\n" % e)
            return 2
        with open(args.extract[1], "wb") as o:
            o.write(data)
        sys.stderr.write("extract: frame %s -> %s (%d bytes)\n"
                         % (args.extract[0], args.extract[1], len(data)))

    if args.stats and rep.rawi and rep.vidf_complete and not args.json:
        r = rep.rawi
        if r["bits_per_pixel"] == 14 and rep.expected_frame_size and r["xRes"] % 8 == 0:
            for idx in sorted(set([0, rep.vidf_complete - 1])):
                data = frame_payload(args.file, rep, idx)
                if len(data) < rep.expected_frame_size:
                    continue
                st = channel_stats(data, r["xRes"], r["yRes"])
                sys.stdout.write("stats frame %d (black %d, white %d):\n"
                                 % (idx, r["black_level"], r["white_level"]))
                for key in sorted(st):
                    s = st[key]
                    sys.stdout.write("  site(row%%2=%d,col%%2=%d): min=%d max=%d mean=%.1f n=%d\n"
                                     % (key[0], key[1], s["min"], s["max"], s["mean"], s["n"]))
        else:
            sys.stdout.write("stats: only uncompressed 14-bit frames are supported\n")

    return rep.exit_code(args.strict)


if __name__ == "__main__":
    sys.exit(main())
