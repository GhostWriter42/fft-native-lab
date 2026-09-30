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

unsigned hle_call(hle_t* h, const char* n, unsigned a0, unsigned a1, unsigned a2, unsigned a3) {
    (void)a3;
    h->calls++;
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
        if (h->p.is_overlay && h->p.is_overlay(h->p.ctx, h->cd_lba)) {         /* code overlay: hand over BEFORE the machine code lands in RAM */
            h->sync_reason = HLE_SYNC_OVERLAY; h->sync_arg0 = a1; h->sync_arg1 = a0; h->sync_arg2 = h->cd_lba; h->stop = 1;
            if (h->p.frame) h->p.frame(h->p.ctx);
            return 1;
        }
        log_call(h, n, a0, a1, a2);
        cd_read(h, h->cd_lba, a0, a1);
        if (h->cb_cd_read) h->p.call(h->p.ctx, h->cb_cd_read, 1, 0);           /* CdlComplete */
        return 1;
    }
    if (streq(n, "CdSync")) { if (a1) h->p.w8(h->p.ctx, a1, 0x02); return 2; }
    if (streq(n, "CdReadSync")) return 0;
    if (streq(n, "DrawSync") || streq(n, "PadRead") || streq(n, "TestEvent")) return 0;
    log_call(h, n, a0, a1, a2);
    return 0;
}
