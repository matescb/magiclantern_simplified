#!/bin/sh
# Host behavioural tests for raw_vidx worker.c + event_pusher.c (see harness.c).
# Usage: run.sh <dir containing worker.c, event_pusher.c, raw_vid*.h, mlv_3.h> [out-dir]
# Example: run.sh /mnt/data/WORK/eos/wt/C4/modules/raw_video/raw_vidx
# Needs a gcc that can build 32-bit (-m32). Exit status = number of failed tests.
set -u
ulimit -c 0 # no core dumps, the root filesystem is nearly full
SRC=${1:?source dir}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${2:-/mnt/data/WORK/eos/sim/C4/hosttest}
mkdir -p "$OUT"
BIN="$OUT/hosttest.$(echo "$SRC" | cksum | cut -d' ' -f1)"

# The module's own headers come from SRC, everything else from stubs/.
gcc -m32 -O1 -g -w -I"$HERE/stubs" -I"$SRC" -c "$SRC/worker.c" -o "$OUT/worker.o" || exit 100
gcc -m32 -O1 -g -w -I"$HERE/stubs" -I"$SRC" -c "$SRC/event_pusher.c" -o "$OUT/event_pusher.o" || exit 100
gcc -m32 -O1 -g -w -I"$HERE/stubs" -I"$SRC" -c "$HERE/fake_mlv.c" -o "$OUT/fake_mlv.o" || exit 100
gcc -m32 -O1 -g -Wall -c "$HERE/harness.c" -o "$OUT/harness.o" || exit 100
gcc -m32 "$OUT/worker.o" "$OUT/event_pusher.o" "$OUT/fake_mlv.o" "$OUT/harness.o" -o "$BIN" || exit 100

fails=0
for t in short_clip long_clip long_clip_lowprio_writer stop_right_after_swap many_sessions \
         start_failure wedge_with_checkpoint wedge_without_checkpoint queue_full slow_card; do
    timeout 60 "$BIN" "$t"
    rc=$?
    if [ $rc -ne 0 ]; then fails=$((fails + 1)); echo "  => $t FAILED (exit $rc)"; fi
done
echo "failed tests: $fails"
exit $fails
