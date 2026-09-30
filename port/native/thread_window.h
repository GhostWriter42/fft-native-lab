#ifndef THREAD_WINDOW_H
#define THREAD_WINDOW_H
/* Where the native cooperative threads' private stacks live in the whole-program lockstep (lockstep.c maps the window; replacements/battle_thread.c and
 * world_thread.c place their stacks in it when built with -DLOCKSTEP_THREAD_WINDOW).
 *
 * Why not ordinary static arrays: a thread function's locals include GPU packets that are linked into ordering tables by their LOW 24 BITS
 * (addPrim) and read back as 0x80000000 | tag -- an x86 address outside 0x80000000..0x80ffffff cannot survive that. The retail threads keep such
 * packets on their 0x400-byte record stacks inside the 2 MiB of RAM; the native stacks (bigger frames) go to a window right above it:
 *   BATTLE slots:  THREAD_STACK_WINDOW + slot * THREAD_STACK_BYTES                     (16 slots)
 *   WORLD slots:   THREAD_STACK_WINDOW + THREAD_STACK_WORLD_OFFSET + slot * THREAD_STACK_BYTES  (17 slots) */
#define THREAD_STACK_WINDOW 0x80400000u
#define THREAD_STACK_WINDOW_BYTES 0x500000u
#define THREAD_STACK_BYTES 0x20000u
#define THREAD_STACK_WORLD_OFFSET 0x200000u
#endif
