/* HLE of the PlayStation SDK hardware layer -- see hle.h. */
#include "hle.h"

static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static unsigned bcd(unsigned v) { return (v >> 4) * 10 + (v & 15); }

void hle_init(hle_t* h, const hle_platform_t* p) {
    unsigned char* q = (unsigned char*)h;
    unsigned i;
    for (i = 0; i < sizeof *h; i++) q[i] = 0;
    h->p = *p;
}

static void log_call(hle_t* h, const char* name, unsigned a0, unsigned a1, unsigned a2) {
    if (h->p.log) h->p.log(h->p.ctx, name, a0, a1, a2);
}

/* Read `count` sectors starting at `lba` into RAM at dst (PS1 address). */
static void cd_read(hle_t* h, unsigned lba, unsigned count, unsigned dst) {
    unsigned char sector[2048];
    unsigned i;
    for (i = 0; i < count; i++) {
        unsigned k;
        if (!h->p.read_sector(h->p.ctx, lba + i, sector)) for (k = 0; k < 2048; k++) sector[k] = 0;
        h->p.write_bytes(h->p.ctx, dst + i * 2048u, sector, 2048);
    }
    h->cd_reads++;
    h->cd_sectors += count;
}

static unsigned fnv(hle_t* h, unsigned addr, unsigned len) {
    unsigned x = 2166136261u;
    while (len--) x = (x ^ h->p.r8(h->p.ctx, addr++)) * 16777619u;
    return x;
}
static unsigned rd32(hle_t* h, unsigned addr) {
    return h->p.r8(h->p.ctx, addr) | (h->p.r8(h->p.ctx, addr + 1) << 8) | (h->p.r8(h->p.ctx, addr + 2) << 16) | (h->p.r8(h->p.ctx, addr + 3) << 24);
}
/* a GPU ordering table by CONTENT: every packet on the chain (its length and words), never the addresses (the two machines' stacks differ) */
static unsigned hash_ot(hle_t* h, unsigned ot) {
    unsigned x = 2166136261u, addr = ot, guard = 0;
    while (guard++ < 200000) {
        unsigned tag = rd32(h, addr), len = tag >> 24, next = tag & 0x00ffffffu, k;
        x = (x ^ len) * 16777619u;
        for (k = 1; k <= len && k < 64; k++) x = (x ^ rd32(h, addr + 4 * k)) * 16777619u;
        if (next == 0x00ffffffu) break;
        addr = 0x80000000u | next;
    }
    return x;
}
/* what the call looks like to a comparison: pointers to structures the caller owns are replaced by a hash of their contents */
static void trace_call(hle_t* h, const char* n, unsigned a0, unsigned a1, unsigned a2, unsigned a3) {
    hle_trace_entry_t* e;
    /* interrupt masking has no platform-visible effect, and the natively replaced libgte (InitGeom & co.) does not perform the critical sections
     * the original SDK code does: not part of the comparison */
    if (streq(n, "EnterCriticalSection") || streq(n, "ExitCriticalSection") || h->no_trace) return;
    if (h->trace_n >= HLE_TRACE_MAX) { h->trace_lost++; return; }
    e = &h->trace[h->trace_n++];
    e->name = n; e->a[0] = a0; e->a[1] = a1; e->a[2] = a2; e->a[3] = a3; e->a[4] = 0;
    /* the two machines' stacks differ in frame layout: a pointer to a stack local compares as "some stack address" (its contents are hashed below where they matter) */
    { int k; for (k = 0; k < 4; k++) if (e->a[k] >= 0x801f0000u && e->a[k] < 0x80200000u) e->a[k] = 0x801f0000u; }
    if (streq(n, "SpuSetReverbModeParam") || streq(n, "SpuSetReverbDepth")) e->a[4] = fnv(h, a0, 0x14);
    else if (streq(n, "SpuSetCommonAttr")) e->a[4] = fnv(h, a0, 0x28);
    else if (streq(n, "SpuSetVoiceAttr")) e->a[4] = fnv(h, a0, 0x40);
    if (e->a[4]) { }
    else if (streq(n, "LoadImage") || streq(n, "StoreImage") || streq(n, "ClearImage") || streq(n, "MoveImage")) { e->a[4] = fnv(h, a0, 8); e->a[0] = 0; }        /* RECT */
    else if (streq(n, "DrawOTag")) { e->a[4] = hash_ot(h, a0); e->a[0] = 0; }
    else if (streq(n, "PutDispEnv")) { e->a[4] = fnv(h, a0, 20); e->a[0] = 0; }                                                                           /* DISPENV */
    else if (streq(n, "PutDrawEnv")) { e->a[4] = fnv(h, a0, 28); e->a[0] = 0; }                                                                           /* DRAWENV without its DR_ENV packet */
    else if (streq(n, "CdControl") || streq(n, "CdControlB") || streq(n, "CdControlF")) {                                                             /* command, its parameter bytes, result buffer given? */
        unsigned len = 0, cmd = a0 & 0xff;
        if (cmd == 0x02) len = 3;                                                                                                                        /* CdlSetloc: minute, second, sector (BCD) */
        else if (cmd == 0x0d) len = 2;                                                                                                                   /* CdlSetfilter: file, channel */
        else if (cmd == 0x0e || cmd == 0x03 || cmd == 0x12) len = 1;                                                                                     /* CdlSetmode, CdlPlay track, CdlSetsession */
        if (a1 && len) e->a[4] = fnv(h, a1, len);
        e->a[1] = a1 != 0; e->a[2] = a2 != 0;
    }
    else if (streq(n, "CdSync") || streq(n, "CdIntToPos")) { e->a[1 + (streq(n, "CdSync") ? 0 : 0)] = a1 != 0; }
    else if (streq(n, "CdPosToInt")) { e->a[4] = fnv(h, a0, 4); e->a[0] = 0; }
}

unsigned hle_call(hle_t* h, const char* n, unsigned nargs, unsigned a0, unsigned a1, unsigned a2, unsigned a3) {
    if (nargs < 4) a3 = 0;                                                      /* registers beyond the function's parameters carry whatever the caller left there */
    if (nargs < 3) a2 = 0;
    if (nargs < 2) a1 = 0;
    if (nargs < 1) a0 = 0;
    h->calls++;
    trace_call(h, n, a0, a1, a2, a3);
    if (streq(n, "VSync")) {
        if ((int)a0 == 0) {                                                     /* wait for the next vertical blank: run the game's own callback once */
            h->frame_counter++;
            if (h->cb_vsync) h->p.call(h->p.ctx, h->cb_vsync, 0, 0);
            h->sync_reason = HLE_SYNC_VSYNC;
            if (h->p.frame) h->p.frame(h->p.ctx);
        }
        return h->frame_counter;
    }
    if (streq(n, "VSyncCallback")) { h->cb_vsync = a0; log_call(h, n, a0, a1, a2); return 0; }
    if (streq(n, "DrawSyncCallback")) { h->cb_drawsync = a0; log_call(h, n, a0, a1, a2); return 0; }
    if (streq(n, "CdReadyCallback")) { h->cb_cd_ready = a0; log_call(h, n, a0, a1, a2); return 0; }
    if (streq(n, "CdReadCallback")) { h->cb_cd_read = a0; log_call(h, n, a0, a1, a2); return 0; }
    if (streq(n, "SpuSetTransferCallback")) { h->cb_spu_transfer = a0; log_call(h, n, a0, a1, a2); return 0; }
    if (streq(n, "SpuWrite") || streq(n, "SpuRead") || streq(n, "SpuWrite0")) {  /* the DMA finishes at once: run the transfer callback */
        log_call(h, n, a0, a1, a2);
        if (h->cb_spu_transfer) h->p.call(h->p.ctx, h->cb_spu_transfer, 0, 0);
        return a1;
    }
    if (streq(n, "SpuIsTransferCompleted")) return 1;
    if (streq(n, "CdInit")) { log_call(h, n, a0, a1, a2); return 1; }
    if (streq(n, "CdIntToPos")) {                                               /* sector number -> minute/second/sector (BCD) at a1 */
        unsigned v = a0 + 150u, m = v / 75u / 60u, sc = v / 75u % 60u, f = v % 75u;
        h->p.w8(h->p.ctx, a1, ((m / 10) << 4) | (m % 10));
        h->p.w8(h->p.ctx, a1 + 1, ((sc / 10) << 4) | (sc % 10));
        h->p.w8(h->p.ctx, a1 + 2, ((f / 10) << 4) | (f % 10));
        h->p.w8(h->p.ctx, a1 + 3, 0);
        return a1;
    }
    if (streq(n, "CdPosToInt")) return (bcd(h->p.r8(h->p.ctx, a0)) * 60u + bcd(h->p.r8(h->p.ctx, a0 + 1))) * 75u + bcd(h->p.r8(h->p.ctx, a0 + 2)) - 150u;
    if (streq(n, "CdControl") || streq(n, "CdControlB") || streq(n, "CdControlF")) {
        unsigned cmd = a0 & 0xff;
        if ((cmd == 0x02 || cmd == 0x15 || cmd == 0x16) && a1) {                /* CdlSetloc / SeekL / SeekP with a position (BCD minute, second, sector) */
            h->cd_lba = (bcd(h->p.r8(h->p.ctx, a1)) * 60u + bcd(h->p.r8(h->p.ctx, a1 + 1))) * 75u + bcd(h->p.r8(h->p.ctx, a1 + 2)) - 150u;
        }
        if (a2) h->p.w8(h->p.ctx, a2, 0x02);                                    /* status: motor on */
        log_call(h, n, a0, a1, a2);
        return 1;
    }
    if (streq(n, "CdRead")) {
        log_call(h, n, a0, a1, a2);
        cd_read(h, h->cd_lba, a0, a1);
        if (h->cb_cd_read) h->p.call(h->p.ctx, h->cb_cd_read, 1, 0);           /* CdlComplete */
        if (h->p.is_overlay && h->p.is_overlay(h->p.ctx, h->cd_lba)) {         /* a code overlay has landed in RAM: hand over so that the driver can switch modules */
            h->sync_reason = HLE_SYNC_OVERLAY; h->sync_arg0 = a1; h->sync_arg1 = a0; h->sync_arg2 = h->cd_lba; h->stop = 1;
            if (h->p.frame) h->p.frame(h->p.ctx);
        }
        return 1;
    }
    if (streq(n, "CdSync")) { if (a1) h->p.w8(h->p.ctx, a1, 0x02); return 2; }
    if (streq(n, "CdReadSync")) return 0;
    if (streq(n, "_otc")) {                                                    /* ClearOTagR's DMA channel 6: link ot[count-1] -> ... -> ot[0] (the chain runs from the highest entry down) */
        unsigned i;
        for (i = a1; i-- > 1;) { unsigned link = (a0 + 4 * (i - 1)) & 0x00ffffffu, b; for (b = 0; b < 4; b++) h->p.w8(h->p.ctx, a0 + 4 * i + b, (link >> (8 * b)) & 0xff); }
        if (a1) { unsigned b; for (b = 0; b < 4; b++) h->p.w8(h->p.ctx, a0 + b, b == 3 ? 0x00 : 0xff); }
        return a1;
    }
    if (streq(n, "PadRead")) return h->pad_mask;                                 /* the scripted controller state (PSX_PAD_*: START 0x800, CROSS 0x40, CIRCLE 0x20, ...) */
    if (streq(n, "DrawSync") || streq(n, "TestEvent")) return 0;
    log_call(h, n, a0, a1, a2);
    return 0;
}
