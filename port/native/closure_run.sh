#!/bin/sh
# Grow a native slice: start from seed sources, repeatedly compile the sources of still-unresolved functions
# (matching $ALLOW), up to $MAX files. Prints the growth and the final unresolved list, then LINKS + RUNS if $HARNESS is set.
# env: ALLOW (regex on function names, default '.'), MAX (default 400), HARNESS (harness .c under /port/native), SKIP (space list)
W=/tmp/cl; rm -rf $W; mkdir -p $W/o
CF="-m32 -O1 -w -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-strict-aliasing -fno-aggressive-loop-optimizations -fwrapv -nostdinc -I/port/build/portable/include -I/port/native/gte -I/port/native $EXTRA_CFLAGS"
ALLOW=${ALLOW:-.}; MAX=${MAX:-400}; SKIP=" ${SKIP} rand abs "
LD=/port/build/native/symbols.ld
sed -E 's/ = .*//' $LD | sort -u > $W/ld.txt
: > $W/queue.txt; : > $W/done.txt
for s in "$@"; do echo "$s" >> $W/queue.txt; done
compile() { # $1 = source path (relative to portable, or absolute)
  case "$1" in /*) sp="$1";; *) sp="/port/build/portable/$1";; esac
  o=$W/o/$(echo "$1" | tr / _).o
  if gcc $CF -c "$sp" -o "$o" 2> $W/err.txt; then echo "$1" >> $W/done.txt; else echo "COMPILE FAIL: $1: $(head -1 $W/err.txt | cut -c1-120)"; echo "$(basename "$1" .c)" >> $W/failed.txt; fi
}
round=0
while [ -s $W/queue.txt ]; do
  round=$((round+1))
  for s in $(cat $W/queue.txt); do compile "$s"; done
  : > $W/queue.txt
  nm --defined-only $W/o/*.o 2>/dev/null | awk 'NF>=3{print $3}' | sort -u > $W/d.txt
  nm -u $W/o/*.o 2>/dev/null | awk '$1=="U"{print $2}' | sort -u > $W/u.txt
  cat $W/d.txt $W/ld.txt | sort -u > $W/alld.txt
  comm -23 $W/u.txt $W/alld.txt > $W/undef.txt
  added=0
  for n in $(cat $W/undef.txt); do
    case "$SKIP" in *" $n "*) continue;; esac
    echo "$n" | grep -Eq "$ALLOW" || continue
    grep -qx "$n" $W/failed.txt 2>/dev/null && continue
    f=$(find /port/build/portable/src -name "$n.c" | head -1)
    [ -n "$f" ] || continue
    total=$(wc -l < $W/done.txt); [ "$total" -ge "$MAX" ] && break
    echo "${f#/port/build/portable/}" >> $W/queue.txt; added=$((added+1))
  done
  echo "round $round: $(wc -l < $W/done.txt) sources compiled, $(wc -l < $W/undef.txt) unresolved, $added added"
  [ "$(wc -l < $W/done.txt)" -ge "$MAX" ] && { echo "hit MAX=$MAX"; break; }
done
echo "== final unresolved (function names, first 60) =="; head -60 $W/undef.txt | tr '\n' ' '; echo
if [ -n "$HARNESS" ]; then
  gcc $CF -c "/port/native/$HARNESS" -o $W/harness.o
  for x in $EXTRA_SRC; do
    case "$x" in */rt.c) gcc $CF -fno-tree-loop-distribute-patterns -c "$x" -o "$W/o/zz_extra_$(basename "$x" .c).o";; *) gcc $CF -c "$x" -o "$W/o/zz_extra_$(basename "$x" .c).o";; esac
  done
  for x in $W/o/zz_extra_*.o; do
    [ -f "$x" ] || continue
    nm -u "$x" | awk '$1=="U"{print $2}' >> $W/u.txt
    nm --defined-only "$x" | awk 'NF>=3{print $3}' >> $W/d.txt
  done
  nm -u $W/harness.o | awk '$1=="U"{print $2}' | sort -u >> $W/u.txt; sort -u $W/u.txt -o $W/u.txt
  nm --defined-only $W/harness.o | awk 'NF>=3{print $3}' >> $W/d.txt; sort -u $W/d.txt -o $W/d.txt
  cat $W/d.txt $W/ld.txt | sort -u > $W/alld.txt; comm -23 $W/u.txt $W/alld.txt > $W/undef.txt
  : > $W/stubs.c; for u in $(cat $W/undef.txt); do echo "int $u() { return 0; }" >> $W/stubs.c; done
  [ -s $W/stubs.c ] && gcc $CF -c $W/stubs.c -o $W/o/zz_stubs.o
  echo "stubbed $(wc -l < $W/undef.txt) unresolved functions"
  gcc -m32 -static -nostdlib -Wl,-e,_start -o $W/prog $W/o/*.o $W/harness.o $LD && nm -n $W/prog > /port/build/native/prog.nm && $W/prog
fi
