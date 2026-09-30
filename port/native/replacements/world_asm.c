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

/* world_gs_sortpoly (WORLD 0x800e0228): libgs' GsSortPoly, not yet decompiled (world.yaml only names the address; yamlfuncs.py EXTRA_FUNCTIONS registers it). Copies the
 * polygon descriptor `poly` into the packet buffer at g_world_gs_out_packet_p, adding the screen offset to every vertex, and links the packet into the ordering table
 * `ot` at depth `pri` through world_ps_sort_sprite_bg, which returns the new buffer cursor. The descriptor's byte 7 is the GPU command: bit 2 = textured (one
 * extra word after the first vertex, one per further vertex), bit 4 = gouraud (an extra colour word per vertex), bit 3 = a fourth vertex. The value in $a3 at entry
 * is not used: the routine counts the packet's words in it. A literal transliteration of the machine code (each optional word moves both pointers on by 4). */
void world_gs_sortpoly(POLY_FT4* poly, s32 arg, s32 type, u32 value) {
    const u8* src = (const u8*)poly;
    u8* start = (u8*)g_world_gs_out_packet_p;
    u8* dst = start;
    u32 code = src[7];
    u32 words = 4;
    u16 ox = (u16)g_world_gs_offset_x, oy = (u16)g_world_gs_offset_y;
    int textured = (code & 0x04) != 0, gouraud = (code & 0x10) != 0, quad = (code & 0x08) != 0;
    (void)value;
    *(u32*)(dst + 4) = *(const u32*)(src + 4);
    *(u16*)(dst + 8) = (u16)(*(const u16*)(src + 8) + ox);
    *(u16*)(dst + 10) = (u16)(*(const u16*)(src + 10) + oy);
    if (textured) { *(u32*)(dst + 12) = *(const u32*)(src + 12); src += 4; dst += 4; words = 5; }
    if (gouraud) { *(u32*)(dst + 12) = *(const u32*)(src + 12); src += 4; dst += 4; words += 1; }
    *(u16*)(dst + 12) = (u16)(*(const u16*)(src + 12) + ox);                 /* vertex 1 */
    *(u16*)(dst + 14) = (u16)(*(const u16*)(src + 14) + oy);
    if (textured) { *(u32*)(dst + 16) = *(const u32*)(src + 16); src += 4; dst += 4; words += 1; }
    if (gouraud) { *(u32*)(dst + 16) = *(const u32*)(src + 16); src += 4; dst += 4; words += 1; }
    *(u16*)(dst + 16) = (u16)(*(const u16*)(src + 16) + ox);                 /* vertex 2 */
    *(u16*)(dst + 18) = (u16)(*(const u16*)(src + 18) + oy);
    if (textured) { *(u32*)(dst + 20) = *(const u32*)(src + 20); src += 4; dst += 4; words += 1; }
    if (quad) {
        if (gouraud) { *(u32*)(dst + 20) = *(const u32*)(src + 20); src += 4; dst += 4; words += 1; }
        *(u16*)(dst + 20) = (u16)(*(const u16*)(src + 20) + ox);             /* vertex 3 */
        *(u16*)(dst + 22) = (u16)(*(const u16*)(src + 22) + oy);
        words += 1;
        if (textured) { *(u32*)(dst + 24) = *(const u32*)(src + 24); words += 1; }
    }
    g_world_gs_out_packet_p = (void*)world_ps_sort_sprite_bg((u32*)start, (GsOT*)arg, type & 0xffff, (s32)words);
}
