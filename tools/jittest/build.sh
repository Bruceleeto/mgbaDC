#!/bin/sh
# Builds the JIT lockstep harness against build-linux32/libmgba.a.
# Configure once:
#   cmake -S . -B build-linux32 -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 \
#         -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_LTO=OFF
# then: cmake --build build-linux32 -j"$(nproc)" && tools/jittest/build.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-build-linux32}
cc -m32 -O2 -g -flto=auto -D_GNU_SOURCE -DCOLOR_16_BIT -DCOLOR_5_6_5 -DM_CORE_GBA -DM_CORE_GB -DM_ARM_JIT \
	-I"$ROOT/include" -I"$ROOT/$BUILD/include" -I"$ROOT/src" \
	"$ROOT/tools/jittest/jittest.c" "$ROOT/$BUILD/libmgba.a" -lm -lpthread \
	-o "$ROOT/$BUILD/jittest"
