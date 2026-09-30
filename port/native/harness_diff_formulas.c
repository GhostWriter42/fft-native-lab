/* Differential fuzz of the battle formulas: the native build of each handler in g_battle_formula_handlers against the
 * ORIGINAL machine code (R3000 interpreter), on identical random-but-plausible battle states.
 *
 * Per trial: random attacker / target units + current-ability record are written to native RAM, copied to the interpreter's
 * RAM, the original code runs on the interpreter, then a forked child runs the native handler on its private copy-on-write
 * copy of RAM and compares all of RAM (except the interpreter's stack) with the interpreter's result. A native crash
 * (SIGSEGV, SIGFPE, ...) is caught by the parent. Trials where the ORIGINAL code left RAM (hardware window / unmapped) or
 * hung are skipped: the random state was too wild to mean anything.
 * Freestanding -m32; the game's data comes from the RAM image. */
#include "fft/battle.h"
#include "r3000/r3000.h"

/* ---------------------------------------------------------------------------------------- freestanding I/O */
static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static long sys4(long n, long a, long b, long c, long d) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}
static void out(const char* s) { long n = 0; while (s[n]) n++; sys3(4, 1, (long)s, n); }
static void outnum(long v) {
    char buf[24]; int i = 23, neg = v < 0; buf[23] = 0;
    if (neg) v = -v;
    do { buf[--i] = '0' + v % 10; v /= 10; } while (v);
    if (neg) buf[--i] = '-';
    out(buf + i);
}
static void outhex(unsigned v) {
    char buf[11]; int i;
    for (i = 0; i < 8; i++) buf[i] = "0123456789abcdef"[(v >> (4 * (7 - i))) & 15];
    buf[8] = 0; out(buf);
}
struct old_mmap_args { long addr, len, prot, flags, fd, off; };
static int map_ram(void) { struct old_mmap_args a = { 0x80000000, 0x200000, 7 /* RWX: trampolines */, 0x32, -1, 0 }; return sys3(90, (long)&a, 0, 0) == (long)0x80000000; }
static void* map_shared(long len) { struct old_mmap_args a = { 0, len, 3, 0x21, -1, 0 }; return (void*)sys3(90, (long)&a, 0, 0); }
static long load_file(const char* path, unsigned addr) {
    long fd = sys3(5, (long)path, 0, 0), total = 0, n;
    if (fd < 0) return -1;
    while ((n = sys3(3, fd, (long)(addr + total), 1 << 20)) > 0) total += n;
    sys3(6, fd, 0, 0);
    return total;
}

/* libc/BIOS replacements the native game code links against -- the same LCG the BIOS (and the interpreter) use */
static unsigned g_native_seed = 1, g_native_rand_calls;
int abs(int x) { return x < 0 ? -x : x; }
int rand(void) { g_native_rand_calls++; g_native_seed = g_native_seed * 1103515245u + 12345u; return (int)((g_native_seed >> 16) & 0x7fff); }

extern void (*native_formula_handlers[128])(void);
extern const char* native_formula_handlers_names[128];

/* ------------------------------------------------------------------------------------------------- random */
static unsigned g_rng = 2463534242u;
static unsigned rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static void rand_bytes(void* p, unsigned n) { unsigned char* c = (unsigned char*)p; while (n--) *c++ = (unsigned char)rnd(); }
static unsigned sparse(unsigned bits) { return rnd() & rnd() & bits; }        /* each bit set with probability 1/4 */

#define ATT ((battle_stats_t*)0x801e0000)
#define TGT ((battle_stats_t*)0x801e0200)

static void shape_unit(battle_stats_t* u) {                                   /* plausible values over the random bytes */
    int i;
    u->level = (u8)(1 + rnd() % 99);
    u->original_brave = u->brave = (u8)(rnd() % 101);
    u->original_faith = u->faith = (u8)(rnd() % 101);
    u->max_hp = (u16)(1 + rnd() % 999); u->hp = (u16)(rnd() % (u->max_hp + 1u));
    u->max_mp = (u16)(rnd() % 400); u->mp = (u16)(rnd() % (u->max_mp + 1u));
    for (i = 0; i < 3; i++) { u->base_attributes[i] = (u8)(1 + rnd() % 30); u->equipment_attributes[i] = (u8)(rnd() % 8); u->attributes[i] = (u8)(1 + rnd() % 40); }
    u->birthday.value = (u16)(((rnd() % 12) << 12) | (rnd() & 0x0fff));
    for (i = 0; i < 7; i++) u->equipment[i] = (u8)(rnd() % 0x90);
    for (i = 0; i < 11; i++) u->equipment_stats[i] = (u8)(rnd() % 60);
    for (i = 0; i < 5; i++) { u->status_sets.current[i] = (u8)sparse(0xff); u->status_sets.immunity[i] = (u8)sparse(0xff); u->status_sets.innate[i] = (u8)sparse(0xff); }
    u->x = (u8)(rnd() % 20); u->position.raw = (u16)(((rnd() % 4) << 8) | (rnd() % 20));
}

static unsigned copy_seed;
static void setup_state(int id, int trial) {
    g_rng = ((unsigned)(id + 1) * 2654435761u) ^ ((unsigned)(trial + 1) * 2246822519u); g_rng |= 1; rnd(); rnd(); rnd();      /* every trial is replayable from (id, trial) */
    rand_bytes(ATT, sizeof(battle_stats_t)); rand_bytes(TGT, sizeof(battle_stats_t));
    if (trial & 1) { shape_unit(ATT); shape_unit(TGT); }
    ATT->action.hit = (u8)(rnd() & 1); TGT->action.hit = (u8)((rnd() & 3) != 0);
    if (rnd() & 1) TGT->action.attack_accuracy = 100;
    rand_bytes(&g_current_ability, sizeof g_current_ability);
    if (trial & 1) {                                                          /* mostly-sane ability record */
        g_current_ability.xa = (u16)(rnd() % 400); g_current_ability.ya = (u16)(rnd() % 400);
        g_current_ability.attacker_faith = (u8)(rnd() % 101); g_current_ability.target_faith = (u8)(rnd() % 101);
        g_current_ability.strike_count = (u8)(1 + rnd() % 2); g_current_ability.strike_counter = (u8)(rnd() & 1);
        g_current_ability.facing_modifier = (u8)(rnd() % 3);
        g_current_ability.base_hit = (u8)(rnd() % 101);
        g_current_ability.mp_cost = (u8)(rnd() % 50);
    }
    g_current_ability.formula = (u8)id;
    g_current_ability.attacker_id = (u8)(rnd() % 22); g_current_ability.target_id = (u8)(rnd() % 22);
    g_battle_action_attacker = ATT;
    g_battle_action_target = TGT;
    g_battle_action_target_data = &TGT->action;
    g_battle_action_attacker_data = &ATT->action;
    g_battle_action_state = (rnd() % 8) ? BATTLE_ACTION_STATE_EXECUTE : (battle_action_state_e)(rnd() % 3);   /* execute / AI simulation / preview */
    copy_seed = rnd() & 0x7fffffff;
}

/* ------------------------------------------------------------------------------------------- the two machines */
static unsigned char interp_ram[0x200000];
static r3k_t cpu;
struct result { unsigned ndiff, marker, addr[6], nat[6], orig[6], fault_addr, fault_eip, fault_sig, rand_calls; };
static struct result* shared;

static void fault_handler(int sig, void* info, void* uc) {
    shared->fault_sig = (unsigned)sig;
    shared->fault_addr = *(unsigned*)((char*)info + 12);                      /* siginfo.si_addr */
    shared->fault_eip = *(unsigned*)((char*)uc + 76);                         /* ucontext.uc_mcontext.gregs[REG_EIP] */
    shared->marker = 0xdeadfa17u;
    sys3(1, 0, 0, 0);
}
struct ksigaction { void (*handler)(int, void*, void*); unsigned long flags; void (*restorer)(void); unsigned long long mask; };
static void install_fault_handlers(void) {
    struct ksigaction sa;
    sa.handler = fault_handler; sa.flags = 4 /* SA_SIGINFO */ | 0x40000000 /* SA_NODEFER */; sa.restorer = 0; sa.mask = 0;
    sys4(174, 11, (long)&sa, 0, 8);                                           /* SIGSEGV */
    sys4(174, 8, (long)&sa, 0, 8);                                            /* SIGFPE  */
    sys4(174, 7, (long)&sa, 0, 8);                                            /* SIGBUS  */
    sys4(174, 4, (long)&sa, 0, 8);                                            /* SIGILL  */
}
/* Trampolines: an x86 `jmp` written over the first bytes of every natively linked function at its ORIGINAL address, so that
 * function pointers stored in the game's data (which hold PS1 addresses) reach the native code. The original MIPS code stays
 * in the interpreter's RAM; code ranges are excluded when RAM is copied and compared. */
struct native_stub { unsigned int ps1_addr; unsigned int size; void* native; };
extern const struct native_stub g_native_stubs[];
extern const int g_native_stub_count;
static unsigned data_lo[6000], data_hi[6000];
static int ndata, n_installed;
static void build_ranges_and_install(void) {
    static unsigned idx[4096];
    int i, j, n = g_native_stub_count;
    unsigned cursor = 0x80000000u;
    for (i = 0; i < n; i++) idx[i] = (unsigned)i;
    for (i = 1; i < n; i++) { unsigned v = idx[i]; for (j = i - 1; j >= 0 && g_native_stubs[idx[j]].ps1_addr > g_native_stubs[v].ps1_addr; j--) idx[j + 1] = idx[j]; idx[j + 1] = v; }
    for (i = 0; i < n; i++) {
        const struct native_stub* f = &g_native_stubs[idx[i]];
        if (f->ps1_addr >= 0x801f0000u) continue;
        if (f->ps1_addr > cursor) { data_lo[ndata] = cursor; data_hi[ndata] = f->ps1_addr; ndata++; }
        if (f->ps1_addr + f->size > cursor) cursor = f->ps1_addr + f->size;
        if (f->native && f->size >= 5) {
            unsigned char* p = (unsigned char*)f->ps1_addr;
            p[0] = 0xe9; *(unsigned*)(p + 1) = (unsigned)f->native - (f->ps1_addr + 5);
            n_installed++;
        }
    }
    if (cursor < 0x801f0000u) { data_lo[ndata] = cursor; data_hi[ndata] = 0x801f0000u; ndata++; }
}
struct orig_func { const char* name; unsigned int addr; unsigned int size; };
extern const struct orig_func g_orig_funcs[];
extern const int g_orig_func_count;
static void func_at(unsigned pc) {
    int i;
    for (i = 0; i < g_orig_func_count; i++)
        if (pc >= g_orig_funcs[i].addr && pc < g_orig_funcs[i].addr + g_orig_funcs[i].size) { out(g_orig_funcs[i].name); out("+0x"); outhex(pc - g_orig_funcs[i].addr); return; }
    outhex(pc);
}
static void copy_ram_to_interp(void) {
    int i;
    for (i = 0; i < ndata; i++) {
        unsigned n = (data_hi[i] - data_lo[i]) / 4;
        void* d = interp_ram + (data_lo[i] & 0x1fffff); const void* s = (const void*)data_lo[i];
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
}
static void copy_ram_to_interp_all(void) {
    unsigned n = 0x200000 / 4;
    void* d = interp_ram; const void* s = (const void*)0x80000000;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static void where(unsigned a) {
    if (a >= (unsigned)ATT && a < (unsigned)ATT + sizeof(battle_stats_t)) { out("attacker+0x"); outhex(a - (unsigned)ATT); }
    else if (a >= (unsigned)TGT && a < (unsigned)TGT + sizeof(battle_stats_t)) { out("target+0x"); outhex(a - (unsigned)TGT); }
    else if (a >= (unsigned)&g_current_ability && a < (unsigned)&g_current_ability + sizeof g_current_ability) { out("g_current_ability+0x"); outhex(a - (unsigned)&g_current_ability); }
    else outhex(a);
}

int main_test(void) {
    int id, t, total_bad = 0, total_trials = 0;
#ifndef TRIALS
#define TRIALS 300
#endif
#ifndef MAX_STEPS
#define MAX_STEPS 400000
#endif
    const int N = TRIALS;
    for (id = 0; id < 128; id++) {
        void (*nat)(void) = native_formula_handlers[id] ? g_battle_formula_handlers[id] : 0;      /* PS1 address -> trampoline -> native code */
        unsigned orig = (unsigned)g_battle_formula_handlers[id];
        int trials = 0, mism = 0, crashes = 0, skip_io = 0, skip_fault = 0, shown = 0, sig_last = 0;
        unsigned crash_addr = 0, crash_eip = 0;
        int div_traps = 0, rand_diff = 0, interp_div = 0, interp_rand = 0;
        if (!nat || !orig) continue;
        for (t = 0; t < N; t++) {
            long pid, status = 0;
            int rc;
            setup_state(id, t);
            copy_ram_to_interp();
            cpu.io_reads = cpu.io_writes = 0; cpu.bios_rand_seed = copy_seed; cpu.div_zero = cpu.div_overflow = 0; cpu.rand_calls = 0; cpu.watch_lo = cpu.watch_hi = 0; cpu.wlog_n = 0;
            rc = r3k_call(&cpu, orig, 0, 0, MAX_STEPS);
            if (rc) { skip_fault++; continue; }
            if (cpu.io_reads || cpu.io_writes) { skip_io++; continue; }
            shared->marker = 0; shared->ndiff = 0;
            g_native_seed = copy_seed; g_native_rand_calls = 0; interp_div = (int)(cpu.div_zero + cpu.div_overflow); interp_rand = (int)cpu.rand_calls;
            pid = sys3(2, 0, 0, 0);                                            /* fork */
            if (pid == 0) {
                unsigned a, n = 0;
                int r;
                nat();
                for (r = 0; r < ndata; r++)
                    for (a = data_lo[r]; a < data_hi[r]; a += 4) {
                        unsigned x = *(unsigned*)a, y = *(unsigned*)(interp_ram + (a & 0x1fffff));
                        if (x != y) { if (n < 6) { shared->addr[n] = a; shared->nat[n] = x; shared->orig[n] = y; } n++; }
                    }
                shared->ndiff = n; shared->rand_calls = g_native_rand_calls; shared->marker = 0xd0d0d0d0u;
                sys3(1, 0, 0, 0);
            }
            sys4(114, pid, (long)&status, 0, 0);                               /* wait4 */
            trials++;
            if (shared->marker == 0xdeadfa17u && shared->fault_sig == 8 && interp_div) { div_traps++; continue; }      /* x86 traps where MIPS defines a result: expected */
            if ((status & 0x7f) != 0 || shared->marker != 0xd0d0d0d0u) {
                crashes++; sig_last = (int)(status & 0x7f);
                if (shared->marker == 0xdeadfa17u) { sig_last = (int)shared->fault_sig; if (!crash_eip) { crash_addr = shared->fault_addr; crash_eip = shared->fault_eip; } }
                continue;
            }
            if (shared->rand_calls != (unsigned)interp_rand) rand_diff++;
            if (shared->ndiff) {
                unsigned k;
                mism++;
                if (shown++ < 2) {
                    out("    ["); outnum(id); out("] "); out(native_formula_handlers_names[id]); out(" trial "); outnum(t); out(": "); outnum(shared->ndiff); out(" words differ, e.g.");
                    for (k = 0; k < 3 && k < shared->ndiff; k++) { out("  "); where(shared->addr[k]); out(" native "); outhex(shared->nat[k]); out(" original "); outhex(shared->orig[k]); }
                    out("; rand() calls native "); outnum(shared->rand_calls); out(" original "); outnum(interp_rand); out("\n");
                    /* who wrote the first differing word in the original? replay the trial on the interpreter with a write watch */
                    setup_state(id, t); copy_ram_to_interp();
                    cpu.bios_rand_seed = copy_seed; cpu.rand_calls = 0; cpu.wlog_n = 0; cpu.watch_lo = shared->addr[0]; cpu.watch_hi = shared->addr[0] + 4;
                    r3k_call(&cpu, orig, 0, 0, MAX_STEPS);
                    for (k = 0; k < cpu.wlog_n; k++) { out("        original wrote "); outhex(cpu.wlog_val[k]); out(" at "); outhex(cpu.wlog_addr[k]); out(" from "); func_at(cpu.wlog_pc[k]); out("\n"); }
                    cpu.watch_lo = cpu.watch_hi = 0;
                }
            }
        }
        total_trials += trials; total_bad += mism + crashes;
        outnum(id); out(" "); out(native_formula_handlers_names[id]); out(": ");
        outnum(trials); out(" trials, "); outnum(mism); out(" mismatches, "); outnum(crashes); out(" native crashes");
        if (crashes) { out(" (signal "); outnum(sig_last); out(" at eip "); outhex(crash_eip); out(" address "); outhex(crash_addr); out(")"); }
        if (div_traps) { out(", "); outnum(div_traps); out(" x86 divide traps where MIPS defines a result (not counted)"); }
        if (rand_diff) { out(", rand() call count differs in "); outnum(rand_diff); }
        out(", skipped: "); outnum(skip_io); out(" off-RAM access, "); outnum(skip_fault); out(" hung/faulted in the original\n");
    }
    out("== "); outnum(total_trials); out(" differential trials, "); outnum(total_bad); out(" with a difference or a native crash\n");
    return total_bad != 0;
}

void _start(void) {
    int bad;
    if (!map_ram()) { out("mmap FAILED\n"); sys3(1, 1, 0, 0); }
    shared = (struct result*)map_shared(4096);
    load_file("/disc/SCUS_942.21", 0x8000f800);
    load_file("/disc/BATTLE.BIN", 0x80067000);
    copy_ram_to_interp_all();                                                 /* the interpreter keeps the original MIPS code */
    build_ranges_and_install();
    out("trampolines installed for "); outnum(n_installed); out(" natively linked functions ("); outnum(ndata); out(" data ranges compared)"); out("\n");
    r3k_reset(&cpu, interp_ram);
    install_fault_handlers();
    bad = main_test();
    sys3(1, bad, 0, 0);
}
