#!/usr/bin/env bash
# run_m50mk2.sh - launch the known-good qemu-eos M50 Mark II model.
#
#   run_m50mk2.sh [options] [-- extra qemu args]
#
#   --sd IMG          SD image (required; make one with mk_sd_image.sh)
#   --cf IMG          CF/IDE image (default m50ii_dumper/qemu_stage/cf.base.raw;
#                     a CF drive is required or Canon prints "CF init failed")
#   --log FILE        UART log file (default ./uart.log). Always written.
#   --tcp PORT        also expose the UART on tcp:127.0.0.1:PORT (server,nowait)
#                     for the serial shell. Without it: -serial file:FILE.
#   --gdb PORT        open a gdbstub on tcp::PORT
#   --freeze          start halted (-S); needs --gdb
#   --boot 0|1        firmware=boot=N  (1 = run autoexec.bin, default; 0 = Canon only)
#   --timeout SECS    hard wall-clock limit, QEMU is killed after it (default 120)
#   --rw              let QEMU write to the SD image (default: snapshot=on,
#                     the image file is never modified)
#   --detach          return right after start; prints "QEMU_PID=<pid>" on
#                     stdout and writes --pidfile. A watchdog still enforces
#                     --timeout. Stop it with: kill <pid>
#   --pidfile FILE    where to write the PID (default <log>.pid)
#
# Documented base command (docs/24 section 1, docs/33 "QEMU test rig"):
#   QEMU_EOS_WORKDIR=<eos>/roms qemu-system-arm -M M50mk2,firmware=boot=1 \
#     -drive if=sd,format=raw,file=SD -drive if=ide,format=raw,file=CF,snapshot=on \
#     -display none -serial ...
#
# Cleanup is PID-based only (never pkill -f: the pattern would match the
# launching shell itself, see docs/20). Foreground mode kills QEMU on
# EXIT/INT/TERM. Exit code: QEMU's status, or 124 if the timeout fired.

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

SD="" CF="$BASE_CF" LOG="uart.log" TCP="" GDB="" FREEZE=0 BOOT=1 TMO=120 RW=0 DETACH=0 PIDFILE=""
EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --sd) SD="$2"; shift 2;;
        --cf) CF="$2"; shift 2;;
        --log) LOG="$2"; shift 2;;
        --tcp) TCP="$2"; shift 2;;
        --gdb) GDB="$2"; shift 2;;
        --freeze) FREEZE=1; shift;;
        --boot) BOOT="$2"; shift 2;;
        --timeout) TMO="$2"; shift 2;;
        --rw) RW=1; shift;;
        --detach) DETACH=1; shift;;
        --pidfile) PIDFILE="$2"; shift 2;;
        -h|--help) sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
        --) shift; EXTRA=("$@"); break;;
        *) die "unknown option $1 (see --help)";;
    esac
done

[ -n "$SD" ] || die "--sd IMG is required"
[ -f "$SD" ] || die "SD image not found: $SD"
[ -f "$CF" ] || die "CF image not found: $CF"
[ -x "$QEMU_BIN" ] || die "qemu binary not found: $QEMU_BIN"
[ -e "$QEMU_WORKDIR/M50mk2/ROM0.BIN" ] || die "ROM missing: $QEMU_WORKDIR/M50mk2/ROM0.BIN"
[ "$FREEZE" -eq 0 ] || [ -n "$GDB" ] || die "--freeze needs --gdb PORT"
case "$TMO" in ''|*[!0-9]*) die "--timeout must be an integer number of seconds";; esac
[ "$(readlink -f "$SD")" != "$(readlink -f "$BASE_SD")" ] || [ "$RW" -eq 0 ] || die "refusing --rw on the pristine base image"

SD="$(readlink -f "$SD")"; CF="$(readlink -f "$CF")"
mkdir -p "$(dirname "$LOG")"
LOG="$(cd "$(dirname "$LOG")" && pwd)/$(basename "$LOG")"
PIDFILE="${PIDFILE:-$LOG.pid}"
: > "$LOG"

port_busy() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
[ -z "$TCP" ] || ! port_busy "$TCP" || die "serial TCP port $TCP already in use"
[ -z "$GDB" ] || ! port_busy "$GDB" || die "gdb TCP port $GDB already in use"

SD_OPTS="if=sd,format=raw,file=$SD"
[ "$RW" -eq 1 ] || SD_OPTS="$SD_OPTS,snapshot=on"

ARGS=( -M "M50mk2,firmware=boot=$BOOT"
       -drive "$SD_OPTS"
       -drive "if=ide,format=raw,file=$CF,snapshot=on"
       -display none -monitor none )
if [ -n "$TCP" ]; then
    # chardev logfile= gives a complete UART log even while no client is attached.
    ARGS+=( -chardev "socket,id=uart0,host=127.0.0.1,port=$TCP,server,nowait,logfile=$LOG,logappend=off"
            -serial chardev:uart0 )
else
    ARGS+=( -serial "file:$LOG" )
fi
[ -z "$GDB" ] || ARGS+=( -gdb "tcp::$GDB" )
[ "$FREEZE" -eq 0 ] || ARGS+=( -S )
ARGS+=( "${EXTRA[@]}" )

note "QEMU_EOS_WORKDIR=$QEMU_WORKDIR $QEMU_BIN ${ARGS[*]}"

# QEMU's own stderr/stdout (model debug prints) go next to the UART log.
QEMU_EOS_WORKDIR="$QEMU_WORKDIR" "$QEMU_BIN" "${ARGS[@]}" >"$LOG.qemu-stderr" 2>&1 </dev/null &
QPID=$!
echo "$QPID" > "$PIDFILE"

# only ever signal the PID we started, and only while it is still our qemu
is_ours() { [ "$(readlink -f "/proc/$QPID/exe" 2>/dev/null)" = "$(readlink -f "$QEMU_BIN")" ]; }
kill_qemu() {
    is_ours || return 0
    kill "$QPID" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do is_ours || return 0; sleep 0.2; done
    kill -9 "$QPID" 2>/dev/null || true
}

TMO_FLAG="$LOG.timedout"; rm -f "$TMO_FLAG"
(
    # watchdog: detached from our stdio so --detach callers are not held open
    exec >/dev/null 2>&1 </dev/null
    end=$(( $(date +%s) + TMO ))
    while [ "$(date +%s)" -lt "$end" ]; do is_ours || exit 0; sleep 1; done
    if is_ours; then : > "$TMO_FLAG"; kill_qemu; fi
) &
WPID=$!

sleep 0.5
if ! is_ours; then
    wait "$QPID" 2>/dev/null && rc=0 || rc=$?
    echo "--- qemu output ---" >&2; cat "$LOG.qemu-stderr" >&2
    die "QEMU exited immediately (status $rc)"
fi

if [ "$DETACH" -eq 1 ]; then
    disown "$QPID" "$WPID" 2>/dev/null || true
    note "detached: pid $QPID, UART log $LOG, timeout ${TMO}s${TCP:+, serial tcp:127.0.0.1:$TCP}${GDB:+, gdb :$GDB}"
    echo "QEMU_PID=$QPID"
    exit 0
fi

trap 'kill_qemu; kill "$WPID" 2>/dev/null || true' EXIT
trap 'exit 130' INT TERM
note "running: pid $QPID, UART log $LOG, timeout ${TMO}s${TCP:+, serial tcp:127.0.0.1:$TCP}${GDB:+, gdb :$GDB}"
wait "$QPID" 2>/dev/null && rc=0 || rc=$?
if [ -e "$TMO_FLAG" ]; then note "timeout after ${TMO}s - QEMU killed"; rc=124; fi
exit "$rc"
