/* Native C versions of more hand-assembled BATTLE routines (the decomp keeps them as raw bytes, `kind: handwritten`).
 * Each is a literal transliteration of the machine code, verified against it by port/native/harness_diff_asm.c.
 * Names are the yaml's; in the PS1-address scheme they are linked as native_<name>. */
#include "fft/battle.h"
#include "psx/types.h"

extern s32 g_battle_camera_cross_product_overflow;      /* 0x80173f58: set to 1 when a 64-bit result does not fit */

/* (a * b) / c through a 64-round shift-subtract divide on the magnitude of the 64-bit product (0x8014ccb8).
 * The retail routine has quirks that are reproduced on purpose:
 *  - negating the product adds the carry into the REMAINDER register (t2), not into the high word;
 *  - a negative divisor is complemented (~c = |c| - 1) and 1 is added to the low word of the product;
 *  - the remainder compare is signed;
 *  - when the signs differ the quotient is negated, and a quotient whose high word is neither 0 nor -1 sets the overflow flag. */
s32 battle_mul_div_s64(s32 a, s32 b, s32 c) {
    long long product = (long long)a * (long long)b;
    u32 t0 = (u32)product;                              /* low word of the dividend chain */
    u32 t1 = (u32)((unsigned long long)product >> 32);                 /* high word */
    u32 t2 = 0;                                         /* remainder chain */
    u32 t3 = 0;                                         /* quotient low */
    u32 t4 = 0;                                         /* quotient high */
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
                g_battle_camera_cross_product_overflow = 1;
            }
        }
        t3 ^= 0xffffffffu;
        t3 += 1;
    }
    return (s32)t3;
}

/* (a * b - c * d) >> 12 in 64-bit arithmetic (0x8014ce08); the Q12 result is the low 32 bits, and the overflow flag is set when
 * the bits above them are not a plain sign extension. */
s32 battle_fixed_cross_product_q12(s32 a, s32 b, s32 c, s32 d) {
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
            g_battle_camera_cross_product_overflow = 1;
        }
    }
    return (s32)((high << 20) | (low >> 12));
}

/* Calls g_battle_thread_call_target. The retail routine switches to the main thread's stack first when a worker thread
 * calls it and forwards a0-a3 untouched; native stacks are large, so the target is called directly with the same arguments. */
s32 battle_thread_call_on_main_stack(s32 a0, s32 a1, s32 a2, s32 a3) {
    return ((s32 (*)(s32, s32, s32, s32))g_battle_thread_call_target)(a0, a1, a2, a3);
}

/* Zero `bytes` bytes (0x8014bed8; word stores after byte stores, so the buffer must be word aligned). */
void battle_clear_menu_render_buffer(void* buffer, s32 bytes) {
    u8* p = (u8*)buffer;
    u32 tail = (u32)bytes & 3;
    u32 words = (u32)bytes >> 2;
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
