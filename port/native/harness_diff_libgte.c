/* Differential test: the native libgte (software GTE + libgte_native.c + the plain-C members compiled from the repo)
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

#define A0 0x801e0000u
#define A1 0x801e0100u
#define A2 0x801e0200u
#define A3 0x801e0300u
#define B0 0x801e0400u
#define B1 0x801e0500u
#define B2 0x801e0600u

int main_test(void) {
    int t, k;
    const int N = 4000;
    unsigned args[10], rn;

    /* ---- geometry setters (the GTE control registers are the output) ---- */
    begin("SetGeomOffset");
    for (t = 0; t < N; t++) { int x = rs32() >> 12, y = rs32() >> 12; random_gte(); trial_start(); SetGeomOffset(x, y); native_done(); args[0] = x; args[1] = y; interp_call("SetGeomOffset", args, 2); trial_end(0, 0, 1); }
    end_group();
    begin("SetGeomScreen");
    for (t = 0; t < N; t++) { int h = rs32(); random_gte(); trial_start(); SetGeomScreen(h); native_done(); args[0] = h; interp_call("SetGeomScreen", args, 1); trial_end(0, 0, 1); }
    end_group();
    begin("ReadGeomScreen");
    for (t = 0; t < N; t++) { random_gte(); trial_start(); rn = (unsigned)ReadGeomScreen(); native_done(); interp_call("ReadGeomScreen", args, 0); trial_end(rn, 1, 1); }
    end_group();
    begin("InitGeom");
    for (t = 0; t < 200; t++) { random_gte(); trial_start(); InitGeom(); native_done(); interp_call("InitGeom", args, 0); trial_end(0, 0, 1); }
    end_group();
    begin("SetRotMatrix");
    for (t = 0; t < N; t++) { fill_random(A0, 32); random_gte(); trial_start(); SetRotMatrix((MATRIX*)nat(A0)); native_done(); args[0] = A0; interp_call("SetRotMatrix", args, 1); trial_end(0, 0, 1); }
    end_group();
    begin("SetTransMatrix");
    for (t = 0; t < N; t++) { fill_random(A0, 32); random_gte(); trial_start(); SetTransMatrix((MATRIX*)nat(A0)); native_done(); args[0] = A0; interp_call("SetTransMatrix", args, 1); trial_end(0, 0, 1); }
    end_group();
    begin("SetColorMatrix");
    for (t = 0; t < N; t++) { fill_random(A0, 32); random_gte(); trial_start(); SetColorMatrix((MATRIX*)nat(A0)); native_done(); args[0] = A0; interp_call("SetColorMatrix", args, 1); trial_end(0, 0, 1); }
    end_group();
    begin("SetLightMatrix");
    for (t = 0; t < N; t++) { fill_random(A0, 32); random_gte(); trial_start(); SetLightMatrix((MATRIX*)nat(A0)); native_done(); args[0] = A0; interp_call("SetLightMatrix", args, 1); trial_end(0, 0, 1); }
    end_group();
    begin("SetBackColor");
    for (t = 0; t < N; t++) { int r = rs32() >> 20, g = rs32() >> 20, b = rs32() >> 20; random_gte(); trial_start(); SetBackColor(r, g, b); native_done(); args[0] = r; args[1] = g; args[2] = b; interp_call("SetBackColor", args, 3); trial_end(0, 0, 1); }
    end_group();
    begin("SetFarColor");
    for (t = 0; t < N; t++) { int r = rs32() >> 20, g = rs32() >> 20, b = rs32() >> 20; random_gte(); trial_start(); SetFarColor(r, g, b); native_done(); args[0] = r; args[1] = g; args[2] = b; interp_call("SetFarColor", args, 3); trial_end(0, 0, 1); }
    end_group();
    begin("Lzc");
    for (t = 0; t < N; t++) { int v = rs32(); random_gte(); trial_start(); rn = (unsigned)Lzc(v); native_done(); args[0] = v; interp_call("Lzc", args, 1); trial_end(rn, 1, 0); }
    end_group();

    /* ---- vector transforms ---- */
    begin("RotTrans");
    for (t = 0; t < N; t++) {
        fill_random(A0, 16); fill_random(B0, 32); random_gte(); trial_start();
        rn = (unsigned)RotTrans((SVECTOR*)nat(A0), (VECTOR*)nat(B0), (long*)nat(B1)); native_done();
        args[0] = A0; args[1] = B0; args[2] = B1; interp_call("RotTrans", args, 3); trial_end(rn, 1, 1);
    }
    end_group();
    begin("RotTransSV");
    for (t = 0; t < N; t++) {
        fill_random(A0, 16); fill_random(B0, 32); random_gte(); trial_start();
        RotTransSV((SVECTOR*)nat(A0), (SVECTOR*)nat(B0), (s32*)nat(B1)); native_done();
        args[0] = A0; args[1] = B0; args[2] = B1; interp_call("RotTransSV", args, 3); trial_end(0, 0, 1);
    }
    end_group();
    begin("RotTransPers");
    for (t = 0; t < N; t++) {
        fill_random(A0, 16); fill_random(B0, 32); random_gte(); trial_start();
        rn = (unsigned)RotTransPers((SVECTOR*)nat(A0), (s32*)nat(B0), (s32*)nat(B1), (s32*)nat(B2)); native_done();
        args[0] = A0; args[1] = B0; args[2] = B1; args[3] = B2; interp_call("RotTransPers", args, 4); trial_end(rn, 1, 1);
    }
    end_group();
    begin("RotTransPers3");
    for (t = 0; t < N; t++) {
        fill_random(A0, 64); fill_random(B0, 64); random_gte(); trial_start();
        rn = (unsigned)RotTransPers3((SVECTOR*)nat(A0), (SVECTOR*)nat(A0 + 8), (SVECTOR*)nat(A0 + 16), (s32*)nat(B0), (s32*)nat(B0 + 4), (s32*)nat(B0 + 8), (s32*)nat(B0 + 12), (s32*)nat(B0 + 16)); native_done();
        args[0] = A0; args[1] = A0 + 8; args[2] = A0 + 16; args[3] = B0; args[4] = B0 + 4; args[5] = B0 + 8; args[6] = B0 + 12; args[7] = B0 + 16;
        interp_call("RotTransPers3", args, 8); trial_end(rn, 1, 1);
    }
    end_group();
    begin("RotTransPers4");
    for (t = 0; t < N; t++) {
        fill_random(A0, 64); fill_random(B0, 64); random_gte(); trial_start();
        rn = (unsigned)RotTransPers4((SVECTOR*)nat(A0), (SVECTOR*)nat(A0 + 8), (SVECTOR*)nat(A0 + 16), (SVECTOR*)nat(A0 + 24),
                                     (s32*)nat(B0), (s32*)nat(B0 + 4), (s32*)nat(B0 + 8), (s32*)nat(B0 + 12), (s32*)nat(B0 + 16), (s32*)nat(B0 + 20)); native_done();
        args[0] = A0; args[1] = A0 + 8; args[2] = A0 + 16; args[3] = A0 + 24; args[4] = B0; args[5] = B0 + 4; args[6] = B0 + 8; args[7] = B0 + 12; args[8] = B0 + 16; args[9] = B0 + 20;
        interp_call("RotTransPers4", args, 10); trial_end(rn, 1, 1);
    }
    end_group();
    begin("NormalClip");
    for (t = 0; t < N; t++) {
        unsigned a = rnd(), b = rnd(), c = rnd();
        random_gte(); trial_start(); rn = (unsigned)NormalClip((long)a, (long)b, (long)c); native_done();
        args[0] = a; args[1] = b; args[2] = c; interp_call("NormalClip", args, 3); trial_end(rn, 1, 1);
    }
    end_group();

    /* ---- matrices ---- */
    begin("MulMatrix0");
    for (t = 0; t < N; t++) {
        for (k = 0; k < 16; k++) { ((short*)nat(A0))[k] = (short)rs16(); ((short*)nat(A1))[k] = (short)rs16(); }
        fill_random(B0, 32); random_gte(); trial_start();
        MulMatrix0((MATRIX*)nat(A0), (MATRIX*)nat(A1), (MATRIX*)nat(B0)); native_done();
        args[0] = A0; args[1] = A1; args[2] = B0; interp_call("MulMatrix0", args, 3); trial_end(0, 0, 0);
    }
    end_group();
    begin("MulMatrix");
    for (t = 0; t < N; t++) {
        for (k = 0; k < 16; k++) { ((short*)nat(A0))[k] = (short)rs16(); ((short*)nat(A1))[k] = (short)rs16(); }
        random_gte(); trial_start();
        MulMatrix((MATRIX*)nat(A0), (MATRIX*)nat(A1)); native_done();
        args[0] = A0; args[1] = A1; interp_call("MulMatrix", args, 2); trial_end(0, 0, 0);
    }
    end_group();
    begin("MulMatrix2");
    for (t = 0; t < N; t++) {
        for (k = 0; k < 16; k++) { ((short*)nat(A0))[k] = (short)rs16(); ((short*)nat(A1))[k] = (short)rs16(); }
        random_gte(); trial_start();
        MulMatrix2((MATRIX*)nat(A0), (MATRIX*)nat(A1)); native_done();
        args[0] = A0; args[1] = A1; interp_call("MulMatrix2", args, 2); trial_end(0, 0, 0);
    }
    end_group();
    begin("TransMatrix");
    for (t = 0; t < N; t++) {
        fill_random(A0, 32); fill_random(A1, 16); random_gte(); trial_start();
        TransMatrix((MATRIX*)nat(A0), (VECTOR*)nat(A1)); native_done();
        args[0] = A0; args[1] = A1; interp_call("TransMatrix", args, 2); trial_end(0, 0, 0);
    }
    end_group();
    begin("ScaleMatrix");
    for (t = 0; t < N; t++) {
        fill_random(A0, 32); fill_random(A1, 16); random_gte(); trial_start();
        ScaleMatrix((MATRIX*)nat(A0), (VECTOR*)nat(A1)); native_done();
        args[0] = A0; args[1] = A1; interp_call("ScaleMatrix", args, 2); trial_end(0, 0, 0);
    }
    end_group();
    begin("ScaleMatrixL");
    for (t = 0; t < N; t++) {
        fill_random(A0, 32); fill_random(A1, 16); random_gte(); trial_start();
        ScaleMatrixL((MATRIX*)nat(A0), (VECTOR*)nat(A1)); native_done();
        args[0] = A0; args[1] = A1; interp_call("ScaleMatrixL", args, 2); trial_end(0, 0, 0);
    }
    end_group();
    begin("PushMatrix/PopMatrix");
    for (t = 0; t < N; t++) {
        int depth = rrange(1, 5), d;
        unsigned v1[5], v2[5];
        for (d = 0; d < 5; d++) { v1[d] = rnd(); v2[d] = rnd(); }
        random_gte(); *(int*)nat(0x8002bbe4u) = rrange(0, 8) * 32; trial_start();
        for (d = 0; d < depth; d++) PushMatrix();
        for (d = 0; d < depth; d++) { gte_ctc2(GTE_C_R11R12, v1[d]); gte_ctc2(GTE_C_TRX, v2[d]); if (d & 1) PopMatrix(); }
        native_done();
        for (d = 0; d < depth; d++) interp_call("PushMatrix", args, 0);
        for (d = 0; d < depth; d++) { gte_ctc2(GTE_C_R11R12, v1[d]); gte_ctc2(GTE_C_TRX, v2[d]); if (d & 1) interp_call("PopMatrix", args, 0); }
        trial_end(0, 0, 1);
    }
    end_group();
    begin("ApplyMatrixLV");
    for (t = 0; t < N; t++) {
        for (k = 0; k < 16; k++) ((short*)nat(A0))[k] = (short)rs16();
        ((int*)nat(A1))[0] = rs32(); ((int*)nat(A1))[1] = rs32(); ((int*)nat(A1))[2] = rs32(); ((int*)nat(A1))[3] = rs32();
        fill_random(B0, 16); random_gte(); trial_start();
        rn = (unsigned)ApplyMatrixLV((MATRIX*)nat(A0), (VECTOR*)nat(A1), (VECTOR*)nat(B0)); native_done();
        args[0] = A0; args[1] = A1; args[2] = B0; interp_call("ApplyMatrixLV", args, 3); trial_end(rn, 1, 0);
    }
    end_group();
    begin("RotMatrix");
    for (t = 0; t < N; t++) {
        for (k = 0; k < 8; k++) ((short*)nat(A0))[k] = (short)(t & 1 ? rs16() : (int)(rnd() % 8192) - 4096);
        fill_random(B0, 32); random_gte(); trial_start();
        RotMatrix((SVECTOR*)nat(A0), (MATRIX*)nat(B0)); native_done();
        args[0] = A0; args[1] = B0; interp_call("RotMatrix", args, 2); trial_end(0, 0, 0);
    }
    end_group();
    begin("VectorNormal");
    for (t = 0; t < N; t++) {
        ((int*)nat(A0))[0] = rrange(-26000, 26000) >> (rnd() % 12); ((int*)nat(A0))[1] = rrange(-26000, 26000) >> (rnd() % 12);
        ((int*)nat(A0))[2] = rrange(-26000, 26000) >> (rnd() % 12); ((int*)nat(A0))[3] = 0;
        fill_random(B0, 16); random_gte(); trial_start();
        VectorNormal((VECTOR*)nat(A0), (VECTOR*)nat(B0)); native_done();
        args[0] = A0; args[1] = B0; interp_call("VectorNormal", args, 2); trial_end(0, 0, 0);
    }
    end_group();
    begin("SquareRoot0");
    for (t = 0; t < N; t++) { int v = rs32(); random_gte(); trial_start(); rn = (unsigned)SquareRoot0(v); native_done(); args[0] = v; interp_call("SquareRoot0", args, 1); trial_end(rn, 1, 0); }
    end_group();
    begin("SquareRoot12");
    for (t = 0; t < N; t++) { int v = rs32(); random_gte(); trial_start(); rn = (unsigned)SquareRoot12(v); native_done(); args[0] = v; interp_call("SquareRoot12", args, 1); trial_end(rn, 1, 0); }
    end_group();

    /* ---- the plain-C members: native compile of the repo's source vs the original code ---- */
    begin("rsin");
    for (t = 0; t < N; t++) { int v = rs32() >> (rnd() % 20); random_gte(); trial_start(); rn = (unsigned)rsin(v); native_done(); args[0] = v; interp_call("rsin", args, 1); trial_end(rn, 1, 0); }
    end_group();
    begin("rcos");
    for (t = 0; t < N; t++) { int v = rs32() >> (rnd() % 20); random_gte(); trial_start(); rn = (unsigned)rcos(v); native_done(); args[0] = v; interp_call("rcos", args, 1); trial_end(rn, 1, 0); }
    end_group();
    begin("ratan2");
    for (t = 0; t < N; t++) { int y = rs32() >> (rnd() % 24), x = rs32() >> (rnd() % 24); random_gte(); trial_start(); rn = (unsigned)ratan2(y, x); native_done(); args[0] = y; args[1] = x; interp_call("ratan2", args, 2); trial_end(rn, 1, 0); }
    end_group();
    begin("csqrt");
    for (t = 0; t < N; t++) { int v = rs32() >> (rnd() % 24); if (v < 0) v = -v; random_gte(); trial_start(); rn = (unsigned)csqrt(v); native_done(); args[0] = v; interp_call("csqrt", args, 1); trial_end(rn, 1, 0); }
    end_group();
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
