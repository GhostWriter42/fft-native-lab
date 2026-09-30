#!/bin/sh
# usage: probe.sh MODE TAG   -- run inside the toolchain image with the port folder mounted at /port.
# Compiles every .c under $PROBE_ROOT with the image's gcc (one process per file, 20-wide) and summarises errors.
MODE=$1; TAG=${2:-$1}
cd "${PROBE_ROOT:?set PROBE_ROOT}"
OUT=/port/build/probe-out/$TAG
rm -rf "$OUT"; mkdir -p "$OUT"
find src -name '*.c' | sort | { if [ -n "$PROBE_EXCLUDE" ]; then grep -Ev "$PROBE_EXCLUDE"; else cat; fi; } | xargs -P 20 -n 1 sh /port/tools/one.sh "$MODE" "$OUT" > "$OUT/status.txt"
total=$(wc -l < "$OUT/status.txt"); ok=$(grep -c '^0 ' "$OUT/status.txt")
echo "[$TAG] files that compile: $ok / $total"
echo "[$TAG] top error messages (numbers normalised):"
cat "$OUT"/*.err | grep -E 'error:' | sed -E "s/^[^ ]+ ?: (fatal )?error: //; s/[0-9]+/N/g; s/'[^']*'/'X'/g" | sort | uniq -c | sort -rn | head -12
