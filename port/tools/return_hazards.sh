#!/bin/sh
# Functions of src/main + src/battle that can fall off the end of a non-void function or `return;` without a value.
# On the PlayStation the caller then sees whatever $v0 held; natively it is whatever eax held -- an unspecified value that the
# oracle reports as a "return value only" mismatch. Output: one function (file stem) per line.
#   sh /port/tools/return_hazards.sh > /port/build/return_hazards.txt
DIRS=${DIRS:-"src/main src/battle"}
export CF="-m32 -O1 -Wreturn-type -std=gnu89 -funsigned-char -fcommon -ffreestanding -fno-builtin -fno-pic -fno-pie -fwrapv -nostdinc -I/port/native/shim -I/port/native/gte -I/port/build/portable/include -include psx/gte_inline.h"
cd /port/build/portable || exit 1
find $DIRS -name '*.c' | sort | xargs -P 18 -I{} sh -c 'gcc $CF -S -o /dev/null {} 2>&1 | grep -q "reaches end of non-void\|with no value, in function returning non-void" && basename {} .c'
