#!/usr/bin/env bash
# mk_sd_image.sh - build a fresh QEMU SD image carrying the CURRENT ML build.
#
#   mk_sd_image.sh OUTDIR [IMAGE_NAME]          (IMAGE_NAME default: sd.raw)
#
# Copies m50ii_dumper/qemu_stage/sd_mltest.raw (never modified) to
# OUTDIR/IMAGE_NAME, then with mtools (no sudo, no mount):
#   - replaces ::/AUTOEXEC.BIN with build/autoexec.bin
#   - deletes ::/ML and recreates it from build/zip/ML (fonts, data, ...),
#     with ML/modules holding ONLY the matching .sym plus the modules listed
#     in $MODULES (default: raw_vidx bench file_man)
#   - lists the image and compares md5 of every file it wrote with its source
#
# Refuses to run when the .sym is older than autoexec.bin (a stale .sym makes
# module symbol resolution point at wrong addresses -> crash on module load).
#
# Environment:
#   MODULES      space-separated module names        (raw_vidx bench file_man)
#   MODULE_SRC   build | tree | auto                 (auto)
#       build = platform/<cam>/build/modules/<m>.mo  (what "make" staged)
#       tree  = modules/**/<m>/build/<m>.mo          (the per-module build)
#       auto  = whichever of the two has the later embedded "Build date"; a
#               warning is printed when they differ, because build/modules
#               can be a stale copy (it was, for all three, on 2026-11-16).
#   ML_BUILD, BASE_SD, FAT_OFFSET ...                see common.sh
#
# Exit code 0 only if every md5 comparison passed.

set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

[ $# -ge 1 ] || die "usage: $0 OUTDIR [IMAGE_NAME]"
OUTDIR="$1"
IMAGE_NAME="${2:-sd.raw}"
MODULES="${MODULES:-raw_vidx bench file_man}"
MODULE_SRC="${MODULE_SRC:-auto}"

AUTOEXEC="$ML_BUILD/autoexec.bin"
SYM="$ML_BUILD/$ML_SYM_NAME"
ZIP_ML="$ML_BUILD/zip/ML"

command -v mcopy >/dev/null 2>&1 || die "mtools not found (apt-get download mtools && dpkg -x into $QEMU_TOOLS_DIR/vendor/mtools)"
[ -f "$BASE_SD" ]  || die "base SD image missing: $BASE_SD"
[ -f "$AUTOEXEC" ] || die "no autoexec.bin in $ML_BUILD (build first)"
[ -f "$SYM" ]      || die "no $ML_SYM_NAME in $ML_BUILD (build first)"
[ -d "$ZIP_ML" ]   || die "no zip/ML staging dir in $ML_BUILD"

# --- stale-symbol guard ------------------------------------------------------
if [ "$SYM" -ot "$AUTOEXEC" ]; then
    die "STALE SYM: $SYM ($(date -r "$SYM" '+%F %T')) is older than autoexec.bin ($(date -r "$AUTOEXEC" '+%F %T')). Rebuild so both come from the same link; refusing to make an image."
fi
# The .sym is generated from the ELF 'magiclantern'; it must not predate it.
if [ -f "$ML_BUILD/magiclantern" ] && [ "$SYM" -ot "$ML_BUILD/magiclantern" ]; then
    die "STALE SYM: $SYM is older than the linked ELF $ML_BUILD/magiclantern."
fi

case "$OUTDIR" in
    "$(dirname "$BASE_SD")"|"$(dirname "$BASE_SD")"/*) die "OUTDIR must not be inside $(dirname "$BASE_SD")";;
esac
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"
IMG="$OUTDIR/$IMAGE_NAME"
[ "$(readlink -f "$IMG")" != "$(readlink -f "$BASE_SD")" ] || die "output would overwrite the base image"

# --- pick module files -------------------------------------------------------
mo_build_date() {   # embedded module build timestamp, e.g. "2026-07-21 15:40:54 UTC"
    local d
    d="$(strings "$1" | grep -m1 -E '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} UTC$' || true)"
    [ -n "$d" ] || d="$(date -u -r "$1" '+%F %T UTC')"
    echo "$d"
}

pick_module() {   # $1 = module name; prints chosen path
    local m="$1" b t
    b="$ML_BUILD/modules/$m.mo"
    t="$(find "$ML_TREE/modules" -path "*/$m/build/$m.mo" -type f 2>/dev/null | head -n1)"
    [ -f "$b" ] || b=""
    case "$MODULE_SRC" in
        build) [ -n "$b" ] || die "module $m.mo not in $ML_BUILD/modules"; echo "$b";;
        tree)  [ -n "$t" ] || die "module $m.mo has no per-module build under $ML_TREE/modules"; echo "$t";;
        auto)
            [ -n "$b$t" ] || die "module $m.mo not found anywhere"
            if [ -n "$b" ] && [ -n "$t" ] && ! cmp -s "$b" "$t"; then
                # build/modules is filled by a plain cp, so its mtime says
                # nothing; compare the "Build date" string embedded in the .mo.
                local db dt
                db="$(mo_build_date "$b")"; dt="$(mo_build_date "$t")"
                if [[ "$dt" > "$db" ]]; then
                    note "WARNING: $m.mo in build/modules is STALE (built $db, $(stat -c %s "$b") bytes); using per-module build $t (built $dt, $(stat -c %s "$t") bytes)"
                    echo "$t"
                else
                    note "WARNING: $m.mo differs between build/modules (built $db) and $t (built $dt); using build/modules"
                    echo "$b"
                fi
            else
                echo "${b:-$t}"
            fi;;
        *) die "MODULE_SRC must be build|tree|auto";;
    esac
}

declare -A MOD_PATH
for m in $MODULES; do MOD_PATH[$m]="$(pick_module "$m")"; done

# --- stage the ML directory --------------------------------------------------
STAGE="$OUTDIR/.stage.$$"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/ML"
( cd "$ZIP_ML" && find . -mindepth 1 -maxdepth 1 ! -name modules -exec cp -r {} "$STAGE/ML/" \; )
mkdir -p "$STAGE/ML/modules"
cp -p "$SYM" "$STAGE/ML/modules/$ML_SYM_NAME"
for m in $MODULES; do cp -p "${MOD_PATH[$m]}" "$STAGE/ML/modules/$m.mo"; done

# --- build the image ---------------------------------------------------------
note "copying base image -> $IMG"
cp --sparse=always "$BASE_SD" "$IMG"
chmod u+w "$IMG"
D="$IMG@@$FAT_OFFSET"

mdir -i "$D" :: >/dev/null || die "cannot read FAT at offset $FAT_OFFSET in $IMG"
mdeltree -i "$D" ::/ML 2>/dev/null || true
mdel     -i "$D" ::/AUTOEXEC.BIN 2>/dev/null || true
mcopy -i "$D" -o -m "$AUTOEXEC" ::/AUTOEXEC.BIN
mcopy -i "$D" -s -m "$STAGE/ML" ::/

# --- verify ------------------------------------------------------------------
echo "=== image contents ($IMG) ==="
mdir -i "$D" -/ ::
echo
echo "=== md5 verification (image vs source) ==="
fail=0
verify() {   # $1 = path in image, $2 = host source file
    local a b
    a="$(mcopy -i "$D" "::/$1" - | md5sum | cut -d' ' -f1)"
    b="$(md5sum < "$2" | cut -d' ' -f1)"
    if [ "$a" = "$b" ]; then printf 'OK    %s  %-28s <- %s\n' "$a" "$1" "$2"
    else printf 'FAIL  %s != %s  %s <- %s\n' "$a" "$b" "$1" "$2"; fail=1; fi
}
verify "AUTOEXEC.BIN" "$AUTOEXEC"
verify "ML/modules/$ML_SYM_NAME" "$SYM"
for m in $MODULES; do verify "ML/modules/$m.mo" "${MOD_PATH[$m]}"; done
# Anything else in ML/modules would be a leftover from the stale image.
extra="$(mdir -i "$D" -b ::/ML/modules/ | sed 's#.*/##' | grep -v -x -F -e "$ML_SYM_NAME" $(for m in $MODULES; do printf -- '-e %s.mo ' "$m"; done) || true)"
if [ -n "$extra" ]; then echo "FAIL  unexpected files in ML/modules: $extra"; fail=1; fi

{
    echo "image=$IMG"
    echo "built=$(date '+%F %T %z')"
    echo "ml_commit=$(git -C "$ML_TREE" rev-parse --short HEAD 2>/dev/null || echo unknown) ($(git -C "$ML_TREE" rev-parse --abbrev-ref HEAD 2>/dev/null || echo ?))"
    echo "autoexec_md5=$(md5sum < "$AUTOEXEC" | cut -d' ' -f1) mtime=$(date -r "$AUTOEXEC" '+%F %T')"
    echo "sym_md5=$(md5sum < "$SYM" | cut -d' ' -f1) mtime=$(date -r "$SYM" '+%F %T')"
    for m in $MODULES; do echo "module $m.mo md5=$(md5sum < "${MOD_PATH[$m]}" | cut -d' ' -f1) built=\"$(mo_build_date "${MOD_PATH[$m]}")\" src=${MOD_PATH[$m]}"; done
} > "$IMG.manifest"

if [ "$fail" -ne 0 ]; then die "verification FAILED for $IMG"; fi
echo
echo "image ready: $IMG  (manifest: $IMG.manifest)"
