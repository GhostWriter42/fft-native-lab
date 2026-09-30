/* Generic function-level differential fuzzer: every decompiled game function (src/main + src/battle) whose signature is
 * simple enough, native build vs the ORIGINAL machine code (R3000 interpreter), on random arguments.
 *
 * A trial: random scalar arguments and pointer arguments that point at random-filled buffers in a scratch window of the RAM
 * image (some words look like pointers into that window, so pointer-chasing code gets somewhere to go); the real disc image
 * supplies every other global. The original code runs first on the interpreter. Trials where it touched hardware, called an
 * SDK routine, read its own machine code as data, used an address a native build cannot reach, or hung are skipped (the
 * random state meant nothing). Otherwise a forked child runs the native function on its private copy-on-write RAM and compares
 * the return value and every data byte of RAM with the interpreter's result. Native crashes are caught by the parent.
 * Freestanding -m32; build with fuzz.ps1. */
#include "gte.h"
#include "r3000/r3000.h"

#ifndef TRIALS
#define TRIALS 40
#endif
#ifndef MAX_STEPS
#define MAX_STEPS 300000
#endif
#ifndef FROM
#define FROM 0
#endif
#ifndef TO
#define TO 100000
#endif

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
static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* libc / BIOS services the native game code links against: the same LCG the BIOS (and the interpreter) use.
 * PS1-address scheme: names that are functions in the game's yaml (rand, srand, bzero, strlen, memset, memmove) are defined
 * as native_<name>; the game's references to the plain names resolve to their PS1 addresses and reach these through the
 * trampolines. `abs` is not in the yaml and keeps its plain name. */
static unsigned g_native_seed = 1, g_native_rand_calls;
int abs(int x) { return x < 0 ? -x : x; }
int native_rand(void) { g_native_rand_calls++; g_native_seed = g_native_seed * 1103515245u + 12345u; return (int)((g_native_seed >> 16) & 0x7fff); }
void native_srand(unsigned seed) { g_native_seed = seed; }
void native_bzero(void* d, int n) { unsigned char* p = (unsigned char*)d; while (n-- > 0) *p++ = 0; }
int native_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
extern unsigned g_stub_calls;             /* incremented by every generated stub (an SDK function the native build does not provide) */

/* ------------------------------------------------------------------------------------------------- tables */
struct fuzz_fn { const char* name; unsigned int addr; unsigned int size; void* nat; unsigned char ret, nargs, kind[10]; };
extern const struct fuzz_fn g_fuzz_fns[];
extern const int g_fuzz_fn_count;
struct sym { unsigned int addr; const char* name; };
extern const struct sym g_syms[];
extern const int g_sym_count;
struct sdk_range { unsigned int lo, hi; const char* id; };
extern const struct sdk_range g_sdk_ranges[];
extern const int g_sdk_range_count;
struct native_stub { unsigned int ps1_addr; unsigned int size; void* native; };
extern const struct native_stub g_native_stubs[];
extern const int g_native_stub_count;
struct orig_func { const char* name; unsigned int addr; unsigned int size; };
extern const struct orig_func g_orig_funcs[];
extern const int g_orig_func_count;

enum { K_S8 = 1, K_U8, K_S16, K_U16, K_S32, K_U32, K_PTR };

extern const unsigned int g_ptr_globals[];
extern const int g_ptr_global_count;
struct scalar_global { unsigned int addr; unsigned char kind; };
extern const struct scalar_global g_scalar_globals[];
extern const int g_scalar_global_count;

/* ------------------------------------------------------------------------------------------------- random */
static unsigned g_rng = 2463534242u;
static unsigned rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

#define WIN_LO 0x801e0000u                       /* scratch window: argument buffers + targets for pointer-like words */
#define WIN_HI 0x801f0000u                       /* [0x801f0000, 0x80200000) is the interpreter's stack: never compared */
#define BUF_STRIDE 0x400u                        /* argument buffers: 10 x 1 KiB at the start of the window */
#define BUF_WORDS_LIMIT 0x10000u                 /* pointer-like words point anywhere into the 64 KiB window */
#define POOL_LO (WIN_LO + 10 * BUF_STRIDE)       /* pointees of the seeded pointer globals: 256 bytes each */
#define POOL_STRIDE 0x100u

/* ---------------------------------------------------------------------------- the two machines and their RAM */
static unsigned char interp_ram[0x200000];
static r3k_t cpu;
struct result { unsigned ndiff, marker, addr[40], nat[40], orig[40], fault_addr, fault_eip, fault_sig, ret_native, stub_calls, rand_calls, trace_on, trace_n, trace[1000], trace_hash[1000]; };
static struct result* shared;
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
        if (cpu.ncode && f->ps1_addr <= cpu.code_hi[cpu.ncode - 1] + 16) { if (f->ps1_addr + f->size > cpu.code_hi[cpu.ncode - 1]) cpu.code_hi[cpu.ncode - 1] = f->ps1_addr + f->size; }   /* adjacent or overlapping: merge */
        else if (cpu.ncode < 4096) { cpu.code_lo[cpu.ncode] = f->ps1_addr; cpu.code_hi[cpu.ncode] = f->ps1_addr + f->size; cpu.ncode++; }
        if (f->ps1_addr + f->size > cursor) cursor = f->ps1_addr + f->size;
        if (f->native && f->size >= 5) {
            unsigned char* p = (unsigned char*)f->ps1_addr;
            p[0] = 0xe9; *(unsigned*)(p + 1) = (unsigned)f->native - (f->ps1_addr + 5);
            n_installed++;
        }
    }
    if (cursor < 0x801f0000u) { data_lo[ndata] = cursor; data_hi[ndata] = 0x801f0000u; ndata++; }
}
static unsigned char pristine_ram[0x200000];               /* the original code image: restored every trial (a wild write must not persist) */
static void copy_ram_to_interp(void) {
    int i;
    for (i = 0; i < (int)cpu.ncode; i++) {
        unsigned lo = cpu.code_lo[i] & 0x1fffff, n = (cpu.code_hi[i] - cpu.code_lo[i] + 3) / 4;
        const void* s = pristine_ram + lo; void* d = interp_ram + lo;
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
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
/* RAM words that legitimately differ: the retail thread switch saves s0-s7/k0/k1/gp/sp/fp/ra into each thread record (+0x10..+0x47),
 * the native scheduler keeps its machine context outside RAM (see replacements/battle_thread.c); the call-on-main-stack routine
 * parks $ra in a word after its own code. */
extern char g_battle_thread_contexts[];
static int __attribute__((no_instrument_function)) ignored_word(unsigned a) {
    unsigned base = (unsigned)g_battle_thread_contexts;
    if (a >= base && a < base + 16 * 0x400) { unsigned off = (a - base) & 0x3ff; if (off >= 0x10 && off < 0x48) return 1; }
    return a == 0x8014cf5cu;
}
static void fault_handler(int sig, void* info, void* uc) {
    shared->fault_sig = (unsigned)sig;
    shared->fault_addr = *(unsigned*)((char*)info + 12);
    shared->fault_eip = *(unsigned*)((char*)uc + 76);
    shared->marker = 0xdeadfa17u;
    sys3(1, 0, 0, 0);
}
struct ksigaction { void (*handler)(int, void*, void*); unsigned long flags; void (*restorer)(void); unsigned long long mask; };
static void install_fault_handlers(void) {
    struct ksigaction sa;
    sa.handler = fault_handler; sa.flags = 4 | 0x40000000; sa.restorer = 0; sa.mask = 0;
    sys4(174, 11, (long)&sa, 0, 8); sys4(174, 8, (long)&sa, 0, 8); sys4(174, 7, (long)&sa, 0, 8); sys4(174, 4, (long)&sa, 0, 8);
}

/* ------------------------------------------------------------------------------------------------- reports */
static void label(unsigned a) {
    int lo = 0, hi = g_sym_count, i;
    if (a >= WIN_LO && a < WIN_LO + 10 * BUF_STRIDE) { out("buffer["); outnum((a - WIN_LO) / BUF_STRIDE); out("]+0x"); outhex((a - WIN_LO) % BUF_STRIDE); return; }
    if (a >= POOL_LO && a < POOL_LO + (unsigned)g_ptr_global_count * POOL_STRIDE) { out("pointee["); outnum((a - POOL_LO) / POOL_STRIDE); out("]+0x"); outhex((a - POOL_LO) % POOL_STRIDE); return; }
    while (lo < hi) { int mid = (lo + hi) / 2; if (g_syms[mid].addr <= a) lo = mid + 1; else hi = mid; }
    i = lo - 1;
    if (i >= 0 && a - g_syms[i].addr < 0x4000) { out(g_syms[i].name); out("+0x"); outhex(a - g_syms[i].addr); return; }
    outhex(a);
}
static void func_at(unsigned pc) {
    int i;
    for (i = 0; i < g_orig_func_count; i++)
        if (pc >= g_orig_funcs[i].addr && pc < g_orig_funcs[i].addr + g_orig_funcs[i].size) { out(g_orig_funcs[i].name); out("+0x"); outhex(pc - g_orig_funcs[i].addr); return; }
    outhex(pc);
}

/* -------------------------------------------------------------------------------------------- trial generation */
static unsigned gen_scalar(int kind) {
    unsigned m = rnd() % 16, v;
    if (m < 5) v = rnd() % 4;
    else if (m < 8) v = rnd() % 64;
    else if (m < 10) v = rnd() % 1024;
    else if (m < 12) v = rnd() & 0xffff;
    else if (m < 13) v = 0u - (rnd() % 4);
    else v = rnd();
    switch (kind) {
    case K_S8: return (unsigned)(int)(signed char)v;
    case K_U8: return v & 0xff;
    case K_S16: return (unsigned)(int)(short)v;
    case K_U16: return v & 0xffff;
    default: return v;
    }
}
static void fill_buffer(unsigned addr) {
    unsigned i;
    for (i = 0; i < BUF_STRIDE; i += 4) {
        unsigned m = rnd() & 3, v;
        if (m == 0) v = WIN_LO + ((rnd() % BUF_WORDS_LIMIT) & ~3u);              /* looks like a pointer into the window */
        else if (m == 1) v = rnd() & 0xff;
        else v = rnd();
        *(unsigned*)(addr + i) = v;
    }
}
static void random_gte(void) {
    int i;
    gte_reset();
    if (rnd() & 1) {
        gte_ctc2(GTE_C_R11R12, 4096); gte_ctc2(GTE_C_R22R23, 4096); gte_ctc2(GTE_C_R33, 4096);
        for (i = 0; i < 3; i++) gte_ctc2(GTE_C_TRX + i, rnd() % 4000);
        gte_ctc2(GTE_C_H, 200 + rnd() % 800);
    } else {
        for (i = 0; i < 5; i++) gte_ctc2(GTE_C_R11R12 + i, rnd());
        for (i = 0; i < 3; i++) gte_ctc2(GTE_C_TRX + i, rnd() % 200000);
        gte_ctc2(GTE_C_H, 1 + rnd() % 2000);
    }
    gte_ctc2(GTE_C_OFX, (rnd() % 400) << 16); gte_ctc2(GTE_C_OFY, (rnd() % 300) << 16);
    for (i = 0; i < 6; i++) gte_mtc2(i, rnd());
}
static unsigned mask_ret(int kind, unsigned v) {
    switch (kind) {
    case K_S8: return (unsigned)(int)(signed char)v;
    case K_U8: return v & 0xff;
    case K_S16: return (unsigned)(int)(short)v;
    case K_U16: return v & 0xffff;
    default: return v;
    }
}

typedef unsigned (*gen10_t)(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);

/* Every trial starts from the pristine disc image (data ranges only: the code ranges hold the trampolines). */
static void restore_native_data(void) {
    int i;
    for (i = 0; i < ndata; i++) {
        unsigned n = (data_hi[i] - data_lo[i]) / 4;
        void* d = (void*)data_lo[i]; const void* src = pristine_ram + (data_lo[i] & 0x1fffff);
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    }
}

static void seed_globals(void) {
    int i;
    for (i = 0; i < g_ptr_global_count; i++) {                                    /* pointer globals -> their own random pointee buffer */
        unsigned pool = POOL_LO + (unsigned)i * POOL_STRIDE, k;
        for (k = 0; k < POOL_STRIDE; k += 4) {
            unsigned m = rnd() & 3;
            *(unsigned*)(pool + k) = m == 0 ? WIN_LO + ((rnd() % BUF_WORDS_LIMIT) & ~3u) : (m == 1 ? (rnd() & 0xff) : rnd());
        }
        if (rnd() % 12) *(unsigned*)g_ptr_globals[i] = pool + (rnd() % 4) * 4;
    }
    for (i = 0; i < g_scalar_global_count; i++) {                                  /* a few scalar globals -> random values */
        if (rnd() % 8 == 0) {
            unsigned v = gen_scalar(g_scalar_globals[i].kind), a = g_scalar_globals[i].addr;
            if (g_scalar_globals[i].kind == K_S8 || g_scalar_globals[i].kind == K_U8) *(unsigned char*)a = (unsigned char)v;
            else if (g_scalar_globals[i].kind == K_S16 || g_scalar_globals[i].kind == K_U16) *(unsigned short*)a = (unsigned short)v;
            else *(unsigned*)a = v;
        }
    }
}

static void setup_trial(int fi, int t, unsigned* args) {
    const struct fuzz_fn* f = &g_fuzz_fns[fi];
    int i;
    g_rng = ((unsigned)(fi + 1) * 2654435761u) ^ ((unsigned)(t + 1) * 2246822519u); g_rng |= 1; rnd(); rnd(); rnd();       /* replayable from (function, trial) */
    restore_native_data();
    for (i = 0; i < 10; i++) args[i] = 0;
    for (i = 0; i < f->nargs; i++) {
        if (f->kind[i] == K_PTR) {
            unsigned base = WIN_LO + (unsigned)i * BUF_STRIDE;
            fill_buffer(base);
            args[i] = (rnd() % 40 == 0) ? 0 : base + ((rnd() % 16) * 4);
        } else {
            args[i] = gen_scalar(f->kind[i]);
        }
    }
    random_gte();
    seed_globals();
}

#if defined(ONLY_FN) || defined(REPLAY_LIST_FILE)
#include "harness_fuzz_replay.h"
#endif

/* ------------------------------------------------------------------------------------------------- the run */
/* Functions whose result IS a CPU register ($sp / $gp): meaningless natively, not a divergence. */
static int known_incompatible(const char* n) {
    static const char* const list[] = { "main_system_store_stack_pointer", "main_restore_game_loop_stack_pointer", "battle_thread_get_current_global_pointer",
                                        "world_thread_get_current_global_pointer", 0 };
    int i;
    for (i = 0; list[i]; i++) if (streq(n, list[i])) return 1;
    return 0;
}

extern const char* const g_unspecified_ret[];          /* functions that can fall off the end of a non-void function (unspecified result) */
static int unspecified_ret(const char* n) {
    int i;
    for (i = 0; g_unspecified_ret[i]; i++) if (streq(n, g_unspecified_ret[i])) return 1;
    return 0;
}

int main_test(void) {
    int fi, t, total_fn = 0, tested_fn = 0, untested_fn = 0, nonnative = 0, bad_fn = 0, known_skipped = 0;
    long total_cmp = 0, total_bad = 0, total_skip = 0, total_unspec = 0;
    static gte_state_t gte_before;
#ifdef ONLY_FN
    replay_one(ONLY_FN, ONLY_TRIAL);
    return 0;
#endif
#ifdef REPLAY_LIST_FILE
    {
        static const int rl[][2] = {
#include "replay_list.h"
        };
        int q;
        for (q = 0; q < (int)(sizeof rl / sizeof rl[0]); q++) { out("=========================================================================
"); replay_one(rl[q][0], rl[q][1]); }
        return 0;
    }
#endif
    for (fi = FROM; fi < g_fuzz_fn_count && fi < TO; fi++) {
        const struct fuzz_fn* f = &g_fuzz_fns[fi];
        int cmp = 0, mism = 0, crashes = 0, div_traps = 0, stub_paths = 0, shown = 0, unspec = 0;
        int sk_fault = 0, sk_io = 0, sk_code = 0, sk_wild = 0, sk_sdk = 0;
        unsigned crash_eip = 0, crash_addr = 0, crash_sig = 0;
        total_fn++;
        if (!f->nat) { nonnative++; continue; }
        if (known_incompatible(f->name)) { known_skipped++; continue; }
        for (t = 0; t < TRIALS; t++) {
            unsigned args[10];
            long pid, status = 0;
            int rc, interp_div;
            unsigned ret_i;
            setup_trial(fi, t, args);
            gte_before = g_gte;
            copy_ram_to_interp();
            cpu.io_reads = cpu.io_writes = 0; cpu.code_reads = cpu.code_writes = 0; cpu.wild = 0; cpu.sdk_hits = 0; cpu.div_zero = cpu.div_overflow = 0;
            cpu.rand_calls = 0; cpu.wlog_n = 0; cpu.watch_lo = cpu.watch_hi = 0; cpu.bios_rand_seed = g_rng;
#ifdef WATCH_ADDR
            out("      watch: native "); outhex(*(unsigned*)WATCH_ADDR); out(" interp "); outhex(*(unsigned*)(interp_ram + (WATCH_ADDR & 0x1fffff))); out("\n");
#endif
            { int q; unsigned n = 0x10000 / 4; void* d = interp_ram + 0x1f0000;                    /* the whole stack region, so copies that run past the window read zeros on both machines */ for (q = 1; q < 32; q++) cpu.r[q] = 0; cpu.hi = cpu.lo = 0;
      __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(0) : "memory"); }   /* no stale registers or stack: every trial starts clean */
            rc = r3k_call(&cpu, f->addr, args, f->nargs, MAX_STEPS);
            g_gte = gte_before;
#ifdef VERBOSE
            out("      trial "); outnum(t); out(": interp rc "); outnum(rc); out(" steps "); outnum((long)cpu.steps); out(" io "); outnum(cpu.io_reads + cpu.io_writes); out(" code "); outnum(cpu.code_reads); out(" wild "); outnum(cpu.wild); out(" sdk "); outnum(cpu.sdk_hits); out(" ret "); outhex(cpu.r[2]); out("\n");
#endif
            if (rc) { sk_fault++; continue; }
            if (cpu.io_reads || cpu.io_writes) { sk_io++; continue; }
            if (cpu.code_reads || cpu.code_writes) { sk_code++; continue; }
            if (cpu.wild) { sk_wild++; continue; }
            if (cpu.sdk_hits) { sk_sdk++; continue; }
            interp_div = (int)(cpu.div_zero + cpu.div_overflow);
            ret_i = mask_ret(f->ret, cpu.r[2]);
            shared->marker = 0; shared->ndiff = 0; shared->stub_calls = 0;
            g_native_seed = g_rng; g_native_rand_calls = 0; g_stub_calls = 0;
            pid = sys3(2, 0, 0, 0);
            if (pid == 0) {
                unsigned a, n = 0, r;
                int rg;
                sys3(27, 5, 0, 0);                                                /* alarm: a native infinite loop ends the child */
                r = ((gen10_t)f->nat)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]);
                shared->ret_native = mask_ret(f->ret, r);
                for (rg = 0; rg < ndata; rg++)
                    for (a = data_lo[rg]; a < data_hi[rg]; a += 4) {
                        unsigned x = *(unsigned*)a, y = *(unsigned*)(interp_ram + (a & 0x1fffff));
                        if (x != y && !ignored_word(a)) { if (n < 40) { shared->addr[n] = a; shared->nat[n] = x; shared->orig[n] = y; } n++; }
                    }
                shared->ndiff = n; shared->stub_calls = g_stub_calls; shared->marker = 0xd0d0d0d0u;
                sys3(1, 0, 0, 0);
            }
            sys4(114, pid, (long)&status, 0, 0);
            if (shared->marker == 0xdeadfa17u && shared->fault_sig == 8 && interp_div) { div_traps++; continue; }
            cmp++;
            if ((status & 0x7f) != 0 || shared->marker != 0xd0d0d0d0u) {
                crashes++;
                if (!crash_eip) { crash_sig = shared->marker == 0xdeadfa17u ? shared->fault_sig : (unsigned)(status & 0x7f); crash_eip = shared->fault_eip; crash_addr = shared->fault_addr; }
                continue;
            }
            if (shared->stub_calls) stub_paths++;
            if (!shared->ndiff && f->ret != 0 && shared->ret_native != ret_i && unspecified_ret(f->name)) { unspec++; continue; }   /* only the unspecified return value differs */
            if (shared->ndiff || (f->ret != 0 && shared->ret_native != ret_i)) {
                unsigned k;
                mism++;
                if (shown++ < 2) {
                    out("    ["); outnum(fi); out("] "); out(f->name); out(" trial "); outnum(t); out(": ");
                    if (f->ret != 0 && shared->ret_native != ret_i) { out("return native "); outhex(shared->ret_native); out(" original "); outhex(ret_i); out("; "); }
                    outnum(shared->ndiff); out(" RAM words differ");
                    for (k = 0; k < 3 && k < shared->ndiff; k++) { out("  "); label(shared->addr[k]); out(" native "); outhex(shared->nat[k]); out(" original "); outhex(shared->orig[k]); }
                    out("\n");
                    if (shared->ndiff) {                                          /* who wrote the first differing word in the original? */
                        setup_trial(fi, t, args); copy_ram_to_interp();
                        cpu.wlog_n = 0; cpu.watch_lo = shared->addr[0]; cpu.watch_hi = shared->addr[0] + 4; cpu.bios_rand_seed = g_rng;
                        r3k_call(&cpu, f->addr, args, f->nargs, MAX_STEPS);
                        for (k = 0; k < cpu.wlog_n && k < 4; k++) { out("        original wrote "); outhex(cpu.wlog_val[k]); out(" at "); outhex(cpu.wlog_addr[k]); out(" from "); func_at(cpu.wlog_pc[k]); out("\n"); }
                        cpu.watch_lo = cpu.watch_hi = 0;
                    }
                }
            }
        }
        total_cmp += cmp; total_bad += mism + crashes; total_skip += sk_fault + sk_io + sk_code + sk_wild + sk_sdk;
        if (cmp) tested_fn++; else untested_fn++;
        total_unspec += unspec;
        if (mism || crashes || stub_paths) {
            bad_fn++;
            out(mism || crashes ? "DIVERGES " : "STUB-PATH "); outnum(fi); out(" "); out(f->name); out(": "); outnum(cmp); out(" compared, "); outnum(mism); out(" mismatches, "); outnum(crashes); out(" native crashes");
            if (crashes) { out(" (signal "); outnum(crash_sig); out(" at eip "); outhex(crash_eip); out(" address "); outhex(crash_addr); out(")"); }
            if (stub_paths) { out(", "); outnum(stub_paths); out(" trials where the native code called an SDK stub"); }
            if (div_traps) { out(", "); outnum(div_traps); out(" divide traps"); }
            out("\n");
        }
    }
    out("== functions: "); outnum(total_fn); out(" considered, "); outnum(tested_fn); out(" compared at least once, "); outnum(untested_fn); out(" never got a usable trial, ");
    outnum(nonnative); out(" not linked natively, "); outnum(known_skipped); out(" known CPU-register functions skipped. Trials: "); outnum(total_cmp); out(" compared, "); outnum(total_skip); out(" skipped; ");
    outnum(bad_fn); out(" functions flagged ("); outnum(total_bad); out(" mismatching or crashing trials); ");
    outnum(total_unspec); out(" more trials differed only in the unspecified return value of a function that can fall off its end\n");
    return total_bad != 0;
}

void _start(void) {
    int bad, i;
    if (!map_ram()) { out("mmap FAILED\n"); sys3(1, 1, 0, 0); }
    shared = (struct result*)map_shared(16384);
    load_file("/disc/SCUS_942.21", 0x8000f800);
    load_file("/disc/BATTLE.BIN", 0x80067000);
    r3k_reset(&cpu, interp_ram);
    copy_ram_to_interp_all();
    { unsigned i; for (i = 0; i < 0x200000; i += 4) *(unsigned*)(pristine_ram + i) = *(unsigned*)(interp_ram + i); }
    build_ranges_and_install();
    for (i = 0; i < g_sdk_range_count && i < 16; i++) { cpu.sdk_lo[i] = g_sdk_ranges[i].lo; cpu.sdk_hi[i] = g_sdk_ranges[i].hi; cpu.nsdk = (unsigned)i + 1; }
    out("trampolines for "); outnum(n_installed); out(" native functions; "); outnum(g_fuzz_fn_count); out(" functions in the fuzz table; "); outnum(ndata); out(" data ranges compared\n");
    gte_reset();
    install_fault_handlers();
    bad = main_test();
    sys3(1, bad, 0, 0);
}
