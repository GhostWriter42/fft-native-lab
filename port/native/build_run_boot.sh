#!/bin/sh
# Runs inside the toolchain image (mounts: /port = port folder, /disc = extracted disc files, /disc.bin = the raw disc image).
# Builds the boot probe (interpreter + software GTE + tables + harness) and runs it.
# env: EXTRA_CFLAGS (e.g. -DMAX_FRAMES=300 -DLOG_LIMIT=1000)
set -e
W=/tmp/bt; rm -rf $W; mkdir -p $W
N=/port/build/native
CF="-m32 -O1 -w -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-strict-aliasing -fwrapv -nostdinc -I/port/native/gte -I/port/native $EXTRA_CFLAGS"
for s in /port/native/gte/gte.c /port/native/r3000/r3000.c $N/func_addrs.c $N/sdk_ranges.c /port/native/harness_boot.c; do
  gcc $CF -c "$s" -o "$W/x_$(basename "$s" .c).o"
done
gcc $CF -fno-tree-loop-distribute-patterns -c /port/native/rt.c -o $W/x_rt.o
gcc -m32 -static -nostdlib -Wl,-e,_start -o $W/prog $W/x_*.o
$W/prog
