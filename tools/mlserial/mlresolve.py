#!/usr/bin/env python3
"""mlresolve - auto-resolve per-boot mlv_lite symbols on the M50 II.

The mlv_lite module loads at a DIFFERENT base every boot, so raw_start_stop (and the
cbr handlers) move each time. All session I re-derived them by hand with ~6 harness
reads + arithmetic every single boot. This does it in one command:

    python3 mlresolve.py

It walks the fixed module_list slot for mlv_lite's cbr table, finds the keypress
(type 8) and vsync (type 6) handlers, computes raw_start_stop = keypress - 0xBC,
VERIFIES the raw_start_stop prologue, and prints a ready-to-use status block:

    raw_start_stop = 0x........   (call it to start/stop record)
    keypress/vsync handlers, vsync-keypress delta
    raw armed? (raw_info buffer)   mount bit 0xaaf14   is_movie_mode()  lv

Reads are done through the running daemon via `mlserial.py cmd`. Fixed anchors:
  module_list mlv_lite cbr-ptr slot = 0x110590   (entry[1]+0xA0)
  raw_info = 0x103ed4 (buffer @+4 = 0x103ed8)     mount = 0xaaf14
  is_movie_mode = 0xde065 (thumb)   lv = 0x149e5c
raw_start_stop prologue (relocation-free, unique):
  E92D4037 E59F4088 E59F3088 E5945210 E3550000 0A000007
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SYM_PATH = os.environ.get("MLSERIAL_SYM", os.path.join(
    HERE, "..", "..", "magiclantern_simplified", "platform", "M50mk2.101", "build", "magiclantern.sym"))


def _load_syms():
    d = {}
    try:
        with open(SYM_PATH) as f:
            for line in f:
                p = line.split()
                if len(p) >= 2:
                    try:
                        d[p[1]] = int(p[0], 16)
                    except ValueError:
                        pass
    except OSError:
        pass
    return d


_SYM = _load_syms()


def _sym(name, fallback):
    """Resolve an ML-core symbol from the flashed build's .sym. These addresses SHIFT every
    rebuild -- hardcoding them once got a stale is_movie_mode `call`ed into the middle of
    prop_request_change_wait and CRASHED the camera. Always resolve from the current sym."""
    a = _SYM.get(name)
    if a is None:
        sys.stderr.write("WARN: '%s' not in %s; using stale fallback 0x%x (may crash on `call`!)\n"
                         % (name, SYM_PATH, fallback))
        return fallback
    return a


MOUNT = 0xaaf14                                  # Canon volume-table flag (stable, ROM-derived)
CBR_PTR_SLOT = 0x110590                          # module_list cbr-ptr slot (module .bss; verify if it moves)
IS_MOVIE_MODE = _sym("is_movie_mode", 0xde065)   # thumb; CALLED -> must match the flashed build
LV = _sym("lv", 0x149e5c)
RAW_INFO_BUF = _sym("raw_info", 0x103ed4) + 0x04  # raw_info.buffer field @ +0x04
RSS_FROM_KEYPRESS = 0xBC
PROLOGUE = [0xE92D4037, 0xE59F4088, 0xE59F3088, 0xE5945210, 0xE3550000, 0x0A000007]
WORD_RE = re.compile(r"0x[0-9A-Fa-f]{8}")


def sh(text, timeout=25):
    r = subprocess.run(["python3", "mlserial.py", "cmd", text], cwd=HERE,
                       capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr


def read_words(addr, nwords):
    """Read nwords 32-bit words starting at addr via `md`. Returns list of ints."""
    out = sh("md 0x%x 0x%x" % (addr, nwords * 4))
    words = []
    for line in out.splitlines():
        m = re.match(r"\s*0x[0-9A-Fa-f]+:\s*(.*)", line)
        if not m:
            continue
        for w in WORD_RE.findall(m.group(1)):
            words.append(int(w, 16))
    return words[:nwords]


def read_word(addr):
    out = sh("rd 0x%x" % addr)
    m = re.search(r"0x[0-9A-Fa-f]+:\s*(0x[0-9A-Fa-f]+)", out)
    if m:
        return int(m.group(1), 16)
    ws = WORD_RE.findall(out)
    return int(ws[-1], 16) if ws else None


def call(addr):
    out = sh("call 0x%x" % addr)
    m = re.search(r"ret\s+(0x[0-9A-Fa-f]+)", out)
    return int(m.group(1), 16) if m else None


def main():
    cbr = read_word(CBR_PTR_SLOT)
    if not cbr or cbr < 0x1000 or cbr > 0xE0000000:
        print("mlv_lite cbr ptr @0x%x = %s -> module NOT loaded this boot." %
              (CBR_PTR_SLOT, ("0x%x" % cbr) if cbr else "null"))
        sys.exit(2)
    words = read_words(cbr, 25)          # ~5 cbr entries of 5 words each
    keypress = vsync = None
    for k in range(len(words) // 5):
        typ = words[5 * k + 2]
        handler = words[5 * k + 3]
        if typ == 8:
            keypress = handler
        elif typ == 6:
            vsync = handler
    if not keypress:
        print("could not find keypress (type 8) handler in cbr table @0x%x" % cbr)
        print("cbr words: " + " ".join("0x%x" % w for w in words[:15]))
        sys.exit(3)
    rss = keypress - RSS_FROM_KEYPRESS
    prol = read_words(rss, 6)
    ok = prol == PROLOGUE
    print("=== mlv_lite per-boot resolution ===")
    print("cbr table      = 0x%x" % cbr)
    print("keypress(t8)   = 0x%x" % keypress)
    if vsync:
        print("vsync(t6)      = 0x%x   (vsync-keypress delta = 0x%x)" % (vsync, vsync - keypress))
    print("raw_start_stop = 0x%x   [prologue %s]" % (rss, "OK" if ok else "MISMATCH!"))
    if not ok:
        print("  got:      " + " ".join("0x%x" % w for w in prol))
        print("  expected: " + " ".join("0x%x" % w for w in PROLOGUE))
        print("  -> do NOT call; re-check module base.")
        sys.exit(4)
    buf = read_word(RAW_INFO_BUF)
    mnt = read_word(MOUNT)
    mm = call(IS_MOVIE_MODE)
    lv = read_word(LV)
    print("--- state ---")
    print("raw armed      = %s (raw_info buf=0x%x)" % ("YES" if buf else "no", buf or 0))
    print("mount 0xaaf14  = 0x%x (bit0x200000 %s)" % (mnt or 0, "SET" if (mnt or 0) & 0x200000 else "CLEAR"))
    print("is_movie_mode  = %s    lv = %s" % (mm, ("0x%x" % lv) if lv is not None else "?"))
    print("")
    print("READY: python3 mlserial.py cmd \"call 0x%x\"   # start/stop raw record" % rss)


if __name__ == "__main__":
    main()
