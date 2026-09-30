#!/bin/sh
# Runs inside the toolchain image (mounts: /port = port folder, /disc = extracted disc files, /repo = fft_decomp).
# usage: build_run_ram.sh HARNESS.c source1.c ...   (sources relative to /port/build/portable)
set -e
H=$1; shift
W=/tmp/nb; rm -rf $W; mkdir -p $W
CF="-m32 -O1 -w -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -nostdinc -I/port/build/portable/include"
OBJS=""
for s in "$@"; do
  o=$W/$(basename "$s" .c).o
  gcc $CF -c "/port/build/portable/$s" -o "$o"; OBJS="$OBJS $o"
done
gcc $CF -c "/port/native/$H" -o $W/harness.o
LD=/port/build/native/symbols.ld
# undefined = referenced but neither defined by an object nor a data symbol from the yaml
nm -u $OBJS $W/harness.o | awk '$1=="U"{print $2}' | sort -u > $W/u1.txt
nm --defined-only $OBJS $W/harness.o | awk 'NF>=3{print $3}' | sort -u > $W/d1.txt
sed -E 's/ = .*//' $LD | sort -u > $W/d2.txt
cat $W/d1.txt $W/d2.txt | sort -u > $W/d.txt
comm -23 $W/u1.txt $W/d.txt > $W/undef.txt
: > $W/stubs.c
for u in $(cat $W/undef.txt); do echo "int $u() { return 0; }" >> $W/stubs.c; done
if [ -s $W/stubs.c ]; then echo "stubbed (unresolved, treated as functions): $(wc -l < $W/undef.txt): $(tr '\n' ' ' < $W/undef.txt | cut -c1-200)"; gcc $CF -c $W/stubs.c -o $W/stubs.o; OBJS="$OBJS $W/stubs.o"; fi
gcc -m32 -static -nostdlib -Wl,-e,_start -o $W/prog $OBJS $W/harness.o $LD
$W/prog
