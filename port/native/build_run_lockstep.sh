#!/bin/sh
# Runs inside the toolchain image (mounts: /port = port folder, /ob = docker volume with the natively compiled objects, /disc = extracted
# disc files, /disc.bin = the raw disc image).
# Whole-program lockstep: links the natively compiled game (every module's game sources + the SDK sources that are not hardware-facing;
# boundary.sh with RENAME=1 -> /ob/ls_pc/o), the software GTE, the native libgte, the C replacements for hand-assembled routines, the HLE of
# the SDK hardware layer (hle/ + hle_generated.c), the R3000 oracle and the driver (lockstep.c), stubs every yaml function that still has
# no native definition with a counting no-op, and runs it.
# env: EXTRA_CFLAGS (e.g. -DMAX_FRAMES=500 -DLOG_LIMIT=200), DIVFIX
set -e
W=/tmp/ls; rm -rf $W; mkdir -p $W
A=${A:-/ob/ls_pc}
N=/port/build/native/ls
P=/port/build/portable/src/psyq/libgte
FN=$N/fn_names.txt
CF="-m32 -O1 -w -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-strict-aliasing -fno-aggressive-loop-optimizations -fno-tree-loop-distribute-patterns -fwrapv -fno-omit-frame-pointer -nostdinc -I/port/native/shim -I/port/native/gte -I/port/build/portable/include -I/port/native -I$N -I/port/build/native -include psx/gte_inline.h -DPC_SCHEME $EXTRA_CFLAGS"
rename_defs() {   # rename defined yaml-function symbols of object $1 to native_<name>
  nm --defined-only -g "$1" | awk '{print $3}' | grep -Fxf "$FN" | awk '{print $1" native_"$1}' > "$1.map" || true
  [ -s "$1.map" ] && objcopy --redefine-syms="$1.map" "$1"
  rm -f "$1.map"
  return 0
}
# game-facing runtime pieces: these define yaml function names (RotTrans, rsin, battle_copy_bytes, ...), so they are renamed too
for s in /port/native/gte/libgte_native.c $P/rsin.c $P/sin_1.c $P/rcos.c $P/ratan2.c $P/csqrt.c $P/psyq_gte_csqrt_kernel.c /port/native/replacements/main_asm.c /port/native/replacements/battle_asm.c /port/native/replacements/battle_asm2.c /port/native/replacements/battle_thread.c /port/native/replacements/world_asm.c /port/native/replacements/world_thread.c; do
  o="$W/x_$(basename "$s" .c).o"
  if [ -n "$DIVFIX" ]; then gcc $CF -S -o "$o.s" "$s" && awk -f /port/tools/divfix.awk "$o.s" > "$o.f.s" && gcc -m32 -c -x assembler "$o.f.s" -o "$o"; rm -f "$o.s" "$o.f.s"
  else gcc $CF -c "$s" -o "$o"; fi
  rename_defs "$o"
done
# the HLE of the SDK hardware layer defines native_<name> for every SDK function it takes over (generated), plus the shared implementation
for s in /port/native/gte/gte.c /port/native/r3000/r3000.c /port/native/hle/hle.c $N/hle_generated.c $N/modules.c $N/sym_table.c /port/native/bios_rt.c /port/native/lockstep.c; do
  gcc $CF -c "$s" -o "$W/x_$(basename "$s" .c).o"
done
gcc $CF -c /port/native/rt.c -o $W/x_rt.o
# ONE nm pass over every game object: who defines / references what
nm -A -g $A/o/*.o 2>/dev/null | awk '{ f = $1; sub(/:[0-9a-f]*$/, "", f); print f, $2, $3 }' > $W/nm_all.txt
# the BIOS tail veneers ("goto" address 0xa0/0xb0/0xc0) cannot run natively: bios_rt.c and the HLE provide those services instead
( cd /port/build/portable && grep -rl 'PSYQ_BIOS_' src/psyq ) | tr / _ | sed "s|^|$A/o/|; s|\$|.o|" | sort > $W/veneers.txt
# objects that define an SDK function the HLE takes over are not linked either (the HLE defines it)
awk 'NR==FNR { h[$1] = 1; next } $2 != "U" && $2 != "w" && ($3 in h) { print $1 }' $N/hle_natives.txt $W/nm_all.txt | sort -u > $W/hle_objs.txt
cat $W/veneers.txt $W/hle_objs.txt | sort -u > $W/dropped.txt
ls $A/o/*.o | sort | comm -23 - $W/dropped.txt > $W/objs.txt
echo "game objects: $(wc -l < $W/objs.txt) linked, $(wc -l < $W/dropped.txt) dropped (BIOS veneers and SDK functions the HLE takes over)"
# names defined / referenced by the linked game objects and by the runtime pieces
awk 'NR==FNR { k[$1] = 1; next } ($1 in k) && $2 != "U" && $2 != "w" { print $3 }' $W/objs.txt $W/nm_all.txt | sort -u > $W/defined_game.txt
awk 'NR==FNR { k[$1] = 1; next } ($1 in k) && $2 == "U" { print $3 }' $W/objs.txt $W/nm_all.txt | sort -u > $W/undef_game.txt
nm --defined-only $W/x_*.o 2>/dev/null | awk 'NF>=3{print $3}' | sort -u > $W/defined_rt.txt
nm -u $W/x_*.o 2>/dev/null | awk '$1=="U"{print $2}' | sort -u > $W/undef_rt.txt
cat $W/defined_game.txt $W/defined_rt.txt | sort -u > $W/defined.txt
# yaml functions that still have no native definition = the SDK surface left over and asm-only routines: stub them, counting calls
sed 's/^/native_/' "$FN" | sort -u > $W/want.txt
comm -23 $W/want.txt $W/defined.txt > $W/missing.txt
NMISS=$(wc -l < $W/missing.txt)
echo "stubbed yaml functions with no native definition: $NMISS"
{
  echo "unsigned g_stub_calls; unsigned g_stub_hits[$((NMISS + 1))];"
  i=0
  for u in $(cat $W/missing.txt); do echo "int $u() { g_stub_calls++; g_stub_hits[$i]++; return 0; }"; i=$((i + 1)); done
  echo "const char* const g_stub_names[] = {"
  for u in $(cat $W/missing.txt); do echo "  \"${u#native_}\","; done
  echo "  0 };"
  echo "const int g_stub_count = $NMISS;"
} > $W/stubs.c
gcc $CF -c $W/stubs.c -o $W/x_stubs.o
cp $W/missing.txt $N/ls_missing_natives.txt
# anything else still undefined (names that are in no yaml) would be a link error: report it
cat $W/undef_game.txt $W/undef_rt.txt | sort -u > $W/undef_all.txt
sed -E 's/ = .*//' $N/symbols_pc.ld | sort -u > $W/ld.txt
cat $W/defined.txt $W/ld.txt $W/missing.txt | sort -u > $W/known.txt
comm -23 $W/undef_all.txt $W/known.txt | grep -vx g_stub_calls > $W/undef_other.txt || true
if [ -s $W/undef_other.txt ]; then echo "undefined and not in any yaml: $(tr '\n' ' ' < $W/undef_other.txt)"; fi
gcc -m32 -static -nostdlib -Wl,-e,_start -o $W/prog $(cat $W/objs.txt) $W/x_*.o $N/symbols_pc.ld
nm -n $W/prog > $N/ls_prog.nm
rc=0; $W/prog > $W/out.txt 2>&1 || rc=$?
# resolve the native addresses of a crash report (@0x080c...) to symbol+offset
awk 'function hex(s,   i, v) { v = 0; s = tolower(s); for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1; return v }
     NR==FNR { if (NF>=3) { a[++n]=hex($1); s[n]=$3 } next }
     { line=$0; res=""; while (match(line, /@0x[0-9a-f]+/)) {
         v=hex(substr(line, RSTART+3, RLENGTH-3)); lo=1; hi=n; r=0
         while (lo<=hi) { m=int((lo+hi)/2); if (a[m]<=v) { r=m; lo=m+1 } else hi=m-1 }
         res = res substr(line, 1, RSTART-1) "@" substr(line, RSTART+1, RLENGTH-1) "(" (r? s[r] : "?") "+" (r? v-a[r] : 0) ")"; line=substr(line, RSTART+RLENGTH) }
       print res line }' $N/ls_prog.nm $W/out.txt
exit $rc
