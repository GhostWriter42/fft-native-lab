#!/bin/sh
# Runs inside the toolchain image. Builds a differential harness: native libgte (+ software GTE) and the R3000 oracle,
# linked against the RAM-image symbols and the table of original function addresses, then runs it.
# usage: build_run_diff.sh HARNESS.c
set -e
W=/tmp/df; rm -rf $W; mkdir -p $W
CF="-m32 -O1 -w -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-strict-aliasing -fwrapv -nostdinc -I/port/build/portable/include -I/port/native/gte -I/port/native"
P=/port/build/portable/src/psyq/libgte
OBJS=""
for s in /port/native/gte/gte.c /port/native/gte/libgte_native.c /port/native/r3000/r3000.c $P/rsin.c $P/sin_1.c $P/rcos.c $P/ratan2.c $P/csqrt.c $P/psyq_gte_csqrt_kernel.c /port/build/native/func_addrs.c $EXTRA_SRC /port/native/$1; do
  o=$W/$(basename "$s" .c).o
  gcc $CF -c "$s" -o "$o"; OBJS="$OBJS $o"
done
gcc $CF -fno-tree-loop-distribute-patterns -c /port/native/rt.c -o $W/rt.o
gcc -m32 -static -nostdlib -Wl,-e,_start -o $W/prog $OBJS $W/rt.o /port/build/native/symbols.ld
$W/prog
