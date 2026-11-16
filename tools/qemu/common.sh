# shellcheck shell=bash
# common.sh - shared paths for the M50 Mark II QEMU harness (tools/qemu).
# Sourced by mk_sd_image.sh / run_m50mk2.sh. Every value can be overridden
# from the environment.

QEMU_TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EOS_ROOT="${EOS_ROOT:-$(cd "$QEMU_TOOLS_DIR/../.." && pwd)}"

ML_TREE="${ML_TREE:-$EOS_ROOT/magiclantern_simplified}"
ML_PLATFORM="${ML_PLATFORM:-M50mk2.101}"
ML_BUILD="${ML_BUILD:-$ML_TREE/platform/$ML_PLATFORM/build}"
ML_SYM_NAME="${ML_SYM_NAME:-M50mk2_101.sym}"

BASE_SD="${BASE_SD:-$EOS_ROOT/m50ii_dumper/qemu_stage/sd_mltest.raw}"
BASE_CF="${BASE_CF:-$EOS_ROOT/m50ii_dumper/qemu_stage/cf.base.raw}"
# FAT16 partition starts at sector 99 (99 * 512 = 50688) in sd_mltest.raw.
FAT_OFFSET="${FAT_OFFSET:-50688}"

QEMU_BIN="${QEMU_BIN:-$EOS_ROOT/qemu-eos-build/arm-softmmu/qemu-system-arm}"
QEMU_WORKDIR="${QEMU_WORKDIR:-$EOS_ROOT/roms}"

# mtools: use the system one if installed, else the copy unpacked (dpkg -x,
# no sudo) under tools/qemu/vendor/mtools.
VENDOR_MTOOLS="$QEMU_TOOLS_DIR/vendor/mtools/usr/bin"
if ! command -v mcopy >/dev/null 2>&1 && [ -x "$VENDOR_MTOOLS/mtools" ]; then
    PATH="$VENDOR_MTOOLS:$PATH"
fi
# The partition geometry in the old image trips mtools' sanity checks.
export MTOOLS_SKIP_CHECK=1

die()  { echo "ERROR: $*" >&2; exit 1; }
note() { echo "[$(basename "$0")] $*" >&2; }
