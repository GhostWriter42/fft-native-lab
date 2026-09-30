#!/bin/sh
# Compile every game-logic source (src/main + src/battle of the portified tree) natively, in parallel, then report the link
# boundary: which functions are called but not defined by game code (the SDK / BIOS surface the native port must provide).
# env: DIRS (default "src/main src/battle"), OUT (default /port/build/native/all)
DIRS=${DIRS:-"src/main src/battle"}; OUT=${OUT:-/port/build/native/all}
rm -rf "$OUT"; mkdir -p "$OUT/o"
CF="-m32 -O1 -w -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-strict-aliasing -fno-aggressive-loop-optimizations -fwrapv -nostdinc -I/port/native/shim -I/port/native/gte -I/port/build/portable/include"
cd /port/build/portable
: > "$OUT/sources.txt"
for d in $DIRS; do find $d -name '*.c' | sort >> "$OUT/sources.txt"; done
echo "sources: $(wc -l < "$OUT/sources.txt")"
export CF OUT
cat "$OUT/sources.txt" | xargs -P 18 -I{} sh -c 'o="$OUT/o/$(echo {} | tr / _).o"; gcc $CF -c {} -o "$o" 2> "$o.err" || { echo {} >> "$OUT/failed.txt"; rm -f "$o"; }; [ -s "$o.err" ] || rm -f "$o.err"'
echo "compiled: $(ls "$OUT/o"/*.o | wc -l), failed: $(wc -l < "$OUT/failed.txt" 2>/dev/null || echo 0)"
[ -f "$OUT/failed.txt" ] && head -20 "$OUT/failed.txt"
cd "$OUT"
nm --defined-only o/*.o 2>/dev/null | awk 'NF>=3{print $3}' | sort -u > defined.txt
nm -u o/*.o 2>/dev/null | awk '$1=="U"{print $2}' | sort -u > undef_all.txt
sed -E 's/ = .*//' /port/build/native/symbols.ld | sort -u > ld.txt
cat defined.txt ld.txt | sort -u > known.txt
comm -23 undef_all.txt known.txt > unresolved.txt
echo "unresolved externals: $(wc -l < unresolved.txt)"
