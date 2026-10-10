/* A virtual memory card -- see card.h. The raw card layout (psx-spx, "Memory Card Data Format"): 1024 frames of 128 bytes; block 0 holds frame 0 = "MC"
 * header, frames 1..15 = one directory entry per data block (state, file size, next block, file name, XOR checksum in byte 127), frames 16..35 = the
 * (empty) broken-sector list, frame 63 = a copy of the header; blocks 1..15 hold the files. The BIOS behaviour follows the same document, A(32h)..A(46h):
 * card I/O moves whole 128-byte frames; lseek knows SEEK_SET (0) and SEEK_CUR (1). (The game seeks with SEEK_CUR to "absolute" offsets only where the
 * position is still 0, and rewrites the 0x80-byte block at 0x100 with lseek(0x100, SET) + lseek(0, CUR): a SEEK_CUR read as SEEK_SET puts it over the header.) */
#include "hle.h"
#include "card.h"

#define BLOCK 8192u
#define FRAME 128u
#define ST_FREE 0xa0u
#define ST_FIRST 0x51u
#define ST_MIDDLE 0x52u
#define ST_LAST 0x53u
/* BIOS events (kernel.h): the software card events (SwCARD) for the BIOS calls, the hardware ones (HwCARD) for _card_clear */
#define EV_SWCARD 0xf4000001u
#define EV_HWCARD 0xf0000011u
#define EVSP_IOE 0x0004u
#define EVSP_TIMEOUT 0x0100u

static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void put16(unsigned char* p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void put32(unsigned char* p, unsigned v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }
static unsigned get16(const unsigned char* p) { return p[0] | (p[1] << 8); }
static unsigned get32(const unsigned char* p) { return get16(p) | (get16(p + 2) << 16); }
static unsigned char* dir(mcard_t* c, unsigned block) { return c->img + block * FRAME; }        /* the directory entry of data block 1..15 */
static void checksum(unsigned char* f) { unsigned x = 0, i; for (i = 0; i < 127; i++) x ^= f[i]; f[127] = (unsigned char)x; }

void card_format(mcard_t* c) {
    unsigned i, b;
    for (i = 0; i < sizeof *c; i++) ((unsigned char*)c)[i] = 0;
    c->img[0] = 'M'; c->img[1] = 'C'; checksum(c->img);
    for (b = 1; b < 16; b++) { unsigned char* d = dir(c, b); put32(d, ST_FREE); put16(d + 8, 0xffff); checksum(d); }
    for (i = 16; i < 36; i++) { unsigned char* f = c->img + i * FRAME; put32(f, 0xffffffffu); put16(f + 8, 0xffff); checksum(f); }
    for (i = 0; i < FRAME; i++) c->img[63 * FRAME + i] = c->img[i];
    c->dirty = 1;
}
int card_valid(const mcard_t* c) { return c->img[0] == 'M' && c->img[1] == 'C'; }

static void deliver(hle_t* h, unsigned desc, unsigned spec) {                   /* DeliverEvent: what the BIOS card driver does when an operation ends */
    unsigned i;
    for (i = 0; i < HLE_EVENTS; i++) if (h->ev[i].used && h->ev[i].enabled && h->ev[i].desc == desc && h->ev[i].spec == spec) h->ev[i].ready = 1;
}
/* "bu00:NAME" -> slot (0 / 1) and the name; -1 when it is not a card path */
static int parse_path(hle_t* h, unsigned addr, char* name, unsigned max) {
    char dev[5];
    unsigned i;
    for (i = 0; i < 5; i++) dev[i] = (char)h->p.r8(h->p.ctx, addr + i);
    if (dev[0] != 'b' || dev[1] != 'u' || dev[4] != ':') return -1;
    for (i = 0; i + 1 < max; i++) { char ch = (char)h->p.r8(h->p.ctx, addr + 5 + i); if (!ch) break; name[i] = ch; }
    name[i] = 0;
    return dev[2] == '0' ? 0 : 1;
}
static int name_is(mcard_t* c, unsigned block, const char* name) {
    const unsigned char* d = dir(c, block);
    unsigned i;
    for (i = 0; i < 20; i++) { if (d[10 + i] != (unsigned char)name[i]) return 0; if (!name[i]) return 1; }
    return name[20] == 0;
}
static unsigned find_file(mcard_t* c, const char* name) {                       /* the first block of a file, 0 = none */
    unsigned b;
    for (b = 1; b < 16; b++) if (get32(dir(c, b)) == ST_FIRST && name_is(c, b, name)) return b;
    return 0;
}
static int match(const char* pat, const unsigned char* name) {                 /* the BIOS's wildcards: '?' one character, '*' the rest */
    unsigned i;
    for (i = 0; i < 20; i++) {
        if (pat[i] == '*') return 1;
        if (!pat[i]) return !name[i];
        if (pat[i] != '?' && (unsigned char)pat[i] != name[i]) return 0;
    }
    return 1;
}
static unsigned block_at(mcard_t* c, unsigned first, unsigned pos) {            /* the data block that holds byte `pos` of a file, 0 = past the end */
    unsigned b = first, k;
    for (k = 0; k < pos / BLOCK; k++) { b = get16(dir(c, b) + 8); if (b == 0xffff) return 0; b += 1; }
    return b;
}
static unsigned file_io(hle_t* h, mcard_t* c, unsigned fd, unsigned addr, unsigned n, int writing) {
    unsigned done = 0;
    if (fd >= CARD_FILES || !c->fd[fd].used) return 0xffffffffu;
    n &= ~(FRAME - 1);                                                           /* whole frames only */
    while (done < n && c->fd[fd].pos < c->fd[fd].size) {
        unsigned pos = c->fd[fd].pos, b = block_at(c, c->fd[fd].first, pos), off, k;
        if (!b) break;
        off = b * BLOCK + pos % BLOCK;
        for (k = 0; k < FRAME; k++) {
            if (writing) c->img[off + k] = (unsigned char)h->p.r8(h->p.ctx, addr + done + k);
            else h->p.w8(h->p.ctx, addr + done + k, c->img[off + k]);
        }
        done += FRAME; c->fd[fd].pos += FRAME;
    }
    if (writing && done) { c->dirty = 1; c->writes++; }
    return done;
}
static unsigned make_file(mcard_t* c, const char* name, unsigned blocks) {     /* allocate a zero-filled file of `blocks` blocks; its first block, 0 = no room */
    unsigned b, n = 0, list[15], k;
    if (!blocks) blocks = 1;
    for (b = 1; b < 16 && n < blocks; b++) if ((get32(dir(c, b)) & 0xf0u) == ST_FREE) list[n++] = b;
    if (n < blocks) return 0;
    for (k = 0; k < n; k++) {
        unsigned char* d = dir(c, list[k]);
        unsigned i;
        for (i = 0; i < FRAME; i++) d[i] = 0;
        put32(d, n == 1 ? ST_FIRST : k == 0 ? ST_FIRST : k + 1 == n ? ST_LAST : ST_MIDDLE);
        if (k == 0) { put32(d + 4, n * BLOCK); for (i = 0; i < 20 && name[i]; i++) d[10 + i] = (unsigned char)name[i]; }
        put16(d + 8, k + 1 < n ? list[k + 1] - 1 : 0xffff);                    /* the next block, counted from 0 = block 1 */
        checksum(d);
        for (i = 0; i < BLOCK; i++) c->img[list[k] * BLOCK + i] = 0;
    }
    c->dirty = 1;
    return list[0];
}
static void fill_direntry(hle_t* h, mcard_t* c, unsigned block, unsigned addr) {
    const unsigned char* d = dir(c, block);
    unsigned i;
    for (i = 0; i < 40; i++) h->p.w8(h->p.ctx, addr + i, 0);
    for (i = 0; i < 20 && d[10 + i]; i++) h->p.w8(h->p.ctx, addr + i, d[10 + i]);
    for (i = 0; i < 4; i++) {
        h->p.w8(h->p.ctx, addr + 20 + i, (0x50u >> (8 * i)) & 0xff);           /* attr */
        h->p.w8(h->p.ctx, addr + 24 + i, (get32(d + 4) >> (8 * i)) & 0xff);    /* size */
        h->p.w8(h->p.ctx, addr + 32 + i, (block >> (8 * i)) & 0xff);           /* head */
    }
}
static unsigned find_from(hle_t* h, mcard_t* c, unsigned addr) {
    for (; c->find_next < 16; c->find_next++) {
        unsigned b = (unsigned)c->find_next;
        if (get32(dir(c, b)) == ST_FIRST && match(c->find_pattern, dir(c, b) + 10)) { fill_direntry(h, c, b, addr); c->find_next++; return addr; }
    }
    return 0;
}

int card_hle_call(hle_t* h, mcard_t* c, const char* n, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned* ret) {
    char name[24];
    int slot;
    (void)a3;
    *ret = 0;
    /* low-level calls: channel / port 0x00 = slot 0 (the card), 0x10 = slot 1 (no card: the BIOS's timeout, as before) */
    if (streq(n, "_card_info") || streq(n, "_card_load") || streq(n, "_card_read") || streq(n, "_card_write") || streq(n, "_card_clear")) {
        unsigned desc = streq(n, "_card_clear") ? EV_HWCARD : EV_SWCARD;
        int present = (a0 & 0x10u) == 0 && card_valid(c);
        if (present && streq(n, "_card_read") && a1 < 1024) { unsigned k; for (k = 0; k < FRAME; k++) h->p.w8(h->p.ctx, a2 + k, c->img[a1 * FRAME + k]); }
        if (present && streq(n, "_card_write") && a1 < 1024) { unsigned k; for (k = 0; k < FRAME; k++) c->img[a1 * FRAME + k] = (unsigned char)h->p.r8(h->p.ctx, a2 + k); c->dirty = 1; c->writes++; }
        deliver(h, desc, present ? EVSP_IOE : EVSP_TIMEOUT);
        *ret = 1;
        return 1;
    }
    if (streq(n, "_new_card")) return 1;
    if (streq(n, "_card_status")) return 1;                                     /* 0: idle (bit 0 = busy) */
    if (streq(n, "_get_error")) return 1;                                       /* 0: no error on the descriptor */
    if (streq(n, "open")) {
        unsigned first, mode = a1, i;
        if ((slot = parse_path(h, a0, name, sizeof name)) != 0 || !card_valid(c)) { *ret = 0xffffffffu; return 1; }
        first = find_file(c, name);
        if (mode & 0x200u) {                                                    /* FCREAT: (blocks << 16) | 0x200; an existing file is an error */
            if (first) { *ret = 0xffffffffu; return 1; }
            first = make_file(c, name, mode >> 16);
        }
        if (!first) { *ret = 0xffffffffu; return 1; }
        for (i = 0; i < CARD_FILES; i++) if (!c->fd[i].used) break;
        if (i == CARD_FILES) { *ret = 0xffffffffu; return 1; }
        c->fd[i].used = 1; c->fd[i].first = first; c->fd[i].pos = 0; c->fd[i].size = get32(dir(c, first) + 4);
        *ret = 2 + i;                                                           /* the BIOS's descriptors 0 and 1 are the console's tty */
        return 1;
    }
    if (streq(n, "close")) { unsigned i = a0 - 2; if (i < CARD_FILES && c->fd[i].used) { c->fd[i].used = 0; *ret = a0; } else *ret = 0xffffffffu; return 1; }
    if (streq(n, "lseek")) {
        unsigned i = a0 - 2;
        unsigned to;
        if (i >= CARD_FILES || !c->fd[i].used || a2 > 1) { *ret = 0xffffffffu; return 1; }
        to = a2 == 1 ? c->fd[i].pos + a1 : a1;                                  /* SEEK_SET / SEEK_CUR */
        if (to > c->fd[i].size) { *ret = 0xffffffffu; return 1; }
        c->fd[i].pos = to;
        *ret = to;
        return 1;
    }
    if (streq(n, "read") || streq(n, "write")) { *ret = file_io(h, c, a0 - 2, a1, a2, n[0] == 'w'); return 1; }
    if (streq(n, "erase")) {
        unsigned b;
        if (parse_path(h, a0, name, sizeof name) != 0 || !(b = find_file(c, name))) return 1;
        for (;;) {
            unsigned char* d = dir(c, b);
            unsigned next = get16(d + 8);
            put32(d, ST_FREE | (get32(d) & 0x0fu));                              /* the BIOS marks the blocks deleted (A1 / A2 / A3) */
            checksum(d);
            if (next == 0xffff) break;
            b = next + 1;
        }
        c->dirty = 1;
        *ret = 1;
        return 1;
    }
    if (streq(n, "format")) { if (parse_path(h, a0, name, sizeof name) != 0) return 1; card_format(c); *ret = 1; return 1; }
    if (streq(n, "firstfile")) {
        unsigned i;
        if (parse_path(h, a0, name, sizeof name) != 0 || !card_valid(c)) return 1;
        for (i = 0; i < sizeof c->find_pattern - 1 && name[i]; i++) c->find_pattern[i] = name[i];
        c->find_pattern[i] = 0;
        if (!i) { c->find_pattern[0] = '*'; c->find_pattern[1] = 0; }
        c->find_next = 1;
        *ret = find_from(h, c, a1);
        return 1;
    }
    if (streq(n, "nextfile")) { *ret = find_from(h, c, a0); return 1; }
    return 0;
}
