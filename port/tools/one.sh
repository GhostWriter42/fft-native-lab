#!/bin/sh
# usage: one.sh MODE OUTDIR FILE
#   MODE = m32 | m64 (syntax only) | m32S | m64S (codegen, no assemble) | m32W (codegen -O2 + undefined-behaviour warnings)
MODE=$1; OUT=$2; f=$3
FLAGS="-std=gnu89 -funsigned-char -fcommon -nostdinc -Iinclude"
case "$MODE" in
  *W) FLAGS="$FLAGS -O2 -S -o /dev/null -Wuninitialized -Wmaybe-uninitialized -Warray-bounds=2 -Wreturn-type -Wsequence-point -Wshift-count-negative -Wshift-count-overflow -Wshift-negative-value -Wdiv-by-zero -Wno-implicit-function-declaration -Wno-implicit-int -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-pointer-sign" ;;
  *S) FLAGS="$FLAGS -w -S -O0 -o /dev/null" ;;
  *)  FLAGS="$FLAGS -w -fsyntax-only" ;;
esac
case "$MODE" in m32*) FLAGS="-m32 $FLAGS" ;; esac
e="$OUT/$(echo "$f" | tr / _).err"
gcc $PRE $FLAGS $EXTRA -x c "$f" 2> "$e"
echo "$? $f"
