#!/bin/sh
# Compile every game-logic source (src/main + src/battle of the portified tree) natively, in parallel, then report the link
# boundary: which functions are called but not defined by game code (the SDK / BIOS surface the native port must provide).
# env: DIRS   (default "src/main src/battle")
#      OUT    (default /port/build/native/all)
#      RENAME (non-empty: PS1-address scheme -- every defined function that appears in fn_names.txt is renamed to native_<name>
#              in its object, so references keep the original name, which symbols_pc.ld binds to the PS1 address)
#      LDFILE (default /port/build/native/symbols.ld; symbols_pc.ld with RENAME)
#      FN     (default /port/build/native/fn_names.txt: the yaml function names that RENAME turns into native_<name>)
#      DIVFIX (non-empty: expand every idiv/div with the MIPS zero/-1 divisor semantics, port/tools/divfix.awk)
#      EXTRA_CFLAGS (e.g. -ftrivial-auto-var-init=zero)
#      SCOPED (directory with scoped_syms.txt / fn_docs.txt from gen_symbols.py: references to per-overlay data copies are renamed to <name>__<document>)
DIRS=${DIRS:-"src/main src/battle"}; OUT=${OUT:-/port/build/native/all}
FN=${FN:-/port/build/native/fn_names.txt}
LDFILE=${LDFILE:-/port/build/native/symbols.ld}
rm -rf "$OUT"; mkdir -p "$OUT/o"
CF="-m32 -O1 -w -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-strict-aliasing -fno-aggressive-loop-optimizations -fwrapv -nostdinc -I/port/native/shim -I/port/native/gte -I/port/build/portable/include -include psx/gte_inline.h $EXTRA_CFLAGS"
cd /port/build/portable
: > "$OUT/sources.txt"
for d in $DIRS; do find $d -name '*.c' | sort >> "$OUT/sources.txt"; done
# sources replaced by hand-written native versions (port/native/replacements): not compiled here
if [ -f /port/native/replacements/replaced.txt ]; then
  grep -vxFf /port/native/replacements/replaced.txt "$OUT/sources.txt" > "$OUT/sources.filtered" && mv "$OUT/sources.filtered" "$OUT/sources.txt"
fi
echo "sources: $(wc -l < "$OUT/sources.txt")"
export CF OUT FN RENAME DIVFIX SCOPED
cat "$OUT/sources.txt" | xargs -P 18 -I{} sh -c '
  o="$OUT/o/$(echo {} | tr / _).o"
  if [ -n "$DIVFIX" ]; then
    gcc $CF -S -o "$o.s" {} 2> "$o.err" && awk -f /port/tools/divfix.awk "$o.s" > "$o.fixed.s" && gcc -m32 -c -x assembler "$o.fixed.s" -o "$o" 2>> "$o.err"; ok=$?
    rm -f "$o.s" "$o.fixed.s"
  else
    gcc $CF -c {} -o "$o" 2> "$o.err"; ok=$?
  fi
  if [ $ok -eq 0 ]; then
    if [ -n "$RENAME" ]; then
      nm --defined-only -g "$o" | awk "{print \$3}" | grep -Fxf "$FN" | awk "{print \$1\" native_\"\$1}" > "$o.map"
      [ -s "$o.map" ] && objcopy --redefine-syms="$o.map" "$o"
      rm -f "$o.map"
      # data symbols that overlay documents define at different addresses: the references in the object of a function go to the copy of the document of that function (gen_symbols.py)
      if [ -n "$SCOPED" ] && [ -s "$SCOPED/fn_docs.txt" ]; then
        doc=$(awk -v f="$(basename {} .c)" "\$1==f{print \$2; exit}" "$SCOPED/fn_docs.txt")
        if [ -n "$doc" ]; then
          nm -u "$o" | awk "{print \$2}" | grep -Fxf "$SCOPED/scoped_syms.txt" | awk -v d="$doc" "{print \$1\" \"\$1\"__\"d}" > "$o.smap"
          [ -s "$o.smap" ] && objcopy --redefine-syms="$o.smap" "$o"
          rm -f "$o.smap"
        fi
      fi
    fi
  else
    echo {} >> "$OUT/failed.txt"; rm -f "$o"
  fi
  [ -s "$o.err" ] || rm -f "$o.err"'
echo "compiled: $(ls "$OUT/o"/*.o | wc -l), failed: $(wc -l < "$OUT/failed.txt" 2>/dev/null || echo 0)"
[ -f "$OUT/failed.txt" ] && head -20 "$OUT/failed.txt"
cd "$OUT"
nm --defined-only o/*.o 2>/dev/null | awk 'NF>=3{print $3}' | sort -u > defined.txt
nm -u o/*.o 2>/dev/null | awk '$1=="U"{print $2}' | sort -u > undef_all.txt
sed -E 's/ = .*//' "$LDFILE" | sort -u > ld.txt
cat defined.txt ld.txt | sort -u > known.txt
comm -23 undef_all.txt known.txt > unresolved.txt
echo "unresolved externals (not defined by game code, not in the symbol script): $(wc -l < unresolved.txt)"
if [ -n "$RENAME" ]; then
  sed 's/^/native_/' "$FN" | sort -u > want.txt
  comm -23 want.txt defined.txt > missing_natives.txt
  echo "yaml functions without a native definition in these objects: $(wc -l < missing_natives.txt)"
fi
