/* Native, headless resolution of a plain weapon attack (battle formula 1) with the game's own code.
 * RAM image from the disc at 0x80000000; units are set up by field name; RNG is a deterministic placeholder LCG
 * (the real generator is a BIOS routine: constants must be verified before trusting the distribution). */
#include "fft/battle.h"

static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static void out(const char* s) { long n = 0; while (s[n]) n++; sys3(4, 1, (long)s, n); }
static void outnum(int v, int width) {
    char buf[16]; int i = 15, neg = v < 0; buf[15] = 0;
    if (neg) v = -v;
    do { buf[--i] = '0' + v % 10; v /= 10; } while (v);
    if (neg) buf[--i] = '-';
    while (15 - i < width) buf[--i] = ' ';
    out(buf + i);
}
struct old_mmap_args { long addr, len, prot, flags, fd, off; };
static int map_ram(void) {
    struct old_mmap_args a = { 0x80000000, 0x200000, 3, 0x32, -1, 0 };
    return sys3(90, (long)&a, 0, 0) == (long)0x80000000;
}
static long load_file(const char* path, unsigned addr) {
    long fd = sys3(5, (long)path, 0, 0), total = 0, n;
    if (fd < 0) return -1;
    while ((n = sys3(3, fd, (long)(addr + total), 1 << 20)) > 0) total += n;
    sys3(6, fd, 0, 0);
    return total;
}

/* libc/BIOS replacements the game code links against */
int abs(int x) { return x < 0 ? -x : x; }
static unsigned long g_seed = 1;
int rand(void) { g_seed = g_seed * 0x41c64e6dUL + 0x3039UL; return (int)((g_seed >> 16) & 0x7fff); }

extern void (*native_formula_handlers[128])(void);

static battle_stats_t attacker, target;
static battle_action_data_t target_data;

static void zero(void* p, int n) { unsigned char* c = p; while (n--) *c++ = 0; }

static void setup_units(int weapon_id, int facing) {
    zero(&attacker, sizeof attacker);
    zero(&target, sizeof target);
    attacker.level = 20; attacker.brave = 70; attacker.faith = 50;
    attacker.hp = attacker.max_hp = 400;
    attacker.attributes[UNIT_ATTRIBUTE_PHYSICAL_ATTACK] = 12;
    attacker.attributes[UNIT_ATTRIBUTE_MAGIC_ATTACK] = 10;
    attacker.attributes[UNIT_ATTRIBUTE_SPEED] = 8;
    attacker.unit_flags = UNIT_FLAG_MALE;
    attacker.birthday.value = 0;                       /* Aries */
    attacker.equipment[UNIT_EQUIPMENT_SLOT_RIGHT_HAND_WEAPON] = weapon_id;
    attacker.x = 5; attacker.position.raw = 5;         /* x=5, y=5, facing 0 */

    target.level = 20; target.brave = 60; target.faith = 50;
    target.hp = target.max_hp = 400;
    target.attributes[UNIT_ATTRIBUTE_PHYSICAL_ATTACK] = 10;
    target.attributes[UNIT_ATTRIBUTE_MAGIC_ATTACK] = 10;
    target.attributes[UNIT_ATTRIBUTE_SPEED] = 8;
    target.unit_flags = UNIT_FLAG_FEMALE;
    target.birthday.value = (u16)(6 << 12);            /* Libra: opposite sign -> best compatibility */
    target.x = 6; target.position.raw = (u16)((facing << 8) | 5);   /* one tile east, facing varies */
    target.equipment_stats[BATTLE_UNIT_EQUIPMENT_STAT_CLASS_PHYSICAL_EVADE] = 15;   /* 15% class evade */
}

static void setup_ability(int weapon_id) {
    item_data_t* item = &g_main_item_primary_data[weapon_id];
    zero(&g_current_ability, sizeof g_current_ability);
    g_current_ability.weapon_id = weapon_id;
    g_current_ability.primary_weapon_id = weapon_id;
    g_current_ability.weapon_data = g_main_item_weapon_data[item->secondary_data_id];
    g_current_ability.formula = 1;
    g_current_ability.attacker_id = 0;
    g_current_ability.target_id = 1;
}

void _start(void) {
    int weapon_id = 27, facing, t, hits, crits, dmin, dmax, dsum;
    if (!map_ram()) { out("mmap FAILED\n"); sys3(1, 1, 0, 0); }
    load_file("/disc/SCUS_942.21", 0x8000f800);
    load_file("/disc/BATTLE.BIN", 0x80067000);

    out("plain weapon attack (formula 1) resolved natively: attacker PA12 Br70 male Aries, weapon item ");
    outnum(weapon_id, 0); out(" (WP "); outnum(g_main_item_weapon_data[g_main_item_primary_data[weapon_id].secondary_data_id].power, 0);
    out(")\ntarget: 400 HP, 15% class evade, female Libra; 2000 trials per direction, RNG = placeholder LCG\n\n");
    out("target facing | classified as | hit%  crit%  dmg min  avg  max\n");
    for (facing = 0; facing < 4; facing++) {
        hits = crits = dsum = 0; dmin = 99999; dmax = 0;
        g_seed = 12345 + facing;
        for (t = 0; t < 2000; t++) {
            setup_units(weapon_id, facing);
            setup_ability(weapon_id);
            zero(&target_data, sizeof target_data);
            target_data.hit = 1;                  /* what battle_action_run_pre_formula_setup establishes */
            target_data.attack_accuracy = 100;
            g_battle_action_state = BATTLE_ACTION_STATE_EXECUTE;
            g_battle_action_attacker = &attacker;
            g_battle_action_target = &target;
            g_battle_action_target_data = &target_data;
            native_formula_handlers[1]();
            if (target_data.hit) {
                hits++;
                if (target_data.critical) crits++;
                dsum += target_data.hp_damage;
                if (target_data.hp_damage < dmin) dmin = target_data.hp_damage;
                if (target_data.hp_damage > dmax) dmax = target_data.hp_damage;
            }
        }
        outnum(facing, 8); outnum(g_current_ability.facing_modifier, 14); out("(0 front 1 side 2 back)");
        outnum(hits * 100 / 2000, 5); outnum(hits ? crits * 100 / hits : 0, 6);
        outnum(hits ? dmin : 0, 9); outnum(hits ? dsum / hits : 0, 6); outnum(dmax, 5); out("\n");
    }
    sys3(1, 0, 0, 0);
}
