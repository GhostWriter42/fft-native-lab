/* HLE of the PlayStation SDK hardware layer -- see hle.h. */
#include "hle.h"
#include "gpu.h"
#include "spu.h"
#include "card.h"

static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static unsigned bcd(unsigned v) { return (v >> 4) * 10 + (v & 15); }

void hle_init(hle_t* h, const hle_platform_t* p) {
    unsigned char* q = (unsigned char*)h;
    unsigned i;
    for (i = 0; i < sizeof *h; i++) q[i] = 0;
    h->p = *p;
}

/* One vertical blank: the game's VSync callback, and the root-counter-2 event handler the sound driver installs (SuzukiSPUInitialiser: OpenEvent(0xf2000002, ..., handler), SetRCnt(..., 0x44e8):
 * 17640 counter ticks = 240 Hz) four times -- the driver's music sequencer and sound effects only advance when it is called. Both machines of the lockstep do the same. */
static void fire_vblank(hle_t* h) {
    h->in_cb++;                                                                 /* SDK calls made by the callbacks are traced even while a re-executed VSync(n) of the interpreter is not */
    if (h->cb_vsync) h->p.call(h->p.ctx, h->cb_vsync, 0, 0);
    if (h->rcnt2_handler && h->ev[h->rcnt2_slot].used && h->ev[h->rcnt2_slot].enabled) { int k; for (k = 0; k < 4; k++) h->p.call(h->p.ctx, h->rcnt2_handler, 0, 0); }
    h->in_cb--;
}

void hle_tick(hle_t* h) {
    h->tick_count++;
    if ((h->tick_count & 3u) == 0) {
        h->frame_counter++;
        h->hblank_quarters = 0;
        fire_vblank(h);
    }
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
static int ot_valid(hle_t* h, unsigned addr) {
    if (h->p.valid) return h->p.valid(h->p.ctx, addr);
    return addr >= 0x80000000u && addr < 0x80200000u;
}
/* the significant bits of word k (1 = the colour/command word) of a GPU packet whose command byte is `code`: the pad fields of the libgpu packet
 * structs (the unused upper halves of the uv words of triangles/quads, the upper byte of the extra colour words) are whatever the caller's stack
 * held, and the GPU ignores them */
static unsigned ot_mask(unsigned code, unsigned k) {
    unsigned m = 0xffffffffu;
    switch (code & 0xfcu) {
    case 0x24: if (k == 7) m = 0x0000ffffu; break;                                     /* POLY_FT3 */
    case 0x2c: if (k == 7 || k == 9) m = 0x0000ffffu; break;                           /* POLY_FT4 */
    case 0x30: if (k == 3 || k == 5) m = 0x00ffffffu; break;                           /* POLY_G3 */
    case 0x34: if (k == 4 || k == 7) m = 0x00ffffffu; else if (k == 9) m = 0x0000ffffu; break;    /* POLY_GT3 */
    case 0x38: if (k == 3 || k == 5 || k == 7) m = 0x00ffffffu; break;                 /* POLY_G4 */
    case 0x3c: if (k == 4 || k == 7 || k == 10) m = 0x00ffffffu; else if (k == 9 || k == 12) m = 0x0000ffffu; break;   /* POLY_GT4 */
    case 0x50: if (k == 3) m = 0x00ffffffu; break;                                     /* LINE_G2 */
    }
    /* a textured primitive with the "raw texture" bit (command bit 0) ignores its colour: the r/g/b bytes are whatever the caller left there */
    if ((code & 1u) && (code & 4u) && ((code & 0xe0u) == 0x20u || (code & 0xe0u) == 0x60u)) {
        if (k == 1) m &= 0xff000000u;
        else if ((code & 0xfcu) == 0x34u && (k == 4 || k == 7)) m = 0;
        else if ((code & 0xfcu) == 0x3cu && (k == 4 || k == 7 || k == 10)) m = 0;
    }
    return m;
}
/* a GPU ordering table by CONTENT: every packet on the chain (its length and words), never the addresses (the two machines' stacks differ) */
static unsigned hash_ot(hle_t* h, unsigned ot) {
    unsigned x = 2166136261u, addr = ot, guard = 0;
    struct hle_otdump* d = h->otdump_n < HLE_OTDUMPS ? &h->otdump[h->otdump_n++] : 0;
    if (d) { d->trace_idx = h->trace_n - 1; d->n = 0; }
    while (guard++ < 200000) {
        unsigned tag, len, next, k;
        if (!ot_valid(h, addr)) {                                               /* the chain leaves memory: remember where (a corrupt table), and stop */
            if (!h->bad_ot_addr) { h->bad_ot_addr = addr; h->bad_ot_head = ot; }
            return x ^ 0xbad0bad0u;
        }
        tag = rd32(h, addr); len = tag >> 24; next = tag & 0x00ffffffu;
        if (len && !ot_valid(h, addr + 4 * (len < 63u ? len : 63u))) { if (!h->bad_ot_addr) { h->bad_ot_addr = addr; h->bad_ot_tag = tag; h->bad_ot_head = ot; } return x ^ 0xbad0bad0u; }
        x = (x ^ len) * 16777619u;
        {
            unsigned code = len ? rd32(h, addr + 4) >> 24 : 0;
            if (d && d->n + 2 + (len < 63u ? len : 63u) <= HLE_OTDUMP_WORDS) d->w[d->n++] = addr, d->w[d->n++] = len;
            else if (d) d = 0;
            for (k = 1; k <= len && k < 64; k++) {
                unsigned w = rd32(h, addr + 4 * k) & ot_mask(code, k);
                if (d) d->w[d->n++] = w;
                x = (x ^ w) * 16777619u;
            }
        }
        if (next == 0x00ffffffu) break;
        addr = 0x80000000u | next;
        if (!ot_valid(h, addr)) { if (!h->bad_ot_addr) { h->bad_ot_addr = addr; h->bad_ot_tag = tag; h->bad_ot_head = ot; } return x ^ 0xbad0bad0u; }
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
    { int k; for (k = 0; k < 4; k++) if ((e->a[k] >= 0x801f0000u && e->a[k] < 0x80200000u) || (e->a[k] >= h->p.stack_lo && e->a[k] < h->p.stack_hi)) e->a[k] = 0x801f0000u; }
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

/* ------------------------------------------------------------------------------------------------------ libspu's own RAM state
 * The real libspu keeps bookkeeping in main RAM that code the HLE does NOT replace reads back (the SPU heap SpuMalloc allocates from, the reverb attributes
 * SpuGetReverbModeParam reports, the key-on mask). The replaced calls therefore update it exactly as the decomp's src/psyq/libspu sources do (_SpuInit,
 * SpuSetKey, SpuSetReverb, SpuSetReverbModeParam, SpuSetReverbDepth); only the hardware side is left to the SPU model. Addresses: SCUS_942.21 data. */
#define SPU_KEYSTAT        0x8002a8dcu
#define SPU_REV_FLAG       0x8002a8e4u
#define SPU_REV_RESERVE_WA 0x8002a8e8u
#define SPU_REV_OFFSETADDR 0x8002a8ecu
#define SPU_REV_MODE       0x8002a8f4u
#define SPU_REV_DEPTH_L    0x8002a8f8u
#define SPU_REV_DEPTH_R    0x8002a8fau
#define SPU_REV_DELAY      0x8002a8fcu
#define SPU_REV_FEEDBACK   0x8002a900u
#define SPU_MEM_MODE_PLUS  0x8002ad6cu
#define SPU_ALLOC_BLOCKS   0x8002ada0u
#define SPU_ALLOC_LAST     0x8002ada4u
#define SPU_MEMLIST        0x8002ada8u
#define SPU_REV_STARTADDR  0x8002adacu
static void wr16(hle_t* h, unsigned a, unsigned v) { h->p.w8(h->p.ctx, a, v & 0xff); h->p.w8(h->p.ctx, a + 1, (v >> 8) & 0xff); }
static void wr32(hle_t* h, unsigned a, unsigned v) { wr16(h, a, v & 0xffff); wr16(h, a + 2, v >> 16); }
static int spu_in_allocated_area(hle_t* h, unsigned address) {                     /* _SpuIsInAllocateArea_: does a reserved heap block reach `address`? */
    unsigned list = rd32(h, SPU_MEMLIST), i;
    address <<= rd32(h, SPU_MEM_MODE_PLUS);
    if (!list) return 0;
    for (i = 0; i < 4096; i++) {
        unsigned start = rd32(h, list + 8 * i);
        if (start & 0x80000000u) continue;                                        /* free block */
        if (start & 0x40000000u) break;                                           /* the tail */
        start &= 0x0fffffffu;
        if (start >= address || address < start + rd32(h, list + 8 * i + 4)) return 1;
    }
    return 0;
}
/* Returns 1 when the call's result is decided here (*ret), 0 when only state was updated (or the name is not libspu's). */
static int libspu_state(hle_t* h, const char* n, unsigned a0, unsigned a1, unsigned a2, unsigned* ret) {
    if (n[0] != 'S' || n[1] != 'p' || n[2] != 'u') return 0;
    if (streq(n, "SpuInit") || streq(n, "SpuInitHot")) {                          /* _SpuInit */
        wr32(h, SPU_REV_FLAG, 0); wr32(h, SPU_REV_RESERVE_WA, 0); wr32(h, SPU_REV_MODE, 0);
        wr16(h, SPU_REV_DEPTH_L, 0); wr16(h, SPU_REV_DEPTH_R, 0); wr32(h, SPU_REV_DELAY, 0); wr32(h, SPU_REV_FEEDBACK, 0);
        wr32(h, SPU_REV_OFFSETADDR, rd32(h, SPU_REV_STARTADDR));
        wr32(h, SPU_ALLOC_BLOCKS, 0); wr32(h, SPU_ALLOC_LAST, 0); wr32(h, SPU_MEMLIST, 0); wr32(h, SPU_KEYSTAT, 0);
        return 0;
    }
    if (streq(n, "SpuSetKey")) {
        unsigned bits = a1 & 0xffffffu, k = rd32(h, SPU_KEYSTAT);
        if (a0 == 1) wr32(h, SPU_KEYSTAT, k | bits); else if (a0 == 0) wr32(h, SPU_KEYSTAT, k & ~bits);
        return 0;
    }
    if (streq(n, "SpuSetReverb")) {                                               /* returns the new _spu_rev_flag */
        if (a0 == 0) wr32(h, SPU_REV_FLAG, 0);
        else if (a0 == 1) wr32(h, SPU_REV_FLAG, (rd32(h, SPU_REV_RESERVE_WA) != 1 && spu_in_allocated_area(h, rd32(h, SPU_REV_OFFSETADDR))) ? 0u : 1u);
        *ret = rd32(h, SPU_REV_FLAG);
        return 1;
    }
    if (streq(n, "SpuSetReverbDepth")) {
        unsigned mask = rd32(h, a0);
        if (mask == 0 || (mask & 2)) wr16(h, SPU_REV_DEPTH_L, h->p.r8(h->p.ctx, a0 + 8) | (h->p.r8(h->p.ctx, a0 + 9) << 8));
        if (mask == 0 || (mask & 4)) wr16(h, SPU_REV_DEPTH_R, h->p.r8(h->p.ctx, a0 + 10) | (h->p.r8(h->p.ctx, a0 + 11) << 8));
        return 0;
    }
    if (streq(n, "SpuSetReverbModeParam")) {                                      /* attr: mask, mode, depth.left/right, delay, feedback */
        unsigned mask = rd32(h, a0), all = mask == 0, changed = 0, mode;
        *ret = 0;
        if (all || (mask & 1)) {
            mode = rd32(h, a0 + 4) & ~0x100u;
            if (mode >= 10 || spu_in_allocated_area(h, rd32(h, SPU_REV_STARTADDR + 4 * mode))) { *ret = 0xffffffffu; return 1; }
            changed = 1;
            wr32(h, SPU_REV_MODE, mode);
            wr32(h, SPU_REV_OFFSETADDR, rd32(h, SPU_REV_STARTADDR + 4 * mode));
            wr32(h, SPU_REV_FEEDBACK, mode == 7 ? 127u : 0u);
            wr32(h, SPU_REV_DELAY, mode == 7 || mode == 8 ? 127u : 0u);
        }
        mode = rd32(h, SPU_REV_MODE);
        if (all || (mask & 8)) wr32(h, SPU_REV_DELAY, (int)mode >= 7 && (int)mode < 9 ? rd32(h, a0 + 12) : 0u);
        if (all || (mask & 0x10)) wr32(h, SPU_REV_FEEDBACK, (int)mode >= 7 && (int)mode < 9 ? rd32(h, a0 + 16) : 0u);
        if (changed) { wr16(h, SPU_REV_DEPTH_L, 0); wr16(h, SPU_REV_DEPTH_R, 0); }
        else {
            if (all || (mask & 2)) wr16(h, SPU_REV_DEPTH_L, h->p.r8(h->p.ctx, a0 + 8) | (h->p.r8(h->p.ctx, a0 + 9) << 8));
            if (all || (mask & 4)) wr16(h, SPU_REV_DEPTH_R, h->p.r8(h->p.ctx, a0 + 10) | (h->p.r8(h->p.ctx, a0 + 11) << 8));
        }
        return 1;
    }
    if (streq(n, "SpuGetVoiceEnvelopeAttr")) {
        /* (voice, &key_stat, &envx): the envelope level is the SPU hardware's, which no machine here models in lockstep (the SPU model is attached to the
         * native machine only, for sound). Deterministic stand-in from the key-on mask: a keyed-on voice reports full level, a keyed-off one 0 (its release is
         * over at once). The music driver only tests envx == 0 (SMD opcode 0xFF waits for it); before, both values were left as stack garbage. Retail writes
         * key_stat as a halfword. */
        unsigned on = a0 < 24 && (rd32(h, SPU_KEYSTAT) >> a0) & 1u, envx = on ? 0x7fffu : 0u;
        if (a2) wr16(h, a2, envx);
        if (a1) wr16(h, a1, on ? 1u : 0u);
        *ret = 0;
        return 1;
    }
    return 0;
}

unsigned hle_call(hle_t* h, const char* n, unsigned nargs, unsigned a0, unsigned a1, unsigned a2, unsigned a3) {
    if (nargs < 4) a3 = 0;                                                      /* registers beyond the function's parameters carry whatever the caller left there */
    if (nargs < 3) a2 = 0;
    if (nargs < 2) a1 = 0;
    if (nargs < 1) a0 = 0;
    if (!(h->p.interp_reexec && h->vsync_left && !h->in_cb)) { h->calls++; trace_call(h, n, a0, a1, a2, a3); }      /* a re-executed VSync is one call */
    {   unsigned lr = 0, r;
        int decided = libspu_state(h, n, a0, a1, a2, &lr);                       /* libspu's RAM bookkeeping first (both machines) */
        if (h->spu && spu_hle_call(h, h->spu, n, a0, a1, a2, a3, &r)) return decided ? lr : r;   /* the software SPU, when one is attached (SpuWrite is only observed: the generic code runs its callback) */
        if (decided) { log_call(h, n, a0, a1, a2); return lr; }
    }          /* the software SPU, when one is attached (SpuWrite is only observed: the generic code runs its callback) */
    if (h->gpu) { unsigned r; if (gpu_hle_call(h, n, a0, a1, a2, a3, &r)) return r; }          /* the software GPU, when one is attached */
    if (streq(n, "VSync")) {
        /* VSync(0): wait for the next vertical blank; VSync(n >= 2): wait until n blanks have passed since the previous call (the machine does no work
         * in zero time, so: n blanks); VSync(1) / VSync(-1): report, do not wait. Each blank runs the game's own vertical-blank callback. */
        int mode = (int)a0;
        unsigned frames = mode == 0 ? 1u : (mode >= 2 ? (unsigned)mode : 0u), k;
        /* Fast effects (an option, not the original): while an ability's effect plays, battle_state_sync_frame paces the battle at 2-4 blanks per frame
         * (30 / 20 / 15 fps). With the option the battle waits one blank in those two states (g_battle_game_state 0x2d ACTION_EXECUTE, 0x33 EFFECT):
         * the effects play at 60 fps. Only the number of blanks changes; the music driver still runs on every blank. */
        if (h->fast_effects && frames > 1) {
            unsigned st = rd32(h, 0x800960e4u);
            if (st == 0x2du || st == 0x33u) frames = 1;
        }
        /* VSync(1) reads the horizontal-blank counter (scanlines since the last vertical blank); the game's AI time-slices itself on it (`VSync(1) >= 0x145`).
         * No machine here runs in real time, so the counter is a function of the call history alone (identical on both machines): one scanline per four queries,
         * restarted at every blank -- an AI pass gets a few hundred queries per frame instead of never (frame counter) or always (a large value). */
        if (mode == 1) { unsigned lines = h->hblank_quarters / 4u; h->hblank_quarters++; return lines; }
        if (h->p.interp_reexec) {                                               /* one blank per execution; the driver comes back for the rest */
            if (h->vsync_left) frames = h->vsync_left;
            if (frames) {
                h->frame_counter++;
                h->hblank_quarters = 0;
                fire_vblank(h);
                h->sync_reason = HLE_SYNC_VSYNC;
                h->vsync_left = frames - 1;
                h->reexec = h->vsync_left != 0;
                if (h->p.frame) h->p.frame(h->p.ctx);
            }
            return h->frame_counter;
        }
        for (k = 0; k < frames; k++) {
            h->frame_counter++;
            h->hblank_quarters = 0;
            fire_vblank(h);
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
        if (cmd == 0x09 || cmd == 0x08) h->cd_streaming = 0;                    /* CdlPause / CdlStop end a CdRead2 stream */
        if (a2) h->p.w8(h->p.ctx, a2, 0x02);                                    /* status: motor on */
        log_call(h, n, a0, a1, a2);
        return 1;
    }
    /* Raw sector streaming (libcd's CdRead2 family, used by the world map's image loader): the disc delivers one 2048-byte sector per CdGetSector, instantly. */
    if (streq(n, "CdRead2")) { log_call(h, n, a0, a1, a2); h->cd_streaming = 1; return 1; }
    if (streq(n, "CdReady")) { if (a1) h->p.w8(h->p.ctx, a1, 0x02); return h->cd_streaming ? 1u : 0u; }              /* CdlDataReady while streaming, else CdlNoIntr */
    if (streq(n, "CdGetSector")) {                                              /* madr, size in words: the next sector's data */
        unsigned char sector[2048];
        unsigned k;
        if (!h->cd_streaming) return 0;
        if (!h->p.read_sector(h->p.ctx, h->cd_lba, sector)) for (k = 0; k < 2048; k++) sector[k] = 0;
        h->p.write_bytes(h->p.ctx, a0, sector, a1 * 4u < 2048u ? a1 * 4u : 2048u);
        h->cd_lba++;
        return 1;
    }
    if (streq(n, "CdDataSync")) return 0;                                       /* the transfer is complete */
    if (streq(n, "CdStatus")) return 0x02;                                      /* motor on, no error, shell closed */
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
    /* BIOS event system. Event handles are 0xf1000000 | (slot + 1). */
    if (streq(n, "OpenEvent")) {
        unsigned i;
        for (i = 0; i < HLE_EVENTS; i++) if (!h->ev[i].used) { h->ev[i].used = 1; h->ev[i].desc = a0; h->ev[i].spec = a1; h->ev[i].enabled = 0; h->ev[i].ready = 0; if (a0 == 0xf2000002u) { h->rcnt2_handler = a3; h->rcnt2_slot = i; } return 0xf1000000u | (i + 1); }
        return 0xffffffffu;
    }
    if (streq(n, "EnableEvent") || streq(n, "DisableEvent") || streq(n, "CloseEvent") || streq(n, "TestEvent") || streq(n, "WaitEvent")) {
        unsigned i = (a0 & 0xffffffu) - 1, r = 0;
        if ((a0 >> 24) == 0xf1 && i < HLE_EVENTS && h->ev[i].used) {
            if (streq(n, "EnableEvent")) { h->ev[i].enabled = 1; r = 1; }
            else if (streq(n, "DisableEvent")) { h->ev[i].enabled = 0; r = 1; }
            else if (streq(n, "CloseEvent")) { h->ev[i].used = 0; r = 1; }
            else { r = h->ev[i].ready; h->ev[i].ready = 0; }                    /* TestEvent / WaitEvent: consume the flag */
        }
        return r;
    }
    if (streq(n, "DeliverEvent") || streq(n, "UnDeliverEvent")) {
        unsigned i;
        for (i = 0; i < HLE_EVENTS; i++) if (h->ev[i].used && h->ev[i].desc == a0 && h->ev[i].spec == a1 && (h->ev[i].enabled || streq(n, "UnDeliverEvent"))) h->ev[i].ready = streq(n, "DeliverEvent");
        return 1;
    }
    if (h->card) { unsigned r; if (card_hle_call(h, h->card, n, a0, a1, a2, a3, &r)) { log_call(h, n, a0, a1, a2); return r; } }      /* the virtual memory card (card.c) */
    /* Memory cards: a console with NO card inserted. Every card command is accepted and answered with the software-card TIMEOUT event, which is what the BIOS
     * delivers when nothing responds. (0xf4000001 = SwCARD, 0x0100 = EvSpTIMOUT.) */
    if (streq(n, "_card_info") || streq(n, "_card_load") || streq(n, "_card_write") || streq(n, "_card_read") || streq(n, "_new_card")) {
        unsigned i;
        log_call(h, n, a0, a1, a2);
        for (i = 0; i < HLE_EVENTS; i++) if (h->ev[i].used && h->ev[i].enabled && h->ev[i].desc == 0xf4000001u && h->ev[i].spec == 0x0100u) h->ev[i].ready = 1;
        return 1;
    }
    if (streq(n, "PadRead")) return h->pad_mask;                                 /* the scripted controller state (PSX_PAD_*: START 0x800, CROSS 0x40, CIRCLE 0x20, ...) */
    if (streq(n, "DrawSync")) return 0;
    if (streq(n, "StoreImage")) {                                               /* VRAM -> RAM. The VRAM is not modelled, but the buffer must not keep its old bytes: the game copies parts of it around (the  */
        static const unsigned char zeros[256];                                  /* BATTLE scroll list derives CLUT rows from one, over the code of whatever overlay is resident -- natively full of trampolines). */
        unsigned w = h->p.r8(h->p.ctx, a0 + 4) | (h->p.r8(h->p.ctx, a0 + 5) << 8), rows = h->p.r8(h->p.ctx, a0 + 6) | (h->p.r8(h->p.ctx, a0 + 7) << 8), bytes = w * rows * 2u, off;
        if (bytes > 0x100000u) bytes = 0x100000u;
        for (off = 0; off < bytes; off += 256) h->p.write_bytes(h->p.ctx, a1 + off, zeros, bytes - off < 256 ? bytes - off : 256);
        return 0;
    }
    log_call(h, n, a0, a1, a2);
    return 0;
}
