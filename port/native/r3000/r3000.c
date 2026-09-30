/* MIPS R3000A interpreter -- see r3000.h. Freestanding, integer only, C89. */
#include "r3000.h"
#include "../gte/gte.h"

#define SIGN16(x) ((int)(short)(x))

static unsigned char* mp(r3k_t* c, unsigned int addr) {
    unsigned int p = addr & 0x1fffffffu;
    if (p < 0x800000u) return c->ram + (p & 0x1fffffu);                     /* 2 MiB RAM, mirrored through 8 MiB */
    if (p >= 0x1f800000u && p < 0x1f800400u) return c->scratch + (p - 0x1f800000u);
    return 0;
}

static void set_fault(r3k_t* c, int kind, unsigned int pc, unsigned int ins, unsigned int addr) {
    c->fault = kind; c->fault_pc = pc; c->fault_instr = ins; c->fault_addr = addr;
}

static void note_wild(r3k_t* c, unsigned int a) { if ((a >> 29) != 4 || (a & 0x1fffffffu) >= 0x200000u) c->wild++; }
static int in_code(r3k_t* c, unsigned int a) {
    unsigned int lo = 0, hi = c->ncode;
    while (lo < hi) {                                           /* binary search for the last range starting at or below a */
        unsigned int mid = (lo + hi) / 2;
        if (c->code_lo[mid] <= a) lo = mid + 1; else hi = mid;
    }
    return lo && a < c->code_hi[lo - 1];
}
static void note_code_read(r3k_t* c, unsigned int a) { if (in_code(c, a)) c->code_reads++; }
static void note_code_write(r3k_t* c, unsigned int a) { if (in_code(c, a)) c->code_writes++; }
static unsigned int rd8(r3k_t* c, unsigned int a) {
    unsigned char* p = mp(c, a);
    if (c->ncode) note_code_read(c, a);
    if (!p) { c->io_reads++; c->io_last_addr = a; return 0; }
    note_wild(c, a);
    return *p;
}
static unsigned int rd16(r3k_t* c, unsigned int a) {
    unsigned char* p = mp(c, a);
    if (c->ncode) note_code_read(c, a);
    if (!p) { c->io_reads++; c->io_last_addr = a; return 0; }
    note_wild(c, a);
    return *(unsigned short*)p;
}
static unsigned int rd32(r3k_t* c, unsigned int a) {
    unsigned char* p = mp(c, a);
    if (c->ncode) note_code_read(c, a);
    if (!p) { c->io_reads++; c->io_last_addr = a; return 0; }
    note_wild(c, a);
    return *(unsigned int*)p;
}
static void wlog(r3k_t* c, unsigned int a, unsigned int v) {
    if (a >= c->watch_lo && a < c->watch_hi && c->wlog_n < 16) { c->wlog_pc[c->wlog_n] = c->cur_pc; c->wlog_addr[c->wlog_n] = a; c->wlog_val[c->wlog_n] = v; c->wlog_n++; }
}
static void wr8(r3k_t* c, unsigned int a, unsigned int v) {
    unsigned char* p = mp(c, a);
    wlog(c, a, v);
    if (c->ncode) note_code_write(c, a);
    if (!p) { c->io_writes++; c->io_last_addr = a; return; }
    note_wild(c, a);
    *p = (unsigned char)v;
}
static void wr16(r3k_t* c, unsigned int a, unsigned int v) {
    unsigned char* p = mp(c, a);
    wlog(c, a, v);
    if (c->ncode) note_code_write(c, a);
    if (!p) { c->io_writes++; c->io_last_addr = a; return; }
    note_wild(c, a);
    *(unsigned short*)p = (unsigned short)v;
}
static void wr32(r3k_t* c, unsigned int a, unsigned int v) {
    unsigned char* p = mp(c, a);
    wlog(c, a, v);
    if (c->ncode) note_code_write(c, a);
    if (!p) { c->io_writes++; c->io_last_addr = a; return; }
    note_wild(c, a);
    *(unsigned int*)p = v;
}

unsigned int r3k_read32(r3k_t* c, unsigned int addr) { return rd32(c, addr); }
void r3k_write32(r3k_t* c, unsigned int addr, unsigned int value) { wr32(c, addr, value); }

void r3k_hle_add(r3k_t* c, unsigned int addr) { unsigned int w = (addr & 0x1fffffu) >> 2; c->hle_bitmap[w >> 3] |= (unsigned char)(1u << (w & 7)); }
void r3k_tick_add(r3k_t* c, unsigned int addr) { unsigned int w = (addr & 0x1fffffu) >> 2; c->tick_bitmap[w >> 3] |= (unsigned char)(1u << (w & 7)); }
void r3k_tick_remove(r3k_t* c, unsigned int addr) { unsigned int w = (addr & 0x1fffffu) >> 2; c->tick_bitmap[w >> 3] &= (unsigned char)~(1u << (w & 7)); }

void r3k_reset(r3k_t* c, unsigned char* ram) {
    unsigned int i;
    for (i = 0; i < 32; i++) c->r[i] = 0;
    c->hi = c->lo = 0; c->pc = c->npc = 0; c->ld_reg = c->ld_val = 0;
    c->ram = ram;
    for (i = 0; i < 1024; i++) c->scratch[i] = 0;
    c->bios_rand_seed = 1;
    c->sp_top = 0x801fff00u;
    c->steps = 0; c->io_reads = c->io_writes = c->io_last_addr = 0; c->syscalls = c->bios_calls = c->rand_calls = 0;
    c->fault = 0; c->fault_pc = c->fault_instr = c->fault_addr = 0;
    c->ncode = c->code_reads = c->code_writes = c->wild = c->nsdk = c->sdk_hits = 0; c->hle = 0; c->hle_calls = 0; c->tick = 0; { unsigned int q; for (q = 0; q < sizeof c->hle_bitmap; q++) { c->hle_bitmap[q] = 0; c->tick_bitmap[q] = 0; } } c->trace_calls = c->call_n = 0; c->call_hook = 0; c->event_hook = 0; c->pending_call = 0; c->cur_pc = 0; c->div_zero = c->div_overflow = 0; c->watch_lo = c->watch_hi = 0; c->wlog_n = 0;
}

/* BIOS A-table entries that the game's C code reaches through the libc stubs (e.g. rand at 0x8002230c: li t2,0xa0; jr t2; li t1,0x2f). */
static int bios_call(r3k_t* c, unsigned int table) {
    unsigned int fn = c->r[9], a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], i;
    c->bios_calls++;
    if (table == 0xb0 && fn == 0x56) {                                                        /* GetC0Table: a fake table whose exception-handler slot points at scratch RAM */
        wr32(c, 0x80000218u, 0x80000400u); c->r[2] = 0x80000200u;
        c->pc = c->r[31]; c->npc = c->pc + 4;
        return 0;
    }
    if (table != 0xa0) { set_fault(c, R3K_FAULT_BIOS, c->pc, fn, table); return 1; }
    /* function numbers as in the game's own veneers (li t1,N): psx-spx numbering; port/native/bios_rt.c implements the same services natively */
    switch (fn) {
    case 0x44: break;                                                                        /* FlushCache */
    case 0x39: break;                                                                        /* InitHeap */
    case 0x0e: case 0x0f: c->r[2] = (int)a0 < 0 ? 0u - a0 : a0; break;                      /* abs, labs */
    case 0x15: { unsigned int e = a0; while (rd8(c, e)) e++; for (i = 0; ; i++) { unsigned int b = rd8(c, a1 + i); wr8(c, e + i, b); if (!b) break; } c->r[2] = a0; } break;   /* strcat(dst, src) */
    case 0x17:                                                                                /* strcmp(a, b): difference of the first differing bytes */
        for (i = 0; ; i++) { unsigned int x = rd8(c, a0 + i), y = rd8(c, a1 + i); if (x != y) { c->r[2] = x - y; break; } if (!x) { c->r[2] = 0; break; } }
        break;
    case 0x19: for (i = 0; ; i++) { unsigned int b = rd8(c, a1 + i); wr8(c, a0 + i, b); if (!b) break; } c->r[2] = a0; break;   /* strcpy(dst, src) */
    case 0x1b: for (i = 0; rd8(c, a0 + i); i++) ; c->r[2] = i; break;                         /* strlen */
    case 0x27: for (i = 0; i < a2; i++) wr8(c, a1 + i, rd8(c, a0 + i)); break;                /* bcopy(src, dst, len) */
    case 0x28: for (i = 0; i < a1; i++) wr8(c, a0 + i, 0); c->r[2] = a0; break;               /* bzero(dst, len) */
    case 0x2a: for (i = 0; i < a2; i++) wr8(c, a0 + i, rd8(c, a1 + i)); c->r[2] = a0; break;  /* memcpy(dst, src, len) */
    case 0x2b: for (i = 0; i < a2; i++) wr8(c, a0 + i, a1); c->r[2] = a0; break;              /* memset(dst, byte, len) */
    case 0x2c:                                                                                /* memmove(dst, src, len) */
        if (a0 <= a1) for (i = 0; i < a2; i++) wr8(c, a0 + i, rd8(c, a1 + i));
        else for (i = a2; i > 0; i--) wr8(c, a0 + i - 1, rd8(c, a1 + i - 1));
        c->r[2] = a0; break;
    case 0x2d:                                                                                /* memcmp */
        c->r[2] = 0;
        for (i = 0; i < a2; i++) { unsigned int x = rd8(c, a0 + i), y = rd8(c, a1 + i); if (x != y) { c->r[2] = x < y ? 0xffffffffu : 1u; break; } }
        break;
    case 0x2e: c->r[2] = 0; for (i = 0; i < a2; i++) if (rd8(c, a0 + i) == (a1 & 0xff)) { c->r[2] = a0 + i; break; } break;   /* memchr(s, c, n) */
    case 0x2f: c->rand_calls++; c->bios_rand_seed = c->bios_rand_seed * 1103515245u + 12345u; c->r[2] = (c->bios_rand_seed >> 16) & 0x7fffu; break;   /* rand */
    case 0x30: c->bios_rand_seed = a0; break;                                                 /* srand */
    default: set_fault(c, R3K_FAULT_BIOS, c->pc, fn, table); return 1;
    }
    c->pc = c->r[31]; c->npc = c->pc + 4;                                                     /* return to the caller ($ra) */
    return 0;
}

#define SETR(reg, val) do { if (reg) c->r[reg] = (val); } while (0)
#define LOAD(reg, val) do { c->ld_reg = (reg); c->ld_val = (val); } while (0)
#define BRANCH(cond) do { if (cond) c->npc = c->pc + ((unsigned int)SIGN16(imm) << 2); } while (0)
#define UNSUPPORTED() do { set_fault(c, R3K_FAULT_UNSUPPORTED, cur, ins, 0); return 1; } while (0)

static int step(r3k_t* c) {
    unsigned int cur = c->pc, ins, op, rs, rt, rd, sh, fn, imm, a, b, addr, v;
    unsigned int pl_reg = c->ld_reg, pl_val = c->ld_val;
    unsigned int phys = cur & 0x1fffffffu;
    unsigned char* ip;
    if (c->pending_call && cur == c->pending_call) {                                  /* a traced call has just been entered (its delay slot has run) */
        c->pending_call = 0;
        if (c->call_n < 1024) { c->call_trace[c->call_n++] = cur; if (c->call_hook) c->call_hook(c, cur); }
        if (c->event_hook) c->event_hook(c, cur);
    }
    if (c->tick && phys < 0x200000u) {                                                 /* observed function entry: run the callback, then the function itself */
        unsigned int w = phys >> 2;
        if (c->tick_bitmap[w >> 3] & (1u << (w & 7))) c->tick(c, cur);
    }
    if (c->hle && phys < 0x200000u) {                                                  /* intercepted function entry */
        unsigned int w = phys >> 2;
        if (c->hle_bitmap[w >> 3] & (1u << (w & 7))) {
            c->hle_calls++;
            if (c->ld_reg) { c->r[c->ld_reg] = c->ld_val; c->ld_reg = 0; }             /* a load in the jal's delay slot has landed by the time the (replaced) callee returns */
            if (c->hle(c, cur)) { set_fault(c, R3K_FAULT_BIOS, cur, 0, cur); return 1; }
            c->pc = c->r[31]; c->npc = c->pc + 4;
            return 0;
        }
    }
    if (phys == 0xa0u || phys == 0xb0u || phys == 0xc0u) return bios_call(c, phys);
    ip = mp(c, cur);
    if ((cur & 3u) || !ip) { set_fault(c, R3K_FAULT_BAD_FETCH, cur, 0, cur); return 1; }
    ins = *(unsigned int*)ip;
    c->cur_pc = cur;
    c->pc = c->npc; c->npc = c->npc + 4;                       /* pc now addresses the delay slot; a taken branch replaces npc */
    c->steps++;
    c->ld_reg = 0;
    op = ins >> 26; rs = (ins >> 21) & 31; rt = (ins >> 16) & 31; rd = (ins >> 11) & 31; sh = (ins >> 6) & 31; fn = ins & 63; imm = ins & 0xffffu;
    a = c->r[rs]; b = c->r[rt];                                /* operands are read BEFORE the previous load lands (delay slot) */
    if (pl_reg) {
        if ((op == 34 || op == 38) && pl_reg == rt) b = pl_val;   /* lwl/lwr merge sees the in-flight load */
        c->r[pl_reg] = pl_val;
    }
    switch (op) {
    case 0:
        switch (fn) {
        case 0x00: SETR(rd, b << sh); break;
        case 0x02: SETR(rd, b >> sh); break;
        case 0x03: SETR(rd, (unsigned int)((int)b >> sh)); break;
        case 0x04: SETR(rd, b << (a & 31)); break;
        case 0x06: SETR(rd, b >> (a & 31)); break;
        case 0x07: SETR(rd, (unsigned int)((int)b >> (a & 31))); break;
        case 0x08: c->npc = a; break;
        case 0x09: SETR(rd, cur + 8); c->npc = a; if (c->nsdk) { unsigned int q; for (q = 0; q < c->nsdk; q++) if (a >= c->sdk_lo[q] && a < c->sdk_hi[q]) c->sdk_hits++; } if (c->trace_calls) c->pending_call = a; break;
        case 0x0c: c->syscalls++; if (c->r[4] == 1) c->r[2] = 1; break;                  /* EnterCriticalSection / ExitCriticalSection */
        case 0x0d: set_fault(c, R3K_FAULT_BREAK, cur, ins, 0); return 1;
        case 0x10: SETR(rd, c->hi); break;
        case 0x11: c->hi = a; break;
        case 0x12: SETR(rd, c->lo); break;
        case 0x13: c->lo = a; break;
        case 0x18: { long long p = (long long)(int)a * (long long)(int)b; c->lo = (unsigned int)p; c->hi = (unsigned int)((unsigned long long)p >> 32); } break;
        case 0x19: { unsigned long long p = (unsigned long long)a * (unsigned long long)b; c->lo = (unsigned int)p; c->hi = (unsigned int)(p >> 32); } break;
        case 0x1a:
            if (b == 0) { c->div_zero++; c->lo = (int)a < 0 ? 1u : 0xffffffffu; c->hi = a; }
            else if (a == 0x80000000u && b == 0xffffffffu) { c->div_overflow++; c->lo = 0x80000000u; c->hi = 0; }
            else { c->lo = (unsigned int)((int)a / (int)b); c->hi = (unsigned int)((int)a % (int)b); }
            break;
        case 0x1b:
            if (b == 0) { c->div_zero++; c->lo = 0xffffffffu; c->hi = a; }
            else { c->lo = a / b; c->hi = a % b; }
            break;
        case 0x20: v = a + b; if (((a ^ v) & (b ^ v)) >> 31) { set_fault(c, R3K_FAULT_OVERFLOW, cur, ins, 0); return 1; } SETR(rd, v); break;
        case 0x21: SETR(rd, a + b); break;
        case 0x22: v = a - b; if (((a ^ b) & (a ^ v)) >> 31) { set_fault(c, R3K_FAULT_OVERFLOW, cur, ins, 0); return 1; } SETR(rd, v); break;
        case 0x23: SETR(rd, a - b); break;
        case 0x24: SETR(rd, a & b); break;
        case 0x25: SETR(rd, a | b); break;
        case 0x26: SETR(rd, a ^ b); break;
        case 0x27: SETR(rd, ~(a | b)); break;
        case 0x2a: SETR(rd, (int)a < (int)b); break;
        case 0x2b: SETR(rd, a < b); break;
        default: UNSUPPORTED();
        }
        break;
    case 1:
        switch (rt) {
        case 0x00: BRANCH((int)a < 0); break;
        case 0x01: BRANCH((int)a >= 0); break;
        case 0x10: SETR(31, cur + 8); BRANCH((int)a < 0); break;
        case 0x11: SETR(31, cur + 8); BRANCH((int)a >= 0); break;
        default: UNSUPPORTED();
        }
        break;
    case 2: c->npc = (c->pc & 0xf0000000u) | ((ins & 0x3ffffffu) << 2); if (c->nsdk) { unsigned int q; for (q = 0; q < c->nsdk; q++) if (c->npc >= c->sdk_lo[q] && c->npc < c->sdk_hi[q]) c->sdk_hits++; } if (c->trace_calls) c->pending_call = c->npc; break;
    case 3: SETR(31, cur + 8); c->npc = (c->pc & 0xf0000000u) | ((ins & 0x3ffffffu) << 2); if (c->nsdk) { unsigned int q; for (q = 0; q < c->nsdk; q++) if (c->npc >= c->sdk_lo[q] && c->npc < c->sdk_hi[q]) c->sdk_hits++; } if (c->trace_calls) c->pending_call = c->npc; break;
    case 4: BRANCH(a == b); break;
    case 5: BRANCH(a != b); break;
    case 6: BRANCH((int)a <= 0); break;
    case 7: BRANCH((int)a > 0); break;
    case 8: v = a + (unsigned int)SIGN16(imm); if (((a ^ v) & ((unsigned int)SIGN16(imm) ^ v)) >> 31) { set_fault(c, R3K_FAULT_OVERFLOW, cur, ins, 0); return 1; } SETR(rt, v); break;
    case 9:
        if (c->zero_frames && rt == 29 && rs == 29 && (imm & 0x8000u)) {                    /* a new stack frame: zero it (see r3k_t.zero_frames) */
            unsigned int lo = a + (unsigned int)SIGN16(imm), n = a - lo;
            if (lo >= 0x80000000u && a <= 0x80200000u && n <= 0x4000u) { unsigned char* z = c->ram + (lo & 0x1fffffu); unsigned int q; for (q = 0; q < n; q++) z[q] = 0; }
            else if (lo >= 0x1f800000u && a <= 0x1f800400u) { unsigned char* z = c->scratch + (lo - 0x1f800000u); unsigned int q; for (q = 0; q < n; q++) z[q] = 0; }     /* a stack in the scratchpad (WLDCORE's frame code) */
        }
        SETR(rt, a + (unsigned int)SIGN16(imm)); break;
    case 10: SETR(rt, (int)a < SIGN16(imm)); break;
    case 11: SETR(rt, a < (unsigned int)SIGN16(imm)); break;
    case 12: SETR(rt, a & imm); break;
    case 13: SETR(rt, a | imm); break;
    case 14: SETR(rt, a ^ imm); break;
    case 15: SETR(rt, imm << 16); break;
    case 16:                                                    /* COP0: harmless reads/writes only */
        if (rs == 0) { LOAD(rt, 0); }
        else if (rs == 4) { }
        else if (rs >= 16 && fn == 0x10) { }                    /* rfe */
        else UNSUPPORTED();
        break;
    case 18:                                                    /* COP2 = GTE */
        if (ins & 0x02000000u) { gte_command(ins & 0x1ffffffu); break; }
        switch (rs) {
        case 0: LOAD(rt, gte_mfc2(rd)); break;
        case 2: LOAD(rt, gte_cfc2(rd)); break;
        case 4: gte_mtc2(rd, b); break;
        case 6: gte_ctc2(rd, b); break;
        default: UNSUPPORTED();
        }
        break;
    case 32: LOAD(rt, (unsigned int)(int)(signed char)rd8(c, a + (unsigned int)SIGN16(imm))); break;
    case 33:
        addr = a + (unsigned int)SIGN16(imm);
        if (addr & 1) { set_fault(c, R3K_FAULT_UNALIGNED, cur, ins, addr); return 1; }
        LOAD(rt, (unsigned int)(int)(short)rd16(c, addr)); break;
    case 34:                                                    /* lwl */
        addr = a + (unsigned int)SIGN16(imm); v = rd32(c, addr & ~3u);
        switch (addr & 3) {
        case 0: v = (b & 0x00ffffffu) | (v << 24); break;
        case 1: v = (b & 0x0000ffffu) | (v << 16); break;
        case 2: v = (b & 0x000000ffu) | (v << 8); break;
        default: break;
        }
        LOAD(rt, v); break;
    case 35:
        addr = a + (unsigned int)SIGN16(imm);
        if (addr & 3) { set_fault(c, R3K_FAULT_UNALIGNED, cur, ins, addr); return 1; }
        LOAD(rt, rd32(c, addr)); break;
    case 36: LOAD(rt, rd8(c, a + (unsigned int)SIGN16(imm))); break;
    case 37:
        addr = a + (unsigned int)SIGN16(imm);
        if (addr & 1) { set_fault(c, R3K_FAULT_UNALIGNED, cur, ins, addr); return 1; }
        LOAD(rt, rd16(c, addr)); break;
    case 38:                                                    /* lwr */
        addr = a + (unsigned int)SIGN16(imm); v = rd32(c, addr & ~3u);
        switch (addr & 3) {
        case 1: v = (b & 0xff000000u) | (v >> 8); break;
        case 2: v = (b & 0xffff0000u) | (v >> 16); break;
        case 3: v = (b & 0xffffff00u) | (v >> 24); break;
        default: break;
        }
        LOAD(rt, v); break;
    case 40: wr8(c, a + (unsigned int)SIGN16(imm), b); break;
    case 41:
        addr = a + (unsigned int)SIGN16(imm);
        if (addr & 1) { set_fault(c, R3K_FAULT_UNALIGNED, cur, ins, addr); return 1; }
        wr16(c, addr, b); break;
    case 42:                                                    /* swl */
        addr = a + (unsigned int)SIGN16(imm); v = rd32(c, addr & ~3u);
        switch (addr & 3) {
        case 0: v = (v & 0xffffff00u) | (b >> 24); break;
        case 1: v = (v & 0xffff0000u) | (b >> 16); break;
        case 2: v = (v & 0xff000000u) | (b >> 8); break;
        default: v = b; break;
        }
        wr32(c, addr & ~3u, v); break;
    case 43:
        addr = a + (unsigned int)SIGN16(imm);
        if (addr & 3) { set_fault(c, R3K_FAULT_UNALIGNED, cur, ins, addr); return 1; }
        wr32(c, addr, b); break;
    case 46:                                                    /* swr */
        addr = a + (unsigned int)SIGN16(imm); v = rd32(c, addr & ~3u);
        switch (addr & 3) {
        case 0: v = b; break;
        case 1: v = (v & 0x000000ffu) | (b << 8); break;
        case 2: v = (v & 0x0000ffffu) | (b << 16); break;
        default: v = (v & 0x00ffffffu) | (b << 24); break;
        }
        wr32(c, addr & ~3u, v); break;
    case 50:                                                    /* lwc2 */
        addr = a + (unsigned int)SIGN16(imm);
        if (addr & 3) { set_fault(c, R3K_FAULT_UNALIGNED, cur, ins, addr); return 1; }
        gte_mtc2((int)rt, rd32(c, addr)); break;
    case 58:                                                    /* swc2 */
        addr = a + (unsigned int)SIGN16(imm);
        if (addr & 3) { set_fault(c, R3K_FAULT_UNALIGNED, cur, ins, addr); return 1; }
        wr32(c, addr, gte_mfc2((int)rt)); break;
    default: UNSUPPORTED();
    }
    return 0;
}

int r3k_call(r3k_t* c, unsigned int addr, const unsigned int* args, int nargs, unsigned long long max_steps) {
    int i;
    unsigned long long n;
    c->fault = 0; c->ld_reg = 0;
    c->r[29] = c->sp_top;
    for (i = 0; i < nargs && i < 4; i++) c->r[4 + i] = args[i];
    for (i = 4; i < nargs; i++) wr32(c, c->r[29] + 4u * (unsigned int)i, args[i]);      /* o32: stack arguments start at sp+16 */
    c->r[31] = R3K_SENTINEL;
    c->pc = addr; c->npc = addr + 4;
    for (n = 0; n < max_steps; n++) {
        if (c->pc == R3K_SENTINEL) {
            if (c->ld_reg) { c->r[c->ld_reg] = c->ld_val; c->ld_reg = 0; }                 /* a load in the final delay slot */
            return R3K_OK;
        }
        if (step(c)) return c->fault;
    }
    set_fault(c, R3K_FAULT_TIMEOUT, c->pc, 0, 0);
    return c->fault;
}

int r3k_run(r3k_t* c, unsigned long long max_steps) {
    unsigned long long n;
    for (n = 0; n < max_steps; n++) {
        if (c->pc == R3K_SENTINEL) {
            if (c->ld_reg) { c->r[c->ld_reg] = c->ld_val; c->ld_reg = 0; }
            return R3K_OK;
        }
        if (step(c)) return c->fault;
    }
    set_fault(c, R3K_FAULT_TIMEOUT, c->pc, 0, 0);
    return c->fault;
}

unsigned int r3k_call_nested(r3k_t* c, unsigned int addr, const unsigned int* args, int nargs, unsigned long long max_steps) {
    unsigned int saved[32], hi = c->hi, lo = c->lo, pc = c->pc, npc = c->npc, ld_reg = c->ld_reg, ld_val = c->ld_val, result;
    int i;
    for (i = 0; i < 32; i++) saved[i] = c->r[i];
    c->r[29] = (c->r[29] - 128u) & ~7u;
    for (i = 0; i < nargs && i < 4; i++) c->r[4 + i] = args[i];
    for (i = 4; i < nargs; i++) wr32(c, c->r[29] + 4u * (unsigned int)i, args[i]);
    c->r[31] = R3K_SENTINEL;
    c->pc = addr; c->npc = addr + 4; c->ld_reg = 0;
    r3k_run(c, max_steps);
    result = c->r[2];
    for (i = 0; i < 32; i++) c->r[i] = saved[i];
    c->hi = hi; c->lo = lo; c->pc = pc; c->npc = npc; c->ld_reg = ld_reg; c->ld_val = ld_val;
    return result;
}
