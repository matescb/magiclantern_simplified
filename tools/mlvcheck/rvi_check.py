#!/usr/bin/env python3
"""Reader for the raw_vidx integrity sidecar (<clip>.RVI), written by the M50 II raw_vidx
module when `rvx_sidecar` is 1 (modules/raw_video/raw_vidx/rvx_step1.h, added by offline task C12).

Usage: rvi_check.py CLIP.RVI [--mlv CLIP.MLV] [--json] [-v]

Exit status: 0 nothing suspicious, 1 torn / differing / dropped frames or a count mismatch,
2 the file is not a readable sidecar.

What the fields can and cannot say:
  torn      source probes (8 x 64 bytes, first to last row) changed between the start and the
            end of the copy: Canon wrote into its buffer meanwhile, the stored frame mixes
            exposures.  A frame NOT flagged is not proven whole: a rewrite that leaves all 512
            probed bytes unchanged (clipped areas) is invisible.
  dst_diff  the copy differs from the source as read after the copy.  Always set together with
            torn.  Set WITHOUT torn it means the copy itself is wrong (e.g. stale cache lines
            with rvx_fast_copy).
  vd gaps   vsync numbers missing between records = frames the module never copied.
"""
import argparse
import json
import struct
import sys

MAGIC = 0x49585652
HDR_FIELDS = (
    "magic version header_size record_size n_records records_lost w h x y vsyncs "
    "frames_stored frames_on_disk dropped_queue dropped_busy dropped_nofile dropped_copy "
    "torn dst_diff copy_prio write_prio chunk_kb sleep_ms burst_frames release_first "
    "fast_copy drop_torn copy_task_fail first_t_us last_t_us fps_x1000_core fps_x1000_measured"
).split()
REC_FIELDS = ("stored_index vd_seq t_vsync_us t_copy_us copy_us hash_before hash_after "
              "hash_dst flags").split()
F_TORN, F_DST_DIFF, F_DROPPED, F_FAST = 1, 2, 4, 8


def parse(data):
    need = 4 * len(HDR_FIELDS)
    if len(data) < need:
        raise ValueError("file shorter than the header (%d < %d bytes)" % (len(data), need))
    hdr = dict(zip(HDR_FIELDS, struct.unpack("<%dI" % len(HDR_FIELDS), data[:need])))
    if hdr["magic"] != MAGIC:
        raise ValueError("bad magic 0x%08X" % hdr["magic"])
    if hdr["version"] != 1:
        raise ValueError("unknown version %d" % hdr["version"])
    if hdr["header_size"] < need or hdr["record_size"] < 4 * len(REC_FIELDS):
        raise ValueError("header_size / record_size smaller than version 1 defines")
    recs = []
    pos = hdr["header_size"]
    for _ in range(hdr["n_records"]):
        chunk = data[pos:pos + 4 * len(REC_FIELDS)]
        if len(chunk) < 4 * len(REC_FIELDS):
            raise ValueError("file ends inside record %d of %d" % (len(recs), hdr["n_records"]))
        recs.append(dict(zip(REC_FIELDS, struct.unpack("<%dI" % len(REC_FIELDS), chunk))))
        pos += hdr["record_size"]
    return hdr, recs


def analyse(hdr, recs):
    """Returns (findings, stats). findings: list of (severity, text), severity 'bad' or 'note'."""
    out = []
    torn = [r for r in recs if r["flags"] & F_TORN]
    diff_only = [r for r in recs if r["flags"] & F_DST_DIFF and not r["flags"] & F_TORN]
    dropped = [r for r in recs if r["flags"] & F_DROPPED]
    stored = [r for r in recs if not r["flags"] & F_DROPPED]
    if torn:
        out.append(("bad", "%d of %d copied frames are torn (source changed during the copy)"
                    % (len(torn), len(recs))))
    if diff_only:
        out.append(("bad", "%d frames: copy differs from an UNCHANGED source - the copy path is "
                    "wrong (fast_copy=%d)" % (len(diff_only), hdr["fast_copy"])))
    if dropped:
        out.append(("bad", "%d torn frames were dropped (rvx_drop_torn)" % len(dropped)))
    gaps = 0
    for a, b in zip(recs, recs[1:]):
        if b["vd_seq"] <= a["vd_seq"]:
            out.append(("bad", "vsync numbers not increasing at record with vd_seq %d" % b["vd_seq"]))
            break
        gaps += b["vd_seq"] - a["vd_seq"] - 1
    if gaps:
        out.append(("bad", "%d vsyncs between the first and last record produced no copied frame"
                    % gaps))
    for name in ("dropped_queue", "dropped_busy", "dropped_nofile", "dropped_copy",
                 "copy_task_fail", "records_lost"):
        if hdr[name]:
            out.append(("bad" if name != "records_lost" else "note", "%s = %d" % (name, hdr[name])))
    if hdr["records_lost"] == 0 and len(stored) != hdr["frames_stored"]:
        out.append(("bad", "records of stored frames %d != frames_stored %d"
                    % (len(stored), hdr["frames_stored"])))
    if hdr["frames_on_disk"] != hdr["frames_stored"]:
        out.append(("bad", "frames_on_disk %d != frames_stored %d"
                    % (hdr["frames_on_disk"], hdr["frames_stored"])))
    idx = [r["stored_index"] for r in stored]
    if hdr["records_lost"] == 0 and idx != list(range(len(idx))):
        out.append(("bad", "stored_index sequence is not 0..n-1"))
    copy = sorted(r["copy_us"] for r in recs)
    stats = {
        "records": len(recs),
        "copy_us_min": copy[0] if copy else None,
        "copy_us_median": copy[len(copy) // 2] if copy else None,
        "copy_us_max": copy[-1] if copy else None,
        "vsync_gaps": gaps,
    }
    if copy and hdr["w"] and hdr["h"]:
        frame_bytes = hdr["w"] * hdr["h"] * 14 // 8
        med = stats["copy_us_median"]
        stats["copy_mb_per_s_median"] = round(frame_bytes / med, 2) if med else None
    core, meas = hdr["fps_x1000_core"], hdr["fps_x1000_measured"]
    if core and meas and abs(core - meas) * 100 > meas:
        out.append(("note", "MLV header fps %.3f differs from the measured vsync rate %.3f by more "
                    "than 1%%" % (core / 1000.0, meas / 1000.0)))
    return out, stats


def mlv_frame_count(path):
    """Counts VIDF blocks by walking the block stream (no dependency on mlv_check)."""
    n = 0
    with open(path, "rb") as f:
        data = f.read()
    pos = 0
    while pos + 8 <= len(data):
        btype, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if size < 16 or pos + size > len(data):
            break
        if btype == b"VIDF":
            n += 1
        pos += size
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(description="Check a raw_vidx integrity sidecar (.RVI).")
    ap.add_argument("file")
    ap.add_argument("--mlv", help="the clip; its VIDF count is compared with the sidecar")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true", help="list every record")
    args = ap.parse_args(argv)
    try:
        with open(args.file, "rb") as f:
            hdr, recs = parse(f.read())
    except (OSError, ValueError) as e:
        print("rvi_check: %s" % e, file=sys.stderr)
        return 2
    findings, stats = analyse(hdr, recs)
    if args.mlv:
        n = mlv_frame_count(args.mlv)
        if n != hdr["frames_on_disk"]:
            findings.append(("bad", "MLV holds %d VIDF blocks, sidecar says %d on disk"
                             % (n, hdr["frames_on_disk"])))
    bad = [t for s, t in findings if s == "bad"]
    if args.json:
        json.dump({"header": hdr, "stats": stats, "findings": findings,
                   "records": recs if args.verbose else None}, sys.stdout, indent=1)
        print()
    else:
        print("window %dx%d at (%d,%d)  vsyncs %d  stored %d  on disk %d  records %d"
              % (hdr["w"], hdr["h"], hdr["x"], hdr["y"], hdr["vsyncs"], hdr["frames_stored"],
                 hdr["frames_on_disk"], len(recs)))
        print("settings: copy_prio 0x%X (0 = in worker)  write_prio 0x%X  chunk %d KiB  sleep %d ms  "
              "burst %d  release_first %d  fast_copy %d  drop_torn %d"
              % (hdr["copy_prio"], hdr["write_prio"], hdr["chunk_kb"], hdr["sleep_ms"],
                 hdr["burst_frames"], hdr["release_first"], hdr["fast_copy"], hdr["drop_torn"]))
        print("fps: core %.3f  measured %.3f" % (hdr["fps_x1000_core"] / 1000.0,
                                                 hdr["fps_x1000_measured"] / 1000.0))
        print("copy time us: min %s median %s max %s  (median %s MB/s)"
              % (stats["copy_us_min"], stats["copy_us_median"], stats["copy_us_max"],
                 stats.get("copy_mb_per_s_median")))
        if args.verbose:
            for r in recs:
                print("  #%-5s vd %-6d t %-10d copy %-7d us  %08X %08X %08X  flags %d"
                      % ("drop" if r["flags"] & F_DROPPED else r["stored_index"], r["vd_seq"],
                         r["t_vsync_us"], r["copy_us"], r["hash_before"], r["hash_after"],
                         r["hash_dst"], r["flags"]))
        for sev, text in findings:
            print("%s: %s" % ("PROBLEM" if sev == "bad" else "note", text))
        print("result: %d problem(s)" % len(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
