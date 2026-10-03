#!/bin/sh
# Runs inside the toolchain image (mounts: /port = port folder, /disc = extracted disc files).
# PS1-address scheme: every natively compiled function is linked as native_<name>; the original names are bound to their PS1
# addresses by symbols_pc.ld, and x86 trampolines written at those addresses reach the native code. Function pointers stored
# in RAM are therefore the canonical PS1 addresses.
# Links every natively compiled game-logic object (boundary.sh with RENAME=1 -> /port/build/native/all_pc/o) with the software
# GTE, the native libgte, the C replacements for hand-assembled routines, the R3000 oracle and the generic fuzz harness, stubs
# every yaml function that still has no native definition (the SDK surface) with a counting no-op, and runs the fuzzer.
# env: EXTRA_CFLAGS (e.g. -DTRIALS=100 -DFROM=0 -DTO=50)
set -e
W=/tmp/fz; rm -rf $W; mkdir -p $W
A=${A:-/port/build/native/all_pc}
N=${N:-/port/build/native}
P=/port/build/portable/src/psyq/libgte
FN=$N/fn_names.txt
CF="-m32 -O1 -w -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-strict-aliasing -fno-aggressive-loop-optimizations -fwrapv -fno-delete-null-pointer-checks -nostdinc -I/port/native/shim -I/port/native/gte -I/port/build/portable/include -I/port/native -I$N -I/port/build/native -include psx/gte_inline.h -DPC_SCHEME $EXTRA_CFLAGS"
rename_defs() {   # rename defined yaml-function symbols of object $1 to native_<name>
  nm --defined-only -g "$1" | awk '{print $3}' | grep -Fxf "$FN" | awk '{print $1" native_"$1}' > "$1.map" || true
  [ -s "$1.map" ] && objcopy --redefine-syms="$1.map" "$1"
  rm -f "$1.map"
  return 0
}
# game-facing runtime pieces: these define yaml function names (RotTrans, rsin, battle_copy_bytes, ...), so they are renamed too
# REPL: the hand-written-routine replacements of the modules in this build (default: main + BATTLE)
REPL=${REPL:-"/port/native/replacements/main_asm.c /port/native/replacements/battle_asm.c /port/native/replacements/battle_asm2.c /port/native/replacements/battle_thread.c"}
for s in /port/native/gte/libgte_native.c $P/rsin.c $P/sin_1.c $P/rcos.c $P/ratan2.c $P/csqrt.c $P/psyq_gte_csqrt_kernel.c $REPL; do
  o="$W/x_$(basename "$s" .c).o"
  if [ -n "$DIVFIX" ]; then gcc $CF -S -o "$o.s" "$s" && awk -f /port/tools/divfix.awk "$o.s" > "$o.f.s" && gcc -m32 -c -x assembler "$o.f.s" -o "$o"; rm -f "$o.s" "$o.f.s"
  else gcc $CF -c "$s" -o "$o"; fi
  rename_defs "$o"
done
# oracle, GTE, tables, harness: their own names stay as they are (the harness defines native_rand & co. itself)
for s in /port/native/gte/gte.c /port/native/r3000/r3000.c $N/func_addrs.c $N/stub_table.c $N/fuzz_table.c $N/sym_table.c $N/sdk_ranges.c $N/seed_globals.c $N/unspecified_ret.c $N/module_files.c /port/native/bios_rt.c /port/native/harness_fuzz.c; do
  gcc $CF -c "$s" -o "$W/x_$(basename "$s" .c).o"
done
gcc $CF -fno-tree-loop-distribute-patterns -c /port/native/rt.c -o $W/x_rt.o
# the BIOS tail veneers (-Sdk builds) cannot run natively ("goto" 0xa0): bios_rt.c provides those services, so their objects are not linked
( cd /port/build/portable && grep -rl 'PSYQ_BIOS_' src/psyq ) | tr / _ | sed "s|^|$A/o/|; s|\$|.o|" | sort > $W/veneers.txt
ls $A/o/*.o | grep -vxFf $W/veneers.txt > $W/objs.txt || true
# yaml functions that still have no native definition = the SDK surface (and asm-only routines): stub them, counting calls
nm --defined-only $(cat $W/objs.txt) $W/*.o 2>/dev/null | awk 'NF>=3{print $3}' | sort -u > $W/defined.txt
sed 's/^/native_/' "$FN" | sort -u > $W/want.txt
comm -23 $W/want.txt $W/defined.txt > $W/missing.txt
echo "stubbed yaml functions with no native definition: $(wc -l < $W/missing.txt)"
echo "unsigned g_stub_calls;" > $W/stubs.c
for u in $(cat $W/missing.txt); do echo "int $u() { g_stub_calls++; return 0; }" >> $W/stubs.c; done
gcc $CF -c $W/stubs.c -o $W/x_stubs.o
# anything else still undefined (names that are in no yaml) would be a link error: report it
nm -u $(cat $W/objs.txt) $W/*.o 2>/dev/null | awk '$1=="U"{print $2}' | sort -u > $W/undef_all.txt
sed -E 's/ = .*//' $N/symbols_pc.ld | sort -u > $W/ld.txt
nm --defined-only $(cat $W/objs.txt) $W/*.o 2>/dev/null | awk 'NF>=3{print $3}' | sort -u > $W/defined2.txt
cat $W/defined2.txt $W/ld.txt | sort -u > $W/known.txt
comm -23 $W/undef_all.txt $W/known.txt | grep -vx g_stub_calls > $W/undef_other.txt || true
if [ -s $W/undef_other.txt ]; then
  echo "undefined and not in any yaml: $(tr '\n' ' ' < $W/undef_other.txt)"
  # references to modules that are not part of this build (WORLD calls BATTLE-hosted functions ...): counting stubs / zero data; a trial of the original that reaches such a function is skipped (g_foreign_entries)
  echo "extern unsigned g_stub_calls;" > $W/xstubs.c
  for u in $(cat $W/undef_other.txt); do
    case "$u" in g_*) echo "long $u[16];" >> $W/xstubs.c ;; *) echo "int $u() { g_stub_calls++; return 0; }" >> $W/xstubs.c ;; esac
  done
  gcc $CF -c $W/xstubs.c -o $W/x_xstubs.o
fi
gcc -m32 -static -nostdlib -Wl,-e,_start -o $W/prog $(cat $W/objs.txt) $W/x_*.o $N/symbols_pc.ld
nm -n $W/prog > $N/fuzz_prog.nm
$W/prog
