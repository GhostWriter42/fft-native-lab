# Reads gcc -S -g1 output on stdin; prints "<file>:<line>  [<translation unit>]" for every idivl/divl (non-constant divisor).
/^[ \t]*\.file[ \t]+[0-9]+[ \t]+"/ { n = $2; name = $3; gsub(/"/, "", name); files[n] = name; next }
/^[ \t]*\.loc[ \t]+[0-9]+[ \t]+[0-9]+/ { curfile = $2; curline = $3; next }
/^[ \t]*(idivl|divl)[ \t]/ { f = (files[curfile] != "" ? files[curfile] : src); print f ":" curline "  [" src "]" }
