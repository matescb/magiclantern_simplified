#!/usr/bin/env bash
# flash-wifi.sh - build the M50 II firmware and push it to the camera over WiFi.
#
# The WiFi sibling of flash.sh: instead of the card shuffle (mount/cp/sync/umount),
# we build autoexec.bin, register the build in the mllog DB, serve it from this host,
# and let the camera dial in and pull it (see tools/mlserial/mlwifi.py for the wire
# protocol). The camera is the TCP client; we are the server.
#
# Usage:
#   ./flash-wifi.sh                    # build, serve --once, trigger camera, confirm reboot
#   ./flash-wifi.sh --note "msg"       # tag the build in the mllog DB
#   ./flash-wifi.sh --build-only       # just build + md5 + record (no serve)
#   ./flash-wifi.sh --no-reboot        # deploy, but don't poll for the post-flash boot
#   ./flash-wifi.sh --power-cycle      # after deploy, force a reboot via plug.py
#   ./flash-wifi.sh --port 5455        # override the TCP port mlwifi listens on
#
# Camera power-cycles are the operator's job unless --power-cycle is given.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PLATDIR="$HERE/../magiclantern_simplified/platform/M50mk2.101"
AUTOEXEC="$PLATDIR/build/autoexec.bin"
MLWIFI="$HERE/mlserial/mlwifi.py"
MLSERIAL="$HERE/mlserial/mlserial.py"
MLLOG="$HERE/mlserial/mllog.py"
PLUG="$HERE/plug/plug.py"
PORT=5455

NOTE=""
BUILD_ONLY=0
NO_REBOOT=0
POWER_CYCLE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --note) shift; NOTE="${1:-}" ;;
    --build-only) BUILD_ONLY=1 ;;
    --no-reboot) NO_REBOOT=1 ;;
    --power-cycle) POWER_CYCLE=1 ;;
    --port) shift; PORT="${1:-5455}" ;;
    *) echo "unknown arg: $1"; exit 2 ;;
  esac; shift
done

# --- 1. build ---------------------------------------------------------------
echo "== build =="
( cd "$PLATDIR" && make ) || { echo "BUILD FAILED"; exit 1; }
[ -f "$AUTOEXEC" ] || { echo "no autoexec.bin at $AUTOEXEC after build"; exit 1; }
MD5="$(md5sum "$AUTOEXEC" | cut -d' ' -f1)"
echo "built autoexec.bin  md5=$MD5"
python3 "$MLWIFI" crc "$AUTOEXEC"

# --- 2. record the build (ties boots -> build in mllog) ---------------------
python3 "$MLLOG" mark-build "$MD5" "$NOTE" >/dev/null 2>&1 || true

if [ "$BUILD_ONLY" = "1" ]; then
  echo "build-only; not serving."
  exit 0
fi

# --- 3. serve --once in the background --------------------------------------
echo "== serve =="
python3 "$MLWIFI" serve --once --port "$PORT" &
SERVER_PID=$!
sleep 0.5   # let it bind before we poke the camera

# --- 4. trigger the camera to start its WiFi pull ---------------------------
# Inject a synthetic INFO key into ML's module-key path. wifidep's CBR_KEYPRESS
# catches INFO and starts the WiFi pull. INFO is a key raw_vidx ignores, so this
# works in EVERY mode (incl. movie) with no conflict over the record key.
# handle_module_keys is a stable core symbol -- adding socket stubs shifts no .text --
# and the scratch address is read back from the `load` reply, so nothing is hardcoded.
SYM="$PLATDIR/build/M50mk2_101.sym"
HMK="0x$(grep -iE ' handle_module_keys$' "$SYM" 2>/dev/null | awk '{print $1}' | head -1)"
EVENT="00000000080000000000000000000000"   # {type=0, param=0x08 (BGMT_INFO), obj=0, arg=0}
echo "== trigger (INFO key -> wifidep) =="
LOADRSP="$(python3 "$MLSERIAL" cmd "load $EVENT" 2>/dev/null)"
SCRATCH="$(echo "$LOADRSP" | grep -oE '@0x[0-9A-Fa-f]+' | head -1 | tr -d '@')"
if [ -n "$SCRATCH" ] && [ "$HMK" != "0x" ]; then
  echo "triggering: call $HMK $SCRATCH"
  python3 "$MLSERIAL" cmd "call $HMK $SCRATCH" || echo "warn: trigger call failed"
else
  echo "!! could not stage trigger (camera booted? scratch='$SCRATCH' hmk='$HMK')"
  echo "   trigger manually: load $EVENT ; call $HMK <scratch-addr-from-load-reply>"
fi

# --- 5. wait for the deploy to finish ---------------------------------------
echo "waiting for camera to connect and pull (mlwifi --once)..."
wait "$SERVER_PID"
DEPLOY_RC=$?
if [ "$DEPLOY_RC" = "0" ]; then
  echo "DEPLOY SUCCESS (build $MD5)"
else
  echo "DEPLOY FAILED (mlwifi exit $DEPLOY_RC)"
  exit "$DEPLOY_RC"
fi

# --- 6. optional forced reboot / reboot confirmation ------------------------
if [ "$POWER_CYCLE" = "1" ]; then
  echo "== power-cycle =="
  python3 "$PLUG" off || echo "warn: plug off failed"
  sleep 3
  python3 "$PLUG" on  || echo "warn: plug on failed"
fi

if [ "$NO_REBOOT" = "1" ]; then
  echo "done (--no-reboot; not polling for the post-flash boot)."
  exit 0
fi

echo "== confirm reboot =="
echo "camera reboots itself into the new build; polling mllog for a fresh boot on ${MD5:0:12}..."
for i in $(seq 1 30); do
  sleep 2
  if python3 "$MLLOG" boots 1 2>/dev/null | grep -q "${MD5:0:12}"; then
    echo "SUCCESS: new boot confirmed on build ${MD5:0:12}"
    python3 "$MLLOG" boots 1
    exit 0
  fi
done
echo "FAIL: no boot on build ${MD5:0:12} seen within ~60s."
echo "  (deploy itself succeeded; the camera may still be rebooting, or the serial"
echo "   daemon isn't capturing it -- check: python3 $MLLOG boots 3)"
exit 4
