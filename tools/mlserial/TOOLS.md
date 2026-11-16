# M50 II harness toolkit — quick reference

The daemon (`mlserald.py`) owns the serial port and logs every UART line to
`run/uart.log` (text) and `run/uart.jsonl` (structured). Everything else talks to it.

## Device interaction — `mlserial.py`
```
python3 mlserial.py cmd "rd 0xaaf14"          # read one word
python3 mlserial.py cmd "md 0x110438 0x40"    # dump memory
python3 mlserial.py cmd "call 0x<addr> [a b]" # call a function, prints ret
python3 mlserial.py cmd "load <hexbytes>"     # stage a blob into the RAM scratch buffer
python3 mlserial.py cmd "exec [a b c d]"      # cache-sync + call the staged blob (thumb)
python3 mlserial.py cmd "wr <addr> <val>"     # write a word (RAM only; ROM refused)
python3 mlserial.py cmd "watch <addr> [ms] [n]"
python3 mlserial.py key <name>                # send a key
python3 mlserial.py log --grep FIO --n 60     # tail the raw log (dumb filter)
python3 mlserial.py symbolize 0x<addr> ...    # static symbol lookup
```
NOTE: run from `tools/mlserial/` — a stray `cd` elsewhere makes reads return empty.

## Per-boot symbol resolution — `mlresolve.py`  (NEW)
The mlv_lite module loads at a different base every boot. Instead of ~6 manual reads:
```
python3 mlresolve.py
```
Walks module_list -> mlv_lite cbr table -> keypress(t8)/vsync(t6) handlers,
computes and PROLOGUE-VERIFIES `raw_start_stop = keypress - 0xBC`, and prints raw
armed?/mount/is_movie_mode/lv plus the ready-to-run `call` line. Exits non-zero if
the module didn't load this boot.

## Structured log analysis — `mllog.py`  (NEW)
Ingests `run/uart.jsonl` into a SQLite DB (`run/mllog.db`), classifies every line,
segments boots, decodes heartbeats. Query the DELTAS instead of reading 400 lines:
```
python3 mllog.py boots [N]              # table: build | reached-big8? | fio | assert | crash
python3 mllog.py errors [--boot ID]     # deduped FIO errors / asserts / crashes (counts)
python3 mllog.py markers [--boot ID]    # OUR [wh]/[ctrl]/[rawrec]/@ML> lines, de-interleaved, ordered
python3 mllog.py heartbeat [--boot ID]  # HEALTHY/ANOMALY verdict (ovr, cpu1rdy) — no reading heartbeats
python3 mllog.py phases  [--boot ID]    # pre.N/big.N/post.N timeline
python3 mllog.py fio     [--boot ID]    # decode @ML> FIO-op lines: op | drive | ret | path | FAIL?
python3 mllog.py tail    [--boot ID] N  # last N non-spam lines, cleaned
```
Default boot = the latest. Auto-ingests before every query.

### The `@ML>` atomic log convention (camera-side)
Canon's debug console and ML share the SAME unlocked UART TX register, so plain
`m50ii_uart_str()` output gets shredded mid-line by Canon's FIO error flood. mlv_lite
now has `m50ii_uart_atomic()` (writes a whole line with IRQs disabled -> uninterruptible)
and `m50ii_log_op(op, path, ret)` which emits:
```
@ML> <op> path=<path> ret=<ret>
```
`mllog.py fio` decodes these and flags the DRIVE (path[0]-'A': A=0, B=1). A `ret<0` on
drive 0 (A:, the absent CF slot) is the M50 II write bug. Always route rec-path
diagnostics through `m50ii_uart_atomic`/`m50ii_log_op` so they survive the interleave.

## One-shot build + flash — `flash.sh`  (NEW)
```
./flash.sh                 # build mlv_lite.mo, flash to card (if in reader), md5-verify from fresh remount
./flash.sh --build-only    # build + md5 only (card in camera)
./flash.sh --note "msg"    # tag the build in the mllog DB (ties boots -> build)
```

## Deploy over serial — `mldeploy.py`  (NEW, experimental)
Writes a file to the SD card OVER THE WIRE using ML's own FIO (no card shuffle), streaming
bytes through the shell's 256-byte `load` scratch. Camera must be booted, ML alive, idle.
```
python3 mldeploy.py --selftest                    # write+verify a small file, report throughput
python3 mldeploy.py <local> B:/path/on/card       # deploy a file
```
STATUS: PROVEN for single small writes (create+write+close+verify size on card, all over serial).
CAVEATS: (1) multi-chunk (>256 B) currently shows size=0 after close — a persistence bug to fix;
(2) throughput ~21 B/s (a 74 KB .mo ≈ 1 hr) — impractical for whole modules until RX is sped up
(bigger scratch + faster poll) and the multi-write bug is fixed. Fine for small patches/config now.
Verify with FIO_GetFileSize_direct (0xcf525, returns size), NOT the out-param GetFileSize (flaky).

## Typical iteration (card-in-reader era)
```
edit mlv_lite.c
./flash.sh --note "what changed"     # build+flash+verify+record
# operator: card -> camera, cold-boot into Movie + LiveView standby
python3 mlresolve.py                  # get raw_start_stop for this boot
python3 mlserial.py cmd "call 0x<raw_start_stop>"
python3 mllog.py markers ; python3 mllog.py errors    # read the verdict, not the log
```

## Raw buffer -> DNG on the host — `raw_dng.py`  (the R2 "prove the buffer" gate)
Unpacks an ML 14-bit packed raw buffer dump into a DNG you can open on the desktop —
**sidesteps the camera FIO/record crash entirely**. We have never rendered a frame from
the M50 II raw buffer; this answers "is it the right buffer?" without a working card writer.
Bit layout grounded in `src/raw.h` raw_pixblock + the PA..PH macros (self-tested: pack/unpack
round-trips all 2.44M pixels exactly). Defaults = the live-confirmed raw_info (2096x1164, 14-bit,
black 2047, white 16200, RGGB).
```
# camera in MOVIE mode, LiveView standby; arm the raw stream first:
python3 mlserial.py cmd "call lv_set_mm 1"
python3 mlserial.py cmd "call lv_save_raw 1"          # ch2 ram_addr @0xd04202a0 now valid
python3 mlserial.py cmd "rd 0xd04202a0"               # read the live buffer ptr (moves per boot!)
python3 mlserial.py cmd "dump 0x<ptr> 0x4125F0 frame.bin"   # 4269552 B = one full frame
python3 raw_dng.py frame.bin -o frame.dng --pgm frame.pgm   # prints per-channel stats + writes DNG
```
Read the stats block: sane means black-ish min ~2047, varied content, R/G/B tile means that
differ. Open frame.dng in RawTherapee/darktable/Adobe (PGM is a quick undemosaiced look).
Garbage => geometry/bit-depth/channel wrong, fix BEFORE more record-path work.

## Golden rule learned the expensive way
Before rebuilding: can `call`/`exec`/`wr`/memory-read answer this LIVE without a
build-flash-boot cycle? Most buffer/FIO/state probes can. Rebuild only to instrument
the real in-situ task flow.
