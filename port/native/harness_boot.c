/* Boot probe: run the game's ORIGINAL boot code (from the PS-X EXE entry point __SN_ENTRY_POINT) on the R3000 interpreter, with the
 * SDK's hardware layer replaced by HLE hooks, to see how far it gets and which SDK calls it makes, in order.
 *
 * Nothing here is game logic: it is the platform layer a native port must provide, prototyped on the interpreter so that the same
 * behaviours can later be written once in C for the native build. The disc is read by LBA from the raw image (/disc.bin).
 *
 * Hooked (HLE): everything in the libspu / libetc / libcd / libcard address ranges, the BIOS event API, and the hardware-facing
 * libgpu entry points. Everything else (the game, libc, the pure libgpu primitive setters) runs as the original code.
 * Freestanding -m32; build/run with boot.ps1. */
#include "gte.h"
#include "r3000/r3000.h"

#ifndef MAX_FRAMES
#define MAX_FRAMES 120
#endif
#ifndef MAX_STEPS_TOTAL
#define MAX_STEPS_TOTAL 400000000ull
#endif
#ifndef LOG_LIMIT
#define LOG_LIMIT 400
#endif

/* ---------------------------------------------------------------------------------------- freestanding I/O */
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
static long load_file(const char* path, unsigned char* dst) {
    long fd = sys3(5, (long)path, 0, 0), total = 0, n;
    if (fd < 0) return -1;
    while ((n = sys3(3, fd, (long)(dst + total), 1 << 20)) > 0) total += n;
    sys3(6, fd, 0, 0);
    return total;
}
static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int starts(const char* s, const char* p) { while (*p) { if (*s++ != *p++) return 0; } return 1; }

/* ------------------------------------------------------------------------------------------------- tables */
struct orig_func { const char* name; unsigned int addr; unsigned int size; };
extern const struct orig_func g_orig_funcs[];
extern const int g_orig_func_count;
struct sdk_range { unsigned int lo, hi; const char* id; };
extern const struct sdk_range g_sdk_ranges[];
extern const int g_sdk_range_count;

static unsigned char ram[0x200000];
static r3k_t cpu;
static int g_disc_fd = -1;

/* -------------------------------------------------------------------------------------------- HLE state */
static unsigned frame_counter;                     /* VSync ticks */
static unsigned cb_vsync, cb_drawsync, cb_cd_ready, cb_cd_read, cb_spu_transfer;
static unsigned cd_lba;                            /* position set by CdlSetloc */
static unsigned cd_sectors_requested;
static int log_count, boot_done;
static const char* last_name;

static const char* name_at(unsigned addr) {
    int i;
    for (i = 0; i < g_orig_func_count; i++) if (g_orig_funcs[i].addr == addr) return g_orig_funcs[i].name;
    return "?";
}
static unsigned bcd(unsigned v) { return (v >> 4) * 10 + (v & 15); }
static void func_at(unsigned pc) {
    int i;
    for (i = 0; i < g_orig_func_count; i++)
        if (pc >= g_orig_funcs[i].addr && pc < g_orig_funcs[i].addr + g_orig_funcs[i].size) { out(g_orig_funcs[i].name); out("+0x"); outhex(pc - g_orig_funcs[i].addr); return; }
    outhex(pc);
}

/* read `count` 2048-byte sectors starting at `lba` from the raw image (MODE2/2352: 24-byte header before the data) into RAM */
static void cd_read_sectors(unsigned lba, unsigned count, unsigned dst) {
    unsigned i;
    unsigned char sector[2352];
    for (i = 0; i < count; i++) {
        long got = 0, want = 2352, n;
        sys3(19, g_disc_fd, (long)((lba + i) * 2352u), 0);                          /* lseek(SEEK_SET) */
        while (got < want && (n = sys3(3, g_disc_fd, (long)(sector + got), want - got)) > 0) got += n;
        {
            unsigned k, d = (dst + i * 2048u) & 0x1fffff;
            for (k = 0; k < 2048; k++) ram[(d + k) & 0x1fffff] = sector[24 + k];
        }
    }
}

/* ------------------------------------------------------------------------------- calling guest code from HLE */
static unsigned call_guest(unsigned addr, unsigned a0, unsigned a1) {
    unsigned args[2];
    args[0] = a0; args[1] = a1;
    return r3k_call_nested(&cpu, addr, args, 2, 5000000ull);
}

static void log_call(const char* name, unsigned a0, unsigned a1, unsigned a2) {
    if (log_count++ < LOG_LIMIT) {
        out("  [f"); outnum(frame_counter); out("] "); out(name); out("("); outhex(a0); out(", "); outhex(a1); out(", "); outhex(a2); out(")\n");
    }
}

/* generic + specific SDK behaviours */
static int boot_hle(r3k_t* c, unsigned addr) {
    const char* n = name_at(addr);
    unsigned a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    last_name = n;
    c->r[2] = 0;
    if (streq(n, "VSync")) {
        if ((int)a0 == 0) {                                             /* wait for the next vertical blank: run its callback once */
            frame_counter++;
            if (cb_vsync) call_guest(cb_vsync, 0, 0);
            if (frame_counter >= MAX_FRAMES) boot_done = 1;
        }
        c->r[2] = frame_counter;
        return boot_done;
    }
    if (streq(n, "VSyncCallback")) { cb_vsync = a0; log_call(n, a0, a1, a2); return 0; }
    if (streq(n, "DrawSyncCallback")) { cb_drawsync = a0; log_call(n, a0, a1, a2); return 0; }
    if (streq(n, "CdReadyCallback")) { cb_cd_ready = a0; log_call(n, a0, a1, a2); return 0; }
    if (streq(n, "CdReadCallback")) { cb_cd_read = a0; log_call(n, a0, a1, a2); return 0; }
    if (streq(n, "SpuSetTransferCallback")) { cb_spu_transfer = a0; log_call(n, a0, a1, a2); return 0; }
    if (streq(n, "SpuWrite") || streq(n, "SpuRead") || streq(n, "SpuWrite0")) {          /* DMA finishes at once: run the transfer callback */
        log_call(n, a0, a1, a2);
        c->r[2] = a1;
        if (cb_spu_transfer) call_guest(cb_spu_transfer, 0, 0);
        return 0;
    }
    if (streq(n, "SpuIsTransferCompleted")) { c->r[2] = 1; return 0; }
    if (streq(n, "CdInit")) { c->r[2] = 1; log_call(n, a0, a1, a2); return 0; }
    if (streq(n, "CdIntToPos")) {                                        /* sector number -> minute/second/sector (BCD) at a1 */
        unsigned v = a0 + 150u, m = v / 75u / 60u, sc = v / 75u % 60u, f = v % 75u, p = a1 & 0x1fffffu;
        ram[p] = (unsigned char)(((m / 10) << 4) | (m % 10)); ram[p + 1] = (unsigned char)(((sc / 10) << 4) | (sc % 10)); ram[p + 2] = (unsigned char)(((f / 10) << 4) | (f % 10)); ram[p + 3] = 0;
        c->r[2] = a1;
        return 0;
    }
    if (streq(n, "CdPosToInt")) {
        unsigned p = a0 & 0x1fffffu;
        c->r[2] = (bcd(ram[p]) * 60u + bcd(ram[p + 1])) * 75u + bcd(ram[p + 2]) - 150u;
        return 0;
    }
    if (streq(n, "CdControl") || streq(n, "CdControlB") || streq(n, "CdControlF")) {
        if (((a0 & 0xff) == 0x02 || (a0 & 0xff) == 0x15 || (a0 & 0xff) == 0x16) && a1) {   /* CdlSetloc / SeekL / SeekP with a position: minute, second, sector (BCD) */
            unsigned p = a1 & 0x1fffff;
            unsigned m = bcd(ram[p]), s = bcd(ram[p + 1]), f = bcd(ram[p + 2]);
            cd_lba = (m * 60u + s) * 75u + f - 150u;
        }
        if (a2) ram[a2 & 0x1fffffu] = 0x02;                             /* status: motor on */
        c->r[2] = 1;
        log_call(n, a0, a1, a2);
        return 0;
    }
    if (streq(n, "CdRead")) {
        cd_sectors_requested = a0;
        cd_read_sectors(cd_lba, a0, a1);
        log_call(n, a0, a1, a2);
        out("      -> read "); outnum(a0); out(" sectors at LBA "); outnum(cd_lba); out("\n");
        c->r[2] = 1;
        if (cb_cd_read) call_guest(cb_cd_read, 1, 0);                    /* CdlComplete */
        return 0;
    }
    if (streq(n, "CdSync") || streq(n, "CdReadSync")) { if (streq(n, "CdSync") && a1) ram[a1 & 0x1fffffu] = 0x02; c->r[2] = streq(n, "CdSync") ? 2 : 0; return 0; }
    if (streq(n, "DrawSync") || streq(n, "PadRead") || streq(n, "TestEvent")) { c->r[2] = 0; return 0; }
    log_call(n, a0, a1, a2);
    return 0;
}

static int is_hle_name(const char* n) {
    static const char* const exact[] = { "ResetGraph", "SetGraphDebug", "DrawSync", "DrawSyncCallback", "PutDispEnv", "PutDrawEnv", "DrawOTag", "DrawPrim",
                                         "LoadImage", "StoreImage", "MoveImage", "ClearImage", "SetDispMask", "GetGraphType",
                                         "OpenEvent", "CloseEvent", "EnableEvent", "DisableEvent", "TestEvent", "WaitEvent", "DeliverEvent", "UnDeliverEvent",
                                         "EnterCriticalSection", "ExitCriticalSection", "SetMem", "PadInit", "PadRead", "PadStop", 0 };
    int i;
    for (i = 0; exact[i]; i++) if (streq(n, exact[i])) return 1;
    return 0;
}

void _start(void) {
    int i, rc;
    unsigned char* p = ram;
    long n;
    (void)p;
    n = load_file("/disc/SCUS_942.21", ram + (0x8000f800u & 0x1fffff));
    out("SCUS_942.21: "); outnum(n); out(" bytes\n");
    g_disc_fd = (int)sys3(5, (long)"/disc.bin", 0, 0);
    if (g_disc_fd < 0) out("no /disc.bin: CdRead will fail\n");
    r3k_reset(&cpu, ram);
    cpu.hle = boot_hle;
    for (i = 0; i < g_orig_func_count; i++) {
        unsigned a = g_orig_funcs[i].addr;
        int hook = is_hle_name(g_orig_funcs[i].name);
        int r;
        for (r = 0; r < g_sdk_range_count; r++) if (a >= g_sdk_ranges[r].lo && a < g_sdk_ranges[r].hi && !starts(g_orig_funcs[i].name, "psyq_gte")) hook = 1;
        if (starts(g_orig_funcs[i].name, "Set") && (a >= 0x80022c24u)) hook = is_hle_name(g_orig_funcs[i].name);   /* libgpu primitive setters run as real code */
        if (hook) r3k_hle_add(&cpu, a);
    }
    cpu.r[29] = 0x801fff00u;
    cpu.pc = 0x80010a30u; cpu.npc = cpu.pc + 4;
    out("running the original boot code from __SN_ENTRY_POINT, HLE for the SDK hardware layer\n");
    rc = r3k_run(&cpu, MAX_STEPS_TOTAL);
    out("stopped: rc "); outnum(rc); out(", fault "); outnum(cpu.fault); out(" at "); func_at(cpu.fault_pc); out(", frames "); outnum(frame_counter);
    out(", steps "); outnum((long)cpu.steps); out(", HLE calls "); outnum(cpu.hle_calls); out(", last SDK call "); out(last_name ? last_name : "-"); out("\n");
    out("last executed instruction: "); func_at(cpu.cur_pc); out("\n");
    sys3(1, 0, 0, 0);
}
