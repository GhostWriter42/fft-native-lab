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
#define THREAD_STACK_WINDOW_BYTES 0x600000u                     /* the thread stacks and the main stack: 0x80400000..0x80a00000 */
/* the main context's stack (1 MiB, top = MAIN_STACK_WINDOW + MAIN_STACK_BYTES): the native frames are larger than the MIPS ones and do not fit the console's 64 KiB below
 * the top of RAM, where overlay data lives */
#define MAIN_STACK_WINDOW 0x80900000u
#define MAIN_STACK_BYTES 0x100000u
#define THREAD_STACK_BYTES 0x20000u
#define THREAD_STACK_WORLD_OFFSET 0x200000u
#endif
