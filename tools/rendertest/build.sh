#!/bin/sh
# Builds the renderer test harness against $BUILD/libmgba.a (default build-linux).
# Rebuild libmgba first: cmake --build build-linux -j"$(nproc)"
# With BUILD=build-linux-prof (configured with -DM_PROFILE=ON) it also prints
# the section profile.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-build-linux}
EXTRA=
if grep -q "M_PROFILE:BOOL=ON" "$ROOT/$BUILD/CMakeCache.txt"; then
	EXTRA=-DM_PROFILE
fi
cc -O2 $EXTRA -flto=auto -D_GNU_SOURCE -DCOLOR_16_BIT -DCOLOR_5_6_5 -DM_CORE_GBA -DM_CORE_GB \
	-I"$ROOT/include" -I"$ROOT/$BUILD/include" -I"$ROOT/src" \
	"$ROOT/tools/rendertest/rendertest.c" "$ROOT/$BUILD/libmgba.a" -lm -lpthread \
	-o "$ROOT/$BUILD/rendertest"
