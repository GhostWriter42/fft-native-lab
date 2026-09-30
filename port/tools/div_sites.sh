#!/bin/sh
# List every source line where native code performs an integer division by a NON-CONSTANT divisor (an x86 idiv/div instruction:
# GCC turns division by a constant into shifts/multiplies). MIPS defines the result of dividing by zero (lo = -1 or 1, hi = the
# dividend; INT_MIN / -1 = INT_MIN); x86 traps and ARM returns 0, so these lines are where a native port needs a defined policy.
# Runs inside the toolchain image over the portified tree:  sh /port/tools/div_sites.sh > div_sites.txt
# env: DIRS (default "src/main src/battle")
DIRS=${DIRS:-"src/main src/battle"}
export CF="-m32 -O1 -w -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-strict-aliasing -fwrapv -nostdinc -I/port/native/shim -I/port/native/gte -I/port/build/portable/include"
cd /port/build/portable || exit 1
find $DIRS -name '*.c' | sort | xargs -P 18 -I{} sh -c 'gcc $CF -S -g1 -o - {} 2>/dev/null | awk -v src={} -f /port/tools/div_sites.awk'
