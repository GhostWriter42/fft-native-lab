/* Native C versions of the BATTLE-overlay routines that the retail binary hand-assembles (the decomp keeps them as inline
 * MIPS asm, which cannot compile natively). Each is a plain-C rendering of the routine's documented behaviour, checked against
 * the original machine code by the differential harness (port/native/harness_diff_asm.c).
 * Compiled INSTEAD of the repo's src/battle/<same name>.c in a native build. */
#include "fft/battle.h"
#include "psx/types.h"

/* Copy a nonempty byte sequence. The retail loop copies the first byte before testing the decremented count, so a count of
 * zero (or a negative one) would run ~4 billion iterations: callers must pass a positive count. */
void battle_copy_bytes(void* destination, const void* source, s32 count) {
    u8* d = (u8*)destination;
    const u8* s = (const u8*)source;
    do {
        *d++ = *s++;
    } while (--count);
}

/* Advance to entry `entry_index` of a packed text section whose entries end with a 0xfe or 0xff byte. */
const u8* battle_find_text_id_location(const u8* text, s32 entry_index) {
    s32 index = 0;
    const u8* cursor = text;
    for (;;) {
        u8 c = *cursor;
        if (entry_index == index) {
            return cursor;
        }
        if ((c & 0xfe) == 0xfe) {
            index++;
        }
        cursor++;
    }
}

/* Thread accessors: the retail versions load the current-thread id and the g_battle_threads pointer through $at. */
s32 battle_thread_get_current_parameter_1(void) { return g_battle_threads[g_battle_current_thread_id].function_parameter_1; }
s32 battle_thread_get_current_parameter_2(void) { return g_battle_threads[g_battle_current_thread_id].function_parameter_2; }
s32 battle_thread_get_current_parameter_3(void) { return g_battle_threads[g_battle_current_thread_id].function_parameter_3; }
/* Thread 0 deliberately underflows to the preceding 0x400-byte record (the scheduler's slot-order invariant). */
s32 battle_thread_is_previous_running(void) { return *(s32*)((u8*)g_battle_threads + ((g_battle_current_thread_id - 1) << 10) + 0x48); }
s32 battle_thread_is_running_8014cc94(s32 thread_id) { return g_battle_threads[thread_id].is_running; }
/* $gp has no native meaning; battle_thread_start only stores it into the new thread's record. */
void* battle_thread_get_current_global_pointer(void) { return 0; }
