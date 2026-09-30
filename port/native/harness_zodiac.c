/* Freestanding 32-bit harness: runs the game's own battle_formula_apply_zodiac_compatibility natively
 * over every attacker/target zodiac pairing, using the REAL compatibility table read from the disc image. */
#include "fft/battle.h"

/* --- minimal Linux/i386 syscalls, no libc --- */
static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static void out(const char* s) {
    long n = 0;
    while (s[n]) n++;
    sys3(4, 1, (long)s, n);
}
static void outnum(int v, int width) {
    char buf[16];
    int i = 15, neg = v < 0;
    buf[15] = 0;
    if (neg) v = -v;
    do { buf[--i] = '0' + v % 10; v /= 10; } while (v);
    if (neg) buf[--i] = '-';
    while (15 - i < width) buf[--i] = ' ';
    out(buf + i);
}

/* --- the game state the function reads (normally at fixed PS1 addresses) --- */
battle_stats_t* g_battle_action_attacker;
battle_stats_t* g_battle_action_target;
battle_current_ability_t g_current_ability;
u8 g_battle_zodiac_compatibility_modifiers[16] = {
#include "zodiac_table.inc"
};

static battle_stats_t attacker_unit, target_unit;

static int run(int attacker_sign, int target_sign, int attacker_flags, int target_flags, int xa) {
    attacker_unit.birthday.value = (u16)(attacker_sign << 12);
    target_unit.birthday.value = (u16)(target_sign << 12);
    attacker_unit.unit_flags = attacker_flags;
    target_unit.unit_flags = target_flags;
    g_battle_action_attacker = &attacker_unit;
    g_battle_action_target = &target_unit;
    g_current_ability.xa = xa;
    battle_formula_apply_zodiac_compatibility();
    return (s16)g_current_ability.xa;
}

static void matrix(const char* title, int af, int tf) {
    int a, t;
    out(title);
    out("\n     target:");
    for (t = 0; t < 13; t++) outnum(t, 5);
    out("\n");
    for (a = 0; a < 13; a++) {
        outnum(a, 4);
        out(" atk |");
        for (t = 0; t < 13; t++) outnum(run(a, t, af, tf, 100), 5);
        out("\n");
    }
}

void _start(void) {
    int i;
    out("table from disc (difference -> modifier): ");
    for (i = 0; i < 12; i++) { outnum(g_battle_zodiac_compatibility_modifiers[i], 2); }
    out("\n(0 neutral, 1 bad, 2 good, 3 gender-dependent, 4 worst, 5 best)\n\n");
    matrix("XA after zodiac compatibility, base XA = 100, MALE attacker vs FEMALE target:", UNIT_FLAG_MALE, UNIT_FLAG_FEMALE);
    out("\n");
    matrix("same, MALE attacker vs MALE target:", UNIT_FLAG_MALE, UNIT_FLAG_MALE);
    out("\n");
    matrix("same, MONSTER attacker vs FEMALE target:", UNIT_FLAG_MONSTER, UNIT_FLAG_FEMALE);
    sys3(1, 0, 0, 0);
}
