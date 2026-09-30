/* Whole-program lockstep: the ORIGINAL game (R3000 interpreter) and the NATIVE build of the same game boot side by side from the
 * start of main(), each on its own RAM image with the same HLE SDK layer (hle/), and their RAM is compared at every VSync.
 *
 * Machine A (original): the interpreter runs __SN_ENTRY_POINT up to main(), then continues from there.
 * Machine B (native):   at main() the interpreter's RAM (data ranges) is copied into the native RAM image and the native game's
 *                       main() runs on its own coroutine stack; every native VSync(0) switches back here.
 * Then, per frame: run B to its next sync point, run A to its next sync point, compare all data RAM (see harness_fuzz.c for the
 * ranges and the words that are legitimately different). A sync point is a VSync(0) or a CD read that loads a code overlay (the
 * native side only knows the modules it was built with, so the comparison stops there). The first mismatching frame is reported
 * with labelled words.
 * Freestanding -m32; build/run with lockstep.ps1. */
#include "gte.h"
#include "r3000/r3000.h"
#include "hle/hle.h"

#ifndef MAX_FRAMES
#define MAX_FRAMES 60
#endif
#ifndef LOG_LIMIT
#define LOG_LIMIT 0
#endif
#ifndef STEP_BUDGET
#define STEP_BUDGET 400000000ull
#endif
#ifndef SHOW_DIFFS
#define SHOW_DIFFS 40
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
static int map_fixed(unsigned addr, unsigned len) { struct old_mmap_args a; a.addr = (long)addr; a.len = (long)len; a.prot = 7; a.flags = 0x32; a.fd = -1; a.off = 0; return sys3(90, (long)&a, 0, 0) == (long)addr; }
static long load_file(const char* path, unsigned char* dst) {
    long fd = sys3(5, (long)path, 0, 0), total = 0, n;
    if (fd < 0) return -1;
    while ((n = sys3(3, fd, (long)(dst + total), 1 << 20)) > 0) total += n;
    sys3(6, fd, 0, 0);
    return total;
}
static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

extern unsigned g_bios_rand_seed;                                            /* bios_rt.c: the native BIOS services (rand, memset, strlen, ...) */

/* ------------------------------------------------------------------------------------------------- tables */
struct mfn { unsigned int addr; unsigned int size; void* native; const char* name; };          /* gen_modules.py: one row per function of a module */
struct module { const char* name; const char* file; unsigned int lba, sectors, load; const struct mfn* fns; int nfns; };
extern const struct module g_modules[];                                     /* [0] = the main executable, then the overlays */
extern const int g_module_count;
struct sym { unsigned int addr; const char* name; };
extern const struct sym g_syms[];
extern const int g_sym_count;
struct hle_module_names { const char* module; const char* const* names; int count; const unsigned char* arity; };      /* gen_hle.py: the functions the HLE takes over, per module */
extern const struct hle_module_names g_hle_module_names[];
extern const int g_hle_module_name_count;
extern unsigned g_stub_calls;                                               /* generated with the stubs of the yaml functions that have no native definition */
extern const char* const g_stub_names[];
extern unsigned g_stub_hits[];
extern const int g_stub_count;

static int g_active[8], g_nactive;                                          /* the code overlays whose functions are installed (they occupy disjoint address ranges) */
static unsigned addr_in(int mod, const char* name) {
    int i;
    for (i = 0; i < g_modules[mod].nfns; i++) if (streq(g_modules[mod].fns[i].name, name)) return g_modules[mod].fns[i].addr;
    return 0;
}
static unsigned addr_of(const char* name) { return addr_in(0, name); }     /* main-executable functions (the SDK entry points and main) */
static const struct mfn* fn_at(unsigned pc, int module) {
    int i;
    for (i = 0; i < g_modules[module].nfns; i++) { const struct mfn* f = &g_modules[module].fns[i]; if (pc >= f->addr && pc < f->addr + f->size) return f; }
    return 0;
}
static void func_at(unsigned pc) {
    const struct mfn* f = fn_at(pc, 0);
    int i;
    for (i = 0; !f && i < g_nactive; i++) f = fn_at(pc, g_active[i]);
    if (f) { out(f->name); out("+0x"); outhex(pc - f->addr); return; }
    outhex(pc);
}
static void label(unsigned a) {
    int lo = 0, hi = g_sym_count, i;
    while (lo < hi) { int mid = (lo + hi) / 2; if (g_syms[mid].addr <= a) lo = mid + 1; else hi = mid; }
    i = lo - 1;
    if (i >= 0 && a - g_syms[i].addr < 0x4000) { out(g_syms[i].name); out("+0x"); outhex(a - g_syms[i].addr); return; }
    outhex(a);
}

/* ------------------------------------------------------------------------------- RAM ranges and trampolines */
static unsigned char interp_ram[0x200000];
static r3k_t cpu;
static unsigned data_lo[6000], data_hi[6000];
static int ndata;

/* Data ranges = everything below the stack area that is not the code of a loaded module (the main executable and the active overlay). */
static void build_ranges(void) {
    static unsigned idx[8192];
    int i, j, n = 0, m;
    unsigned cursor = 0x80000000u;
    static const struct mfn* tab[8192];                                     /* static: an auto-initialised local array would call memset before the trampolines exist */
    for (m = 0; m <= g_nactive; m++) {
        int mod = m == 0 ? 0 : g_active[m - 1];
        for (i = 0; i < g_modules[mod].nfns && n < 8192; i++) tab[n++] = &g_modules[mod].fns[i];
    }
    for (i = 0; i < n; i++) idx[i] = (unsigned)i;
    for (i = 1; i < n; i++) { unsigned v = idx[i]; for (j = i - 1; j >= 0 && tab[idx[j]]->addr > tab[v]->addr; j--) idx[j + 1] = idx[j]; idx[j + 1] = v; }
    ndata = 0;
    for (i = 0; i < n; i++) {
        const struct mfn* f = tab[idx[i]];
        if (f->addr >= 0x801f0000u) continue;
        if (f->addr > cursor) { data_lo[ndata] = cursor; data_hi[ndata] = f->addr; ndata++; }
        if (f->addr + f->size > cursor) cursor = f->addr + f->size;
    }
    if (cursor < 0x801f0000u) { data_lo[ndata] = cursor; data_hi[ndata] = 0x801f0000u; ndata++; }
}
/* x86 `jmp` trampolines at the PS1 addresses of a module's natively compiled functions; returns how many were installed */
static int install_module(int mod) {
    int i, n = 0;
    for (i = 0; i < g_modules[mod].nfns; i++) {
        const struct mfn* f = &g_modules[mod].fns[i];
        if (f->native && f->size >= 5) {
            unsigned char* p = (unsigned char*)f->addr;
            p[0] = 0xe9; *(unsigned*)(p + 1) = (unsigned)f->native - (f->addr + 5);
            n++;
        }
    }
    return n;
}
/* the code bytes of the module that is going away come back from the interpreter's RAM (which holds whatever the game put there) */
static void uninstall_module(int mod);
static unsigned char interp_ram_bytes(unsigned a);
static void uninstall_module(int mod) {
    int i, k;
    for (i = 0; i < g_modules[mod].nfns; i++) {
        const struct mfn* f = &g_modules[mod].fns[i];
        if (f->native && f->size >= 5) for (k = 0; k < 5; k++) ((unsigned char*)f->addr)[k] = interp_ram_bytes(f->addr + (unsigned)k);
    }
}
/* words that legitimately differ: the retail thread switch saves registers into the thread records, the native scheduler keeps them
 * outside RAM; battle_thread_call_on_main_stack and the C runtime keep a saved $ra in RAM */
extern char g_battle_thread_contexts[];
static int ignored_word(unsigned a) {
    unsigned base = (unsigned)g_battle_thread_contexts;
    if (a < 0x8000f800u) return 1;                             /* BIOS/kernel area: exception-vector patches by the SDK's GTE/card init (the interpreter fakes the kernel tables) */
    if (a >= base && a < base + 16 * 0x400) { unsigned off = (a - base) & 0x3ff; if (off >= 0x10 && off < 0x48) return 1; }
    switch (a) {
    case 0x80028ae4u:                                          /* g_psyq_crt_constructors_ran: set by __main, which the native main() does not call */
    case 0x8002b8a8u: case 0x8002bbd8u: case 0x800329bcu: case 0x800329c0u: case 0x800329d0u: case 0x800329e0u:   /* g_psyq_*_saved_ra: return addresses saved by hand-written SDK code */
    case 0x8014cf5cu:                                          /* battle_thread_call_on_main_stack's saved $ra */
        return 1;
    }
    return 0;
}
static void copy_data_interp_to_native(void) {
    int i;
    for (i = 0; i < ndata; i++) {
        unsigned n = (data_hi[i] - data_lo[i]) / 4;
        const void* s = interp_ram + (data_lo[i] & 0x1fffff); void* d = (void*)data_lo[i];
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
}
static int compare_ram(int show) {
    int r, diffs = 0;
    unsigned a;
    for (r = 0; r < ndata; r++)
        for (a = data_lo[r]; a < data_hi[r]; a += 4) {
            unsigned x = *(unsigned*)a, y = *(unsigned*)(interp_ram + (a & 0x1fffff));
            if (x != y && !ignored_word(a)) {
                if (show && diffs < show) { out("    "); label(a); out("  native "); outhex(x); out("  original "); outhex(y); out("\n"); }
                diffs++;
            }
        }
    return diffs;
}
static unsigned char interp_ram_bytes(unsigned a) { return interp_ram[a & 0x1fffff]; }
static int compare_scratch(void) {
    int i, diffs = 0;
    for (i = 0; i < g_nactive; i++) if (streq(g_modules[g_active[i]].name, "wldcore")) return 0;   /* WLDCORE runs its frame code on a stack IN the scratchpad (wldcore_switch_to_stack): the retail spills are not reproduced natively */
    for (i = 0; i < 256; i++) if (((unsigned*)0x1f800000u)[i] != ((unsigned*)cpu.scratch)[i]) diffs++;
    return diffs;
}

/* -------------------------------------------------------------------------------------------- the disc */
static int g_disc_fd = -1;
static int read_sector(void* ctx, unsigned lba, unsigned char* dst) {
    unsigned char raw[2352];
    long got = 0, n;
    unsigned k;
    (void)ctx;
    if (g_disc_fd < 0) return 0;
    sys3(19, g_disc_fd, (long)(lba * 2352u), 0);
    while (got < 2352 && (n = sys3(3, g_disc_fd, (long)(raw + got), 2352 - got)) > 0) got += n;
    if (got < 2352) return 0;
    for (k = 0; k < 2048; k++) dst[k] = raw[24 + k];
    return 1;
}
static int module_at(unsigned lba) {                                        /* the overlay whose file starts at or contains this sector, else -1 */
    int i;
    for (i = 1; i < g_module_count; i++) if (lba >= g_modules[i].lba && lba < g_modules[i].lba + g_modules[i].sectors) return i;
    return -1;
}
static int is_overlay_lba(void* ctx, unsigned lba) { (void)ctx; return module_at(lba) >= 0; }
/* scripted controller input: {first frame, buttons}; the state holds until the next entry (-Pad in lockstep.ps1) */
static const unsigned g_pad_script[][2] = {
#include "pad_script.h"
    , { 0xffffffffu, 0 } };
static unsigned pad_at(unsigned frame) {
    unsigned mask = 0;
    int i;
    for (i = 0; g_pad_script[i][0] != 0xffffffffu; i++) if (g_pad_script[i][0] <= frame) mask = g_pad_script[i][1];
    return mask;
}
/* symbols to watch: their (interpreter-side) 32-bit value is printed whenever it changes (-Watch in lockstep.ps1) */
struct watch { unsigned addr; const char* name; };
static const struct watch g_watch[] = {
#include "watch.h"
    { 0, 0 } };
static unsigned g_watch_last[64];
static void report_watch(int frame) {
    int i;
    for (i = 0; g_watch[i].addr && i < 64; i++) {
        unsigned v = *(unsigned*)(interp_ram + (g_watch[i].addr & 0x1fffff));
        if (v != g_watch_last[i] || frame == 1) { g_watch_last[i] = v; out("  [frame "); outnum(frame); out("] "); out(g_watch[i].name); out(" = 0x"); outhex(v); out("\n"); }
    }
}
static int g_log_budget = LOG_LIMIT;
static void log_call(void* ctx, const char* what, unsigned a0, unsigned a1, unsigned a2) {
    if (g_log_budget > 0) { g_log_budget--; out("    sdk["); out((const char*)ctx); out("] "); out(what); out("("); outhex(a0); out(", "); outhex(a1); out(", "); outhex(a2); out(")\n"); }
}

/* ------------------------------------------------------------------------------ machine A: the interpreter */
static hle_t hle_i;
static unsigned ia_r8(void* c, unsigned a) { (void)c; return interp_ram[a & 0x1fffff]; }
static void ia_w8(void* c, unsigned a, unsigned v) { (void)c; interp_ram[a & 0x1fffff] = (unsigned char)v; }
static void ia_wb(void* c, unsigned a, const unsigned char* s, unsigned n) { unsigned k; (void)c; for (k = 0; k < n; k++) interp_ram[(a + k) & 0x1fffff] = s[k]; }
static unsigned ia_call(void* c, unsigned addr, unsigned a0, unsigned a1) { unsigned args[2]; (void)c; args[0] = a0; args[1] = a1; return r3k_call_nested(&cpu, addr, args, 2, 5000000ull); }
static void ia_frame(void* c) { (void)c; hle_i.stop = 1; }
extern const unsigned g_hle_untraced[];                                      /* gen_hle.py: [lo, hi) address ranges of natively replaced SDK libraries */
static int untraced_caller(unsigned ra) {
    int i;
    for (i = 0; g_hle_untraced[i] || g_hle_untraced[i + 1]; i += 2) if (ra >= g_hle_untraced[i] && ra < g_hle_untraced[i + 1]) return 1;
    return 0;
}
static struct { unsigned addr; const char* name; int module; int arity; } g_hooks[1024];
static int n_hooks;
static unsigned g_main_addr;
static int g_at_main;
static void unhook(unsigned addr);
#ifdef SCENARIO_TITLE
/* Scenario "title": the game loop decides at the top of every iteration whether OPEN plays the opening movie (result != 5) or goes straight
 * to the title menu (5 = "coming back from the world map"). Both machines run main_item_init_new_game_inventory as usual and then the
 * driver stores 5 into g_main_system_frontend_world_result, i.e. steers the game past the FMV (whose streaming hardware is not modelled). */
#define SCENARIO_FN "main_item_init_new_game_inventory"
#define SCENARIO_WORD 0x80045978u
#define SCENARIO_VALUE 5u
static unsigned g_scn_addr;
static int g_scn_active;
#endif
static int interp_hle(r3k_t* c, unsigned addr) {
    int i, arity = 4;
    const char* n = 0;
    if (addr == g_main_addr) { g_at_main = 1; return 1; }                    /* stop at the entry of main() without running it */
#ifdef SCENARIO_TITLE
    if (addr == g_scn_addr && g_scn_addr) {                                  /* run the original function, then poke */
        unsigned args[4];
        args[0] = c->r[4]; args[1] = c->r[5]; args[2] = c->r[6]; args[3] = c->r[7];
        unhook(addr);
        c->r[2] = r3k_call_nested(c, addr, args, 4, 5000000ull);
        r3k_hle_add(c, addr);
        interp_ram[SCENARIO_WORD & 0x1fffff] = SCENARIO_VALUE; interp_ram[(SCENARIO_WORD & 0x1fffff) + 1] = 0; interp_ram[(SCENARIO_WORD & 0x1fffff) + 2] = 0; interp_ram[(SCENARIO_WORD & 0x1fffff) + 3] = 0;
        return 0;
    }
#endif
    for (i = 0; i < n_hooks; i++) if (g_hooks[i].addr == addr) { n = g_hooks[i].name; arity = g_hooks[i].arity; break; }
    hle_i.no_trace = untraced_caller(c->r[31]);                              /* an HLE call made from inside the (natively replaced) libgte is not compared */
    c->r[2] = hle_call(&hle_i, n ? n : "?", (unsigned)arity, c->r[4], c->r[5], c->r[6], c->r[7]);
    hle_i.no_trace = 0;
    return hle_i.stop;
}
static void unhook(unsigned addr) { unsigned w = (addr & 0x1fffffu) >> 2; cpu.hle_bitmap[w >> 3] &= (unsigned char)~(1u << (w & 7)); }
/* the interpreter intercepts the ORIGINAL entry addresses of the functions the HLE takes over, only while their module is loaded
 * (an overlay's addresses hold other code before and after) */
static int install_hle_hooks(int mod) {
    int j, i, n = 0;
    for (j = 0; j < g_hle_module_name_count; j++) {
        if (!streq(g_hle_module_names[j].module, g_modules[mod].name)) continue;
        for (i = 0; i < g_hle_module_names[j].count && n_hooks < 1024; i++) {
            unsigned a = addr_in(mod, g_hle_module_names[j].names[i]);
            if (a) { g_hooks[n_hooks].addr = a; g_hooks[n_hooks].name = g_hle_module_names[j].names[i]; g_hooks[n_hooks].module = mod; g_hooks[n_hooks].arity = g_hle_module_names[j].arity[i]; n_hooks++; r3k_hle_add(&cpu, a); n++; }
        }
    }
    return n;
}
static void remove_hle_hooks(int mod) {
    int i, k = 0;
    for (i = 0; i < n_hooks; i++) {
        if (g_hooks[i].module == mod) unhook(g_hooks[i].addr);
        else g_hooks[k++] = g_hooks[i];
    }
    n_hooks = k;
}

/* --------------------------------------------------------------------------------- machine B: the native game */
hle_t g_hle_native;
extern void native_main(void);
static void* g_driver_esp;
static void* g_game_esp;
static int g_native_dead;
void ls_switch(void** save_esp, void* new_esp);
__asm__(".text\n"
        ".globl ls_switch\n"
        "ls_switch:\n"
        "    movl 4(%esp), %eax\n"
        "    movl 8(%esp), %edx\n"
        "    pushl %ebp\n"
        "    pushl %ebx\n"
        "    pushl %esi\n"
        "    pushl %edi\n"
        "    movl %esp, (%eax)\n"
        "    movl %edx, %esp\n"
        "    popl %edi\n"
        "    popl %esi\n"
        "    popl %ebx\n"
        "    popl %ebp\n"
        "    ret\n");
static void game_entry(void) {
    native_main();
    g_native_dead = 1;
    for (;;) ls_switch(&g_game_esp, g_driver_esp);
}
static unsigned nb_r8(void* c, unsigned a) { (void)c; return *(volatile unsigned char*)a; }
static void nb_w8(void* c, unsigned a, unsigned v) { (void)c; *(volatile unsigned char*)a = (unsigned char)v; }
static void nb_wb(void* c, unsigned a, const unsigned char* s, unsigned n) { unsigned k; (void)c; for (k = 0; k < n; k++) ((volatile unsigned char*)a)[k] = s[k]; }
static unsigned nb_call(void* c, unsigned addr, unsigned a0, unsigned a1) { (void)c; return ((unsigned (*)(unsigned, unsigned))addr)(a0, a1); }
static void nb_frame(void* c) { (void)c; ls_switch(&g_game_esp, g_driver_esp); }
static void start_native_game(void) {
    /* the game's stack lives in the RAM image, at the top like the console's: locals whose addresses go into GPU ordering tables / RAM structures keep valid 24-bit PS1 addresses */
    unsigned* top = (unsigned*)0x80200000u;
    top -= 5;
    *--top = (unsigned)game_entry;
    *--top = 0; *--top = 0; *--top = 0; *--top = 0;
    g_game_esp = top;
}

#ifdef SCENARIO_TITLE
extern void native_main_item_init_new_game_inventory(void);
static void scn_wrapper(void) { native_main_item_init_new_game_inventory(); *(volatile unsigned*)SCENARIO_WORD = SCENARIO_VALUE; }
#endif

/* -------------------------------------------------------------------------- crash report for the native side */
static const char* g_where = "before main";
struct ksigaction { void (*handler)(int, void*, void*); unsigned long flags; void (*restorer)(void); unsigned long long mask; };
static void fault_handler(int sig, void* info, void* uc) {
    unsigned eip = *(unsigned*)((char*)uc + 76), ebp = *(unsigned*)((char*)uc + 44);
    int depth;
    out("\nNATIVE CRASH: signal "); outnum(sig); out(" at @0x"); outhex(eip); out(", address 0x"); outhex(*(unsigned*)((char*)info + 12));
    out("; last sync: "); out(g_where); out(", native VSync count "); outnum(g_hle_native.frame_counter); out("\n  backtrace:");
    for (depth = 0; depth < 24 && ebp >= 0x08000000u && ebp < 0xc0000000u && (ebp & 3) == 0; depth++) {
        unsigned ra = ((unsigned*)ebp)[1];
        out(" @0x"); outhex(ra);
        if (((unsigned*)ebp)[0] <= ebp) break;
        ebp = ((unsigned*)ebp)[0];
    }
    out("\n");
    sys3(1, 3, 0, 0);
}
static void install_fault_handlers(void) {
    struct ksigaction sa;
    sa.handler = fault_handler; sa.flags = 4 | 0x40000000; sa.restorer = 0; sa.mask = 0;
    sys4(174, 11, (long)&sa, 0, 8); sys4(174, 8, (long)&sa, 0, 8); sys4(174, 7, (long)&sa, 0, 8); sys4(174, 4, (long)&sa, 0, 8);
}

/* ------------------------------------------------------------------------------------------------ the driver */
static gte_state_t gte_of[2];                                               /* [0] original, [1] native: the software GTE is one global, swapped at every switch */
static int run_native(void) {                                               /* -> sync reason, or -1 when the native game returned from main() */
    g_gte = gte_of[1];
    g_hle_native.sync_reason = HLE_SYNC_NONE;
    ls_switch(&g_driver_esp, g_game_esp);
    gte_of[1] = g_gte;
    return g_native_dead ? -1 : g_hle_native.sync_reason;
}
static int g_orig_rc;
static int run_orig(void) {                                                 /* -> sync reason, or 0 when the interpreter faulted / ran out of steps (g_orig_rc) */
    g_gte = gte_of[0];
    hle_i.stop = 0; hle_i.sync_reason = HLE_SYNC_NONE;
    g_orig_rc = r3k_run(&cpu, STEP_BUDGET);
    gte_of[0] = g_gte;
    return hle_i.stop ? hle_i.sync_reason : 0;
}
/* compare the calls the two machines made into the HLE this frame (the platform-layer traffic) */
static void print_trace_entry(const hle_trace_entry_t* e) {
    out(e->name); out("("); outhex(e->a[0]); out(", "); outhex(e->a[1]); out(", "); outhex(e->a[2]); out(", "); outhex(e->a[3]); out(") pointee-hash "); outhex(e->a[4]);
}
static int compare_traces(void) {
    unsigned i, n = hle_i.trace_n < g_hle_native.trace_n ? hle_i.trace_n : g_hle_native.trace_n;
    for (i = 0; i < n; i++) {
        const hle_trace_entry_t* x = &g_hle_native.trace[i];
        const hle_trace_entry_t* y = &hle_i.trace[i];
        int k, same = streq(x->name, y->name);
        for (k = 0; k < 5; k++) if (x->a[k] != y->a[k]) same = 0;
        if (!same) {
            unsigned c0 = i > 8 ? i - 8 : 0, c;
            for (c = c0; c < i; c++) { out("    #"); outnum(c); out(" (both) "); print_trace_entry(&hle_i.trace[c]); out("\n"); }
            out("  HLE call #"); outnum(i); out(" differs:\n    native   "); print_trace_entry(x); out("\n    original "); print_trace_entry(y); out("\n");
            return 1;
        }
    }
    if (hle_i.trace_n != g_hle_native.trace_n) {
        out("  the machines made a different number of HLE calls: native "); outnum(g_hle_native.trace_n); out(", original "); outnum(hle_i.trace_n);
        if (i < hle_i.trace_n) { out("; original's next: "); print_trace_entry(&hle_i.trace[i]); }
        if (i < g_hle_native.trace_n) { out("; native's next: "); print_trace_entry(&g_hle_native.trace[i]); }
        out("\n");
        return 1;
    }
    return 0;
}
static void report_stubs(void) {
    int i, n = 0;
    for (i = 0; i < g_stub_count; i++) if (g_stub_hits[i]) n++;
    out("native calls into yaml functions that have no native definition (stubs returning 0): "); outnum(n); out(" distinct, "); outnum(g_stub_calls); out(" calls\n");
    for (i = 0; i < g_stub_count; i++) if (g_stub_hits[i]) { out("    "); out(g_stub_names[i]); out(" x"); outnum(g_stub_hits[i]); out("\n"); }
}

static int main_test(void) {
    int frame, rn, ro, diffs;
    hle_platform_t pa, pb;
    pa.ctx = (void*)"original"; pa.r8 = ia_r8; pa.w8 = ia_w8; pa.write_bytes = ia_wb; pa.call = ia_call; pa.read_sector = read_sector; pa.frame = ia_frame; pa.log = log_call; pa.is_overlay = is_overlay_lba;
    pb.ctx = (void*)"native"; pb.r8 = nb_r8; pb.w8 = nb_w8; pb.write_bytes = nb_wb; pb.call = nb_call; pb.read_sector = read_sector; pb.frame = nb_frame; pb.log = log_call; pb.is_overlay = is_overlay_lba;
    hle_init(&hle_i, &pa);
    hle_init(&g_hle_native, &pb);

    /* --- run the ORIGINAL from the entry point to main() --- */
    g_main_addr = addr_of("main");
    cpu.hle = interp_hle;
    install_hle_hooks(0);
    r3k_hle_add(&cpu, g_main_addr);
#ifdef SCENARIO_TITLE
    if (g_scn_addr) r3k_hle_add(&cpu, g_scn_addr);
#endif
    cpu.r[29] = 0x801fff00u;
    cpu.pc = addr_of("__SN_ENTRY_POINT"); cpu.npc = cpu.pc + 4;
    g_orig_rc = r3k_run(&cpu, STEP_BUDGET);
    if (!g_at_main) { out("the original did not reach main(): rc "); outnum(g_orig_rc); out(" at "); func_at(cpu.cur_pc); out("\n"); return 1; }
    cpu.fault = 0; unhook(g_main_addr);
    out("original is at main() after "); outnum((long)cpu.steps); out(" instructions; copying its RAM to the native image and starting the native game\n");
    copy_data_interp_to_native();
    g_bios_rand_seed = cpu.bios_rand_seed;
    gte_of[0] = gte_of[1] = g_gte;
    hle_i.cd_reads = 0; hle_i.cd_sectors = 0; hle_i.calls = 0;
    start_native_game();

    /* --- frame by frame --- */
    for (frame = 1; frame <= MAX_FRAMES; frame++) {
        g_hle_native.pad_mask = hle_i.pad_mask = pad_at((unsigned)frame);                  /* both machines see the same controller */
        hle_i.trace_n = g_hle_native.trace_n = 0; hle_i.trace_lost = g_hle_native.trace_lost = 0;
        rn = run_native();
        if (rn < 0) { out("native main() returned before frame "); outnum(frame); out("\n"); return 1; }
        ro = run_orig();
        if (!ro) { out("original stopped without syncing before frame "); outnum(frame); out(": rc "); outnum(g_orig_rc); out(" at "); func_at(cpu.cur_pc); out(" (steps "); outnum((long)cpu.steps); out(")\n"); report_stubs(); return 1; }
        g_where = ro == HLE_SYNC_OVERLAY ? "overlay load" : "VSync";
        if (rn != ro) { out("FRAME "); outnum(frame); out(": the machines synced for different reasons: native "); outnum(rn); out(", original "); outnum(ro); out("\n"); report_stubs(); return 2; }
        diffs = compare_ram(0);
        if (compare_traces()) { out("FRAME "); outnum(frame); out(": the machines' calls into the HLE differ (RAM differs in "); outnum(diffs); out(" words)\n"); if (diffs) compare_ram(SHOW_DIFFS); report_stubs(); return 2; }
        if (diffs || compare_scratch()) {
            out("FRAME "); outnum(frame); out(": RAM differs in "); outnum(diffs); out(" words (native vs original)"); if (compare_scratch()) out(" and in the scratchpad"); out("; first ones:\n");
            compare_ram(SHOW_DIFFS);
            out("native VSync count "); outnum(g_hle_native.frame_counter); out(", original "); outnum(hle_i.frame_counter);
            out("; CD reads native "); outnum(g_hle_native.cd_reads); out(" original "); outnum(hle_i.cd_reads); out("; original steps "); outnum((long)cpu.steps); out("\n");
            report_stubs();
            return 2;
        }
        if (ro == HLE_SYNC_OVERLAY) {
            int mi = module_at(hle_i.sync_arg2), k, j, installed;
            out("frame "); outnum(frame); out(": the game loads a code overlay ("); out(mi >= 0 ? g_modules[mi].file : "?"); out(", "); outnum(hle_i.sync_arg1); out(" sectors at LBA "); outnum(hle_i.sync_arg2);
            out(" -> 0x"); outhex(hle_i.sync_arg0); out("); RAM identical; original steps "); outnum((long)cpu.steps); out("\n");
            if (g_hle_native.sync_arg0 != hle_i.sync_arg0 || g_hle_native.sync_arg1 != hle_i.sync_arg1 || g_hle_native.sync_arg2 != hle_i.sync_arg2) { out("BUT the two machines asked for different reads\n"); return 2; }
            /* the new module evicts every active module whose address range it overlaps */
            for (k = 0; k < g_nactive; k++) {
                const struct module* a = &g_modules[g_active[k]];
                unsigned alo = a->load, ahi = a->load + a->sectors * 2048u, blo = g_modules[mi].load, bhi = g_modules[mi].load + g_modules[mi].sectors * 2048u;
                if (alo < bhi && blo < ahi) { uninstall_module(g_active[k]); remove_hle_hooks(g_active[k]); for (j = k; j + 1 < g_nactive; j++) g_active[j] = g_active[j + 1]; g_nactive--; k--; }
            }
            g_active[g_nactive++] = mi;
            installed = install_module(mi);
            j = install_hle_hooks(mi);
            build_ranges();
            out("  module "); out(g_modules[mi].name); out(": "); outnum(installed); out(" of "); outnum(g_modules[mi].nfns); out(" functions have native code, "); outnum(j); out(" taken over by the HLE; "); outnum(ndata); out(" data ranges compared from now on\n");
            if (!installed) { out("  no native code for this module -- stopping (the machines agreed up to here)\n"); report_stubs(); return 0; }
        } else
        if (g_hle_native.frame_counter != hle_i.frame_counter || g_hle_native.cd_reads != hle_i.cd_reads) { out("FRAME "); outnum(frame); out(": VSync/CD counters differ\n"); return 2; }
        report_watch(frame);
        if (frame % 25 == 0 || frame == 1) {
            out("frame "); outnum(frame); out(": RAM identical ("); outnum(ndata); out(" data ranges; original steps "); outnum((long)cpu.steps); out(", CD reads "); outnum(hle_i.cd_reads); out(", stub calls "); outnum(g_stub_calls); out(")\n");
        }
        cpu.fault = 0; cpu.pc = cpu.r[31]; cpu.npc = cpu.pc + 4;               /* the HLE'd VSync returns to its caller */
    }
    out("== "); outnum(MAX_FRAMES); out(" frames: RAM identical at every VSync\n");
    report_stubs();
    return 0;
}

void _start(void) {
    int bad;
    long n, n_tramp;
    if (!map_fixed(0x80000000u, 0x200000u)) { out("mmap of the RAM image FAILED\n"); sys3(1, 1, 0, 0); }
    map_fixed(0x1f800000u, 0x1000u);                                         /* scratchpad (the native game may use it); I/O registers stay unmapped: a touch is a crash report */
    install_fault_handlers();
    n = load_file("/disc/SCUS_942.21", (unsigned char*)0x8000f800u);
    build_ranges();
    n_tramp = install_module(0);                                              /* first: compiler-generated calls to memset/memcpy resolve to PS1 addresses (trampolines) */
    load_file("/disc/SCUS_942.21", interp_ram + (0x8000f800u & 0x1fffff));
    out("SCUS_942.21: "); outnum(n); out(" bytes\n");
    g_disc_fd = (int)sys3(5, (long)"/disc.bin", 0, 0);
    r3k_reset(&cpu, interp_ram);
    outnum(n_tramp); out(" trampolines of the main executable (native image only), "); outnum(ndata); out(" data ranges compared\n");
#ifdef SCENARIO_TITLE
    g_scn_addr = addr_of(SCENARIO_FN);
    if (g_scn_addr) {                                                         /* point the trampoline of that function at the wrapper */
        unsigned char* tp = (unsigned char*)g_scn_addr;
        tp[0] = 0xe9; *(unsigned*)(tp + 1) = (unsigned)scn_wrapper - (g_scn_addr + 5);
        out("scenario title: main_item_init_new_game_inventory will set g_main_system_frontend_world_result = 5 on both machines\n");
    }
#endif
    gte_reset();
    bad = main_test();
    sys3(1, bad, 0, 0);
}
