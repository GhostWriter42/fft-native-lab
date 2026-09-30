#!/bin/sh
# Runs inside the toolchain image. Compiles game sources (portified) + a harness for 32-bit, links freestanding, runs.
# usage: build_run.sh HARNESS.c source1.c [source2.c ...]     (sources relative to /port/build/portable)
set -e
H=$1; shift
W=/tmp/nb; rm -rf $W; mkdir -p $W
CF="-m32 -O1 -w -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -nostdinc -I/port/build/portable/include -I/port/build/native"
OBJS=""
for s in "$@"; do
  o=$W/$(basename "$s" .c).o
  gcc $CF -c "/port/build/portable/$s" -o "$o"
  OBJS="$OBJS $o"
done
gcc $CF -c "/port/native/$H" -o $W/harness.o
# undefined = referenced by some object but defined by none; those become zeroed blobs so the link succeeds
nm -u $OBJS $W/harness.o 2>/dev/null | awk '$1=="U"{print $2}' | sort -u > $W/u1.txt || true
nm --defined-only $OBJS $W/harness.o | awk 'NF>=3{print $3}' | sort -u > $W/d1.txt
comm -23 $W/u1.txt $W/d1.txt > $W/undef.txt
: > $W/stubs.c
for u in $(cat $W/undef.txt); do echo "char $u[65536] __attribute__((aligned(16)));" >> $W/stubs.c; done
if [ -s $W/stubs.c ]; then echo "stubbed undefined symbols: $(wc -l < $W/undef.txt): $(tr '
' ' ' < $W/undef.txt)"; gcc $CF -c $W/stubs.c -o $W/stubs.o; OBJS="$OBJS $W/stubs.o"; fi
gcc -m32 -static -nostdlib -Wl,-e,_start -o $W/prog $OBJS $W/harness.o
$W/prog
