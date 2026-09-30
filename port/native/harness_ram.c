/* Freestanding 32-bit harness with a PS1 "RAM image": 2 MiB mapped at 0x80000000 and loaded from the disc's
 * SCUS_942.21 and BATTLE.BIN, so the game's globals (linked at their original addresses) hold real data. */
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
static void outhex(unsigned v, int digits) {
    char buf[12]; int i;
    for (i = 0; i < digits; i++) buf[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15];
    buf[digits] = 0; out(buf);
}

struct old_mmap_args { long addr, len, prot, flags, fd, off; };
static int map_ram(void) {
    struct old_mmap_args a = { 0x80000000, 0x200000, 3 /* RW */, 0x32 /* PRIVATE|FIXED|ANON */, -1, 0 };
    long r = sys3(90, (long)&a, 0, 0);
    return r == (long)0x80000000;
}
static long load_file(const char* path, unsigned addr) {
    long fd = sys3(5, (long)path, 0, 0), total = 0, n;
    if (fd < 0) return -1;
    while ((n = sys3(3, fd, (long)(addr + total), 1 << 20)) > 0) total += n;
    sys3(6, fd, 0, 0);
    return total;
}

static battle_stats_t attacker;

void _start(void) {
    long n1, n2;
    int id, shown = 0;
    if (!map_ram()) { out("mmap at 0x80000000 FAILED\n"); sys3(1, 1, 0, 0); }
    n1 = load_file("/disc/SCUS_942.21", 0x8000f800);
    n2 = load_file("/disc/BATTLE.BIN", 0x80067000);
    out("RAM image mapped at 0x80000000: SCUS_942.21 "); outnum(n1, 0);
    out(" bytes @0x8000f800, BATTLE.BIN "); outnum(n2, 0); out(" bytes @0x80067000\n");
    out("real data read through the game's own symbols: g_main_item_primary_data @0x");
    outhex((unsigned)&g_main_item_primary_data[0], 8); out("\n\n");

    /* attacker PA 12, MA 10, Speed 8, Brave 70 */
    attacker.attributes[UNIT_ATTRIBUTE_PHYSICAL_ATTACK] = 12;
    attacker.attributes[UNIT_ATTRIBUTE_MAGIC_ATTACK] = 10;
    attacker.attributes[UNIT_ATTRIBUTE_SPEED] = 8;
    attacker.brave = 70;
    g_battle_action_attacker = &attacker;

    out("weapon item id | type | formula | WP | XA (PA12 MA10 SP8 Br70) | XA*WP\n");
    for (id = 1; id < 0x80 && shown < 40; id++) {
        item_data_t* item = &g_main_item_primary_data[id];
        weapon_data_t* w;
        if (item->type == 0 || item->type > 0x14) continue;
        w = &g_main_item_weapon_data[item->secondary_data_id];
        if (w->power == 0) continue;
        g_current_ability.weapon_id = id;
        g_current_ability.weapon_data = *w;
        battle_formula_calculate_base_xa();
        outnum(id, 10); outnum(item->type, 8); outnum(w->formula, 8); outnum(w->power, 6);
        outnum((s16)g_current_ability.xa, 20); outnum((s16)g_current_ability.xa * (s16)g_current_ability.ya, 10); out("\n");
        shown++;
    }
    sys3(1, 0, 0, 0);
}
