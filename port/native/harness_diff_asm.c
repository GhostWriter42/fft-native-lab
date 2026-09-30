/* (shared prologue copied from harness_diff_libgte.c) Differential test: the native libgte (software GTE + libgte_native.c + the plain-C members compiled from the repo)
 * against the ORIGINAL libgte machine code from SCUS_942.21, run on the R3000 interpreter (r3000/). Same inputs, same
 * starting GTE state; results (return value, memory, GTE registers) must be identical bit for bit.
 * Freestanding -m32; the game's tables come from the RAM image. */
#include "psx/libgte.h"
#include "gte.h"
#include "r3000/r3000.h"

struct orig_func { const char* name; unsigned int addr; unsigned int size; };
extern const struct orig_func g_orig_funcs[];
extern const int g_orig_func_count;

/* ------------------------------------------------------------------------------------------- freestanding I/O */
static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
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
static int map_ram(void) { struct old_mmap_args a = { 0x80000000, 0x200000, 3, 0x32, -1, 0 }; return sys3(90, (long)&a, 0, 0) == (long)0x80000000; }
static long load_file(const char* path, unsigned addr) {
    long fd = sys3(5, (long)path, 0, 0), total = 0, n;
    if (fd < 0) return -1;
    while ((n = sys3(3, fd, (long)(addr + total), 1 << 20)) > 0) total += n;
    sys3(6, fd, 0, 0);
    return total;
}
static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* ------------------------------------------------------------------------------------------------- random */
static unsigned g_rng = 2463534242u;
static unsigned rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int rrange(int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }
static int rs16(void) {                          /* random s16, with a fair share of edge values */
    unsigned m = rnd() % 8;
    if (m == 0) { static const int e[8] = { 0, 1, -1, 0x7fff, -0x8000, 4096, -4096, 0x4000 }; return e[rnd() % 8]; }
    if (m < 4) return rrange(-6000, 6000);
    return (int)(short)(rnd() & 0xffff);
}
static int rs32(void) {
    unsigned m = rnd() % 8;
    if (m == 0) { static const int e[6] = { 0, 1, -1, 0x7fffffff, (int)0x80000000, 0x8000 }; return e[rnd() % 6]; }
    if (m < 4) return rrange(-200000, 200000);
    if (m < 6) return (int)(rnd() >> (rnd() % 31));
    return (int)rnd();
}

/* ------------------------------------------------------------------------------------- the two machines */
#define TEST_LO 0x801e0000u                      /* scratch area for arguments / results, compared after every call */
#define TEST_HI 0x801f0000u                      /* [0x801f0000, 0x80200000) is the interpreter's stack: never compared */
#define STACK_LO 0x8002b800u                     /* libgte's own globals (matrix stack) */
#define STACK_HI 0x8002c000u
static unsigned char interp_ram[0x200000];
static r3k_t cpu;

static unsigned orig_addr(const char* name) {
    int i;
    for (i = 0; i < g_orig_func_count; i++) if (streq(g_orig_funcs[i].name, name)) return g_orig_funcs[i].addr;
    out("no such original function: "); out(name); out("\n"); sys3(1, 2, 0, 0);
    return 0;
}
static unsigned char* nat(unsigned addr) { return (unsigned char*)addr; }      /* native RAM is mapped at the PS1 addresses */

static void copy_in(unsigned lo, unsigned hi) { unsigned i; for (i = lo; i < hi; i += 4) *(unsigned*)(interp_ram + (i & 0x1fffff)) = *(unsigned*)i; }
static void fill_random(unsigned addr, unsigned nbytes) { unsigned i; for (i = 0; i < nbytes; i += 4) *(unsigned*)(addr + i) = rnd(); }

static gte_state_t gte_before, gte_after_native;
static const char* t_name;
static int t_trials, t_ret, t_mem, t_gte_ctrl, t_gte_data, t_shown, t_skipped, g_trial_trapped;
static int t_data_idx[32];
static int total_trials, total_bad;
static int sync_areas(void) { copy_in(TEST_LO, TEST_HI); copy_in(STACK_LO, STACK_HI); return 0; }

static void begin(const char* name) {
    int i;
    t_name = name; t_trials = t_ret = t_mem = t_gte_ctrl = t_gte_data = t_shown = t_skipped = g_trial_trapped = 0;
    for (i = 0; i < 32; i++) t_data_idx[i] = 0;
}

static void report(const char* what, unsigned a, unsigned b, unsigned where) {
    if (t_shown++ < 3) { out("    "); out(t_name); out(": "); out(what); out(" at "); outhex(where); out(": native "); outhex(a); out(" original "); outhex(b); out("\n"); }
}
static unsigned interp_call(const char* fname, const unsigned* args, int nargs) {
    int rc = r3k_call(&cpu, orig_addr(fname), args, nargs, 2000000);
    if (rc == R3K_FAULT_OVERFLOW) { g_trial_trapped = 1; return 0; }             /* a trapping add overflowed: the console would have taken an exception */
    if (rc) {
        out("  original code faulted in "); out(fname); out(": kind "); outnum(rc); out(" pc "); outhex(cpu.fault_pc);
        out(" instr "); outhex(cpu.fault_instr); out(" addr "); outhex(cpu.fault_addr); out("\n");
        sys3(1, 2, 0, 0);
    }
    return cpu.r[2];
}
/* begin a trial: snapshot GTE, copy the inputs the test wrote in native RAM to the interpreter's RAM */
static void trial_start(void) { gte_before = g_gte; sync_areas(); }
static void native_done(void) { gte_after_native = g_gte; g_gte = gte_before; }
/* after the original code ran: compare everything. use_ret: compare the return value; strict_gte: GTE registers are outputs. */
static void trial_end(unsigned ret_native, int use_ret, int strict_gte) {
    unsigned i;
    int bad = 0;
    if (g_trial_trapped) { g_trial_trapped = 0; t_skipped++; return; }
    t_trials++;
    if (use_ret && ret_native != cpu.r[2]) { t_ret++; bad = 1; report("return value", ret_native, cpu.r[2], 0); }
    for (i = TEST_LO; i < TEST_HI; i += 4) {
        unsigned x = *(unsigned*)i, y = *(unsigned*)(interp_ram + (i & 0x1fffff));
        if (x != y) { t_mem++; bad = 1; report("memory", x, y, i); break; }
    }
    for (i = STACK_LO; i < STACK_HI; i += 4) {
        unsigned x = *(unsigned*)i, y = *(unsigned*)(interp_ram + (i & 0x1fffff));
        if (i == 0x8002b8a8u || i == 0x8002bbd8u) continue;                       /* g_psyq_gte_*_saved_ra: the retail code parks $ra in a global */
        if (x != y) { t_mem++; bad = 1; report("libgte global", x, y, i); break; }
    }
    for (i = 0; i < 32; i++) {
        unsigned x = (unsigned)gte_after_native.ctrl[i], y = (unsigned)g_gte.ctrl[i];
        if (i == GTE_C_FLAG) { x = gte_after_native.flag; y = g_gte.flag; }
        if (x != y) { t_gte_ctrl++; if (strict_gte) { bad = 1; report("GTE control reg", x, y, i); } break; }
    }
    for (i = 0; i < 32; i++) {
        if (i == GTE_D_ORGB || i == GTE_D_IRGB || i == GTE_D_LZCR) continue;
        if ((unsigned)gte_after_native.data[i] != (unsigned)g_gte.data[i]) { t_gte_data++; t_data_idx[i]++; if (strict_gte) { bad = 1; report("GTE data reg", (unsigned)gte_after_native.data[i], (unsigned)g_gte.data[i], i); } break; }
    }
    total_bad += bad;
}
static void end_group(void) {
    total_trials += t_trials;
    out(t_name); out(": "); outnum(t_trials); out(" trials, mismatches: return "); outnum(t_ret); out(", memory "); outnum(t_mem);
    out("   (final GTE state differs: control "); outnum(t_gte_ctrl); out(", data "); outnum(t_gte_data); out(")");
    if (t_skipped) { out("  [skipped "); outnum(t_skipped); out(": the original trapping add overflowed]"); }
    out("\n");
    if (t_gte_data) {
        int i;
        out("      first differing GTE data registers (index:count):");
        for (i = 0; i < 32; i++) if (t_data_idx[i]) { out(" "); outnum(i); out(":"); outnum(t_data_idx[i]); }
        out("\n");
    }
}

/* random GTE environment shared by both machines */
static void identity_gte_rot(int scale);
static void random_gte(void) {
    int i;
    gte_reset();
    if (rnd() & 1) {                                   /* a plausible camera: near-identity rotation, small translation */
        identity_gte_rot(rrange(3500, 4096));
        for (i = 0; i < 3; i++) gte_ctc2(GTE_C_TRX + i, (unsigned)rrange(-2000, 2000));
        gte_ctc2(GTE_C_TRZ, (unsigned)rrange(200, 4000));
        gte_ctc2(GTE_C_H, (unsigned)rrange(200, 1000));
    } else {
        for (i = 0; i < 5; i++) gte_ctc2(GTE_C_R11R12 + i, rnd());
        for (i = 0; i < 3; i++) gte_ctc2(GTE_C_TRX + i, (unsigned)rrange(-300000, 300000));
        gte_ctc2(GTE_C_H, (unsigned)rrange(1, 2000));
    }
    gte_ctc2(GTE_C_OFX, (unsigned)rrange(-100, 400) << 16); gte_ctc2(GTE_C_OFY, (unsigned)rrange(-100, 300) << 16);
    gte_ctc2(GTE_C_DQA, (unsigned)rrange(-9000, 9000)); gte_ctc2(GTE_C_DQB, rnd());
    gte_ctc2(GTE_C_ZSF3, (unsigned)rrange(0, 700)); gte_ctc2(GTE_C_ZSF4, (unsigned)rrange(0, 500));
    for (i = 0; i < 6; i++) gte_mtc2(i, rnd());
}
static void identity_gte_rot(int scale) {        /* a plausible camera: near-identity rotation */
    gte_ctc2(GTE_C_R11R12, (unsigned)scale & 0xffff);
    gte_ctc2(GTE_C_R13R21, 0); gte_ctc2(GTE_C_R22R23, (unsigned)scale & 0xffff); gte_ctc2(GTE_C_R31R32, 0); gte_ctc2(GTE_C_R33, (unsigned)scale);
}

#define BUF0 0x801e0000u
#define BUF1 0x801e2000u
#define THREADS 0x801e4000u           /* 16 thread records of 0x400 bytes */
#define G_THREADS_PTR 0x80165f98u     /* g_battle_threads */
#define G_THREAD_ID 0x80174038u       /* g_battle_current_thread_id */

extern void battle_copy_bytes(void* destination, const void* source, int count);
extern const unsigned char* battle_find_text_id_location(const unsigned char* text, int entry_index);
extern int battle_thread_get_current_parameter_1(void);
extern int battle_thread_get_current_parameter_2(void);
extern int battle_thread_get_current_parameter_3(void);
extern int battle_thread_is_previous_running(void);
extern int battle_thread_is_running_8014cc94(int thread_id);
extern int battle_mul_div_s64(int a, int b, int c);
extern int battle_fixed_cross_product_q12(int a, int b, int c, int d);
extern void battle_clear_menu_render_buffer(void* buffer, int bytes);
#define G_OVERFLOW 0x80173f58u
/* compare one extra word (outside the standard compared regions) after trial_end */
static void extra_cmp(const char* what, unsigned x, unsigned y, unsigned where) {
    if (x != y) { t_mem++; total_bad++; report(what, x, y, where); }
}

int main_test(void) {
    int t, k;
    const int N = 4000;
    unsigned args[10], rn;

    begin("battle_copy_bytes");
    for (t = 0; t < N; t++) {
        int count = 1 + (int)(rnd() % 300), so = (int)(rnd() % 64), dof = (int)(rnd() % 64);
        int overlap = (t % 5) == 0;                                       /* forward copy with overlap smears bytes; both must agree */
        fill_random(BUF0, 1024); fill_random(BUF1, 1024);
        random_gte(); trial_start();
        if (overlap) battle_copy_bytes((void*)(BUF0 + dof), (const void*)(BUF0 + so), count);
        else battle_copy_bytes((void*)(BUF1 + dof), (const void*)(BUF0 + so), count);
        native_done();
        args[0] = overlap ? BUF0 + dof : BUF1 + dof; args[1] = BUF0 + so; args[2] = (unsigned)count;
        interp_call("battle_copy_bytes", args, 3); trial_end(0, 0, 0);
    }
    end_group();

    begin("battle_find_text_id_location");
    for (t = 0; t < N; t++) {
        int i, terminators = 0, entry;
        unsigned char* p = (unsigned char*)BUF0;
        for (i = 0; i < 600; i++) { p[i] = (unsigned char)rnd(); if ((rnd() & 7) == 0) p[i] = (unsigned char)(0xfe | (rnd() & 1)); if ((p[i] & 0xfe) == 0xfe) terminators++; }
        for (i = 600; i < 1024; i++) p[i] = (unsigned char)(0xfe | (rnd() & 1));       /* guarantee the search terminates */
        entry = (int)(rnd() % (unsigned)(terminators + 1));
        random_gte(); trial_start();
        rn = (unsigned)battle_find_text_id_location((const unsigned char*)BUF0, entry); native_done();
        args[0] = BUF0; args[1] = (unsigned)entry; interp_call("battle_find_text_id_location", args, 2); trial_end(rn, 1, 0);
    }
    end_group();

    begin("battle_thread_get_current_parameter_1..3");
    for (t = 0; t < N; t++) {
        int id = 1 + (int)(rnd() % 15);
        fill_random(THREADS, 16 * 0x400);
        *(unsigned*)G_THREADS_PTR = THREADS; *(int*)G_THREAD_ID = id; copy_in(G_THREADS_PTR, G_THREADS_PTR + 4); copy_in(G_THREAD_ID, G_THREAD_ID + 4);
        random_gte(); trial_start();
        rn = (unsigned)battle_thread_get_current_parameter_1(); native_done(); interp_call("battle_thread_get_current_parameter_1", args, 0); trial_end(rn, 1, 0);
        trial_start();
        rn = (unsigned)battle_thread_get_current_parameter_2(); native_done(); interp_call("battle_thread_get_current_parameter_2", args, 0); trial_end(rn, 1, 0);
        trial_start();
        rn = (unsigned)battle_thread_get_current_parameter_3(); native_done(); interp_call("battle_thread_get_current_parameter_3", args, 0); trial_end(rn, 1, 0);
    }
    end_group();

    begin("battle_thread_is_previous_running / is_running_8014cc94");
    for (t = 0; t < N; t++) {
        int id = 1 + (int)(rnd() % 15);
        fill_random(THREADS, 16 * 0x400);
        *(unsigned*)G_THREADS_PTR = THREADS; *(int*)G_THREAD_ID = id; copy_in(G_THREADS_PTR, G_THREADS_PTR + 4); copy_in(G_THREAD_ID, G_THREAD_ID + 4);
        random_gte(); trial_start();
        rn = (unsigned)battle_thread_is_previous_running(); native_done(); interp_call("battle_thread_is_previous_running", args, 0); trial_end(rn, 1, 0);
        args[0] = (unsigned)(rnd() % 16);
        trial_start();
        rn = (unsigned)battle_thread_is_running_8014cc94((int)args[0]); native_done(); interp_call("battle_thread_is_running_8014cc94", args, 1); trial_end(rn, 1, 0);
    }
    end_group();
    begin("battle_mul_div_s64");
    for (t = 0; t < 20000; t++) {
        int a, b, c;
        unsigned m = rnd() % 8;
        a = m < 3 ? rrange(-100, 100) : (m < 6 ? rrange(-100000, 100000) : rs32());
        b = m < 2 ? rrange(-100, 100) : (m < 5 ? rrange(-5000, 5000) : rs32());
        c = (rnd() % 16 == 0) ? 0 : ((rnd() & 1) ? rrange(-2000, 2000) : (rnd() % 4 ? rs32() : rrange(1, 64)));
        *(int*)G_OVERFLOW = 0; copy_in(G_OVERFLOW, G_OVERFLOW + 4);
        random_gte(); trial_start();
        rn = (unsigned)battle_mul_div_s64(a, b, c); native_done();
        args[0] = (unsigned)a; args[1] = (unsigned)b; args[2] = (unsigned)c;
        interp_call("battle_mul_div_s64", args, 3); trial_end(rn, 1, 0);
        extra_cmp("overflow flag", *(unsigned*)G_OVERFLOW, *(unsigned*)(interp_ram + (G_OVERFLOW & 0x1fffff)), G_OVERFLOW);
    }
    end_group();
    begin("battle_fixed_cross_product_q12");
    for (t = 0; t < 20000; t++) {
        int a = (t & 1) ? rrange(-20000, 20000) : rs32(), b = (t & 2) ? rrange(-20000, 20000) : rs32();
        int c = (t & 4) ? rrange(-20000, 20000) : rs32(), d = (t & 8) ? rrange(-20000, 20000) : rs32();
        *(int*)G_OVERFLOW = 0; copy_in(G_OVERFLOW, G_OVERFLOW + 4);
        random_gte(); trial_start();
        rn = (unsigned)battle_fixed_cross_product_q12(a, b, c, d); native_done();
        args[0] = (unsigned)a; args[1] = (unsigned)b; args[2] = (unsigned)c; args[3] = (unsigned)d;
        interp_call("battle_fixed_cross_product_q12", args, 4); trial_end(rn, 1, 0);
        extra_cmp("overflow flag", *(unsigned*)G_OVERFLOW, *(unsigned*)(interp_ram + (G_OVERFLOW & 0x1fffff)), G_OVERFLOW);
    }
    end_group();
    begin("battle_clear_menu_render_buffer");
    for (t = 0; t < 4000; t++) {
        int bytes = (int)(rnd() % 700);
        unsigned off = (rnd() % 16) * 4;                                  /* the routine needs a word-aligned buffer */
        fill_random(BUF0, 1024);
        random_gte(); trial_start();
        battle_clear_menu_render_buffer((void*)(BUF0 + off), bytes); native_done();
        args[0] = BUF0 + off; args[1] = (unsigned)bytes; interp_call("battle_clear_menu_render_buffer", args, 2); trial_end(0, 0, 0);
    }
    end_group();
    (void)k;
    return total_bad != 0;
}

void _start(void) {
    int bad;
    unsigned i;
    if (!map_ram()) { out("mmap FAILED\n"); sys3(1, 1, 0, 0); }
    load_file("/disc/SCUS_942.21", 0x8000f800);
    load_file("/disc/BATTLE.BIN", 0x80067000);
    for (i = 0x80000000u; i < 0x80200000u; i += 4) *(unsigned*)(interp_ram + (i & 0x1fffff)) = *(unsigned*)i;
    r3k_reset(&cpu, interp_ram);
    gte_reset();
    bad = main_test();
    out("== "); outnum(total_trials); out(" differential trials, "); outnum(total_bad); out(" with a difference\n");
    sys3(1, bad, 0, 0);
}
