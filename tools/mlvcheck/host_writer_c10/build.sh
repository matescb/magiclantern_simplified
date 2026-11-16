#!/bin/bash
# Build the host MLV lane OUT of the ML tree:
#   bin/mlv_dump      modules/raw_video/mlv_rec/mlv_dump.c (+lzma, lj92, dng, raw_proc), -m32
#   bin/create_mlv    raw_vidx/mlv_3_sample_code/create_mlv.c (stock stand-ins)
#   bin/rvx_host_mlv  raw_vidx/mlv_3.c (unmodified) + worker.c replay (this dir)
# Nothing is written inside the ML checkout.
#
# usage: build.sh [ML_TREE] [OUT_DIR]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ML="${1:-/mnt/data/WORK/eos/magiclantern_simplified}"
OUT="${2:-/mnt/data/WORK/eos/sim/C1}"
RVX="$ML/modules/raw_video/raw_vidx"
SC="$RVX/mlv_3_sample_code"
mkdir -p "$OUT/bin" "$OUT/obj/dump" "$OUT/obj/rvx" "$OUT/obj/sample"

# ---- mlv_dump -------------------------------------------------------------
# mlv_dump.c uses includes relative to modules/mlv_rec (../../module_strings_wrapper.h,
# ../../lv_rec/lv_rec.h, ../../../src/raw.h, ./build/module_strings.h); on this branch the
# directory lives one level deeper (modules/raw_video/mlv_rec), so the in-tree Makefile
# target cannot resolve them.  Mirror the layout the sources expect.
T="$OUT/tree"
mkdir -p "$T/modules"
rsync -a --delete --exclude 'build/*.o' --exclude 'build/*.d' --exclude 'build/*.mo' \
      --exclude 'build/*.sym' --exclude 'build/*.dep' --exclude obj \
      "$ML/modules/raw_video/mlv_rec" "$T/modules/"
ln -sfn "$ML/src" "$T/src"
ln -sfn "$ML/modules/lv_rec" "$T/modules/lv_rec"
ln -sfn "$ML/modules/module_strings_wrapper.h" "$T/modules/module_strings_wrapper.h"
if [ ! -f "$T/modules/mlv_rec/build/module_strings.h" ]; then
    echo "missing build/module_strings.h (build the mlv_rec module once)"; exit 1
fi
# src/raw.h uses SIZE_CHECK_STRUCT without including compiler.h -> neutralise on host.
DCF=(-m32 -O2 -std=gnu99 -w -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
     -I. -I../../src -Ilzma -DMLV_USE_LZMA -DMLV_USE_LJ92 -D_7ZIP_ST
     -mno-ms-bitfields '-DSIZE_CHECK_STRUCT(a,b)=')
LZ="7zAlloc 7zBuf 7zBuf2 7zCrc 7zCrcOpt 7zDec 7zFile 7zIn 7zStream Alloc Bcj2 Bra Bra86
    BraIA64 CpuArch Delta LzFind Lzma2Dec Lzma2Enc Lzma86Dec Lzma86Enc LzmaDec LzmaEnc
    LzmaLib Ppmd7 Ppmd7Dec Ppmd7Enc Sha256 Xz XzCrc64"
( cd "$T/modules/mlv_rec"
  for f in $LZ; do gcc "${DCF[@]}" -c "lzma/$f.c" -o "$OUT/obj/dump/lz_$f.o"; done
  for f in mlv_dump lj92 dng/dng raw_proc/stripes raw_proc/pixel_proc raw_proc/patternnoise \
           raw_proc/histogram; do
      gcc "${DCF[@]}" -c "$f.c" -o "$OUT/obj/dump/$(basename "$f").o"
  done )
gcc -m32 "$OUT"/obj/dump/*.o -o "$OUT/bin/mlv_dump" -lm
echo "built $OUT/bin/mlv_dump"

# ---- mlv_3.c (shared by both writers) --------------------------------------
# -DMODULE: mlv_3.c is module code; without it src/property.h hits
# "#error No PROP_PICSTYLE scheme defined" (the stock sample Makefile lacks it).
MCF=(-m32 -ggdb3 -DMODULE -I"$RVX" -I"$SC/include" -I"$ML/src")
gcc "${MCF[@]}" -c "$RVX/mlv_3.c" -o "$OUT/obj/rvx/mlv_3.o"

# ---- stock sample: create_mlv ----------------------------------------------
# raw_vid.h now routes SEND_LOG_DATA_STR to m50ii_uart_str(), which the stock
# function_overrides.c does not provide -> add a one-line stub out of tree.
gcc "${MCF[@]}" -c "$SC/function_overrides.c" -o "$OUT/obj/sample/function_overrides.o"
printf 'void m50ii_uart_str(const char *s) { (void)s; }\n' > "$OUT/obj/sample/uart_stub.c"
gcc -m32 -c "$OUT/obj/sample/uart_stub.c" -o "$OUT/obj/sample/uart_stub.o"
gcc "${MCF[@]}" -o "$OUT/bin/create_mlv" "$SC/create_mlv.c" \
    "$OUT/obj/sample/function_overrides.o" "$OUT/obj/sample/uart_stub.o" "$OUT/obj/rvx/mlv_3.o"
echo "built $OUT/bin/create_mlv"

# ---- rvx_host_mlv -----------------------------------------------------------
gcc "${MCF[@]}" -I"$HERE" -c "$HERE/rvx_standins.c" -o "$OUT/obj/rvx/rvx_standins.o"
gcc -m32 -O1 -Wall -I"$RVX" -I"$HERE" -c "$HERE/rvx_host_mlv.c" -o "$OUT/obj/rvx/rvx_host_mlv.o"
gcc -m32 -o "$OUT/bin/rvx_host_mlv" "$OUT/obj/rvx/rvx_host_mlv.o" \
    "$OUT/obj/rvx/rvx_standins.o" "$OUT/obj/rvx/mlv_3.o"
echo "built $OUT/bin/rvx_host_mlv"
