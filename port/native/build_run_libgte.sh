#!/bin/sh
# Runs inside the toolchain image (mounts: /port = port folder, /disc = extracted disc files).
# Builds the native libgte (software GTE + libgte_native.c + the plain-C libgte members from the portified repo) with a
# test harness and runs it against the game's own sine / sqrt / atan tables (RAM image).
set -e
W=/tmp/lg; rm -rf $W; mkdir -p $W
CF="-m32 -O1 -w -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-strict-aliasing -fwrapv -nostdinc -I/port/build/portable/include -I/port/native/gte"
P=/port/build/portable/src/psyq/libgte
OBJS=""
for s in /port/native/gte/gte.c /port/native/gte/libgte_native.c $P/rsin.c $P/sin_1.c $P/rcos.c $P/ratan2.c $P/csqrt.c $P/psyq_gte_csqrt_kernel.c /port/native/${1:-harness_libgte.c}; do
  o=$W/$(basename "$s" .c).o
  gcc $CF -c "$s" -o "$o"; OBJS="$OBJS $o"
done
gcc -m32 -static -nostdlib -Wl,-e,_start -o $W/prog $OBJS /port/build/native/symbols.ld
$W/prog
