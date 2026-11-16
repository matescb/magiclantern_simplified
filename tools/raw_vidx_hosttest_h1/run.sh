#!/bin/sh
# Host behavioural tests for raw_vidx worker.c + event_pusher.c + rvx_step1.c (see harness.c).
# H1 copy of tools/raw_vidx_hosttest_g1: adds the vsync-hook rule (no wait / allocator / task / UART in the hook) and the h1_* tests.
# Usage: run.sh <dir containing worker.c, event_pusher.c, raw_vid*.h, mlv_3.h> [out-dir]
# Example: run.sh /mnt/data/WORK/eos/wt/C4/modules/raw_video/raw_vidx
# Needs a gcc that can build 32-bit (-m32). Exit status = number of failed tests.
set -u
ulimit -c 0 # no core dumps, the root filesystem is nearly full
SRC=${1:?source dir}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${2:-/mnt/data/WORK/eos/sim/H1/hosttest}
mkdir -p "$OUT"
BIN="$OUT/hosttest.$(echo "$SRC" | cksum | cut -d' ' -f1)"

# The module's own headers come from SRC, everything else from stubs/.
gcc -m32 -O1 -g -w -I"$HERE/stubs" -I"$SRC" -c "$SRC/worker.c" -o "$OUT/worker.o" || exit 100
gcc -m32 -O1 -g -w -I"$HERE/stubs" -I"$SRC" -c "$SRC/event_pusher.c" -o "$OUT/event_pusher.o" || exit 100
gcc -m32 -O1 -g -w -I"$HERE/stubs" -I"$SRC" -c "$SRC/rvx_step1.c" -o "$OUT/rvx_step1.o" || exit 100
gcc -m32 -O1 -g -w -I"$HERE/stubs" -I"$SRC" -c "$SRC/rvx_sync.c" -o "$OUT/rvx_sync.o" || exit 100
gcc -m32 -O1 -g -w -I"$HERE/stubs" -I"$SRC" -c "$HERE/fake_mlv.c" -o "$OUT/fake_mlv.o" || exit 100
gcc -m32 -O1 -g -Wall -c "$HERE/harness.c" -o "$OUT/harness.o" || exit 100
gcc -m32 "$OUT/worker.o" "$OUT/event_pusher.o" "$OUT/rvx_step1.o" "$OUT/rvx_sync.o" "$OUT/fake_mlv.o" "$OUT/harness.o" -o "$BIN" || exit 100

fails=0
for t in short_clip long_clip long_clip_lowprio_writer stop_right_after_swap many_sessions \
         start_failure wedge_with_checkpoint wedge_without_checkpoint queue_full slow_card \
         c12_defaults c12_burst_target c12_burst_ram_full c12_burst_backlog c12_burst_release_first \
         c12_burst_720p c12_prio c12_throttle c12_torn_flagged c12_torn_dropped c12_integrity_clean \
         c12_fast_copy_aligned c12_fast_copy_unaligned c12_crop_reject \
         g1_defaults g1_evf_install g1_evf_refuse g1_evf_foreign_chain g1_evf_start_failure \
         g1_mem1_install g1_mem1_refuse g1_mem1_canon_clears g1_mem1_canon_rewrites \
         g1_bad_src g1_pool_stop g1_pool_stop_hooked g1_pool_unarmed g1_pool_measure_only \
         g1_pool_off g1_pool_walk \
         h1_hook_defaults h1_hook_capture_bufs h1_hook_window_reject; do
    HOSTTEST_DUMP="$OUT/dump.$t" ; mkdir -p "$HOSTTEST_DUMP"; export HOSTTEST_DUMP
    timeout 60 "$BIN" "$t"
    rc=$?
    if [ $rc -ne 0 ]; then fails=$((fails + 1)); echo "  => $t FAILED (exit $rc)"; fi
done
echo "failed tests: $fails"
exit $fails
