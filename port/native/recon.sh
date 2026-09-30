#!/bin/sh
# Reconnaissance: compile seed sources (portified) natively, report unresolved functions and whether a source file exists.
# usage: recon.sh source1.c ...    (relative to /port/build/portable)
W=/tmp/rc; rm -rf $W; mkdir -p $W
CF="-m32 -O1 -w -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -nostdinc -I/port/build/portable/include"
OBJS=""; fail=0
for s in "$@"; do
  o=$W/$(basename "$s" .c).o
  case "$s" in /*) sp="$s";; *) sp="/port/build/portable/$s";; esac; if gcc $CF -c "$sp" -o "$o" 2> $W/err.txt; then OBJS="$OBJS $o"; else fail=$((fail+1)); echo "COMPILE FAIL: $s: $(head -1 $W/err.txt)"; fi
done
echo "compiled $(echo $OBJS | wc -w) of $# sources ($fail failed)"
nm -u $OBJS | awk '$1=="U"{print $2}' | sort | uniq -c | sort -rn > $W/u.txt
nm --defined-only $OBJS | awk 'NF>=3{print $3}' | sort -u > $W/d.txt
sed -E 's/ = .*//' /port/build/native/symbols.ld | sort -u > $W/ld.txt
cat $W/d.txt $W/ld.txt | sort -u > $W/all_defined.txt
awk '{print $2}' $W/u.txt | sort > $W/uu.txt
comm -23 $W/uu.txt $W/all_defined.txt > $W/undef.txt
echo "undefined symbols: $(wc -l < $W/undef.txt)"
for n in $(cat $W/undef.txt); do
  f=$(find /port/build/portable/src -name "$n.c" | head -1)
  c=$(grep -E " $n\$" $W/u.txt | awk '{print $1}')
  echo "$c $n ${f:+HAS-SOURCE:$(echo $f | sed 's#/port/build/portable/##')}"
done | sort -rn | head -${TOP:-60}
