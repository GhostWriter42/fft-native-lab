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

#define BLIT_LHU(p, off) ((u32)*(const u16*)((const u8*)(p) + (off)))
/* 4bpp image -> 4bpp image, one 32-bit word (8 pixels) at a time through nibble masks; the retail routine's behaviour is reproduced on purpose, quirks included:
 *  - the colour offset (the word at +12 of the destination record) only reaches the pixel when the source nibble is the lowest one of its word (the add happens before the
 *    mask at the source position, so its bits vanish for every other nibble);
 *  - a source word that is entirely zero lets the routine skip 8 pixels at once, whatever the two bit offsets are (only exact when both are 0).
 * Registers of the original are kept as variables (t2 rows left, t3 pixels left, s2/s3 source word pointer and word, s4/s5 destination word pointer and word, t4/t5 nibble
 * masks, t6/t7 bit offsets). Its scratchpad temporaries (0x1f800000..0x5b) are not reproduced: the lockstep ignores them. */
static void blit_glyph_rows(const u8* src, u8* dst, const u8* srect, const u8* drect, int check_x256) {
    u32 s7 = BLIT_LHU(srect, 0), t1 = BLIT_LHU(srect, 2), s2 = BLIT_LHU(srect, 8);
    u32 t8 = BLIT_LHU(drect, 0), t9, s4, s8;
    u32 src_stride_bits, dst_stride_bits, width, height, v0, src_bit, dst_bit, src_ptr, dst_ptr;
    u32 t2, t3, t4, t5, t6, t7, s3, s5;
    if (check_x256 && (s32)t8 >= 256) return;                                    /* blit_text_glyph only: nothing is drawn right of the 256-pixel screen */
    t9 = BLIT_LHU(drect, 2); s4 = BLIT_LHU(drect, 8); s8 = *(const u32*)(drect + 12);
    src_stride_bits = s2 << 2; s2 >>= 1;
    width = BLIT_LHU(srect, 4); height = BLIT_LHU(srect, 6);
    v0 = (s7 & 1) << 2; s7 >>= 1;
    s2 = s2 * t1;
    dst_stride_bits = s4 << 2; s4 >>= 1;
    s2 = s2 + s7 + (u32)src;
    src_bit = ((s2 & 3) << 3) + v0;
    src_ptr = s2 & ~3u;
    s4 = s4 * t9;
    v0 = (t8 & 1) << 2; t8 >>= 1;
    s4 = s4 + t8 + (u32)dst;
    dst_bit = ((s4 & 3) << 3) + v0;
    dst_ptr = s4 & ~3u;
    t2 = height;
    do {
        s2 = src_ptr; t6 = src_bit; t4 = 15u << t6; s3 = *(const u32*)s2;
        s4 = dst_ptr; t7 = dst_bit; t5 = 15u << t7; s5 = *(u32*)s4;
        t3 = width;
        for (;;) {
            if (s3 == 0 && (s32)t3 >= 8) {
                if (t3 == 8) break;                                              /* exactly one word of zeros left: the row is done */
                *(u32*)s4 = s5; s2 += 4; s4 += 4; s5 = *(u32*)s4; s3 = *(const u32*)s2; t3 -= 7;
            } else {
                v0 = s3 & t4;
                if (v0) {
                    v0 = (v0 + s8) & t4;
                    s5 &= ~t5;
                    if ((s32)t6 < (s32)t7) v0 <<= (t7 - t6); else v0 >>= (t6 - t7);
                    s5 |= v0;
                }
                t6 += 4; t4 <<= 4;
                if (t4 == 0) { s2 += 4; t4 = 15; t6 = 0; s3 = *(const u32*)s2; }
                t7 += 4; t5 <<= 4;
                if (t5 == 0) { *(u32*)s4 = s5; s4 += 4; t5 = 15; t7 = 0; s5 = *(u32*)s4; }
            }
            t3--;
            if (t3 == 0) break;
        }
        *(u32*)s4 = s5;
        v0 = dst_stride_bits + dst_bit; dst_ptr += (v0 >> 5) << 2; dst_bit = v0 & 0x1f;
        v0 = src_stride_bits + src_bit; src_ptr += (v0 >> 5) << 2; src_bit = v0 & 0x1f;
        t2--;
    } while (t2 != 0);
}

extern s32 g_battle_text_substitution_value_27;                /* 0x80165f90: first glyph row to draw */
extern s32 g_battle_text_substitution_value_28;                /* 0x80165f94: glyph row limit */

/* BATTLE 0x8014bae4: the twin of world_text_blit_glyph, which also draws nothing when the destination x is 256 or more. */
void blit_text_glyph(void* text, void* pixels, void* glyph, void* position) {
    blit_glyph_rows((const u8*)text, (u8*)pixels, (const u8*)glyph, (const u8*)position, 1);
}

/* BATTLE 0x8014bd88: the twin of world_text_blit_font_glyph_to_4bpp. Rows from the limit on are skipped without consuming glyph bits; rows before the first row consume their
 * bits but draw nothing. */
void battle_text_render_glyph_to_4bpp_image(const u8* glyph_bitmap, s32 image, u16* origin, s32 palette_offset) {
    s32 first_row = g_battle_text_substitution_value_27, limit = g_battle_text_substitution_value_28, r, c;
    u32 ox = origin[0], oy = origin[1], stride = (u32)origin[4] >> 1;
    u8* row = (u8*)image + stride * oy;
    u32 bits = *glyph_bitmap, left = 4;
    for (r = 0; r != 14; r++, row += stride) {
        if (!(r < limit)) continue;
        for (c = 0; c != 10; c++) {
            u32 code = (bits & 0xc0) >> 6;
            if (code && !(r < first_row)) {
                u32 value = code + (u32)palette_offset, px = (u32)c + ox;
                u8* p = row + (px >> 1);
                u32 b = *p;
                if (!(px & 1)) b &= 0xf0; else { b &= 0x0f; value <<= 4; }
                *p = (u8)(b | value);
            }
            bits <<= 2;
            if (--left == 0) { left = 4; glyph_bitmap++; bits = *glyph_bitmap; }
        }
    }
}
