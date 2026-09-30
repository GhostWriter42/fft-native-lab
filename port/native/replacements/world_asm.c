/* Native C versions of the WORLD-overlay routines that the retail binary hand-assembles; see battle_asm.c. */
#include "fft/world.h"
#include "psx/types.h"

extern s32 g_world_thread_current_id;

void world_script_copy_bytes(void* destination, const void* source, s32 count) {
    u8* d = (u8*)destination;
    const u8* s = (const u8*)source;
    do {
        *d++ = *s++;
    } while (--count);
}

const u8* world_text_skip_to_entry(const u8* text, s32 entry_index) {
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

void* world_thread_get_current_parameter_1(void) { return (void*)g_world_threads[g_world_thread_current_id].function_parameter_1; }
s32 world_thread_get_current_parameter_2(void) { return g_world_threads[g_world_thread_current_id].function_parameter_2; }
s32 world_thread_get_current_parameter_3(void) { return g_world_threads[g_world_thread_current_id].function_parameter_3; }
s32 world_thread_is_previous_running(void) { return *(s32*)((u8*)g_world_threads + ((g_world_thread_current_id - 1) << 10) + 0x48); }
s32 world_thread_is_running_80100164(s32 thread_id) { return g_world_threads[thread_id].is_running; }
void* world_thread_get_current_global_pointer(void) { return 0; }
