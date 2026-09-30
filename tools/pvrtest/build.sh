#!/bin/sh
# Builds the offline PVR renderer check against build-linux32/libmgba.a and
# bloom's KOS shim + libpvr (a software PVR), both 32-bit like the shim.
# Rebuild libmgba first: cmake --build build-linux32 -j"$(nproc)"
# LIBPVR_DIR: libpvr + KOS shim, a copy of rearmed_bloom's plugins/gpu_pvr in tools/gpu_pvr.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-build-linux32}
GPU=${LIBPVR_DIR:-$ROOT/tools/gpu_pvr}
OUT="$ROOT/$BUILD/pvrtest-obj"
mkdir -p "$OUT"
for f in pvr ta core texture; do
	[ "$OUT/$f.o" -nt "$GPU/libpvr/$f.cpp" ] || \
		g++ -m32 -msse2 -mfpmath=sse -std=c++17 -O2 -w -I"$GPU" -c "$GPU/libpvr/$f.cpp" -o "$OUT/$f.o"
done
[ "$OUT/kos_pvr.o" -nt "$GPU/kos_pvr.cpp" ] || \
	g++ -m32 -msse2 -mfpmath=sse -std=c++17 -O2 -w -I"$GPU" -c "$GPU/kos_pvr.cpp" -o "$OUT/kos_pvr.o"
gcc -m32 -msse2 -mfpmath=sse -O2 -w -I"$GPU" -I"$GPU/kos" -c "$GPU/kos_prim.c" -o "$OUT/kos_prim.o"
CFLAGS="-m32 -msse2 -mfpmath=sse -O2 -g -D_GNU_SOURCE -DCOLOR_16_BIT -DCOLOR_5_6_5 -DM_CORE_GBA -DM_CORE_GB \
	-DPVR_GBA_WIDTH=${PVR_W:-240} -DPVR_GBA_HEIGHT=${PVR_H:-160} -DPVR_GBA_DEBUG \
	-I$ROOT/include -I$ROOT/$BUILD/include -I$ROOT/src -I$ROOT/src/platform/dreamcast \
	-I$ROOT/tools/pvrtest/include -I$GPU/kos"
gcc $CFLAGS -c "$ROOT/src/platform/dreamcast/pvr-gba.c" -o "$OUT/pvr-gba.o"
gcc $CFLAGS -c "$ROOT/tools/pvrtest/pvrtest.c" -o "$OUT/pvrtest.o"
g++ -m32 -flto=auto "$OUT/pvrtest.o" "$OUT/pvr-gba.o" "$OUT/kos_prim.o" "$OUT/kos_pvr.o" \
	"$OUT/pvr.o" "$OUT/ta.o" "$OUT/core.o" "$OUT/texture.o" \
	"$ROOT/$BUILD/libmgba.a" -lm -lpthread -o "$ROOT/$BUILD/pvrtest${PVR_W:+-$PVR_W}"
echo "built $ROOT/$BUILD/pvrtest${PVR_W:+-$PVR_W}"
