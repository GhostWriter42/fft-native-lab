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

/* Byte-for-byte twins of the BATTLE routines (battle_asm2.c, verified against the original code there) except for the address of the
 * overflow flag and of the call-target word: world_mul_div_64 = battle_mul_div_s64 (0x80100188), world_cross_product_q12 =
 * battle_fixed_cross_product_q12 (0x801002d8), world_clear_menu_render_buffer = battle_clear_menu_render_buffer (identical machine
 * code, 0x800ff3d4), world_thread_call_on_main_stack = battle_thread_call_on_main_stack (0x80100384). */
s32 world_mul_div_64(s32 a, s32 b, s32 c) {
    long long product = (long long)a * (long long)b;
    u32 t0 = (u32)product;
    u32 t1 = (u32)((unsigned long long)product >> 32);
    u32 t2 = 0;
    u32 t3 = 0;
    u32 t4 = 0;
    u32 divisor = (u32)c;
    u32 product_negative = t1 >> 31;
    u32 divisor_negative;
    u32 rounds;
    if (product_negative) {
        t0 ^= 0xffffffffu;
        t1 ^= 0xffffffffu;
        t0 += 1;
        if (t0 == 0) {
            t2 += 1;
        }
    }
    divisor_negative = divisor >> 31;
    if (divisor_negative) {
        divisor ^= 0xffffffffu;
        t0 += 1;
    }
    for (rounds = 64; rounds != 0; rounds--) {
        u32 carry_from_t0 = t0 >> 31;
        u32 carry_from_t1 = t1 >> 31;
        u32 quotient_bit = 0;
        u32 carry_from_t3;
        t2 <<= 1;
        if (carry_from_t1) {
            t2 |= 1;
        }
        t1 <<= 1;
        if (carry_from_t0) {
            t1 |= 1;
        }
        t0 <<= 1;
        if (!((s32)t2 < (s32)divisor)) {
            t2 -= divisor;
            quotient_bit = 1;
        }
        carry_from_t3 = t3 >> 31;
        t4 <<= 1;
        if (carry_from_t3) {
            t4 |= 1;
        }
        t3 <<= 1;
        t3 |= quotient_bit;
    }
    if (divisor_negative != product_negative) {
        if (t4 != 0) {
            t4 += 1;
            if (t4 != 0) {
                g_world_fixed_math_overflow = 1;
            }
        }
        t3 ^= 0xffffffffu;
        t3 += 1;
    }
    return (s32)t3;
}

s32 world_cross_product_q12(s32 a, s32 b, s32 c, s32 d) {
    long long first = (long long)a * (long long)b;
    long long second = (long long)c * (long long)d;
    u32 first_low = (u32)first, first_high = (u32)((unsigned long long)first >> 32);
    u32 second_low = (u32)second, second_high = (u32)((unsigned long long)second >> 32);
    u32 low = first_low - second_low;
    u32 borrow = first_low < second_low;
    u32 high = first_high - second_high - borrow;
    s32 top = (s32)high >> 12;
    if (top != 0) {
        top += 1;
        if (top != 0) {
            g_world_fixed_math_overflow = 1;
        }
    }
    return (s32)((high << 20) | (low >> 12));
}

s32 world_thread_call_on_main_stack(s32 a0, s32 a1, s32 a2, s32 a3) {
    return ((s32 (*)(s32, s32, s32, s32))g_world_thread_call_target)(a0, a1, a2, a3);
}

void world_clear_menu_render_buffer(u8* buffer, u32 bytes) {
    u8* p = buffer;
    u32 tail = bytes & 3;
    u32 words = bytes >> 2;
    while (words != 0) {
        *(u32*)p = 0;
        p += 4;
        words--;
    }
    while (tail != 0) {
        *p++ = 0;
        tail--;
    }
}

/* wldcore_switch_to_stack (WLDCORE 0x80092b04) moves $sp into the scratchpad (0x1F8003FC) for the world-frame drawing code and a
 * sibling routine moves it back; natively the stack stays where it is (the scratchpad is not compared while WLDCORE is active). */
void wldcore_switch_to_stack(void* new_stack) { (void)new_stack; }
void wldcore_restore_previous_stack(void) {}
