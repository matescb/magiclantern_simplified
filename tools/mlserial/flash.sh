#!/usr/bin/env bash
# flash.sh - one-shot build + stage mlv_lite.mo to the SD card + verify + record build.
#
# Replaces the manual build / mount / cp / sync / umount / remount / md5 dance that
# was done by hand every iteration. Verifies the on-card md5 from a FRESH remount
# (page cache lies) and records the build in the mllog DB so boots tie to a build.
#
# Usage:
#   ./flash.sh                 # build, then flash if the card is in the reader
#   ./flash.sh --build-only    # just build + md5 (card in camera)
#   ./flash.sh --note "msg"    # attach a note to the recorded build
#
# Card is /dev/sdc1 (EOS_DEVELOP). Camera power-cycles are done by the operator.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
MODDIR="/mnt/data/WORK/eos/magiclantern_simplified/modules/raw_video/mlv_lite"
MO="$MODDIR/build/mlv_lite.mo"
CARD_DEV="/dev/sdc1"
MNT="/mnt/mlcard"
ONCARD="ML/MODULES/mlv_lite.mo"
NOTE=""
BUILD_ONLY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --build-only) BUILD_ONLY=1 ;;
    --note) shift; NOTE="${1:-}" ;;
    *) echo "unknown arg: $1"; exit 2 ;;
  esac; shift
done

echo "== build =="
( cd "$MODDIR" && make ) || { echo "BUILD FAILED"; exit 1; }
MD5="$(md5sum "$MO" | cut -d' ' -f1)"
echo "built mlv_lite.mo  md5=$MD5"

python3 "$HERE/mllog.py" mark-build "$MD5" "$NOTE" >/dev/null 2>&1 || true

if [ "$BUILD_ONLY" = "1" ]; then
  echo "build-only; not flashing."
  exit 0
fi

# flash only if the card is actually in the reader (partition size > 0)
SZ="$(lsblk -bno SIZE "$CARD_DEV" 2>/dev/null | head -1)"
if [ -z "$SZ" ] || [ "$SZ" = "0" ]; then
  echo "SD card not in reader ($CARD_DEV absent/0B) -> card is in the camera."
  echo "Move the card to the reader and re-run, or use the over-serial deploy (Phase B)."
  exit 3
fi

echo "== flash =="
sudo mkdir -p "$MNT" 2>/dev/null
sudo mount "$CARD_DEV" "$MNT" || { echo "mount failed"; exit 4; }
sudo rm -f "$MNT"/M50TEST.MLV "$MNT"/*.TXT 2>/dev/null
sudo cp "$MO" "$MNT/$ONCARD" && sync
sudo umount "$MNT"
sudo mount "$CARD_DEV" "$MNT"
CARDMD5="$(sudo md5sum "$MNT/$ONCARD" | cut -d' ' -f1)"
sudo umount "$MNT"
if [ "$CARDMD5" = "$MD5" ]; then
  echo "on-card md5 (fresh remount) = $CARDMD5  OK"
  echo "SAFE TO REMOVE. Put card in camera, cold-boot into Movie+LiveView, then: python3 mlresolve.py"
else
  echo "MD5 MISMATCH: card=$CARDMD5 expected=$MD5  -> re-flash, do NOT boot"
  exit 5
fi
